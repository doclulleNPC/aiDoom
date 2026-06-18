// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Chocolate/Crispy-compatible network CLIENT (see d_netcl.h).  Stage 3 of
//	the multiplayer-interop work: the connection state machine, the reliable-
//	packet layer, and the GAMEDATA send/receive tic windows -- a clean-room
//	reimplementation of the Chocolate-Doom protocol.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <windows.h>
  #define usleep(us) Sleep ((us) / 1000)
#else
  #include <unistd.h>
#endif

#include "doomtype.h"
#include "d_ticcmd.h"
#include "i_udp.h"
#include "d_netcl.h"

#define NET_RELIABLE_FLAG	0x8000
#define GAMEVERSION_DOOM_1_9	5		// exe_doom_1_9 (d_mode.h)

// ticcmd diff bitmask (net_defs.h NET_TICDIFF_*).  The high two bits (RAVEN,
// STRIFE) never appear in the Doom protocol.
#define TICDIFF_FORWARD		(1<<0)
#define TICDIFF_SIDE		(1<<1)
#define TICDIFF_TURN		(1<<2)
#define TICDIFF_BUTTONS		(1<<3)
#define TICDIFF_CONSISTANCY	(1<<4)
#define TICDIFF_CHATCHAR	(1<<5)
#define TICDIFF_UNSUPPORTED	0xc0

enum { CL_WAITING_LAUNCH, CL_WAITING_START, CL_IN_GAME };

typedef struct { byte mask; ticcmd_t vals; } netdiff_t;

typedef struct reliable_s
{
    struct reliable_s*	next;
    net_packet_t*	packet;
    unsigned		seq;		// byte sequence number of this packet
    int			last_send_time;	// -1 => never sent
} reliable_t;

typedef struct
{
    int			active;
    unsigned		seq;
    unsigned		time;
    netdiff_t		diff;
} sendslot_t;

typedef struct
{
    int			active;
    byte		ingame_bits;
    netdiff_t		d[NET_PROTO_MAXPLAYERS];
} recvslot_t;

// --- module state ---------------------------------------------------------
static int		connected;
static int		disconnected;
static int		clientstate;
static int		is_drone;
static int		lowres_turn;
static netcl_settings_t	settings;

static unsigned		reliable_send_seq;	// next byte seq for our sends
static unsigned		reliable_recv_seq;	// next byte seq we expect
static reliable_t*	reliable_head;

static unsigned		last_send_time;
static unsigned		last_recv_time;

// lobby status (from WAITING_DATA): how many players are connected and whether
// we are the controlling client (the only one whose LAUNCH the server honors).
static int		lobby_players = 1;
static int		lobby_is_controller;

// send side (our tics)
static ticcmd_t		last_sent;
static sendslot_t	send_queue[NET_BACKUPTICS];
static unsigned		maketic;

// receive side (merged tics from the server)
static recvslot_t	recvwindow[NET_BACKUPTICS];
static ticcmd_t		recv_base[NET_PROTO_MAXPLAYERS];
static unsigned		recvwindow_start;
static unsigned		recvtic;
// persistent ring of fully-merged tics, indexed by absolute tic % BACKUPTICS,
// so the game loop can pull them after the recv window has drained past.
static ticcmd_t		merged_cmds[NET_PROTO_MAXPLAYERS][NET_BACKUPTICS];
static byte		merged_ingame[NET_BACKUPTICS];
static int		need_to_acknowledge;
static unsigned		gamedata_recv_time;

// connect parameters (kept for SYN retransmit)
static char		cl_version[128];
static char		cl_playername[64];
static int		cl_gamemode, cl_gamemission;

//
// ---- low-level send helpers ----------------------------------------------
//
static void SendRaw (net_packet_t* p)
{
    UDP_Send (p);
    last_send_time = NET_GetTimeMS ();
}

static void SendSimple (int type)
{
    net_packet_t* p = NET_NewPacket (8);
    NET_WriteInt16 (p, type);
    SendRaw (p);
    NET_FreePacket (p);
}

