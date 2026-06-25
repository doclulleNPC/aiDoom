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
#include <signal.h>

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
#include "p_ai_director.h"	// L4D stress fields + spawn act verbs (-aidirector)

extern boolean	P_SetMobjState (mobj_t* mobj, statenum_t state);
extern boolean	P_CheckSight (mobj_t* t1, mobj_t* t2);
extern fixed_t	P_AproxDistance (fixed_t dx, fixed_t dy);

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

// Step toward (gx,gy) by following the BSP portal pathfinder's next waypoint -- so a
// directed monster rounds corners and crosses rooms toward its target instead of the
// vanilla straight-line 8-dir walk that jams on the first wall.  Falls back to a
// direct bearing if there is no route (same graph the buddy navigates).
static void AI_MoveToward (mobj_t* a, fixed_t gx, fixed_t gy)
{
    fixed_t wx, wy;
    if (P_AICoop_NextWaypoint (a, gx, gy, &wx, &wy))
	AI_MoveDir (a, AI_DirTo (a, wx, wy));
    else
	AI_MoveDir (a, AI_DirTo (a, gx, gy));
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
	// navigate to the ambush point via the pathfinder (not straight 8-dir)
	AI_MoveToward (actor, e ? e->tx : actor->target->x,
			      e ? e->ty : actor->target->y);
	break;

      case AIO_USEDOOR:		// head to target via the pathfinder (P_Move opens doors)
      case AIO_FOCUS:
      case AIO_CHASE:
      default:
	// Pathfind toward the target so the monster rounds corners / crosses rooms
	// instead of grinding the nearest wall with the vanilla straight chase.
	AI_MoveToward (actor, actor->target->x, actor->target->y);
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

#include "r_state.h"		// sectors / numsectors / lines / numlines (room topology)

#define OBSBUF	32768
static char obsbuf[OBSBUF];

// Sector index a map object stands in (its "region" / room for the LLM).
#define AI_REGION(mo)	((int)((mo)->subsector->sector - sectors))

// Set of sectors that currently contain an entity (player / buddy / monsters);
// the room-link graph is built only over these so it stays small + token-cheap.
#define AI_MAXOCC	64
static int  ai_occ[AI_MAXOCC], ai_nocc;
static void AI_OccReset (void) { ai_nocc = 0; }
static void AI_OccAdd (int s)
{
    int i;
    if (s < 0) return;
    for (i = 0; i < ai_nocc; i++) if (ai_occ[i] == s) return;
    if (ai_nocc < AI_MAXOCC) ai_occ[ai_nocc++] = s;
}
static int AI_InOcc (int s)
{
    int i;
    for (i = 0; i < ai_nocc; i++) if (ai_occ[i] == s) return 1;
    return 0;
}
static int AI_InOccN (int s, int upto)	// membership in just the first `upto` entries
{
    int i;
    for (i = 0; i < upto && i < ai_nocc; i++) if (ai_occ[i] == s) return 1;
    return 0;
}
static int AI_OccIndex (int s)
{
    int i;
    for (i = 0; i < ai_nocc; i++) if (ai_occ[i] == s) return i;
    return -1;
}

// Classify a linedef special: 0 = plain opening, 1 = door, 2 = locked door.
static int AI_DoorKind (int sp)
{
    switch (sp)
    {
      case 26: case 27: case 28: case 32: case 33: case 34:
      case 99: case 133: case 134: case 135: case 136: case 137:
	return 2;						// needs a key
      case 1: case 2: case 3: case 4: case 16: case 29: case 31: case 42:
      case 46: case 50: case 61: case 63: case 75: case 76: case 86: case 90:
      case 103: case 105: case 106: case 107: case 108: case 109: case 110:
      case 111: case 112: case 113: case 114: case 115: case 116: case 117: case 118:
	return 1;						// a door
    }
    return 0;
}

static int AI_Serialize (void)
{
    player_t*	pl = &players[consoleplayer];
    int		bs = P_AICoop_Slot ();
    mobj_t*	bmo = (P_AICoop_AIMode () && bs >= 0 && playeringame[bs]) ? players[bs].mo : NULL;
    int		n = 0;
    int		i;
    int		fx = FRACUNIT;

    AI_BuildRegistry ();
    AI_OccReset ();

    if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, "{\"tic\":%d,\"player\":{", leveltime);
    if (pl->mo)
    {
	int reg = AI_REGION (pl->mo); AI_OccAdd (reg);
	if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n,
		"\"pos\":[%d,%d,%d],\"angle\":%u,\"health\":%d,\"armor\":%d,\"weapon\":%d,\"region\":%d}",
		pl->mo->x/fx, pl->mo->y/fx, pl->mo->z/fx,
		(unsigned)(((uint64_t)pl->mo->angle * 360u) >> 32),	// BAM -> degrees
		pl->health, pl->armorpoints, pl->readyweapon, reg);
    }
    else
	if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, "\"dead\":true}");

    // Buddy (-aicoop only): the director also commands the player's AI companion.
    {
	if (bmo)
	{
	    player_t*	b = &players[bs];
	    int		breg = AI_REGION (bmo); AI_OccAdd (breg);
	    int		w = b->readyweapon;
	    int		ammo = (w >= 0 && w < NUMWEAPONS && weaponinfo[w].ammo < NUMAMMO)
			       ? b->ammo[weaponinfo[w].ammo] : -1;
	    static const char* sname[] = {"follow","fight","heal","hold","come","grab"};
	    int		st = P_AICoop_State ();
	    fixed_t	rx[6], ry[6];
	    int		nr = P_AICoop_NavRoute (rx, ry, 6), r;
	    int dpl = pl->mo ? (int)(P_AproxDistance (b->mo->x - pl->mo->x,
						     b->mo->y - pl->mo->y) >> FRACBITS) : -1;
	    if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n,
		",\"buddy\":{\"pos\":[%d,%d],\"health\":%d,\"armor\":%d,"
		"\"weapon\":%d,\"ammo\":%d,\"state\":\"%s\",\"region\":%d,\"d_player\":%d,\"route\":[",
		b->mo->x/fx, b->mo->y/fx, b->health, b->armorpoints,
		w, ammo, (st>=0 && st<6) ? sname[st] : "follow", breg, dpl);
	    for (r = 0; r < nr; r++)
		if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, "%s[%d,%d]", r?",":"", rx[r]/fx, ry[r]/fx);
	    if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, "]}");
	}
    }

    if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, ",\"monsters\":[");
    for (i = 0; i < aient_count; i++)
    {
	mobj_t* m = aient[i].mo;
	int see   = (pl->mo && P_CheckSight (m, pl->mo)) ? 1 : 0;
	int seeb  = (bmo && P_CheckSight (m, bmo)) ? 1 : 0;
	int dpl   = pl->mo ? (int)(P_AproxDistance (m->x - pl->mo->x, m->y - pl->mo->y) >> FRACBITS) : -1;
	int dbud  = bmo    ? (int)(P_AproxDistance (m->x - bmo->x,    m->y - bmo->y)    >> FRACBITS) : -1;
	int reg   = AI_REGION (m); AI_OccAdd (reg);
	if (i)
	    if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, ",");
	if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n,
	    "{\"id\":%d,\"type\":\"%s\",\"pos\":[%d,%d],\"hp\":%d,\"region\":%d,"
	    "\"see_player\":%s,\"see_buddy\":%s,\"d_player\":%d,\"d_buddy\":%d,\"order\":\"%s\"}",
	    i+1, AI_TypeName(m->type), m->x/fx, m->y/fx, m->health, reg,
	    see ? "true" : "false", seeb ? "true" : "false", dpl, dbud,
	    AI_OrderName(aient[i].order));
	if (n > OBSBUF - 512)
	    break;		// buffer guard
    }
    if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, "],\"count\":%d", aient_count);

    // Room graph: lets the LLM reason about walls / flanking lanes / doors, not just
    // raw coordinates.  Expand the entity-occupied regions by one hop (so the
    // corridors/connectors *between* rooms appear), give each region a centroid the
    // LLM can navigate to, then list the connections:
    //   "regions":[[id,x,y],...]   "links":[[a,b,"open"|"door"|"locked"],...]
    {
	struct { int a, b, k; } pr[160];
	int      npr = 0, j, p, base = ai_nocc;
	int64_t  cx[AI_MAXOCC], cy[AI_MAXOCC]; int cnt[AI_MAXOCC];

	// one hop: pull in sectors directly adjacent to an entity-occupied one
	for (j = 0; j < numlines && ai_nocc < AI_MAXOCC; j++)
	{
	    line_t* ld = &lines[j];
	    int s1, s2;
	    if (!ld->backsector) continue;
	    s1 = (int)(ld->frontsector - sectors);
	    s2 = (int)(ld->backsector  - sectors);
	    if (AI_InOccN (s1, base) && !AI_InOcc (s2)) AI_OccAdd (s2);
	    else if (AI_InOccN (s2, base) && !AI_InOcc (s1)) AI_OccAdd (s1);
	}

	for (p = 0; p < ai_nocc; p++) { cx[p] = cy[p] = 0; cnt[p] = 0; }

	// single pass over lines: accumulate region centroids + collect links
	for (j = 0; j < numlines; j++)
	{
	    line_t* ld = &lines[j];
	    int s1, s2, i1, i2, k;
	    s1 = (int)(ld->frontsector - sectors); i1 = AI_OccIndex (s1);
	    if (i1 >= 0) { cx[i1] += (ld->v1->x + ld->v2->x) >> 1; cy[i1] += (ld->v1->y + ld->v2->y) >> 1; cnt[i1]++; }
	    if (!ld->backsector) continue;
	    s2 = (int)(ld->backsector - sectors); i2 = AI_OccIndex (s2);
	    if (i2 >= 0) { cx[i2] += (ld->v1->x + ld->v2->x) >> 1; cy[i2] += (ld->v1->y + ld->v2->y) >> 1; cnt[i2]++; }
	    if (i1 < 0 || i2 < 0 || s1 == s2) continue;
	    { int a = s1, b = s2; if (a > b) { int t = a; a = b; b = t; }
	      k = AI_DoorKind (ld->special);
	      for (p = 0; p < npr; p++) if (pr[p].a == a && pr[p].b == b) { if (k > pr[p].k) pr[p].k = k; break; }
	      if (p == npr && npr < 160) { pr[p].a = a; pr[p].b = b; pr[p].k = k; npr++; } }
	}

	if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, ",\"regions\":[");
	for (p = 0; p < ai_nocc; p++)
	    if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, "%s[%d,%d,%d]", p ? "," : "", ai_occ[p],
		cnt[p] ? (int)(cx[p]/cnt[p]/fx) : 0, cnt[p] ? (int)(cy[p]/cnt[p]/fx) : 0);
	if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, "],\"links\":[");
	for (p = 0; p < npr; p++)
	    if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, "%s[%d,%d,\"%s\"]", p ? "," : "",
		pr[p].a, pr[p].b, pr[p].k == 2 ? "locked" : pr[p].k == 1 ? "door" : "open");
	if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, "]");
    }

    // L4D director stress (so the LLM can pace spawns): intensity 0..100, FSM state,
    // recent burst damage, carried-ammo %.  See p_ai_director.c.
    if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n,
	",\"director\":{\"intensity\":%d,\"state\":%d,\"recent_dmg\":%d,\"ammo_pct\":%d}",
	P_Director_Intensity(), P_Director_State(), P_Director_RecentDmg(), P_Director_AmmoPct());

    if (n < OBSBUF) n += snprintf (obsbuf+n, OBSBUF-n, "}\n");
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
	{
	    int ord = AI_OrderByName(order_s);
	    AI_Apply (ord, ids, tx, ty, focus, fortics, after);
	    // The Director narrates the tactic it just ordered (ambient, rate-limited).
	    if      (ord == AIO_FLANK_L || ord == AIO_FLANK_R) P_Director_Say ("dir:flank", 2, 0);
	    else if (ord == AIO_AMBUSH)                        P_Director_Say ("dir:ambush", 2, 0);
	    else if (ord == AIO_FOCUS)                         P_Director_Say ("dir:focus", 2, 0);
	    else if (ord == AIO_FALLBACK)                      P_Director_Say ("dir:fallback", 2, 0);
	}
	if (client >= 0) (void)!write (client, "ok\n", 3);
	return;
    }
    // L4D director:  spawn type=<name> count=<n>  |  spawn item=<medkit|ammo>
    if (!strncmp(line, "spawn", 5))
    {
	char	type_s[32] = "imp", item_s[32] = "";
	int	count = 1;
	char*	kv;
	char	work[256];
	strncpy (work, line+5, sizeof(work)-1); work[sizeof(work)-1] = 0;
	for (kv = strtok(work, " \t\r\n"); kv; kv = strtok(NULL, " \t\r\n"))
	{
	    char* eq = strchr(kv, '='); if (!eq) continue; *eq++ = 0;
	    if      (!strcmp(kv,"type"))  { strncpy(type_s,eq,sizeof(type_s)-1); type_s[sizeof(type_s)-1]=0; }
	    else if (!strcmp(kv,"item"))  { strncpy(item_s,eq,sizeof(item_s)-1); item_s[sizeof(item_s)-1]=0; }
	    else if (!strcmp(kv,"count")) count = atoi(eq);
	}
	if (item_s[0]) P_Director_LLMItem (item_s);
	else           P_Director_LLMSpawn (type_s, count);
	if (client >= 0) (void)!write (client, "ok\n", 3);
	return;
    }
    // director relax  -- enter the calm/relax phase (stop spawning for a while)
    if (!strncmp(line, "director", 8))
    {
	if (strstr(line, "relax")) P_Director_Relax ();
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
// Rule-based coordinated tactics for the offline L4D director (-director, no LLM):
// flank / focus-fire / fall-back for the AWAKE monsters that are hunting the player,
// assigned by heuristics into the SAME directive side-table and executed by
// A_LLMChase.  Only touches monsters that already have a target -- idle objective
// guards (no target) are left alone to wait/ambush.  Deterministic (geometry).
// Called from the director ticker whenever the rule layer is in charge.
// ---------------------------------------------------------------------------
#define TAC_MELEE	(64*FRACUNIT)		// ~melee reach
#define TAC_FLANK_MIN	(256*FRACUNIT)		// don't peel point-blank monsters off to flank

void P_AI_RuleTactics (void)
{
    static int	replan;
    static char	los[AI_MAX];
    mobj_t*	pl;
    int		i, nseers = 0, flanked = 0, flank_budget, side = 0, did_flank = 0;

    if (!ai_inited) P_AI_Init ();
    ai_on = 1; ai_enabled = 1;		// arm the directive executor (A_LLMChase)

    if (gametic < replan) return;
    replan = gametic + 35;		// re-plan ~1/s

    pl = players[consoleplayer].mo;
    if (!pl || pl->health <= 0) return;

    AI_BuildRegistry ();			// rebuild ids (keeps still-live directives)

    // Pass 1: AWAKE monsters (have a target) with LOS to the player = the visible group.
    // Idle guards have no target -> los[i]=0 -> never directed, they keep waiting.
    for (i = 0; i < aient_count; i++)
    {
	mobj_t* m = aient[i].mo;
	los[i] = (m && m->health > 0 && (m->flags & MF_COUNTKILL) && m->target
		  && P_CheckSight (m, pl)) ? 1 : 0;
	if (los[i]) nseers++;
    }
    flank_budget = nseers / 3;		// ~1/3 of the visible group flanks (pincer)

    // Pass 2: order the awake hunters; leave idle guards / out-of-sight monsters alone.
    for (i = 0; i < aient_count; i++)
    {
	mobj_t*	m = aient[i].mo;
	int	order = AIO_NONE;
	fixed_t	d;
	int	wounded;

	if (!m || m->health <= 0 || !(m->flags & MF_COUNTKILL) || !m->target)
	    { aient[i].order = AIO_NONE; aient[i].for_tics = 0; continue; }	// idle guard / dead

	if (los[i])
	{
	    d       = P_AproxDistance (m->x - pl->x, m->y - pl->y);
	    wounded = (m->info->spawnhealth > 0 && m->health * 4 <= m->info->spawnhealth);
	    if (wounded && d > TAC_MELEE*2)
		order = AIO_FALLBACK;				// hurt & not cornered -> kite/retreat
	    else if (nseers >= 4 && flanked < flank_budget && d > TAC_FLANK_MIN)
		{ order = side ? AIO_FLANK_R : AIO_FLANK_L; side ^= 1; flanked++; did_flank = 1; }
	    else
		order = AIO_FOCUS;				// commit: press the player
	}

	if (order != AIO_NONE)
	{
	    aient[i].order      = order;
	    aient[i].focus_id   = 0;		// FOCUS -> the player
	    aient[i].for_tics   = 105;		// ~3 s, then auto-expires
	    aient[i].after_tics = 0;
	}
	else
	    { aient[i].order = AIO_NONE; aient[i].for_tics = 0; }	// awake but no LOS -> vanilla chase
    }

    if (did_flank) P_Director_Say ("dir:flank", 2, 0);	// narrate (rate-limited)
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

#ifndef _WIN32
    // A director that drops mid-write must not kill us with SIGPIPE (exit 141);
    // the write() to the dead socket should fail with EPIPE instead.
    signal (SIGPIPE, SIG_IGN);
#endif

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

// Service the director TCP socket regardless of gamestate.  Called every
// G_Ticker so the link survives the inter-map intermission/finale, when
// P_Ticker -- and thus P_AI_Ticker -- doesn't run.  Without this, `observe`
// goes unanswered during the tally screen, the external director times out
// (~5s) and drops the connection, with nothing left for it to reconnect to.
void P_AI_NetService (void)
{
    if (!ai_inited)
	P_AI_Init ();
    if (!ai_on || !ai_enabled)
	return;

    AI_PollSocket ();
}

void P_AI_Ticker (void)
{
    int i;

    if (!ai_inited)
	P_AI_Init ();
    if (!ai_on || !ai_enabled)
	return;

    // Socket is serviced by P_AI_NetService (every G_Ticker, all gamestates);
    // here we only run the playsim-tied parts (demo planner + directive timers).
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
