// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Full agent / LLM control of the HUMAN player (-aiplayer).  AGENT_CONTROL.md
//	turned into code: a hook in G_BuildTiccmd hands the player's ticcmd to this
//	module instead of reading the keyboard.  An external brain (an LLM via the
//	run/llm_player.py client, or anything that speaks the line protocol) connects
//	over a TCP socket, asks for `observe` (one JSON line of game state) and issues
//	high-level INTENTS; a C reflex controller here turns the current intent into a
//	concrete ticcmd every tic (aim, step-toward, fire windows) -- the LLM plans, the
//	reflex executes, so the agent reacts at 35 Hz while the LLM thinks at its own pace.
//
//	`-aiplayer [port]`     LLM mode: listen on 127.0.0.1:<port> (default 31700).
//	`-aiplayer demo`       built-in scripted brain (no LLM) -- engages monsters and
//	                       wanders forward, to prove the hook drives the player.
//
//	Full as-built reference (protocol, reflex flow, demo brain, LLM client): AIPLAYER.md.
//	Design rationale: AGENT_CONTROL.md sec.1-11.
//
//	Protocol (one command per line, replies one line):
//	  map                     -> {"start":..,"exit":..,"doors":[..],"lines":[..]}  (static, once)
//	  observe                 -> {"tic":..,"player":{..},"exit":..,"buddy":..,"things":[..],"doors":[..]}
//	  goto <x> <y>            walk to a map point (BSP pathfinder routes there)
//	  face <x> <y>            turn to look at a point
//	  turn <degrees>          turn by a relative amount
//	  attack <0|1>            hold fire on/off (auto-aims the nearest visible monster)
//	  target <id>             attack a specific thing from the last observe
//	  weapon <1..8>           switch weapon
//	  use                     press USE once (doors/switches)
//	  stop                    clear movement/attack intent
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define close(fd)	closesocket(fd)
#define read(fd,b,n)	recv((fd),(char*)(b),(int)(n),0)
#define write(fd,b,n)	send((fd),(const char*)(b),(int)(n),0)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_ticcmd.h"
#include "m_argv.h"
#include "m_fixed.h"
#include "tables.h"
#include "p_mobj.h"
#include "r_defs.h"		// line_t / vertex_t (door + exit-switch detection)
#include "info.h"
#include "p_ai_coop.h"		// P_AICoop_NextWaypoint -- shared BSP pathfinder
#include "g_agent.h"
#include "sounds.h"

// Play-sim hooks declared by hand: p_local.h pulls p_spec.h, whose `open`/`close`
// enum values collide with the socket close()/read()/write() used here.
extern thinker_t	thinkercap;
extern void		P_MobjThinker (mobj_t*);
extern boolean		P_CheckSight (mobj_t* t1, mobj_t* t2);
extern fixed_t		P_AproxDistance (fixed_t dx, fixed_t dy);
extern angle_t		R_PointToAngle2 (fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2);
extern int		numlines;
extern line_t*		lines;
extern boolean		P_CheckPosition (mobj_t* thing, fixed_t x, fixed_t y);
extern fixed_t		tmceilingz;
extern fixed_t		tmfloorz;
extern fixed_t		tmdropoffz;

// ---- autonomous helpers: the level exit + doors/switches in the way ---------
static int	exit_level = -1;	// gameepisode*100+gamemap the exit was located for
static fixed_t	exit_x, exit_y;
static int	exit_set;

// ---- key fetch: when a locked door blocks us, detour to its key first -------
static int	key_seek;		// key colour we're fetching (0 = none), persists
static fixed_t	key_x, key_y;		// that key's location

// ---- door map: the level's doorways, collected once at start --------------
// The route to a key or the exit almost always runs through one or more doors, so the
// explorer steers toward the nearest not-yet-visited doorway -- directed exploration that
// threads the map, rather than a random walk that wanders off or an exit beeline that pins.
#define AGENT_MAXDOORS	96
static fixed_t	door_x[AGENT_MAXDOORS], door_y[AGENT_MAXDOORS];
static unsigned char door_seen[AGENT_MAXDOORS];
static int	door_n;
static int	door_wait_timer;
static int	waiting_at_door;

static boolean Agent_IsDoorSpecial (int sp)
{
    return sp==1||sp==31||sp==117||sp==118 ||		// plain DR/D1 doors
	   sp==26||sp==27||sp==28 || sp==32||sp==33||sp==34;	// locked doors
}

// Locate the level exit + collect the door linedefs, once per map.
static void Agent_FindExit (void)
{
    int i, lvl = gameepisode*100 + gamemap;
    if (exit_level == lvl) return;
    exit_level = lvl; exit_set = 0;
    key_seek = 0;					// drop any stale key detour from the old map
    door_n = 0;
    for (i = 0 ; i < numlines ; i++)
    {
	int sp = lines[i].special;
	fixed_t mx, my;
	if (sp==11 || sp==51 || sp==52 || sp==124)	// S1/W1 exit + secret-exit switches
	{
	    if (!exit_set) {
		fixed_t dx = lines[i].v2->x - lines[i].v1->x;
		fixed_t dy = lines[i].v2->y - lines[i].v1->y;
		fixed_t len = P_AproxDistance (dx, dy);
		if (len > 0) {
		    fixed_t nx = FixedDiv (dy, len) * 32;
		    fixed_t ny = FixedDiv (-dx, len) * 32;
		    exit_x = ((lines[i].v1->x + lines[i].v2->x) >> 1) + nx;
		    exit_y = ((lines[i].v1->y + lines[i].v2->y) >> 1) + ny;
		    exit_set = 1;
		}
	    }
	    continue;
	}
	if (Agent_IsDoorSpecial (sp) && door_n < AGENT_MAXDOORS)
	{
	    mx = (lines[i].v1->x + lines[i].v2->x) >> 1; my = (lines[i].v1->y + lines[i].v2->y) >> 1;
	    door_x[door_n] = mx; door_y[door_n] = my; door_seen[door_n] = 0; door_n++;
	}
    }
}