//
// ---- reliable-packet layer -----------------------------------------------
//
static net_packet_t* NewReliable (int type)
{
    net_packet_t*	p = NET_NewPacket (32);
    reliable_t*		r;
    reliable_t*		t;

    NET_WriteInt16 (p, type | NET_RELIABLE_FLAG);
    NET_WriteInt8 (p, reliable_send_seq & 0xff);

    r = malloc (sizeof(*r));
    r->next = NULL;
    r->packet = p;
    r->seq = reliable_send_seq & 0xff;
    r->last_send_time = -1;

    if (!reliable_head)
	reliable_head = r;
    else { for (t = reliable_head ; t->next ; t = t->next) ; t->next = r; }

    reliable_send_seq = (reliable_send_seq + 1) & 0xff;
    return p;		// caller appends payload; RunReliable transmits it
}

static void RunReliable (unsigned now)
{
    reliable_t* r = reliable_head;		// only the head is ever (re)sent
    if (r && (r->last_send_time < 0 || (int)(now - r->last_send_time) > 1000))
    {
	SendRaw (r->packet);
	r->last_send_time = now;
    }
}

static void ParseReliableACK (net_packet_t* p)
{
    unsigned ack;
    if (!NET_ReadInt8 (p, &ack)) return;
    if (reliable_head && ack == ((reliable_head->seq + 1) & 0xff))
    {
	reliable_t* r = reliable_head;
	reliable_head = r->next;
	NET_FreePacket (r->packet);
	free (r);
    }
}

// Process the seq byte of an inbound reliable packet; ack it; return whether
// the payload is fresh (not a duplicate) and should be acted on.
static boolean ReliableRecv (net_packet_t* p)
{
    unsigned	seq;
    boolean	fresh;
    net_packet_t* ack;

    if (!NET_ReadInt8 (p, &seq)) return false;

    if (seq == (reliable_recv_seq & 0xff))
    { fresh = true; reliable_recv_seq = (reliable_recv_seq + 1) & 0xff; }
    else
	fresh = false;

    ack = NET_NewPacket (8);				// ack the NEXT expected seq
    NET_WriteInt16 (ack, NET_PACKET_TYPE_RELIABLE_ACK);
    NET_WriteInt8 (ack, reliable_recv_seq & 0xff);
    SendRaw (ack);
    NET_FreePacket (ack);
    return fresh;
}

//
// ---- ticcmd diff codec ---------------------------------------------------
//
static void ComputeDiff (ticcmd_t* prev, ticcmd_t* cur, netdiff_t* out)
{
    out->mask = 0;
    out->vals = *cur;
    if (cur->forwardmove != prev->forwardmove)	out->mask |= TICDIFF_FORWARD;
    if (cur->sidemove    != prev->sidemove)	out->mask |= TICDIFF_SIDE;
    if (cur->angleturn   != prev->angleturn)	out->mask |= TICDIFF_TURN;
    if (cur->buttons     != prev->buttons)	out->mask |= TICDIFF_BUTTONS;
    if (cur->consistancy != prev->consistancy)	out->mask |= TICDIFF_CONSISTANCY;
    if (cur->chatchar != 0)			out->mask |= TICDIFF_CHATCHAR;
}

static void WriteDiff (net_packet_t* p, netdiff_t* d, int lowres)
{
    NET_WriteInt8 (p, d->mask);
    if (d->mask & TICDIFF_FORWARD)	NET_WriteInt8 (p, (unsigned char) d->vals.forwardmove);
    if (d->mask & TICDIFF_SIDE)		NET_WriteInt8 (p, (unsigned char) d->vals.sidemove);
    if (d->mask & TICDIFF_TURN)
    {
	if (lowres)	NET_WriteInt8 (p, (d->vals.angleturn / 256) & 0xff);
	else		NET_WriteInt16 (p, (unsigned short) d->vals.angleturn);
    }
    if (d->mask & TICDIFF_BUTTONS)	NET_WriteInt8 (p, d->vals.buttons);
    if (d->mask & TICDIFF_CONSISTANCY)	NET_WriteInt8 (p, d->vals.consistancy & 0xff);
    if (d->mask & TICDIFF_CHATCHAR)	NET_WriteInt8 (p, d->vals.chatchar);
}

