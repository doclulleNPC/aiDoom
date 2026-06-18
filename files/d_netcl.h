// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Chocolate/Crispy-compatible network CLIENT: connection state machine
//	(SYN -> lobby -> launch -> gamestart -> in-game), the reliable-packet
//	layer, and the GAMEDATA send/receive tic windows.  Clean-room reimpl of
//	the Chocolate-Doom protocol (the wire format is an interop fact); no
//	GPL source is used.  Stage 3 of the multiplayer-interop work.
//
//	This module owns only the *transport-side* lockstep machinery (building
//	the local ticcmd diff, exchanging/acking tics through the relay server).
//	Splicing it into the game's D_DoomLoop is Stage 4.
//
//-----------------------------------------------------------------------------

#ifndef __D_NETCL__
#define __D_NETCL__

#include "doomtype.h"
#include "d_ticcmd.h"

#define NET_PROTO_MAXPLAYERS	8	// protocol player count (we use MAXPLAYERS)
#define NET_BACKUPTICS		128	// must match Chocolate's BACKUPTICS

// Settings adopted from the server's GAMESTART (the authoritative game config).
typedef struct
{
    int		consoleplayer;		// our player index (negative => drone)
    int		num_players;
    int		ticdup;
    int		extratics;
    int		lowres_turn;
    int		new_sync;
    int		deathmatch;
    int		episode, map, skill;
} netcl_settings_t;

// Connect to a Chocolate/Crispy server and run the connection handshake.
// Returns true once we reach the lobby (CLIENT_STATE_WAITING_LAUNCH).
boolean		D_NetCl_Connect (const char* hostport, const char* version,
				 int gamemode, int gamemission, const char* playername);

// Drive the network once: drain packets, run reliability/keepalive, and (when
// in game) advance the tic windows.  Call frequently.
void		D_NetCl_Run (void);

// Lobby control: ask the server to launch (controller), and request game start
// with the given settings.  Both are reliable.
void		D_NetCl_LaunchGame (void);
void		D_NetCl_StartGame (const netcl_settings_t* want);

// Queue the local player's command for tic `maketic` and ship it (with the
// extratics redundancy window) as a GAMEDATA packet.  Stage-4 hook.
void		D_NetCl_SendTiccmd (ticcmd_t* cmd, int maketic);

// State accessors.
boolean		D_NetCl_Connected (void);
boolean		D_NetCl_InGame (void);
boolean		D_NetCl_LobbyLaunched (void);		// past the lobby?
boolean		D_NetCl_IsDisconnected (void);
const netcl_settings_t* D_NetCl_Settings (void);
int		D_NetCl_MakeTic (void);
int		D_NetCl_RecvTic (void);

// Game-loop tic pull: TicReady(tic) is true once the merged tic is available;
// GetTic copies the first `count` players' commands + in-game flags for it.
boolean		D_NetCl_TicReady (int tic);
void		D_NetCl_GetTic (int tic, ticcmd_t* cmds, boolean* ingame, int count);

// Blocking join: connect -> wait in lobby for min_players -> launch (if we are
// the controller) -> gamestart.  min_players<=1 launches immediately.
boolean		D_NetCl_JoinGame (const char* hostport, const char* version,
				  int gamemode, int gamemission, const char* playername,
				  int min_players, const netcl_settings_t* want,
				  netcl_settings_t* got);
int		D_NetCl_LobbyPlayers (void);
boolean		D_NetCl_IsController (void);

// Disconnect cleanly (sends DISCONNECT and waits briefly for the ack).
void		D_NetCl_Disconnect (void);

// Stage-3 headless interop self-test: connect, auto-launch a 1-player game,
// reach IN_GAME, and run the idle GAMEDATA exchange loop for a few seconds,
// proving the tic round-trip interoperates with a real chocolate-server.
// Invoked by -netclient <host[:port]>.
void		I_NetClientTest (const char* hostport, const char* version,
				 int gamemode, int gamemission);

#endif
