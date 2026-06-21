// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	LLM "AI Director" for monster control.
//
//	An external director connects over a TCP line protocol (or the built-in
//	-aidemo director runs internally) and issues high-level *orders* to
//	monsters; the engine's existing AI primitives (P_Move, A_FaceTarget,
//	attack states) execute them every tic.  Decisions are *latched* with
//	for_tics/after_tics so the 35 Hz loop never blocks on the director.
//
//	Design rationale lives in AGENT_CONTROL.md sections 12-13 and
//	monster_llm_control.md.
//
//	Protocol (newline-delimited, one client):
//	    observe\n                     -> server replies one JSON line
//	    act order=<name> ids=<csv> [x=<n> y=<n> focus=<id> for=<n> after=<n>]\n
//	    reset\n
//	Orders: chase hold fallback flank_left flank_right ambush focus_fire use_door
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN	/* keep rpcndr.h out: its byte/boolean clash with doomtype.h */
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
// Win32 sockets aren't fd's: route the POSIX socket I/O used below through Winsock.
// (p_ai_llm.c does no file I/O, so blanket-mapping these names is safe here.)
#define close(fd)	closesocket(fd)
#define read(fd,b,n)	recv((fd),(char*)(b),(int)(n),0)
#define write(fd,b,n)	send((fd),(const char*)(b),(int)(n),0)
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#endif
#include <errno.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_think.h"
#include "tables.h"
#include "m_random.h"
#include "m_argv.h"
#include "p_mobj.h"
// NOTE: not including p_local.h here -- it pulls in p_spec.h, which defines
// enum constants named `open`/`close` that collide with <unistd.h>. We declare
// the few engine functions we need by hand instead.
#include "info.h"
#include "d_items.h"		// weaponinfo (buddy ammo in the observation)
#include "s_sound.h"
#include "r_main.h"

#include "p_ai_llm.h"
#include "p_ai_coop.h"		// buddy observation + director directives (-aicoop)

extern boolean	P_SetMobjState (mobj_t* mobj, statenum_t state);
extern boolean	P_CheckSight (mobj_t* t1, mobj_t* t2);

// ---- engine internals we reuse (defined in p_enemy.c, no public header) ----
extern void	P_NewChaseDir (mobj_t* actor);
extern boolean	P_Move (mobj_t* actor);
extern boolean	P_CheckMeleeRange (mobj_t* actor);
extern boolean	P_CheckMissileRange (mobj_t* actor);
extern boolean	P_LookForPlayers (mobj_t* actor, boolean allaround);
extern void	A_FaceTarget (mobj_t* actor);
extern void	P_MobjThinker (mobj_t* mobj);
extern thinker_t	thinkercap;

// ---------------------------------------------------------------------------
// Orders
// ---------------------------------------------------------------------------
enum
{
    AIO_NONE = 0,
    AIO_CHASE,			// == vanilla; handled by A_Chase, not diverted
    AIO_HOLD,
    AIO_FALLBACK,
    AIO_FLANK_L,
    AIO_FLANK_R,
    AIO_AMBUSH,
    AIO_FOCUS,
    AIO_USEDOOR
};

typedef struct
{
    mobj_t*	mo;		// the monster (NULL = free slot)
    int		order;
    fixed_t	tx, ty;		// ambush point
    int		focus_id;	// focus_fire target id (0 = the player)
    int		for_tics;	// remaining validity
    int		after_tics;	// delay before the order activates
} aientry_t;

#define AI_MAX	256
static aientry_t	aient[AI_MAX];
static int		aient_count;

// runtime config
static int	ai_on;			// transport exists (-aidirector/-aidemo or console)
static int	ai_enabled = 1;		// runtime toggle (console "director on/off")
static int	ai_demo;		// -aidemo given
static int	ai_inited;
static int	ai_port = 31666;