static boolean ReadDiff (net_packet_t* p, netdiff_t* d, int lowres)
{
    unsigned	m, uv;
    int		sv;

    if (!NET_ReadInt8 (p, &m)) return false;
    d->mask = m;
    memset (&d->vals, 0, sizeof(d->vals));
    if (m & TICDIFF_UNSUPPORTED) return false;		// Raven/Strife: not Doom

    if (m & TICDIFF_FORWARD)	{ if (!NET_ReadSInt8 (p, &sv)) return false; d->vals.forwardmove = sv; }
    if (m & TICDIFF_SIDE)	{ if (!NET_ReadSInt8 (p, &sv)) return false; d->vals.sidemove = sv; }
    if (m & TICDIFF_TURN)
    {
	if (lowres)	{ if (!NET_ReadSInt8 (p, &sv)) return false; d->vals.angleturn = sv * 256; }
	else		{ if (!NET_ReadSInt16 (p, &sv)) return false; d->vals.angleturn = sv; }
    }
    if (m & TICDIFF_BUTTONS)	{ if (!NET_ReadInt8 (p, &uv)) return false; d->vals.buttons = uv; }
    if (m & TICDIFF_CONSISTANCY){ if (!NET_ReadInt8 (p, &uv)) return false; d->vals.consistancy = uv; }
    if (m & TICDIFF_CHATCHAR)	{ if (!NET_ReadInt8 (p, &uv)) return false; d->vals.chatchar = uv; }
    return true;
}

static void ApplyDiff (ticcmd_t* base, netdiff_t* d, ticcmd_t* out)
{
    *out = *base;
    if (d->mask & TICDIFF_FORWARD)	out->forwardmove = d->vals.forwardmove;
    if (d->mask & TICDIFF_SIDE)		out->sidemove = d->vals.sidemove;
    if (d->mask & TICDIFF_TURN)		out->angleturn = d->vals.angleturn;
    if (d->mask & TICDIFF_BUTTONS)	out->buttons = d->vals.buttons;
    if (d->mask & TICDIFF_CONSISTANCY)	out->consistancy = d->vals.consistancy;
    out->chatchar = (d->mask & TICDIFF_CHATCHAR) ? d->vals.chatchar : 0;
}

static boolean ReadFullTiccmd (net_packet_t* p, recvslot_t* slot, int lowres)
{
    int		latency, i;
    unsigned	bits;

    if (!NET_ReadSInt16 (p, &latency)) return false;
    if (!NET_ReadInt8 (p, &bits)) return false;
    slot->ingame_bits = bits;
    for (i = 0 ; i < NET_PROTO_MAXPLAYERS ; i++)
    {
	memset (&slot->d[i], 0, sizeof(slot->d[i]));
	if (bits & (1 << i))
	    if (!ReadDiff (p, &slot->d[i], lowres))
		return false;
    }
    return true;
}

//
// ---- gamesettings codec --------------------------------------------------
//
static void WriteSettings (net_packet_t* p, const netcl_settings_t* s)
{
    NET_WriteInt8 (p, s->ticdup);
    NET_WriteInt8 (p, s->extratics);
    NET_WriteInt8 (p, s->deathmatch);
    NET_WriteInt8 (p, 0);			// nomonsters
    NET_WriteInt8 (p, 0);			// fast_monsters
    NET_WriteInt8 (p, 0);			// respawn_monsters
    NET_WriteInt8 (p, s->episode);
    NET_WriteInt8 (p, s->map);
    NET_WriteInt8 (p, s->skill);
    NET_WriteInt8 (p, GAMEVERSION_DOOM_1_9);
    NET_WriteInt8 (p, s->lowres_turn);
    NET_WriteInt8 (p, s->new_sync);
    NET_WriteInt32 (p, 0);			// timelimit
    NET_WriteInt8 (p, 0xff);			// loadgame = -1 (none)
    NET_WriteInt8 (p, 0);			// random [Strife]
    NET_WriteInt8 (p, s->num_players);
    NET_WriteInt8 (p, s->consoleplayer);
    {	int i; for (i = 0 ; i < s->num_players ; i++) NET_WriteInt8 (p, 0); }	// player_classes
}