// Is a door (incl. locked) OR the level-exit switch within USE reach, ahead of us?
// So the marine opens its own way through and presses the exit switch -- the buddy does
// the same; without it the agent paths up to a shut door and never opens it.
static boolean Agent_UseAhead (mobj_t* mo)
{
    int		i, fa = mo->angle >> ANGLETOFINESHIFT;
    fixed_t	fwx = finecosine[fa], fwy = finesine[fa];
    for (i = 0 ; i < numlines ; i++)
    {
	line_t*	ld = &lines[i];
	int	sp = ld->special;
	fixed_t	mx, my, dx, dy;
	if (!(sp==1||sp==31||sp==117||sp==118 ||			// plain DR/D1 doors
	      sp==26||sp==27||sp==28 || sp==32||sp==33||sp==34 ||	// locked doors
	      sp==11||sp==51)) continue;				// exit switch

	// Skip doors that are already open/passable
	if (sp != 11 && sp != 51 && ld->frontsector && ld->backsector)
	{
	    sector_t* fs = ld->frontsector;
	    sector_t* bs = ld->backsector;
	    fixed_t opening = (fs->ceilingheight < bs->ceilingheight ? fs->ceilingheight : bs->ceilingheight)
		- (fs->floorheight   > bs->floorheight   ? fs->floorheight   : bs->floorheight);
	    if (opening >= 56*FRACUNIT) continue;
	}

	mx = (ld->v1->x + ld->v2->x) >> 1; my = (ld->v1->y + ld->v2->y) >> 1;
	dx = mx - mo->x; dy = my - mo->y;
	if (P_AproxDistance (dx, dy) > 80*FRACUNIT) continue;
	if (FixedMul (dx, fwx) + FixedMul (dy, fwy) > 0) return true;	// in the forward arc
    }
    return false;
}

#define AGENT_TURN	1400		// max angleturn/tic (~7.7 deg)
#define AGENT_FACING	900		// |turn remaining| under which we open fire (~5 deg,
					//  ~the auto-aim cone, so a shot actually connects)
#define AGENT_FORWARD	50		// run-speed forwardmove
#define AGENT_SIGHT	(2048*FRACUNIT)	// monster auto-aim acquisition range
#define AGENT_FIRE_RANGE (1024*FRACUNIT)// don't open fire past this -- close in first
#define AGENT_REACH	(64*FRACUNIT)	// goal considered reached within this
#define AGENT_MAXTHINGS	256		// observe + target registry size

int		agent_active;
static int	agent_port = 31700;
static int	agent_demo;		// built-in scripted brain (no client)

// ---- current intent (set by the brain/client, consumed by the reflex) ------
static int	in_have_goal;
static fixed_t	in_gx, in_gy;
static int	in_attack;		// hold fire
static int	in_use;			// one-shot USE
static int	in_weapon = -1;		// pending weapon 0..8, else -1
static int	in_have_face;
static angle_t	in_face;
static int	in_target = -1;		// registry id to attack, else -1

// ---- thing registry (rebuilt on each observe; target ids resolve through it) -
static mobj_t*	reg[AGENT_MAXTHINGS];
static int	reg_n;

static mobj_t*	Agent_Buddy (void);	// the friendly co-op buddy (defined below)

// ---------------------------------------------------------------------------
// Non-blocking TCP server (one client) -- same shape as p_ai_llm.c
// ---------------------------------------------------------------------------
static int	listen_fd = -1;
static int	client_fd = -1;
static char	linebuf[1024];
static int	linelen;