// ---------------------------------------------------------------------------
// Registry: rebuilt on each observe (and by the demo director). Assigns each
// live monster a small stable id (= slot+1) until the next rebuild / reset.
// ---------------------------------------------------------------------------
static void AI_BuildRegistry (void)
{
    thinker_t*	th;
    mobj_t*	mo;
    aientry_t	old[AI_MAX];
    int		oldn = aient_count;
    int		i;

    // Snapshot existing directives so a rebuild (e.g. a fresh `observe`) doesn't
    // clobber orders issued since the last one -- carry them over by mobj.
    memcpy (old, aient, sizeof(aientry_t) * (oldn > 0 ? oldn : 0));

    aient_count = 0;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	mo = (mobj_t *)th;
	if (mo->health <= 0)
	    continue;
	if (!(mo->flags & MF_COUNTKILL))	// monsters only
	    continue;
	if (aient_count >= AI_MAX)
	    break;
	aient[aient_count].mo = mo;
	aient[aient_count].order = AIO_NONE;
	aient[aient_count].tx = aient[aient_count].ty = 0;
	aient[aient_count].focus_id = 0;
	aient[aient_count].for_tics = 0;
	aient[aient_count].after_tics = 0;
	// preserve a still-valid directive from the previous registry
	for (i = 0; i < oldn; i++)
	    if (old[i].mo == mo && old[i].for_tics > 0)
	    {
		aient[aient_count].order = old[i].order;
		aient[aient_count].tx = old[i].tx;
		aient[aient_count].ty = old[i].ty;
		aient[aient_count].focus_id = old[i].focus_id;
		aient[aient_count].for_tics = old[i].for_tics;
		aient[aient_count].after_tics = old[i].after_tics;
		break;
	    }
	aient_count++;
    }
}

static aientry_t* AI_FindByMobj (mobj_t* mo)
{
    int i;
    for (i = 0; i < aient_count; i++)
	if (aient[i].mo == mo)
	    return &aient[i];
    return NULL;
}