static boolean ReadSettings (net_packet_t* p, netcl_settings_t* s)
{
    unsigned	ticdup, extratics, dm, nomon, fast, respawn, episode, map;
    unsigned	gameversion, lowres, newsync, random, num_players;
    int		skill, loadgame, consoleplayer;
    unsigned	timelimit;
    int		i;

    if (!NET_ReadInt8 (p, &ticdup) || !NET_ReadInt8 (p, &extratics)
     || !NET_ReadInt8 (p, &dm) || !NET_ReadInt8 (p, &nomon)
     || !NET_ReadInt8 (p, &fast) || !NET_ReadInt8 (p, &respawn)
     || !NET_ReadInt8 (p, &episode) || !NET_ReadInt8 (p, &map)
     || !NET_ReadSInt8 (p, &skill) || !NET_ReadInt8 (p, &gameversion)
     || !NET_ReadInt8 (p, &lowres) || !NET_ReadInt8 (p, &newsync)
     || !NET_ReadInt32 (p, &timelimit) || !NET_ReadSInt8 (p, &loadgame)
     || !NET_ReadInt8 (p, &random) || !NET_ReadInt8 (p, &num_players)
     || !NET_ReadSInt8 (p, &consoleplayer))
	return false;

    s->ticdup = ticdup ? ticdup : 1;
    s->extratics = extratics;
    s->deathmatch = dm;
    s->episode = episode;
    s->map = map;
    s->skill = skill;
    s->lowres_turn = lowres;
    s->new_sync = newsync;
    s->num_players = num_players;
    s->consoleplayer = consoleplayer;
    for (i = 0 ; i < (int)num_players && i < NET_PROTO_MAXPLAYERS ; i++)
    {
	unsigned cls;
	if (!NET_ReadInt8 (p, &cls)) return false;	// player_classes
    }
    return true;
}

//
// ---- sequence-number expansion (NET_ExpandTicNum) ------------------------
//
static unsigned ExpandTicNum (unsigned base, unsigned b)
{
    unsigned l = base & 0xff;
    unsigned h = base & ~0xffu;
    unsigned result = h | (b & 0xff);
    if (l < 0x40 && (b & 0xff) > 0xb0) result -= 0x100;
    if (l > 0xb0 && (b & 0xff) < 0x40) result += 0x100;
    return result;
}

//
// ---- GAMEDATA send/receive -----------------------------------------------
//
static void SendTics (int start, int end)
{
    net_packet_t*	p;
    int			i;

    if (start < 0) start = 0;
    if (end < start) return;

    p = NET_NewPacket (64);
    NET_WriteInt16 (p, NET_PACKET_TYPE_GAMEDATA);
    NET_WriteInt8 (p, recvwindow_start & 0xff);		// piggybacked ack
    NET_WriteInt8 (p, start & 0xff);
    NET_WriteInt8 (p, (end - start + 1) & 0xff);
    for (i = start ; i <= end ; i++)
    {
	NET_WriteInt16 (p, 0);				// latency
	WriteDiff (p, &send_queue[i % NET_BACKUPTICS].diff, lowres_turn);
    }
    SendRaw (p);
    NET_FreePacket (p);
    need_to_acknowledge = false;
}

void D_NetCl_SendTiccmd (ticcmd_t* cmd, int mt)
{
    int		start;
    sendslot_t*	slot = &send_queue[mt % NET_BACKUPTICS];

    ComputeDiff (&last_sent, cmd, &slot->diff);
    last_sent = *cmd;
    slot->active = 1;
    slot->seq = mt;
    slot->time = NET_GetTimeMS ();

    start = mt - settings.extratics;			// redundancy window
    SendTics (start, mt);
}

