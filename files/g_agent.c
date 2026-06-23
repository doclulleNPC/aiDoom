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
//	Protocol (one command per line, replies one line):
//	  observe                 -> {"tic":..,"player":{..},"things":[..]}
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
#include "info.h"
#include "p_ai_coop.h"		// P_AICoop_NextWaypoint -- shared BSP pathfinder
#include "g_agent.h"

// Play-sim hooks declared by hand: p_local.h pulls p_spec.h, whose `open`/`close`
// enum values collide with the socket close()/read()/write() used here.
extern thinker_t	thinkercap;
extern void		P_MobjThinker (mobj_t*);
extern boolean		P_CheckSight (mobj_t* t1, mobj_t* t2);
extern fixed_t		P_AproxDistance (fixed_t dx, fixed_t dy);
extern angle_t		R_PointToAngle2 (fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2);

#define AGENT_TURN	1400		// max angleturn/tic (~7.7 deg)
#define AGENT_FACING	1200		// |turn remaining| under which we open fire
#define AGENT_FORWARD	50		// run-speed forwardmove
#define AGENT_SIGHT	(2048*FRACUNIT)	// monster auto-aim acquisition range
#define AGENT_REACH	(64*FRACUNIT)	// goal considered reached within this
#define AGENT_MAXTHINGS	24		// observe + target registry size

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

static void Agent_Observe (void)
{
    player_t*	p = &players[consoleplayer];
    mobj_t*	mo = p->mo;
    thinker_t*	th;
    char	buf[4096];
    int		off, first = 1;

    reg_n = 0;
    if (!mo) { Agent_Send ("{\"player\":null}\n"); return; }

    off = snprintf (buf, sizeof buf,
	"{\"tic\":%d,\"player\":{\"x\":%d,\"y\":%d,\"z\":%d,\"angle\":%u,"
	"\"health\":%d,\"armor\":%d,\"weapon\":%d,\"ammo\":[%d,%d,%d,%d],"
	"\"kills\":%d,\"items\":%d},\"things\":[",
	leveltime, mo->x>>FRACBITS, mo->y>>FRACBITS, mo->z>>FRACBITS,
	(unsigned)(mo->angle / (ANG45/45)), p->health, p->armorpoints, p->readyweapon,
	p->ammo[0], p->ammo[1], p->ammo[2], p->ammo[3], p->killcount, p->itemcount);

    for (th = thinkercap.next ; th != &thinkercap && reg_n < AGENT_MAXTHINGS ; th = th->next)
    {
	mobj_t*	m; fixed_t d; int relangle, vis;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (m == mo) continue;
	if (!(m->flags & (MF_COUNTKILL|MF_SPECIAL)) && m->type != MT_BARREL) continue;
	d = P_AproxDistance (m->x - mo->x, m->y - mo->y);
	if (d > AGENT_SIGHT) continue;
	relangle = (int)(((R_PointToAngle2 (mo->x, mo->y, m->x, m->y) - mo->angle)) / (ANG45/45));
	if (relangle > 180) relangle -= 360;
	vis = P_CheckSight (mo, m) ? 1 : 0;
	reg[reg_n] = m;
	off += snprintf (buf+off, sizeof buf-off,
	    "%s{\"id\":%d,\"type\":\"%s\",\"dist\":%d,\"rel\":%d,\"hp\":%d,\"vis\":%d}",
	    first?"":",", reg_n, Agent_TypeName (m->type), d>>FRACBITS, relangle, m->health, vis);
	first = 0; reg_n++;
    }
    off += snprintf (buf+off, sizeof buf-off, "]}\n");
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

// ---------------------------------------------------------------------------
// Built-in scripted brain (-aiplayer demo): engage monsters, else wander.
// ---------------------------------------------------------------------------
static void Agent_Brain (void)
{
    static unsigned seed = 1;			// own RNG -- never touch the playsim's
    in_attack = 1;				// always engage what we can see
    if (!in_have_goal && (leveltime & 63) == 0)	// every ~1.8 s pick a new wander goal
    {
	mobj_t* mo = players[consoleplayer].mo;
	if (mo)
	{
	    int turn;
	    seed = seed*1103515245u + 12345u;
	    turn = (int)((seed >> 16) & 127) - 64;	// -64..+63 degrees
	    int fa = (mo->angle + (angle_t)(turn * (ANG45/45))) >> ANGLETOFINESHIFT;
	    in_gx = mo->x + FixedMul (512*FRACUNIT, finecosine[fa]);
	    in_gy = mo->y + FixedMul (512*FRACUNIT, finesine[fa]);
	    in_have_goal = 1;
	}
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
    int		rem, turn, moving = 0;

    memset (cmd, 0, sizeof(*cmd));
    Agent_Poll ();
    if (agent_demo) Agent_Brain ();
    if (!mo || p->playerstate != PST_LIVE) return;

    // Target priority: an explicit `target`, else the nearest visible monster while
    // `attack` is held.  Aim there; otherwise honour an explicit face, then the goal.
    tgt = Agent_Target ();
    if (!tgt && in_attack) tgt = Agent_NearestMonster (mo);

    want = mo->angle;
    if (tgt)               want = R_PointToAngle2 (mo->x, mo->y, tgt->x, tgt->y);
    else if (in_have_face) want = in_face;
    else if (in_have_goal)
    {
	fixed_t wx, wy;
	if (P_AICoop_NextWaypoint (mo, in_gx, in_gy, &wx, &wy))
	    { want = R_PointToAngle2 (mo->x, mo->y, wx, wy); moving = 1; }
	if (P_AproxDistance (mo->x - in_gx, mo->y - in_gy) < AGENT_REACH)
	    { in_have_goal = 0; moving = 0; }		// arrived
    }

    rem  = (short)((want - mo->angle) >> 16);
    turn = rem;
    if (turn >  AGENT_TURN) turn =  AGENT_TURN;
    if (turn < -AGENT_TURN) turn = -AGENT_TURN;
    cmd->angleturn = (short)turn;

    if (in_have_face && abs(rem) < 600) in_have_face = 0;	// face reached

    // fire when roughly on target (auto-aim does the rest)
    if (tgt && abs(rem) < AGENT_FACING)        cmd->buttons |= BT_ATTACK;
    else if (in_attack && !tgt)                cmd->buttons |= BT_ATTACK;	// blind fire if asked

    // advance toward the goal once we're roughly facing the waypoint
    if (moving && abs(rem) < (ANG45>>16))      cmd->forwardmove = AGENT_FORWARD;

    if (in_use)         { cmd->buttons |= BT_USE; in_use = 0; }
    if (in_weapon >= 0) { cmd->buttons |= BT_CHANGE | ((in_weapon << BT_WEAPONSHIFT) & BT_WEAPONMASK);
			  in_weapon = -1; }
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