static mobj_t* AI_MobjById (int id)
{
    if (id >= 1 && id <= aient_count)
	return aient[id-1].mo;
    return NULL;
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
// Quantize the angle from `a` to (x,y) into one of the 8 movedir directions.
static int AI_DirTo (mobj_t* a, fixed_t x, fixed_t y)
{
    angle_t ang = R_PointToAngle2 (a->x, a->y, x, y);
    return ((ang + (ANG45/2)) >> 29) & 7;	// ANG45 == 1<<29
}

static void AI_MoveDir (mobj_t* a, int dir)
{
    a->movedir = dir;
    if (!P_Move (a))
	P_NewChaseDir (a);		// blocked -> fall back to pathing
}

// ---------------------------------------------------------------------------
// A_LLMChase -- execute the current directive.
// Mirrors A_Chase's upkeep + attack logic; only the *movement* changes per
// order.  Monsters still acquire targets, turn, and fire normally.
// ---------------------------------------------------------------------------
void A_LLMChase (mobj_t* actor)
{
    aientry_t*	e = AI_FindByMobj (actor);
    int		order = e ? e->order : AIO_CHASE;
    int		delta;
    int		dir;

    if (actor->reactiontime)
	actor->reactiontime--;

    if (actor->threshold)
    {
	if (!actor->target || actor->target->health <= 0)
	    actor->threshold = 0;
	else
	    actor->threshold--;
    }

    // focus_fire: retarget onto the requested mobj (default: the player).
    if (order == AIO_FOCUS)
    {
	mobj_t* f = (e && e->focus_id) ? AI_MobjById(e->focus_id) : NULL;
	if (!f)
	    f = players[consoleplayer].mo;
	if (f && f->health > 0)
	    actor->target = f;
    }

    // turn towards movement direction if not there yet
    if (actor->movedir < 8)
    {
	actor->angle &= (7<<29);
	delta = actor->angle - (actor->movedir << 29);
	if (delta > 0)
	    actor->angle -= ANG90/2;
	else if (delta < 0)
	    actor->angle += ANG90/2;
    }

    if (!actor->target || !(actor->target->flags & MF_SHOOTABLE))
    {
	if (P_LookForPlayers (actor, true))
	    return;
	P_SetMobjState (actor, actor->info->spawnstate);
	return;
    }

    if (actor->flags & MF_JUSTATTACKED)
    {
	actor->flags &= ~MF_JUSTATTACKED;
	if (gameskill != sk_nightmare && !fastparm)
	    P_NewChaseDir (actor);
	return;
    }

    // melee attack
    if (actor->info->meleestate && P_CheckMeleeRange (actor))
    {
	if (actor->info->attacksound)
	    S_StartSound (actor, actor->info->attacksound);
	P_SetMobjState (actor, actor->info->meleestate);
	return;
    }

    // missile attack (a HOLD/flank monster still shoots when it can)
    if (actor->info->missilestate
	&& !(gameskill < sk_nightmare && !fastparm && actor->movecount))
    {
	if (P_CheckMissileRange (actor))
	{
	    P_SetMobjState (actor, actor->info->missilestate);
	    actor->flags |= MF_JUSTATTACKED;
	    return;
	}
    }

    // movement, per order
    switch (order)
    {
      case AIO_HOLD:
	A_FaceTarget (actor);				// stand and face
	break;

      case AIO_FALLBACK:
	dir = (AI_DirTo (actor, actor->target->x, actor->target->y) + 4) & 7;
	AI_MoveDir (actor, dir);			// retreat from target
	break;

      case AIO_FLANK_L:
	dir = (AI_DirTo (actor, actor->target->x, actor->target->y) + 2) & 7;
	AI_MoveDir (actor, dir);
	break;

      case AIO_FLANK_R:
	dir = (AI_DirTo (actor, actor->target->x, actor->target->y) + 6) & 7;
	AI_MoveDir (actor, dir);
	break;

      case AIO_AMBUSH:
	dir = AI_DirTo (actor, e ? e->tx : actor->target->x,
			       e ? e->ty : actor->target->y);
	AI_MoveDir (actor, dir);
	break;

      case AIO_USEDOOR:		// approximate: head to target, P_Move opens doors
      case AIO_FOCUS:
      case AIO_CHASE:
      default:
	if (--actor->movecount < 0 || !P_Move (actor))
	    P_NewChaseDir (actor);
	break;
    }

    if (actor->info->activesound && P_Random () < 3)
	S_StartSound (actor, actor->info->activesound);
}

// ---------------------------------------------------------------------------
// A_Chase asks this whether to defer.  Only diverts for an active, non-default
// order (CHASE/NONE -> let the vanilla A_Chase run unchanged).
// ---------------------------------------------------------------------------
int P_AI_Active (mobj_t* actor)
{
    aientry_t* e;

    if (!ai_on || !ai_enabled)
	return 0;
    e = AI_FindByMobj (actor);
    if (!e || e->order <= AIO_CHASE)
	return 0;
    if (e->after_tics > 0 || e->for_tics <= 0)
	return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// Observation serializer
// ---------------------------------------------------------------------------
static const char* AI_OrderName (int o)
{
    switch (o)
    {
      case AIO_CHASE:	return "chase";
      case AIO_HOLD:	return "hold";
      case AIO_FALLBACK:return "fallback";
      case AIO_FLANK_L:	return "flank_left";
      case AIO_FLANK_R:	return "flank_right";
      case AIO_AMBUSH:	return "ambush";
      case AIO_FOCUS:	return "focus_fire";
      case AIO_USEDOOR:	return "use_door";
      default:		return "none";
    }
}

static const char* AI_TypeName (mobjtype_t t)
{
    switch (t)
    {
      case MT_POSSESSED:	return "zombieman";
      case MT_SHOTGUY:		return "shotgunguy";
      case MT_TROOP:		return "imp";
      case MT_SERGEANT:		return "pinky";
      default:			return "monster";
    }
}

// Resolve a registry id (as exposed in `observe`) to its monster, or NULL.
// Used to let the buddy `focus` a specific monster the director names.
static mobj_t* P_AI_MobjForId (int id)
{
    if (id >= 1 && id <= aient_count)
	return aient[id-1].mo;
    return NULL;
}

#define OBSBUF	32768
static char obsbuf[OBSBUF];

static int AI_Serialize (void)
{
    player_t*	pl = &players[consoleplayer];
    int		n = 0;
    int		i;
    int		fx = FRACUNIT;

    AI_BuildRegistry ();

    n += snprintf (obsbuf+n, OBSBUF-n, "{\"tic\":%d,\"player\":{", leveltime);
    if (pl->mo)
	n += snprintf (obsbuf+n, OBSBUF-n,
		"\"pos\":[%d,%d,%d],\"angle\":%u,\"health\":%d,\"armor\":%d,\"weapon\":%d}",
		pl->mo->x/fx, pl->mo->y/fx, pl->mo->z/fx,
		(unsigned)(((uint64_t)pl->mo->angle * 360u) >> 32),	// BAM -> degrees
		pl->health, pl->armorpoints, pl->readyweapon);
    else
	n += snprintf (obsbuf+n, OBSBUF-n, "\"dead\":true}");

    // Buddy (-aicoop only): the director also commands the player's AI companion.
    {
	int bs = P_AICoop_Slot ();
	if (P_AICoop_AIMode () && bs >= 0 && playeringame[bs] && players[bs].mo)
	{
	    player_t*	b = &players[bs];
	    int		w = b->readyweapon;
	    int		ammo = (w >= 0 && w < NUMWEAPONS && weaponinfo[w].ammo < NUMAMMO)
			       ? b->ammo[weaponinfo[w].ammo] : -1;
	    static const char* sname[] = {"follow","fight","heal","hold","come","grab"};
	    int		st = P_AICoop_State ();
	    fixed_t	rx[6], ry[6];
	    int		nr = P_AICoop_NavRoute (rx, ry, 6), r;
	    n += snprintf (obsbuf+n, OBSBUF-n,
		",\"buddy\":{\"pos\":[%d,%d],\"health\":%d,\"armor\":%d,"
		"\"weapon\":%d,\"ammo\":%d,\"state\":\"%s\",\"route\":[",
		b->mo->x/fx, b->mo->y/fx, b->health, b->armorpoints,
		w, ammo, (st>=0 && st<6) ? sname[st] : "follow");
	    for (r = 0; r < nr; r++)
		n += snprintf (obsbuf+n, OBSBUF-n, "%s[%d,%d]", r?",":"", rx[r]/fx, ry[r]/fx);
	    n += snprintf (obsbuf+n, OBSBUF-n, "]}");
	}
    }

    n += snprintf (obsbuf+n, OBSBUF-n, ",\"monsters\":[");
    for (i = 0; i < aient_count; i++)
    {
	mobj_t* m = aient[i].mo;
	int see = (pl->mo && P_CheckSight (m, pl->mo)) ? 1 : 0;
	if (i)
	    n += snprintf (obsbuf+n, OBSBUF-n, ",");
	n += snprintf (obsbuf+n, OBSBUF-n,
	    "{\"id\":%d,\"type\":\"%s\",\"pos\":[%d,%d],\"hp\":%d,"
	    "\"see_player\":%s,\"order\":\"%s\"}",
	    i+1, AI_TypeName(m->type), m->x/fx, m->y/fx, m->health,
	    see ? "true" : "false", AI_OrderName(aient[i].order));
	if (n > OBSBUF - 256)
	    break;		// buffer guard
    }
    n += snprintf (obsbuf+n, OBSBUF-n, "],\"count\":%d}\n", aient_count);
    return n;
}

// ---------------------------------------------------------------------------
// act parser:  act order=flank_left ids=1,2 for=70 after=0 x=.. y=.. focus=..
// ---------------------------------------------------------------------------
static int AI_OrderByName (const char* s)
{
    if (!strcmp(s,"chase"))		return AIO_CHASE;
    if (!strcmp(s,"hold"))		return AIO_HOLD;
    if (!strcmp(s,"fallback"))		return AIO_FALLBACK;
    if (!strcmp(s,"flank_left"))	return AIO_FLANK_L;
    if (!strcmp(s,"flank_right"))	return AIO_FLANK_R;
    if (!strcmp(s,"ambush"))		return AIO_AMBUSH;
    if (!strcmp(s,"focus_fire"))	return AIO_FOCUS;
    if (!strcmp(s,"use_door"))		return AIO_USEDOOR;
    return AIO_NONE;
}

static int AI_BuddyTactic (const char* s)
{
    if (!strcmp(s,"engage"))	return BUD_ENGAGE;
    if (!strcmp(s,"defend"))	return BUD_DEFEND;
    if (!strcmp(s,"hold"))	return BUD_HOLD;
    if (!strcmp(s,"regroup"))	return BUD_REGROUP;
    if (!strcmp(s,"retreat"))	return BUD_RETREAT;
    if (!strcmp(s,"goto"))	return BUD_GOTO;
    if (!strcmp(s,"grab"))	return BUD_GRAB;
    return BUD_AUTO;
}

static void AI_Apply (int order, const char* ids,
		      fixed_t tx, fixed_t ty, int focus, int fortics, int after)
{
    char	tmp[256];
    char*	tok;

    if (fortics <= 0)
	fortics = 70;			// default ~2 s
    strncpy (tmp, ids, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = 0;

    for (tok = strtok(tmp, ","); tok; tok = strtok(NULL, ","))
    {
	int id = atoi(tok);
	if (id >= 1 && id <= aient_count)
	{
	    aient[id-1].order = order;
	    aient[id-1].tx = tx;
	    aient[id-1].ty = ty;
	    aient[id-1].focus_id = focus;
	    aient[id-1].for_tics = fortics;
	    aient[id-1].after_tics = (after > 0) ? after : 0;
	}
    }
}

static void AI_HandleLine (char* line, int client)
{
    if (!strncmp(line, "observe", 7))
    {
	int len = AI_Serialize();
	if (client >= 0)
	    (void)!write (client, obsbuf, len);
	return;
    }
    if (!strncmp(line, "reset", 5))
    {
	int i; for (i=0;i<aient_count;i++) aient[i].order = AIO_NONE;
	if (client >= 0) (void)!write (client, "ok\n", 3);
	return;
    }
    if (!strncmp(line, "wake", 4))
    {
	// Testing aid: wake every registered monster (target the player and
	// drop it into its chase state, exactly as A_Look would on sighting).
	mobj_t* plmo = players[consoleplayer].mo;
	int i;
	AI_BuildRegistry ();
	for (i = 0; i < aient_count; i++)
	{
	    mobj_t* m = aient[i].mo;
	    if (m && m->health > 0 && plmo)
	    {
		m->target = plmo;
		if (m->info->seestate)
		    P_SetMobjState (m, m->info->seestate);
	    }
	}
	if (client >= 0) (void)!write (client, "ok\n", 3);
	return;
    }
    if (!strncmp(line, "act", 3))
    {
	char	order_s[32] = "chase";
	char	ids[200] = "";
	int	focus = 0, fortics = 0, after = 0;
	fixed_t	tx = 0, ty = 0;
	char*	kv;
	char	work[512];

	strncpy (work, line+3, sizeof(work)-1);
	work[sizeof(work)-1] = 0;
	for (kv = strtok(work, " \t\r\n"); kv; kv = strtok(NULL, " \t\r\n"))
	{
	    char* eq = strchr(kv, '=');
	    if (!eq) continue;
	    *eq++ = 0;
	    if (!strcmp(kv,"order")) { strncpy(order_s,eq,sizeof(order_s)-1); order_s[sizeof(order_s)-1]=0; }
	    else if (!strcmp(kv,"ids")) { strncpy(ids,eq,sizeof(ids)-1); ids[sizeof(ids)-1]=0; }
	    else if (!strcmp(kv,"x")) tx = atoi(eq)*FRACUNIT;
	    else if (!strcmp(kv,"y")) ty = atoi(eq)*FRACUNIT;
	    else if (!strcmp(kv,"focus")) focus = atoi(eq);
	    else if (!strcmp(kv,"for")) fortics = atoi(eq);
	    else if (!strcmp(kv,"after")) after = atoi(eq);
	}
	AI_Apply (AI_OrderByName(order_s), ids, tx, ty, focus, fortics, after);
	if (client >= 0) (void)!write (client, "ok\n", 3);
	return;
    }
    if (!strncmp(line, "buddy", 5))
    {
	char	order_s[32] = "auto";
	int	focus = 0, fortics = 0;
	fixed_t	bx = 0, by = 0;
	char*	kv;
	char	work[256];

	strncpy (work, line+5, sizeof(work)-1);
	work[sizeof(work)-1] = 0;
	for (kv = strtok(work, " \t\r\n"); kv; kv = strtok(NULL, " \t\r\n"))
	{
	    char* eq = strchr(kv, '=');
	    if (!eq) continue;
	    *eq++ = 0;
	    if      (!strcmp(kv,"order")) { strncpy(order_s,eq,sizeof(order_s)-1); order_s[sizeof(order_s)-1]=0; }
	    else if (!strcmp(kv,"focus")) focus   = atoi(eq);
	    else if (!strcmp(kv,"x"))     bx      = atoi(eq)*FRACUNIT;
	    else if (!strcmp(kv,"y"))     by      = atoi(eq)*FRACUNIT;
	    else if (!strcmp(kv,"for"))   fortics = atoi(eq);
	}
	P_AICoop_SetDirective (AI_BuddyTactic(order_s), P_AI_MobjForId(focus), bx, by, fortics);
	if (client >= 0) (void)!write (client, "ok\n", 3);
	return;
    }
}

// ---------------------------------------------------------------------------
// Non-blocking TCP server (one client)
// ---------------------------------------------------------------------------
static int	listen_fd = -1;
static int	client_fd = -1;
static char	linebuf[2048];
static int	linelen;

static void AI_SetNonBlock (int fd)
{
#ifdef _WIN32
    unsigned long nb = 1;
    ioctlsocket (fd, FIONBIO, &nb);
#else
    int fl = fcntl (fd, F_GETFL, 0);
    if (fl >= 0)
	fcntl (fd, F_SETFL, fl | O_NONBLOCK);
#endif
}

static void AI_OpenSocket (void)
{
    struct sockaddr_in	addr;
    int			yes = 1;
#ifdef _WIN32
    WSADATA		wsadata;
    WSAStartup (MAKEWORD(2,2), &wsadata);
#endif

    listen_fd = socket (AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
	{ printf ("P_AI: socket() failed\n"); ai_on = 0; return; }
    setsockopt (listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset (&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    addr.sin_port = htons (ai_port);

    if (bind (listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{ printf ("P_AI: bind(%d) failed\n", ai_port); close(listen_fd); listen_fd=-1; ai_on=0; return; }
    if (listen (listen_fd, 1) < 0)
	{ printf ("P_AI: listen() failed\n"); close(listen_fd); listen_fd=-1; ai_on=0; return; }
    AI_SetNonBlock (listen_fd);
    printf ("P_AI: AI Director listening on 127.0.0.1:%d\n", ai_port);
}

static void AI_PollSocket (void)
{
    int	r;

    if (listen_fd < 0)
	return;

    if (client_fd < 0)
    {
	int c = accept (listen_fd, NULL, NULL);
	if (c >= 0) { client_fd = c; AI_SetNonBlock(client_fd); linelen = 0; }
    }
    if (client_fd < 0)
	return;

    // drain whatever is available; split into lines
    for (;;)
    {
	char ch;
	r = read (client_fd, &ch, 1);
	if (r == 0) { close(client_fd); client_fd = -1; return; }	// closed
	if (r < 0) { /* EAGAIN: nothing more this tic */ return; }
	if (ch == '\n')
	{
	    linebuf[linelen] = 0;
	    AI_HandleLine (linebuf, client_fd);
	    linelen = 0;
	}
	else if (linelen < (int)sizeof(linebuf)-1)
	    linebuf[linelen++] = ch;
    }
}

// ---------------------------------------------------------------------------
// Built-in demo director (no LLM): every ~2 s, split monsters into flankers
// and a fallback group, to visibly prove the hook drives behaviour.
// ---------------------------------------------------------------------------
static void AI_DemoDirector (void)
{
    static int next = 0;
    int i;

    if (leveltime < next)
	return;
    next = leveltime + 105;		// re-plan every 3 s

    AI_BuildRegistry ();
    for (i = 0; i < aient_count; i++)
    {
	// alternate: even ids flank left, odd ids fall back briefly then chase
	aient[i].order      = (i & 1) ? AIO_FALLBACK : AIO_FLANK_L;
	aient[i].for_tics   = 140;
	aient[i].after_tics = 0;
    }
    if (aient_count)
	printf ("P_AI(demo): directing %d monsters (flank/fallback)\n", aient_count);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void P_AI_Init (void)
{
    int p;

    if (ai_inited)
	return;
    ai_inited = 1;

    p = M_CheckParm ("-aidirector");
    if (p)
    {
	ai_on = 1;
	if (p < myargc-1 && myargv[p+1][0] != '-')
	    ai_port = atoi (myargv[p+1]);
	AI_OpenSocket ();
    }
    if (M_CheckParm ("-aidemo"))
    {
	ai_on = 1;
	ai_demo = 1;
	printf ("P_AI: built-in demo director enabled\n");
    }
    // -aicoop drives the buddy through the same director transport, so open the
    // socket for it too (on the -aidirector port if given, else the default).
    if (M_CheckParm ("-aicoop"))
    {
	ai_on = 1;
	if (listen_fd < 0)
	    AI_OpenSocket ();
    }
}

void P_AI_Reset (void)
{
    aient_count = 0;
}

void P_AI_Ticker (void)
{
    int i;

    if (!ai_inited)
	P_AI_Init ();
    if (!ai_on || !ai_enabled)
	return;

    AI_PollSocket ();
    if (ai_demo)
	AI_DemoDirector ();

    // age directive timers (latched execution)
    for (i = 0; i < aient_count; i++)
    {
	if (aient[i].order <= AIO_CHASE)
	    continue;
	if (aient[i].after_tics > 0)
	    aient[i].after_tics--;
	else if (aient[i].for_tics > 0)
	{
	    if (--aient[i].for_tics == 0)
		aient[i].order = AIO_NONE;	// expired -> back to vanilla
	}
    }
}

// ---------------------------------------------------------------------------
// Console: turn the LLM<->Doom monster director on/off at runtime.
//   arg: "on" | "off" | "demo" | "" (toggle)
// ---------------------------------------------------------------------------
const char* P_AI_Console (const char* arg)
{
    static char	msg[160];
    char	a[16];
    int		i, want;

    if (!ai_inited)
	P_AI_Init ();

    for (i = 0; arg && arg[i] && i < 15; i++) a[i] = (char)tolower((unsigned char)arg[i]);
    a[i] = 0;

    if (!strcmp(a, "demo"))
    {
	ai_demo = !ai_demo;
	if (ai_demo) { ai_on = 1; ai_enabled = 1; }
	snprintf (msg, sizeof(msg), "AI director: built-in demo %s", ai_demo ? "ON" : "off");
	return msg;
    }

    if      (!strcmp(a, "on"))  want = 1;
    else if (!strcmp(a, "off")) want = 0;
    else                        want = !ai_enabled;	// toggle

    if (want)
    {
	if (listen_fd < 0)		// no transport yet -> open the TCP server
	    AI_OpenSocket ();
	ai_on = 1;
	ai_enabled = 1;
	snprintf (msg, sizeof(msg),
		  "AI director ON -- monsters under director control (TCP port %d)", ai_port);
    }
    else
    {
	ai_enabled = 0;
	aient_count = 0;		// drop directives -> monsters revert to vanilla now
	snprintf (msg, sizeof(msg), "AI director OFF -- monsters use vanilla AI");
    }
    return msg;
}