static void ParseGameData (net_packet_t* p)
{
    unsigned	seq, num, full;
    int		i, index;

    if (!NET_ReadInt8 (p, &seq) || !NET_ReadInt8 (p, &num)) return;
    need_to_acknowledge = true;
    gamedata_recv_time = NET_GetTimeMS ();
    full = ExpandTicNum (recvwindow_start, seq);

    for (i = 0 ; i < (int)num ; i++)
    {
	recvslot_t slot;
	memset (&slot, 0, sizeof(slot));
	if (!ReadFullTiccmd (p, &slot, lowres_turn)) break;
	index = (int)(full - recvwindow_start) + i;
	if (index >= 0 && index < NET_BACKUPTICS)
	{
	    slot.active = 1;
	    recvwindow[index] = slot;
	}
    }
}

static void AdvanceWindow (void)
{
    while (recvwindow[0].active)
    {
	int i, slot = recvtic % NET_BACKUPTICS;
	for (i = 0 ; i < NET_PROTO_MAXPLAYERS ; i++)
	{
	    if (recvwindow[0].ingame_bits & (1 << i))
	    {
		ticcmd_t out;
		ApplyDiff (&recv_base[i], &recvwindow[0].d[i], &out);
		recv_base[i] = out;		// running base for the diffs
	    }
	    merged_cmds[i][slot] = recv_base[i];	// last-known cmd for all
	}
	merged_ingame[slot] = recvwindow[0].ingame_bits;
	// The merged tic is now available via D_NetCl_GetTic() for the game loop.
	recvtic++;
	memmove (recvwindow, recvwindow + 1,
		 sizeof(recvslot_t) * (NET_BACKUPTICS - 1));
	memset (&recvwindow[NET_BACKUPTICS - 1], 0, sizeof(recvslot_t));
	recvwindow_start++;
    }
}

static void ParseResendRequest (net_packet_t* p)
{
    unsigned start, num;
    if (!NET_ReadInt32 (p, &start) || !NET_ReadInt8 (p, &num)) return;
    SendTics ((int)start, (int)(start + num - 1));
}

static void CheckResends (unsigned now)
{
    if (need_to_acknowledge && (int)(now - gamedata_recv_time) > 200)
    {
	net_packet_t* a = NET_NewPacket (8);
	NET_WriteInt16 (a, NET_PACKET_TYPE_GAMEDATA_ACK);
	NET_WriteInt8 (a, recvwindow_start & 0xff);
	SendRaw (a);
	NET_FreePacket (a);
	need_to_acknowledge = false;
    }
    // deadlock breaker: front tic still missing and the line has gone quiet
    if (!recvwindow[0].active && (int)(now - gamedata_recv_time) > 1000)
    {
	net_packet_t* r = NET_NewPacket (12);
	NET_WriteInt16 (r, NET_PACKET_TYPE_GAMEDATA_RESEND);
	NET_WriteInt32 (r, recvwindow_start);
	NET_WriteInt8 (r, 1);
	SendRaw (r);
	NET_FreePacket (r);
	gamedata_recv_time = now;			// throttle
    }
}

//
// ---- per-game-start reset ------------------------------------------------
//
static void EnterGame (void)
{
    memset (send_queue, 0, sizeof(send_queue));
    memset (recvwindow, 0, sizeof(recvwindow));
    memset (recv_base, 0, sizeof(recv_base));
    memset (merged_cmds, 0, sizeof(merged_cmds));
    memset (merged_ingame, 0, sizeof(merged_ingame));
    memset (&last_sent, 0, sizeof(last_sent));
    recvwindow_start = 0;
    maketic = 0;
    recvtic = 0;
    need_to_acknowledge = false;
    gamedata_recv_time = NET_GetTimeMS ();
    lowres_turn = settings.lowres_turn;
    is_drone = settings.consoleplayer < 0;
    clientstate = CL_IN_GAME;
}

