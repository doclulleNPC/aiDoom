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
//	  observe                 -> {"tic":..,"player":{..},"exit":[x,y],"buddy":{friend,..},"things":[..]}
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

// Play-sim hooks declared by hand: p_local.h pulls p_spec.h, whose `open`/`close`
// enum values collide with the socket close()/read()/write() used here.
extern thinker_t	thinkercap;
extern void		P_MobjThinker (mobj_t*);
extern boolean		P_CheckSight (mobj_t* t1, mobj_t* t2);
extern fixed_t		P_AproxDistance (fixed_t dx, fixed_t dy);
extern angle_t		R_PointToAngle2 (fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2);
extern int		numlines;
extern line_t*		lines;

// ---- autonomous helpers: the level exit + doors/switches in the way ---------
static int	exit_level = -1;	// gameepisode*100+gamemap the exit was located for
static fixed_t	exit_x, exit_y;
static int	exit_set;

// ---- key fetch: when a locked door blocks us, detour to its key first -------
static int	key_seek;		// key colour we're fetching (0 = none), persists
static fixed_t	key_x, key_y;		// that key's location

// Locate the level exit once per map (an exit linedef special), like the director's.
static void Agent_FindExit (void)
{
    int i, lvl = gameepisode*100 + gamemap;
    if (exit_level == lvl) return;
    exit_level = lvl; exit_set = 0;
    key_seek = 0;					// drop any stale key detour from the old map
    for (i = 0 ; i < numlines ; i++)
    {
	int sp = lines[i].special;
	if (sp==11 || sp==51 || sp==52 || sp==124)	// S1/W1 exit + secret-exit switches
	{
	    exit_x = (lines[i].v1->x + lines[i].v2->x) >> 1;
	    exit_y = (lines[i].v1->y + lines[i].v2->y) >> 1;
	    exit_set = 1; return;
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

static void Agent_Observe (void)
{
    player_t*	p = &players[consoleplayer];
    mobj_t*	mo = p->mo;
    thinker_t*	th;
    char	buf[4096];
    int		off, first = 1;

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
    // Engage what we can see, and EXPLORE: every ~1.8 s pick a fresh random wander goal
    // (the reflex A*-routes to it).  Random exploration covers the map -- threading doors
    // and rooms -- and actually finishes levels, where a fixed beeline at the exit just
    // pins on the first wall/ledge between here and it.
    static unsigned seed = 1;			// own RNG -- never touch the playsim's
    in_attack = 1;
    if (!in_have_goal && (leveltime & 63) == 0)
    {
	mobj_t* mo = players[consoleplayer].mo;
	if (mo)
	{
	    int spread, fa;
	    // Bias the random heading toward the level EXIT (wide +/-128 deg spread), so the
	    // marine explores but TRENDS toward the exit instead of wandering off at random.
	    angle_t base = exit_set ? R_PointToAngle2 (mo->x, mo->y, exit_x, exit_y) : mo->angle;
	    seed = seed*1103515245u + 12345u;
	    spread = (int)((seed >> 16) & 255) - 128;	// -128..+127 deg off the exit direction
	    fa = (base + (angle_t)(spread * (ANG45/45))) >> ANGLETOFINESHIFT;
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
    int		rem, turn, chase = 0, los = 0;
    fixed_t	td = 0;			// distance to the current target

    memset (cmd, 0, sizeof(*cmd));
    Agent_Poll ();
    // The level-end intermission (and finale) screens wait for a key -- pulse fire to
    // advance them, else the LLM marine stalls on the tally screen forever.
    if (gamestate != GS_LEVEL)
	{ if ((gametic & 15) == 0) cmd->buttons = BT_ATTACK; return; }
    Agent_FindExit ();
    if (agent_demo) Agent_Brain ();
    if (!mo || p->playerstate != PST_LIVE) return;

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
    {
	fixed_t gx = 0, gy = 0;
	int	havegoal = 0;
	if (tgt)
	{
	    // Shoot only at a target we can SEE *and* that's in effective range -- never
	    // fire blind through a wall, and don't waste shots at long range where they
	    // miss.  Out of sight OR too far -> close in until we have a clean shot.
	    los = P_CheckSight (mo, tgt);
	    td  = P_AproxDistance (tgt->x - mo->x, tgt->y - mo->y);
	    if (los && td < AGENT_FIRE_RANGE)
		want = R_PointToAngle2 (mo->x, mo->y, tgt->x, tgt->y);	// aim + fire, hold
	    else
		{ gx = tgt->x; gy = tgt->y; havegoal = 1; }		// close in
	}
	else if (key_seek)						// fetch the needed key
	    { gx = key_x; gy = key_y; havegoal = 1; }
	else if (in_have_face) want = in_face;
	else if (in_have_goal)
	{
	    gx = in_gx; gy = in_gy; havegoal = 1;
	    if (P_AproxDistance (mo->x - in_gx, mo->y - in_gy) < AGENT_REACH)
		{ in_have_goal = 0; havegoal = 0; }			// arrived
	}
	else if (exit_set)						// idle -> head to the exit
	    { gx = exit_x; gy = exit_y; havegoal = 1; }

	if (havegoal)
	{
	    // A* portal route gives the coarse next waypoint; AICoop_ChaseDir does the LOCAL
	    // corner-rounding to it (DOOM P_NewChaseDir movement) so the marine threads walls
	    // and doorways instead of walking straight into a corner and sticking.
	    static chasedir_t agent_chase = { -1, 0, 0 };
	    fixed_t wx, wy, nx, ny;
	    if (P_AICoop_NextWaypoint (mo, gx, gy, &wx, &wy)) { nx = wx; ny = wy; }
	    else                                              { nx = gx; ny = gy; }
	    want  = AICoop_ChaseDir (mo, nx, ny, &agent_chase);	// marine's OWN heading state
	    chase = 1;
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

    // Advance toward the goal / out-of-sight target once roughly facing the way.
    if (chase && abs(rem) < (ANG45>>16))       cmd->forwardmove = AGENT_FORWARD;

    if (in_use)         { cmd->buttons |= BT_USE; in_use = 0; }
    if (in_weapon >= 0) { cmd->buttons |= BT_CHANGE | ((in_weapon << BT_WEAPONSHIFT) & BT_WEAPONMASK);
			  in_weapon = -1; }

    // Autonomy: open a door / hit the exit switch in our way, and un-stick if blocked --
    // without these the marine paths up to a shut door and never gets through.
    {
	static int	usewait;
	static fixed_t	lastx, lasty;
	static int	stuck;
	if (usewait > 0) usewait--;
	if (chase && usewait == 0 && Agent_UseAhead (mo)) { cmd->buttons |= BT_USE; usewait = 16; }

	if (chase)
	{
	    if (P_AproxDistance (mo->x - lastx, mo->y - lasty) < 3*FRACUNIT) stuck++;
	    else                                                            stuck = 0;
	    lastx = mo->x; lasty = mo->y;
	    if (stuck > 5)
	    {
		// Pinned (the reflex heads straight at the waypoint, no corner-rounding):
		// SWEEP the view one way while pushing forward + strafing so the marine
		// rotates until the gap opens; flip the sweep every ~32 tics.
		cmd->angleturn   = (stuck & 32) ?  AGENT_TURN : -AGENT_TURN;
		cmd->forwardmove = AGENT_FORWARD;
		cmd->sidemove    = (stuck & 32) ?  30 : -30;
		cmd->buttons    |= BT_USE;	// in case a shut door is pinning us
	    }
	}
	else stuck = 0;
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