static void Agent_NonBlock (int fd)
{
#ifdef _WIN32
    unsigned long nb = 1; ioctlsocket (fd, FIONBIO, &nb);
#else
    int fl = fcntl (fd, F_GETFL, 0);
    if (fl >= 0) fcntl (fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static void Agent_OpenSocket (void)
{
    struct sockaddr_in addr;
    int yes = 1;
#ifdef _WIN32
    WSADATA wsadata; WSAStartup (MAKEWORD(2,2), &wsadata);
#endif
    listen_fd = socket (AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { printf ("agent: socket() failed\n"); return; }
    setsockopt (listen_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    memset (&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    addr.sin_port = htons (agent_port);
    if (bind (listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{ printf ("agent: bind(%d) failed\n", agent_port); close(listen_fd); listen_fd=-1; return; }
    if (listen (listen_fd, 1) < 0)
	{ printf ("agent: listen() failed\n"); close(listen_fd); listen_fd=-1; return; }
    Agent_NonBlock (listen_fd);
    printf ("agent: -aiplayer listening on 127.0.0.1:%d\n", agent_port);
}

static void Agent_Send (const char* s) { if (client_fd >= 0) write (client_fd, s, (int)strlen(s)); }

// ---------------------------------------------------------------------------
// Observation: one JSON line of player state + nearby things (with ids).
// ---------------------------------------------------------------------------
static const char* Agent_TypeName (mobjtype_t t)
{
    switch (t)
    {
      case MT_POSSESSED: return "zombieman";
      case MT_SHOTGUY:   return "shotgunner";
      case MT_TROOP:     return "imp";
      case MT_SERGEANT:  return "demon";
      case MT_SHADOWS:   return "spectre";
      case MT_HEAD:      return "cacodemon";
      case MT_BRUISER:   return "baron";
      case MT_KNIGHT:    return "hellknight";
      case MT_SKULL:     return "lostsoul";
      case MT_UNDEAD:    return "revenant";
      case MT_FATSO:     return "mancubus";
      case MT_BABY:      return "arachnotron";
      case MT_CHAINGUY:  return "chaingunner";
      case MT_PAIN:      return "painelemental";
      case MT_VILE:      return "archvile";
      case MT_SPIDER:    return "spidermastermind";
      case MT_CYBORG:    return "cyberdemon";
      case MT_BARREL:    return "barrel";
      default:           return "thing";
    }
}

static void Agent_SendMap (void)
{
    char* buf = malloc (65536);
    int off = 0, i;

    Agent_FindExit ();
    off += snprintf (buf+off, 65536-off, "{\"start\":[%d,%d],\"exit\":",
	playerstarts[consoleplayer].x, playerstarts[consoleplayer].y);
    if (exit_set) off += snprintf (buf+off, 65536-off, "[%d,%d]", exit_x>>FRACBITS, exit_y>>FRACBITS);
    else          off += snprintf (buf+off, 65536-off, "null");

    off += snprintf (buf+off, 65536-off, ",\"doors\":[");
    for (i = 0; i < door_n; i++)
    {
	off += snprintf (buf+off, 65536-off, "%s[%d,%d]",
	    i == 0 ? "" : ",", door_x[i]>>FRACBITS, door_y[i]>>FRACBITS);
    }
    off += snprintf (buf+off, 65536-off, "],\"lines\":[");

    int max_lines = numlines < 1200 ? numlines : 1200;
    for (i = 0; i < max_lines; i++)
    {
	off += snprintf (buf+off, 65536-off, "%s[%d,%d,%d,%d,%d]",
	    i == 0 ? "" : ",",
	    lines[i].v1->x>>FRACBITS, lines[i].v1->y>>FRACBITS,
	    lines[i].v2->x>>FRACBITS, lines[i].v2->y>>FRACBITS,
	    lines[i].special);
    }
    off += snprintf (buf+off, 65536-off, "]}\n");
    Agent_Send (buf);
    free (buf);
}

static int Agent_TraceDistance (mobj_t* mo, angle_t angle)
{
    int i;
    fixed_t fz = mo->z;
    int fa = angle >> ANGLETOFINESHIFT;
    fixed_t cos_v = finecosine[fa];
    fixed_t sin_v = finesine[fa];

    for (i = 1; i <= 64; i++)
    {
	fixed_t dist = i * 16 * FRACUNIT;
	fixed_t px = mo->x + FixedMul (cos_v, dist);
	fixed_t py = mo->y + FixedMul (sin_v, dist);

	if (!P_CheckPosition (mo, px, py))		break;
	if (tmceilingz - tmfloorz < mo->height)	break;
	if (tmceilingz - fz < mo->height)		break;
	if (tmfloorz - fz > 24*FRACUNIT)		break;
	if (tmfloorz - tmdropoffz > 24*FRACUNIT)	break;

	fz = tmfloorz;
    }
    return (i - 1) * 16;
}

#define MAX_LOGGED_SOUNDS 8
typedef struct {
    char name[16];
    int dist;
    int dir;
} agent_sound_t;

static agent_sound_t logged_sounds[MAX_LOGGED_SOUNDS];
static int logged_sounds_n = 0;

void G_Agent_LogSound (void* origin, int sfx_id)
{
    mobj_t* mo = (mobj_t*)origin;
    mobj_t* player_mo = players[consoleplayer].mo;
    if (!player_mo || !mo || sfx_id <= 0) return;

    fixed_t d = P_AproxDistance (mo->x - player_mo->x, mo->y - player_mo->y);
    if (d > 1600*FRACUNIT) return; // limit hearing distance

    int relangle = (int)(((R_PointToAngle2 (player_mo->x, player_mo->y, mo->x, mo->y) - player_mo->angle)) / (ANG45/45));
    if (relangle > 180) relangle -= 360;

    if (logged_sounds_n < MAX_LOGGED_SOUNDS)
    {
	agent_sound_t* s = &logged_sounds[logged_sounds_n];
	strncpy (s->name, S_sfx[sfx_id].name, sizeof(s->name)-1);
	s->name[sizeof(s->name)-1] = 0;
	s->dist = (int)(d >> FRACBITS);
	s->dir = relangle;
	logged_sounds_n++;
    }
}

static void Agent_Observe (void)
{
    player_t*	p = &players[consoleplayer];
    mobj_t*	mo = p->mo;
    thinker_t*	th;
    char	buf[32768];
    int		off, first = 1, i;

    reg_n = 0;
    Agent_FindExit ();
    if (!mo) { Agent_Send ("{\"player\":null}\n"); return; }

    off = snprintf (buf, sizeof buf,
	"{\"tic\":%d,\"player\":{\"x\":%d,\"y\":%d,\"z\":%d,\"angle\":%u,"
	"\"health\":%d,\"armor\":%d,\"weapon\":%d,\"ammo\":[%d,%d,%d,%d],"
	"\"kills\":%d,\"items\":%d}",
	leveltime, mo->x>>FRACBITS, mo->y>>FRACBITS, mo->z>>FRACBITS,
	(unsigned)(mo->angle / (ANG45/45)), p->health, p->armorpoints, p->readyweapon,
	p->ammo[0], p->ammo[1], p->ammo[2], p->ammo[3], p->killcount, p->itemcount);
    if (exit_set) off += snprintf (buf+off, sizeof buf-off, ",\"exit\":[%d,%d]", exit_x>>FRACBITS, exit_y>>FRACBITS);
    else          off += snprintf (buf+off, sizeof buf-off, ",\"exit\":null");
    {
	// The AI co-op buddy is a FRIEND (a fellow marine), NOT in `things` and never a
	// target -- reported separately so the brain knows it's there and can coordinate.
	mobj_t* b = Agent_Buddy ();
	if (b) off += snprintf (buf+off, sizeof buf-off,
		",\"buddy\":{\"friend\":true,\"x\":%d,\"y\":%d,\"health\":%d}",
		b->x>>FRACBITS, b->y>>FRACBITS, b->health);
	else   off += snprintf (buf+off, sizeof buf-off, ",\"buddy\":null");
    }
    off += snprintf (buf+off, sizeof buf-off, ",\"things\":[");

    for (th = thinkercap.next ; th != &thinkercap && reg_n < AGENT_MAXTHINGS ; th = th->next)
    {
	mobj_t*	m; fixed_t d; int relangle, vis;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (m == mo) continue;
	if (!(m->flags & (MF_COUNTKILL|MF_SPECIAL))) continue;	// monsters + pickups only (no barrels)
	if (m->health <= 0) continue;				// skip corpses
	d = P_AproxDistance (m->x - mo->x, m->y - mo->y);
	relangle = (int)(((R_PointToAngle2 (mo->x, mo->y, m->x, m->y) - mo->angle)) / (ANG45/45));
	if (relangle > 180) relangle -= 360;
	vis = P_CheckSight (mo, m) ? 1 : 0;
	reg[reg_n] = m;
	off += snprintf (buf+off, sizeof buf-off,
	    "%s{\"id\":%d,\"type\":\"%s\",\"dist\":%d,\"rel\":%d,\"hp\":%d,\"vis\":%d,\"x\":%d,\"y\":%d,\"z\":%d}",
	    first?"":",", reg_n, Agent_TypeName (m->type), d>>FRACBITS, relangle, m->health, vis,
	    m->x>>FRACBITS, m->y>>FRACBITS, m->z>>FRACBITS);
	first = 0; reg_n++;
    }
    off += snprintf (buf+off, sizeof buf-off, "]");

    // Expose topological doors
    off += snprintf (buf+off, sizeof buf-off, ",\"doors\":[");
    for (i = 0; i < door_n; i++)
    {
	off += snprintf (buf+off, sizeof buf-off, "%s{\"pos\":[%d,%d],\"seen\":%d}",
	    i == 0 ? "" : ",", door_x[i]>>FRACBITS, door_y[i]>>FRACBITS, door_seen[i]);
    }
    off += snprintf (buf+off, sizeof buf-off, "]");

    // Expose lidar data
    {
	int lidar[8];
	for (i = 0; i < 8; i++)
	{
	    lidar[i] = Agent_TraceDistance (mo, mo->angle + (angle_t)i * ANG45);
	}
	off += snprintf (buf+off, sizeof buf-off, ",\"lidar\":[%d,%d,%d,%d,%d,%d,%d,%d]",
	    lidar[0], lidar[1], lidar[2], lidar[3], lidar[4], lidar[5], lidar[6], lidar[7]);
    }

    // Expose sound events
    {
	off += snprintf (buf+off, sizeof buf-off, ",\"sounds\":[");
	for (i = 0; i < logged_sounds_n; i++)
	{
	    off += snprintf (buf+off, sizeof buf-off, "%s{\"name\":\"%s\",\"dist\":%d,\"rel\":%d}",
		i == 0 ? "" : ",", logged_sounds[i].name, logged_sounds[i].dist, logged_sounds[i].dir);
	}
	off += snprintf (buf+off, sizeof buf-off, "]");
	logged_sounds_n = 0;
    }

    off += snprintf (buf+off, sizeof buf-off, ",\"waiting_at_door\":%s", waiting_at_door ? "true" : "false");
    off += snprintf (buf+off, sizeof buf-off, "}\n");
    Agent_Send (buf);
}

// ---------------------------------------------------------------------------
// Intent commands (one per line)
// ---------------------------------------------------------------------------
static void Agent_HandleLine (char* line)
{
    int a, b;
    while (*line == ' ') line++;
    if      (!strncmp (line, "observe", 7))      Agent_Observe ();
    else if (!strncmp (line, "map", 3))          Agent_SendMap ();
    else if (sscanf (line, "goto %d %d", &a, &b) == 2)
	{ in_gx = a*FRACUNIT; in_gy = b*FRACUNIT; in_have_goal = 1; in_have_face = 0; }
    else if (sscanf (line, "face %d %d", &a, &b) == 2)
	{ in_face = R_PointToAngle2 (players[consoleplayer].mo ? players[consoleplayer].mo->x:0,
				     players[consoleplayer].mo ? players[consoleplayer].mo->y:0,
				     a*FRACUNIT, b*FRACUNIT); in_have_face = 1; }
    else if (sscanf (line, "turn %d", &a) == 1)
	{ in_face = (players[consoleplayer].mo ? players[consoleplayer].mo->angle:0) + (angle_t)(a*(ANG45/45));
	  in_have_face = 1; }
    else if (sscanf (line, "attack %d", &a) == 1) in_attack = a ? 1 : 0;
    else if (sscanf (line, "target %d", &a) == 1) { in_target = a; in_attack = 1; }
    else if (sscanf (line, "weapon %d", &a) == 1) in_weapon = (a-1) & 7;	// 1-based -> wp_*
    else if (!strncmp (line, "use", 3))   in_use = 1;
    else if (!strncmp (line, "stop", 4))  { in_have_goal = in_attack = in_have_face = 0; in_target = -1; }
}

static void Agent_Poll (void)
{
    if (listen_fd < 0) return;
    if (client_fd < 0)
    {
	int c = accept (listen_fd, NULL, NULL);
	if (c >= 0) { client_fd = c; Agent_NonBlock (c); linelen = 0; }
    }
    if (client_fd < 0) return;
    for (;;)
    {
	char ch; int r = read (client_fd, &ch, 1);
	if (r == 0) { close(client_fd); client_fd = -1; return; }
	if (r < 0)  return;					// nothing more this tic
	if (ch == '\n') { linebuf[linelen] = 0; Agent_HandleLine (linebuf); linelen = 0; }
	else if (linelen < (int)sizeof(linebuf)-1) linebuf[linelen++] = ch;
    }
}

// Resolve a target id from the last observe to a still-live monster.  Scans the
// thinker list to confirm the stored pointer is STILL a live mobj before touching it
// (it may have died + been freed between the observe and the `target` -- no UAF).
static mobj_t* Agent_Target (void)
{
    thinker_t*	th;
    mobj_t*	want;
    if (in_target < 0 || in_target >= reg_n) return NULL;
    want = reg[in_target];
    for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
	if (th->function.acp1 == (actionf_p1)P_MobjThinker && (mobj_t*)th == want)
	    return (want->health > 0 && (want->flags & MF_SHOOTABLE)) ? want : NULL;
    return NULL;					// gone
}

// Nearest visible shootable monster -- the auto-aim target when `attack 1`.
static mobj_t* Agent_NearestMonster (mobj_t* mo)
{
    thinker_t* th; mobj_t* best = NULL; fixed_t bestd = AGENT_SIGHT;
    for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
    {
	mobj_t* m; fixed_t d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (m == mo || m->health <= 0) continue;
	if (!(m->flags & MF_COUNTKILL) || !(m->flags & MF_SHOOTABLE)) continue;
	d = P_AproxDistance (m->x - mo->x, m->y - mo->y);
	if (d < bestd && P_CheckSight (mo, m)) { bestd = d; best = m; }
    }
    return best;
}

// The AI co-op buddy's mobj (player 2), or NULL if no buddy is in the game.  The buddy
// is a FRIEND -- never a target -- and the marine must not shoot through it.
static mobj_t* Agent_Buddy (void)
{
    int s = P_AICoop_Slot ();
    if (s < 0 || !playeringame[s]) return NULL;
    return players[s].mo;
}

// Would a shot at `tgt` pass through the buddy?  True if the buddy is roughly on the
// line of fire AND closer than the target (so it'd take the hit) -- then hold fire.
static boolean Agent_BuddyInLine (mobj_t* mo, mobj_t* tgt)
{
    mobj_t*	b = Agent_Buddy ();
    int		txi, tyi, bxi, byi, dti, perp;
    if (!b || b->health <= 0) return false;
    txi = (tgt->x - mo->x) >> FRACBITS; tyi = (tgt->y - mo->y) >> FRACBITS;
    bxi = (b->x   - mo->x) >> FRACBITS; byi = (b->y   - mo->y) >> FRACBITS;
    dti = P_AproxDistance (tgt->x - mo->x, tgt->y - mo->y) >> FRACBITS;
    if (dti < 1)                         return false;
    if (bxi*txi + byi*tyi <= 0)          return false;	// buddy is behind/beside us
    if (bxi*bxi + byi*byi >= dti*dti)    return false;	// buddy is farther than the target
    perp = abs (bxi*tyi - byi*txi) / dti;		// buddy's distance from the shot line
    return perp < 40;					// within ~a body width -> would be hit
}

// An explosive barrel sitting in our line of fire?  Don't shoot it -- the blast is as
// likely to hurt us as the target.  Same in-line test as the buddy guard, over MT_BARREL.
static boolean Agent_BarrelInLine (mobj_t* mo, mobj_t* tgt)
{
    thinker_t*	th;
    int		txi = (tgt->x - mo->x) >> FRACBITS, tyi = (tgt->y - mo->y) >> FRACBITS;
    int		dti = P_AproxDistance (tgt->x - mo->x, tgt->y - mo->y) >> FRACBITS;
    if (dti < 1) return false;
    for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
    {
	mobj_t*	m; int bxi, byi;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (m->type != MT_BARREL || m->health <= 0) continue;
	bxi = (m->x - mo->x) >> FRACBITS; byi = (m->y - mo->y) >> FRACBITS;
	if (bxi*txi + byi*tyi <= 0)       continue;		// behind/beside us
	if (bxi*bxi + byi*byi >= dti*dti) continue;		// past the target
	if (abs (bxi*tyi - byi*txi) / dti < 36) return true;	// roughly on the shot line
    }
    return false;
}

// ---- locked doors: fetch the right key before trying to open them -----------
// A locked-door linedef special encodes the key colour it needs (DOOM specials 26/32 =
// blue, 27/34 = yellow, 28/33 = red).  We can't open it without the matching card/skull.
static boolean Agent_HasKey (int color)
{
    player_t* p = &players[consoleplayer];
    switch (color)
    {
      case 1: return p->cards[it_bluecard]   || p->cards[it_blueskull];
      case 2: return p->cards[it_yellowcard] || p->cards[it_yellowskull];
      case 3: return p->cards[it_redcard]    || p->cards[it_redskull];
    }
    return true;
}

// A locked door right ahead that we CAN'T open yet -> its key colour (1/2/3), else 0.
static int Agent_LockedNeedAhead (mobj_t* mo)
{
    int		i, fa = mo->angle >> ANGLETOFINESHIFT;
    fixed_t	fwx = finecosine[fa], fwy = finesine[fa];
    for (i = 0 ; i < numlines ; i++)
    {
	int	sp = lines[i].special, color;
	fixed_t	mx, my, dx, dy;
	if      (sp==26 || sp==32) color = 1;		// blue
	else if (sp==27 || sp==34) color = 2;		// yellow
	else if (sp==28 || sp==33) color = 3;		// red
	else continue;
	if (Agent_HasKey (color)) continue;		// already openable
	mx = (lines[i].v1->x + lines[i].v2->x) >> 1; my = (lines[i].v1->y + lines[i].v2->y) >> 1;
	dx = mx - mo->x; dy = my - mo->y;
	if (P_AproxDistance (dx, dy) > 256*FRACUNIT) continue;	// detect it as we approach
	if (FixedMul (dx, fwx) + FixedMul (dy, fwy) > 0) return color;	// in front of us
    }
    return 0;
}

// Locate a key item of the given colour anywhere in the map -> its position.
static boolean Agent_FindKey (int color, fixed_t* kx, fixed_t* ky)
{
    thinker_t* th;
    for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
    {
	mobj_t*	m; int hit = 0;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (color==1) hit = (m->sprite==SPR_BKEY || m->sprite==SPR_BSKU);
	if (color==2) hit = (m->sprite==SPR_YKEY || m->sprite==SPR_YSKU);
	if (color==3) hit = (m->sprite==SPR_RKEY || m->sprite==SPR_RSKU);
	if (hit) { *kx = m->x; *ky = m->y; return true; }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Built-in scripted brain (-aiplayer demo): engage monsters, else wander.
// ---------------------------------------------------------------------------
static void Agent_Brain (void)
{
    static unsigned seed = 1;			// own RNG -- never touch the playsim's
    static int goaltic, cur_door = -1;
    mobj_t* mo = players[consoleplayer].mo;
    in_attack = 1;
    if (!mo) return;

    // PRIMARY DIRECTIVE: kill monsters.  A foe in view -> drop the wander goal and let the
    // reflex lock onto it (face + fire + kite) instead of strolling past it.
    if (Agent_NearestMonster (mo)) { in_have_goal = 0; return; }

    // Abandon a goal chased >3 s without arriving; if it was a door, mark it done so we
    // stop retrying one we can't reach (e.g. still locked).
    if (in_have_goal && leveltime - goaltic > 105)
    {
	in_have_goal = 0;
	if (cur_door >= 0) door_seen[cur_door] = 1;
	cur_door = -1;
    }

    if (!in_have_goal && (leveltime & 31) == 0)
    {
	// Near the exit -> go finish it (Agent_UseAhead then presses the exit switch).
	if (exit_set && P_AproxDistance (mo->x - exit_x, mo->y - exit_y) < 512*FRACUNIT)
	    { in_gx = exit_x; in_gy = exit_y; cur_door = -1; }
	else
	{
	    // SECONDARY: thread the map through its doors -- steer to the nearest doorway we
	    // haven't been to yet (marking ones we pass as done).  The key/exit route runs
	    // through doors, so this explores far more purposefully than a random walk.
	    int i, best = -1; fixed_t bd = (fixed_t)1<<30;
	    for (i = 0 ; i < door_n ; i++)
	    {
		fixed_t d = P_AproxDistance (mo->x - door_x[i], mo->y - door_y[i]);
		if (d < 160*FRACUNIT) door_seen[i] = 1;
		if (!door_seen[i] && d < bd) { bd = d; best = i; }
	    }
	    if (best >= 0) { in_gx = door_x[best]; in_gy = door_y[best]; cur_door = best; }
	    else
	    {
		// All doors done (or none) -> random walk to cover open areas / reach the exit.
		int turn, fa;
		seed = seed*1103515245u + 12345u;
		turn = (int)((seed >> 16) & 127) - 64;
		fa = (mo->angle + (angle_t)(turn * (ANG45/45))) >> ANGLETOFINESHIFT;
		in_gx = mo->x + FixedMul (512*FRACUNIT, finecosine[fa]);
		in_gy = mo->y + FixedMul (512*FRACUNIT, finesine[fa]);
		cur_door = -1;
	    }
	}
	in_have_goal = 1; goaltic = leveltime;
    }
}

// ---------------------------------------------------------------------------
// The reflex controller: current intent -> one ticcmd.  Called from G_BuildTiccmd.
// ---------------------------------------------------------------------------
void G_AgentBuildTiccmd (ticcmd_t* cmd)
{
    player_t*	p = &players[consoleplayer];
    mobj_t*	mo = p->mo;
    mobj_t*	tgt;
    angle_t	want;
    int		rem, turn, chase = 0, los = 0;
    fixed_t	td = 0;			// distance to the current target
    static int	stuck;

    waiting_at_door = 0;
    memset (cmd, 0, sizeof(*cmd));
    Agent_Poll ();
    if (door_wait_timer > 0) door_wait_timer--;
    // The level-end intermission (and finale) screens wait for a key -- pulse fire to
    // advance them, else the LLM marine stalls on the tally screen forever.
    if (gamestate != GS_LEVEL)
	{ if ((gametic & 15) == 0) cmd->buttons = BT_ATTACK; return; }
    Agent_FindExit ();
    if (agent_demo) Agent_Brain ();
    if (!mo || p->playerstate != PST_LIVE) return;

    // Update seen status of doors the player passes near
    {
	int i;
	for (i = 0 ; i < door_n ; i++)
	{
	    if (P_AproxDistance (mo->x - door_x[i], mo->y - door_y[i]) < 160*FRACUNIT)
		door_seen[i] = 1;
	}
    }

    // Target priority: an explicit `target`, else the nearest visible monster while
    // `attack` is held.  Aim there; otherwise honour an explicit face, then the goal.
    tgt = Agent_Target ();
    if (!tgt && in_attack) tgt = Agent_NearestMonster (mo);

    // Key fetch: drop the detour once we have the key; otherwise, if a locked door we
    // can't open is right ahead and its key exists in the map, start fetching that key.
    if (key_seek && Agent_HasKey (key_seek)) key_seek = 0;
    if (!key_seek)
    {
	int c = Agent_LockedNeedAhead (mo);
	if (c && Agent_FindKey (c, &key_x, &key_y)) key_seek = c;
    }

    want = mo->angle;
    if (tgt)
    {
	los = P_CheckSight (mo, tgt);
	td  = P_AproxDistance (tgt->x - mo->x, tgt->y - mo->y);
    }

    // 1. Determine AIM Target (want)
    if (tgt && los && td < AGENT_FIRE_RANGE)
    {
	want = R_PointToAngle2 (mo->x, mo->y, tgt->x, tgt->y);
    }
    else if (in_have_face)
    {
	want = in_face;
    }

    // 2. Determine MOVEMENT Target (gx, gy, havegoal)
    fixed_t gx = 0, gy = 0;
    int havegoal = 0;
    int force_straight = (leveltime < 105 && !in_have_goal && !key_seek && !tgt);

    if (in_have_goal)
    {
	gx = in_gx; gy = in_gy; havegoal = 1;
	if (P_AproxDistance (mo->x - in_gx, mo->y - in_gy) < AGENT_REACH)
	{
	    in_have_goal = 0;
	    havegoal = 0;
	}
    }
    else if (key_seek)
    {
	gx = key_x; gy = key_y; havegoal = 1;
    }
    else if (tgt)
    {
	// Close in on target if we don't have a specific path goal
	gx = tgt->x; gy = tgt->y; havegoal = 1;
    }
    else if (exit_set && !force_straight)
    {
	gx = exit_x; gy = exit_y; havegoal = 1;
    }

    // 3. Process pathfinding and movement heading
    angle_t move_angle = mo->angle;
    if (havegoal)
    {
	static chasedir_t agent_chase = { -1, 0, 0 };
	fixed_t wx, wy, nx, ny;
	fixed_t ddx, ddy, stx, sty;

	if (P_AICoop_NextWaypoint (mo, gx, gy, &wx, &wy)) { nx = wx; ny = wy; }
	else                                              { nx = gx; ny = gy; }

	// Determine stx / sty (steer target point) using buddy navigation logic:
	if (AICoop_FindDoorAhead (mo, gx, gy, &ddx, &ddy))
	{
	    stx = ddx; sty = ddy; // Doorway in reach -> head straight to it
	    if (P_AproxDistance (mo->x - ddx, mo->y - ddy) < 80*FRACUNIT)
	    {
		waiting_at_door = 1;
	    }
	}
	else if (AICoop_CanReach (mo, gx, gy, false))
	{
	    stx = gx; sty = gy; // Final target reachable -> head straight to it
	}
	else if (AICoop_CanReach (mo, nx, ny, true))
	{
	    stx = nx; sty = ny; // Waypoint in reach -> head straight to it
	}
	else
	{
	    // Waypoint or doorway is behind a wall/corner -> use corner-rounding ChaseDir
	    angle_t cd = AICoop_ChaseDir (mo, nx, ny, &agent_chase);
	    angle_t a  = cd >> ANGLETOFINESHIFT;
	    stx = mo->x + FixedMul (96*FRACUNIT, finecosine[a]);
	    sty = mo->y + FixedMul (96*FRACUNIT, finesine[a]);
	}

	move_angle = R_PointToAngle2 (mo->x, mo->y, stx, sty);
	chase = 1;

	// If we are waiting for a door to open, face it and prevent stuck wiggling
	if (waiting_at_door)
	{
	    stuck = 0;
	    want = move_angle;
	}
	// If we are NOT aiming at a monster or face target, face where we walk
	else if (!(tgt && los && td < AGENT_FIRE_RANGE) && !in_have_face)
	    want = move_angle;
    }

    if (force_straight)
    {
	move_angle = mo->angle;
	chase = 0;
	want = mo->angle;
	cmd->forwardmove = AGENT_FORWARD / 2;
	cmd->sidemove = 0;
    }

    if (exit_set)
    {
	int i;
	fixed_t bestd = 512*FRACUNIT;
	fixed_t mx = exit_x, my = exit_y;
	for (i = 0; i < numlines; i++)
	{
	    int sp = lines[i].special;
	    if (sp==11 || sp==51 || sp==52 || sp==124)
	    {
		fixed_t lx = (lines[i].v1->x + lines[i].v2->x) >> 1;
		fixed_t ly = (lines[i].v1->y + lines[i].v2->y) >> 1;
		fixed_t d = P_AproxDistance (mo->x - lx, mo->y - ly);
		if (d < bestd)
		{
		    bestd = d;
		    mx = lx;
		    my = ly;
		}
	    }
	}

	if (bestd < 56*FRACUNIT)
	{
	    want = R_PointToAngle2 (mo->x, mo->y, mx, my);
	    cmd->buttons |= BT_USE;
	    chase = 0;
	}
    }

    rem  = (short)((want - mo->angle) >> 16);
    turn = rem;
    if (turn >  AGENT_TURN) turn =  AGENT_TURN;
    if (turn < -AGENT_TURN) turn = -AGENT_TURN;
    cmd->angleturn = (short)turn;

    if (in_have_face && abs(rem) < 600) in_have_face = 0;	// face reached

    // Fire ONLY at a target we can SEE, in range, and tightly lined up on (auto-aim does
    // the vertical), and NEVER with the AI buddy in the line of fire (hold + reposition).
    if (tgt && los && td < AGENT_FIRE_RANGE && abs(rem) < AGENT_FACING
	&& !Agent_BuddyInLine (mo, tgt) && !Agent_BarrelInLine (mo, tgt))
	cmd->buttons |= BT_ATTACK;

    // Move relative to current view angle (strafe + forward/back)
    if (chase)
    {
	angle_t diff = move_angle - mo->angle;
	int fa = diff >> ANGLETOFINESHIFT;
	cmd->forwardmove = (char)((FixedMul (finecosine[fa], AGENT_FORWARD * FRACUNIT)) >> FRACBITS);
	cmd->sidemove    = (char)(-((FixedMul (finesine[fa], AGENT_FORWARD * FRACUNIT)) >> FRACBITS));
    }

    // Survival (primary directive: kill monsters, STAY ALIVE).  Keep a melee threat -- a
    // charging pinky -- at arm's length: once a visible target gets close, BACK AWAY while
    // we keep firing (kite), and from further out when we're hurt.
    if (tgt && los)
    {
	fixed_t kite = (p->health < 40) ? 384*FRACUNIT : 128*FRACUNIT;
	if (td < kite)
	{
	    cmd->forwardmove = -AGENT_FORWARD;
	    cmd->sidemove    = 0;
	}
    }

    if (in_use)         { cmd->buttons |= BT_USE; in_use = 0; }
    if (in_weapon >= 0) { cmd->buttons |= BT_CHANGE | ((in_weapon << BT_WEAPONSHIFT) & BT_WEAPONMASK);
			  in_weapon = -1; }

    // Bring the best owned weapon to bear instead of plinking with the fist/pistol, so a
    // pinky dies before it reaches melee.  One-shot (only when not already switching).
    if (tgt && !(cmd->buttons & BT_CHANGE) && p->pendingweapon == wp_nochange
	&& (p->readyweapon == wp_fist || p->readyweapon == wp_pistol))
    {
	int best = -1;
	if      (p->weaponowned[wp_chaingun]) best = wp_chaingun;
	else if (p->weaponowned[wp_shotgun])  best = wp_shotgun;
	if (best >= 0)
	    cmd->buttons |= BT_CHANGE | ((best << BT_WEAPONSHIFT) & BT_WEAPONMASK);
    }

    // Autonomy: open a door / hit the exit switch in our way, and un-stick if blocked --
    // without these the marine paths up to a shut door and never gets through.
    {
	static int	usewait;
	static fixed_t	lastx, lasty;
	if (usewait > 0) usewait--;
	if ((chase || waiting_at_door) && usewait == 0 && door_wait_timer == 0 && Agent_UseAhead (mo)) { cmd->buttons |= BT_USE; usewait = 16; }

	if (chase)
	{
	    if (P_AproxDistance (mo->x - lastx, mo->y - lasty) < 3*FRACUNIT) stuck++;
	    else                                                            stuck = 0;
	    lastx = mo->x; lasty = mo->y;
	    if (stuck > 30)
	    {
		// Pinned (the reflex heads straight at the waypoint, no corner-rounding):
		// SWEEP the view one way while pushing forward + strafing so the marine
		// rotates until the gap opens; flip the sweep every ~32 tics.
		cmd->angleturn   = (stuck & 32) ?  AGENT_TURN : -AGENT_TURN;
		cmd->forwardmove = AGENT_FORWARD;
		cmd->sidemove    = (stuck & 32) ?  30 : -30;
		if (door_wait_timer == 0) cmd->buttons |= BT_USE;	// in case a shut door is pinning us
	    }
	}
	else stuck = 0;
    }

    if ((cmd->buttons & BT_USE) && Agent_UseAhead (mo))
    {
	door_wait_timer = 80;
    }
}

// ---------------------------------------------------------------------------
// Startup: parse -aiplayer [port|demo] and open the socket (unless demo).
// ---------------------------------------------------------------------------
void G_AgentInit (void)
{
    int pp = M_CheckParm ("-aiplayer");
    if (!pp) return;
    agent_active = 1;
    if (pp+1 < myargc && myargv[pp+1][0] != '-')
    {
	if (!strcmp (myargv[pp+1], "demo")) agent_demo = 1;
	else                                agent_port = atoi (myargv[pp+1]);
    }
    if (agent_demo) printf ("agent: -aiplayer demo (built-in scripted brain)\n");
    else            Agent_OpenSocket ();
}

int G_AgentActive (void) { return agent_active; }

extern byte* save_p;

void G_Agent_Archive (void)
{
    int i;
    // Archive intents
    memcpy (save_p, &in_have_goal, sizeof(int)); save_p += sizeof(int);
    memcpy (save_p, &in_gx, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    memcpy (save_p, &in_gy, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    memcpy (save_p, &in_attack, sizeof(int)); save_p += sizeof(int);
    memcpy (save_p, &in_use, sizeof(int)); save_p += sizeof(int);
    memcpy (save_p, &in_weapon, sizeof(int)); save_p += sizeof(int);
    memcpy (save_p, &in_have_face, sizeof(int)); save_p += sizeof(int);
    memcpy (save_p, &in_face, sizeof(angle_t)); save_p += sizeof(angle_t);
    memcpy (save_p, &in_target, sizeof(int)); save_p += sizeof(int);

    // Archive level exit & key seek info
    memcpy (save_p, &exit_level, sizeof(int)); save_p += sizeof(int);
    memcpy (save_p, &exit_x, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    memcpy (save_p, &exit_y, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    memcpy (save_p, &exit_set, sizeof(int)); save_p += sizeof(int);
    memcpy (save_p, &key_seek, sizeof(int)); save_p += sizeof(int);
    memcpy (save_p, &key_x, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    memcpy (save_p, &key_y, sizeof(fixed_t)); save_p += sizeof(fixed_t);

    // Archive door seen statuses & waiting timer
    memcpy (save_p, &door_wait_timer, sizeof(int)); save_p += sizeof(int);
    memcpy (save_p, &door_n, sizeof(int)); save_p += sizeof(int);
    for (i = 0; i < door_n; i++)
    {
	memcpy (save_p, &door_seen[i], sizeof(unsigned char)); save_p += sizeof(unsigned char);
    }
}

void G_Agent_UnArchive (void)
{
    int i, saved_door_n = 0;

    // Run FindExit to ensure static geometry (door_x/y, exit_x/y) is loaded first
    exit_level = -1; // Force FindExit to repopulate
    Agent_FindExit();

    // Unarchive intents
    memcpy (&in_have_goal, save_p, sizeof(int)); save_p += sizeof(int);
    memcpy (&in_gx, save_p, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    memcpy (&in_gy, save_p, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    memcpy (&in_attack, save_p, sizeof(int)); save_p += sizeof(int);
    memcpy (&in_use, save_p, sizeof(int)); save_p += sizeof(int);
    memcpy (&in_weapon, save_p, sizeof(int)); save_p += sizeof(int);
    memcpy (&in_have_face, save_p, sizeof(int)); save_p += sizeof(int);
    memcpy (&in_face, save_p, sizeof(angle_t)); save_p += sizeof(angle_t);
    memcpy (&in_target, save_p, sizeof(int)); save_p += sizeof(int);

    // Unarchive level exit & key seek info
    memcpy (&exit_level, save_p, sizeof(int)); save_p += sizeof(int);
    memcpy (&exit_x, save_p, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    memcpy (&exit_y, save_p, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    memcpy (&exit_set, save_p, sizeof(int)); save_p += sizeof(int);
    memcpy (&key_seek, save_p, sizeof(int)); save_p += sizeof(int);
    memcpy (&key_x, save_p, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    memcpy (&key_y, save_p, sizeof(fixed_t)); save_p += sizeof(fixed_t);

    // Unarchive door seen statuses & waiting timer
    memcpy (&door_wait_timer, save_p, sizeof(int)); save_p += sizeof(int);
    memcpy (&saved_door_n, save_p, sizeof(int)); save_p += sizeof(int);

    for (i = 0; i < saved_door_n; i++)
    {
	unsigned char seen = 0;
	memcpy (&seen, save_p, sizeof(unsigned char)); save_p += sizeof(unsigned char);
	if (i < door_n)
	{
	    door_seen[i] = seen;
	}
    }
}