//
// ---- inbound packet dispatch ---------------------------------------------
//
static void ParsePacket (net_packet_t* p)
{
    unsigned	type, realtype, np;
    boolean	reliable;

    if (!NET_ReadInt16 (p, &type)) return;
    last_recv_time = NET_GetTimeMS ();
    reliable = (type & NET_RELIABLE_FLAG) != 0;
    realtype = type & 0x7fff;
    if (reliable && !ReliableRecv (p))
	return;					// duplicate: already re-acked

    switch (realtype)
    {
      case NET_PACKET_TYPE_SYN:
	if (!connected) { connected = 1; clientstate = CL_WAITING_LAUNCH; }
	break;
      case NET_PACKET_TYPE_REJECTED:
	{ char* r = NET_ReadString (p);
	  printf ("  server rejected us: %s\n", r ? r : "(no reason)");
	  disconnected = 1; }
	break;
      case NET_PACKET_TYPE_KEEPALIVE:
	break;
      case NET_PACKET_TYPE_WAITING_DATA:
	{   // net_waitdata_t header: num_players, num_drones, ready_players,
	    // max_players, is_controller, ... (we only need the first + 5th)
	    unsigned num_players, num_drones, ready, maxp, is_ctrl;
	    if (NET_ReadInt8 (p, &num_players) && NET_ReadInt8 (p, &num_drones)
	     && NET_ReadInt8 (p, &ready) && NET_ReadInt8 (p, &maxp)
	     && NET_ReadInt8 (p, &is_ctrl))
	    {
		lobby_players = num_players;
		lobby_is_controller = is_ctrl;
	    }
	}
	break;
      case NET_PACKET_TYPE_LAUNCH:
	if (clientstate == CL_WAITING_LAUNCH)
	{ NET_ReadInt8 (p, &np); clientstate = CL_WAITING_START; }
	break;
      case NET_PACKET_TYPE_GAMESTART:
	if (clientstate != CL_IN_GAME && ReadSettings (p, &settings))
	    EnterGame ();
	break;
      case NET_PACKET_TYPE_GAMEDATA:
	if (clientstate == CL_IN_GAME) ParseGameData (p);
	break;
      case NET_PACKET_TYPE_GAMEDATA_RESEND:
	if (clientstate == CL_IN_GAME) ParseResendRequest (p);
	break;
      case NET_PACKET_TYPE_RELIABLE_ACK:
	ParseReliableACK (p);
	break;
      case NET_PACKET_TYPE_DISCONNECT:
	SendSimple (NET_PACKET_TYPE_DISCONNECT_ACK);
	disconnected = 1;
	break;
      default:
	break;
    }
}

//
// ---- public driver -------------------------------------------------------
//
void D_NetCl_Run (void)
{
    net_packet_t*	p;
    unsigned		now;

    if (disconnected) return;

    while ((p = UDP_Recv (0)) != NULL)
    {
	ParsePacket (p);
	NET_FreePacket (p);
	if (disconnected) return;
    }

    now = NET_GetTimeMS ();
    RunReliable (now);
    if ((int)(now - last_send_time) > 1000)		// keepalive
	SendSimple (NET_PACKET_TYPE_KEEPALIVE);
    if ((int)(now - last_recv_time) > 30000)		// timeout
    { disconnected = 1; return; }

    if (clientstate == CL_IN_GAME)
    { AdvanceWindow (); CheckResends (now); }
}

void D_NetCl_LaunchGame (void)
{
    NewReliable (NET_PACKET_TYPE_LAUNCH);		// no payload
}

void D_NetCl_StartGame (const netcl_settings_t* want)
{
    net_packet_t* p = NewReliable (NET_PACKET_TYPE_GAMESTART);
    WriteSettings (p, want);
}

boolean D_NetCl_Connected (void)	{ return connected && !disconnected; }
boolean D_NetCl_InGame (void)		{ return clientstate == CL_IN_GAME && !disconnected; }
boolean D_NetCl_LobbyLaunched (void)	{ return clientstate != CL_WAITING_LAUNCH; }
boolean D_NetCl_IsDisconnected (void)	{ return disconnected; }
const netcl_settings_t* D_NetCl_Settings (void) { return &settings; }
int D_NetCl_MakeTic (void)		{ return maketic; }
int D_NetCl_RecvTic (void)		{ return recvtic; }

// The game loop pulls completed tics from the merged ring.  TicReady tells it
// how far it can run; GetTic copies the `count` players' commands for tic `tic`.
boolean D_NetCl_TicReady (int tic)
{
    return tic >= 0 && tic < (int)recvtic && (int)recvtic - tic <= NET_BACKUPTICS;
}

void D_NetCl_GetTic (int tic, ticcmd_t* cmds, boolean* ingame, int count)
{
    int slot = ((tic % NET_BACKUPTICS) + NET_BACKUPTICS) % NET_BACKUPTICS;
    int i;
    for (i = 0 ; i < count && i < NET_PROTO_MAXPLAYERS ; i++)
    {
	cmds[i] = merged_cmds[i][slot];
	ingame[i] = (merged_ingame[slot] & (1 << i)) != 0;
    }
}

// Pump the network until `done` returns true, or until timeout.  Returns the
// final value of done().
static boolean PumpUntil (boolean (*done)(void), int timeout_ms)
{
    unsigned t0 = NET_GetTimeMS ();
    while (!done () && !disconnected
	   && (int)(NET_GetTimeMS () - t0) < timeout_ms)
    { D_NetCl_Run (); usleep (2000); }
    return done ();
}

// Full blocking join: connect -> lobby -> launch -> gamestart.  We act as the
// controller (send LAUNCH, then GAMESTART with `want`).  On success the
// server's authoritative settings are copied to *got and we are IN_GAME.
int  D_NetCl_LobbyPlayers (void)	{ return lobby_players; }
boolean D_NetCl_IsController (void)	{ return lobby_is_controller; }

boolean D_NetCl_JoinGame (const char* hostport, const char* version,
			  int gamemode, int gamemission, const char* playername,
			  int min_players, const netcl_settings_t* want,
			  netcl_settings_t* got)
{
    unsigned t0;

    if (!D_NetCl_Connect (hostport, version, gamemode, gamemission, playername))
	return false;

    // Wait in the lobby until enough players have joined (up to 60s).  The
    // controller then triggers the launch; non-controllers just wait for it.
    t0 = NET_GetTimeMS ();
    while (lobby_players < min_players && !disconnected
	   && (int)(NET_GetTimeMS () - t0) < 60000)
    { D_NetCl_Run (); usleep (5000); }

    if (lobby_is_controller || min_players <= 1)
	D_NetCl_LaunchGame ();

    if (!PumpUntil (D_NetCl_LobbyLaunched, 30000)) return false;
    D_NetCl_StartGame (want);
    if (!PumpUntil (D_NetCl_InGame, 30000)) return false;
    if (got) *got = settings;
    return true;
}

//
// Build the SYN connect packet (matches I_ConnectChocServer's format).
//
static net_packet_t* BuildSYN (void)
{
    net_packet_t*	syn = NET_NewPacket (96);
    int			i;

    NET_WriteInt16 (syn, NET_PACKET_TYPE_SYN);
    NET_WriteInt32 (syn, NET_MAGIC_NUMBER);
    NET_WriteString (syn, cl_version);
    NET_WriteInt8 (syn, 1);				// num protocols
    NET_WriteString (syn, "CHOCOLATE_DOOM_0");
    NET_WriteInt8 (syn, cl_gamemode);
    NET_WriteInt8 (syn, cl_gamemission);
    NET_WriteInt8 (syn, 0);				// lowres_turn
    NET_WriteInt8 (syn, 0);				// drone
    NET_WriteInt8 (syn, NET_PROTO_MAXPLAYERS);		// max_players
    NET_WriteInt8 (syn, 0);				// is_freedoom
    for (i = 0 ; i < 40 ; i++) NET_WriteInt8 (syn, 0);	// wad+deh sha1 (zeros)
    NET_WriteInt8 (syn, 0);				// player_class
    NET_WriteString (syn, cl_playername);
    return syn;
}

boolean D_NetCl_Connect (const char* hostport, const char* version,
			 int gamemode, int gamemission, const char* playername)
{
    net_packet_t*	syn;
    net_packet_t*	p;
    unsigned		start, lastsyn, now;

    strncpy (cl_version, version, sizeof(cl_version)-1);    cl_version[sizeof(cl_version)-1] = 0;
    strncpy (cl_playername, playername, sizeof(cl_playername)-1); cl_playername[sizeof(cl_playername)-1] = 0;
    cl_gamemode = gamemode;
    cl_gamemission = gamemission;

    connected = disconnected = 0;
    clientstate = CL_WAITING_LAUNCH;
    reliable_send_seq = reliable_recv_seq = 0;
    reliable_head = NULL;

    if (!UDP_OpenClient ()) { printf ("  socket error\n"); return false; }
    if (!UDP_Resolve (hostport, NET_DEFAULT_PORT))
    { printf ("  could not resolve %s\n", hostport); UDP_Close (); return false; }

    last_recv_time = last_send_time = NET_GetTimeMS ();
    syn = BuildSYN ();
    start = NET_GetTimeMS ();
    lastsyn = 0;

    while (!connected && !disconnected)
    {
	now = NET_GetTimeMS ();
	if (lastsyn == 0 || (int)(now - lastsyn) > 1000)
	{ SendRaw (syn); lastsyn = now; }
	while ((p = UDP_Recv (100)) != NULL)
	{
	    ParsePacket (p);
	    NET_FreePacket (p);
	    if (connected || disconnected) break;
	}
	if ((int)(NET_GetTimeMS () - start) > 8000) break;	// timeout
    }
    NET_FreePacket (syn);
    return connected && !disconnected;
}

void D_NetCl_Disconnect (void)
{
    int i;
    for (i = 0 ; i < 3 && !disconnected ; i++)
    {
	SendSimple (NET_PACKET_TYPE_DISCONNECT);
	usleep (50000);
	D_NetCl_Run ();
    }
    UDP_Close ();
    connected = 0;
}

//
// ---- Stage-3 headless interop self-test ----------------------------------
//
void I_NetClientTest (const char* hostport, const char* version,
		      int gamemode, int gamemission)
{
    netcl_settings_t	want;
    unsigned		gstart, lastreport;

    printf ("Net client self-test -> %s (as \"%s\", gamemode %d mission %d)\n",
	    hostport, version, gamemode, gamemission);

    memset (&want, 0, sizeof(want));
    want.ticdup = 1;  want.extratics = 1;  want.new_sync = 1;
    want.episode = 1; want.map = 1;  want.skill = 3;
    want.num_players = 1; want.consoleplayer = 0;

    if (!D_NetCl_JoinGame (hostport, version, gamemode, gamemission,
			   "sdldoom", 1, &want, NULL))
    {
	printf ("  join failed (no response / rejected / no game start).\n");
	D_NetCl_Disconnect ();
	return;
    }
    printf ("  [1-3/4] joined: IN GAME as player %d of %d (ticdup=%d extratics=%d new_sync=%d).\n",
	    settings.consoleplayer, settings.num_players,
	    settings.ticdup, settings.extratics, settings.new_sync);

    // Idle lockstep loop: feed zeroed ticcmds at 35Hz and drain server tics.
    printf ("  [4/4] running idle GAMEDATA exchange for 3s...\n");
    gstart = lastreport = NET_GetTimeMS ();
    while ((int)(NET_GetTimeMS () - gstart) < 3000 && !disconnected)
    {
	int target;
	D_NetCl_Run ();
	target = (int)((NET_GetTimeMS () - gstart) * 35 / 1000);
	while ((int)maketic < target && (int)(maketic - recvtic) < 8)
	{
	    ticcmd_t cmd;
	    memset (&cmd, 0, sizeof(cmd));
	    D_NetCl_SendTiccmd (&cmd, maketic);
	    maketic++;
	}
	usleep (1000);
	if ((int)(NET_GetTimeMS () - lastreport) >= 1000)
	{
	    printf ("      t=%us  sent=%u  received=%u\n",
		    (NET_GetTimeMS () - gstart) / 1000, maketic, recvtic);
	    lastreport = NET_GetTimeMS ();
	}
    }

    if (disconnected)
	printf ("  disconnected during play.\n");
    else if (maketic > 0 && recvtic > 0)
	printf ("  RESULT: GAMEDATA round-trip OK -- sent %u tics, received %u merged tics.\n",
		maketic, recvtic);
    else
	printf ("  RESULT: no tic round-trip (sent %u, received %u).\n", maketic, recvtic);

    D_NetCl_Disconnect ();
}
