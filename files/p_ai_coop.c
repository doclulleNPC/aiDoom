// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	AI-controlled co-op companion (player 2).  Enabled with -aicoop.
//
//	Navigation reuses the engine's monster path logic (P_NewChaseDir / P_Move):
//	it steps around walls and opens doors by itself.  The companion stays a real
//	player (weapons, damage, pickups, reborn all work) -- movement is done on the
//	mobj via movedir, aiming via mo->angle, and firing via the ticcmd, so it can
//	navigate and shoot independently.
//
//	Behaviour:
//	  * auto-heals +1 HP/s up to 100
//	  * yields (steps aside) when the human bumps into it
//	  * below coop_heal_hp it flees monsters and hides until auto-heal brings it
//	    back to coop_defend_hp (hysteresis)
//	  * with monsters in sight: fights (charges when healthy, kites when low)
//	  * no monsters in sight: fetches nearby health / bonus / armor, else follows
//	  * never stands still longer than ~3 s
//
//	Single-machine only (cmd generated locally, never sent over the wire).
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "m_argv.h"
#include "p_local.h"
#include "p_mobj.h"
#include "info.h"
#include "r_main.h"
#include "tables.h"
#include "m_fixed.h"

#include "p_ai_coop.h"

// engine internals (p_enemy.c -- no public header)
extern void	P_NewChaseDir (mobj_t* actor);
extern boolean	P_Move (mobj_t* actor);
extern boolean	P_SetMobjState (mobj_t* mobj, statenum_t state);

static int	aicoop;			// -aicoop given
static int	coop_state;		// what we're doing (for the console "where")
static int	summon;			// >0: "come" was ordered -- run to the player
static int	wantmove;		// set when we actually tried to move this tic
static int	hold;			// "wait/stay" -- hold position
static int	forceaggro;		// >0: "attack" ordered -- charge forcetarget
static mobj_t*	forcetarget;		// the forced attack target
static int	doorwait;		// >0: waiting for a door we opened to finish

// ---- grid A* pathfinding (no LLM) -----------------------------------------
#define PF_CELL		64		// nav-cell size in map units
#define PF_MAXEXP	4000		// cap A* expansions (cost bound)
#define PF_LOOKAHEAD	4		// waypoint this many cells ahead
static byte*	pf_walk;		// 1 = the companion fits in this cell
static byte*	pf_edge;		// per cell: 8-bit "edge open" mask (traversable)
static fixed_t*	pf_floor;		// per cell: sector floor height (for step checks)
static byte*	pf_state;		// A* per-node: 0 none, 1 open, 2 closed
static int*	pf_g;			// A* cost-so-far
static int*	pf_came;		// A* predecessor cell
static int*	pf_olist;		// A* open set (cell indices)
static int	pf_w, pf_h;		// grid dimensions
static fixed_t	pf_orgx, pf_orgy;	// grid origin (map units, fixed)
static int	pf_level = -1;		// episode*100+map the grid was built for
static fixed_t	pf_wpx, pf_wpy;		// current waypoint
static mobj_t*	pf_goal;		// goal the waypoint was computed for
static int	pf_timer;		// tics until the next re-path
// coop_state codes (index into the report text): 0 hold 1 follow 2 fight 3 flee
//                                                4 items 5 yield

#define COOP_KEEP	(192*FRACUNIT)	// fight: hold this far from a monster
#define YIELD_DIST	(48*FRACUNIT)	// step aside when the human is this close
#define IDLE_TICS	105		// ~3 s: don't stay stuck longer than this

// --- Behaviour knobs (loaded from aidoom.cfg via m_misc.c, edited by aicoop_config) ---
int	coop_defend_hp  = 35;		// below this HP: don't charge / kite
int	coop_heal_hp    = 20;		// below this HP: flee & hide until coop_defend_hp
int	coop_sight      = 1280;		// monster acquisition range (map units)
int	coop_follow     = 256;		// follow distance to the human (map units)
int	coop_heal_range = 1024;		// pickup search range (map units)
int	coop_speed      = 13;		// step size per tic for P_Move (player run ~= 16-17)
#define COOP_SIGHT	((fixed_t)coop_sight      * FRACUNIT)
#define COOP_NEAR	((fixed_t)coop_follow     * FRACUNIT)
#define COOP_ITEM_RANGE	((fixed_t)coop_heal_range * FRACUNIT)


void P_AICoop_Init (void)
{
    if (!M_CheckParm ("-aicoop"))
	return;
    aicoop = 1;
    playeringame[1] = true;		// spawn player 2 at the map's co-op start
    if (coop_speed < 4)  coop_speed = 4;
    if (coop_speed > 30) coop_speed = 30;
    mobjinfo[MT_PLAYER].speed = coop_speed;	// step size for P_Move (tunable)
    printf ("P_AICoop: AI co-op companion (player 2) enabled\n");
}


// Nearest live, shootable, visible monster within range (never the human).
static mobj_t* AICoop_FindTarget (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m; fixed_t d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (m == self || m->type == MT_PLAYER)	continue;
	if (!(m->flags & MF_COUNTKILL))		continue;	// monsters only (skip barrels)
	if (!(m->flags & MF_SHOOTABLE))		continue;
	if (m->flags & MF_CORPSE || m->health <= 0) continue;
	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > COOP_SIGHT)			continue;
	if (!P_CheckSight (self, m))		continue;
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}

// Nearest pickup worth grabbing: health, health/armor bonuses, armor.
static mobj_t* AICoop_FindItem (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m; fixed_t d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (!(m->flags & MF_SPECIAL))	continue;	// a pickup still in the world
	switch (m->sprite)
	{
	  case SPR_STIM: case SPR_MEDI: case SPR_SOUL: case SPR_MEGA:	// health
	  case SPR_BON1: case SPR_BON2:					// health/armor bonus
	  case SPR_ARM1: case SPR_ARM2:					// armor
	    break;
	  default:
	    continue;
	}
	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > COOP_ITEM_RANGE)	continue;
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}

// Nearest live shootable monster to a point (used by the "attack" order).
static mobj_t* AICoop_NearestMonsterTo (fixed_t x, fixed_t y)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m; fixed_t d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (m->type == MT_PLAYER)		continue;
	if (!(m->flags & MF_COUNTKILL))		continue;
	if (!(m->flags & MF_SHOOTABLE))		continue;
	if (m->flags & MF_CORPSE || m->health <= 0) continue;
	d = P_AproxDistance (m->x - x, m->y - y);
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}

// Step away from (fx,fy): pick the 8-dir leading away, try it then turned dirs.
static void AICoop_MoveAway (mobj_t* mo, fixed_t fx, fixed_t fy)
{
    angle_t	a = R_PointToAngle2 (fx, fy, mo->x, mo->y);	// from threat -> us
    wantmove = 1;
    int		base = (int)((a + (1u<<28)) >> 29) & 7;		// nearest of 8 dirs
    static const int off[5] = { 0, 1, 7, 2, 6 };		// straight, +-45, +-90
    int		i;
    for (i = 0; i < 5; i++)
    {
	mo->movedir = (base + off[i]) & 7;
	if (P_Move (mo)) return;
    }
    mo->movedir = 8;		// DI_NODIR (p_enemy.c) -- fully blocked, no move
}


// Step toward a point with the engine mover (handles fine collision + doors).
static void AICoop_MoveToPoint (mobj_t* mo, fixed_t tx, fixed_t ty)
{
    static const int	off[5] = { 0, 1, 7, 2, 6 };	// straight, +-45, +-90
    angle_t		a;
    int			base, i;

    // While a door we opened is still moving, hold -- re-triggering it would
    // reverse it (the companion is a real player), which is the "use-loop stuck".
    if (doorwait > 0) { doorwait--; return; }

    a = R_PointToAngle2 (mo->x, mo->y, tx, ty);
    base = (int)((a + (1u<<28)) >> 29) & 7;
    wantmove = 1;
    for (i = 0; i < 5; i++)
    {
	fixed_t ox = mo->x, oy = mo->y;
	mo->movedir = (base + off[i]) & 7;
	if (P_Move (mo))
	{
	    // P_Move returns true but the mobj didn't move => it opened a door (or
	    // used a line).  Wait for it to finish instead of pressing use again.
	    if (mo->x == ox && mo->y == oy)
		doorwait = 35;
	    return;
	}
    }
    mo->movedir = 8;
}

// Build the walkability grid for the current level by probing where the
// companion fits (P_CheckPosition + head-room).  Cheap, done once per level.
static void PF_Build (mobj_t* probe)
{
    int	i, j, n;
    pf_orgx = bmaporgx;
    pf_orgy = bmaporgy;
    pf_w = bmapwidth  * (128 / PF_CELL);
    pf_h = bmapheight * (128 / PF_CELL);
    if (pf_w < 1) pf_w = 1;
    if (pf_h < 1) pf_h = 1;
    n = pf_w * pf_h;

    free (pf_walk); free (pf_edge); free (pf_floor);
    free (pf_state); free (pf_g); free (pf_came); free (pf_olist);
    pf_walk  = malloc (n);
    pf_edge  = malloc (n);
    pf_floor = malloc (n * sizeof(fixed_t));
    pf_state = malloc (n);
    pf_g     = malloc (n * sizeof(int));
    pf_came  = malloc (n * sizeof(int));
    pf_olist = malloc (n * sizeof(int));

    // 1) which cells the marine fits in (centre probe + head-room), and the
    //    floor height there (for step-up checks below).
    for (j = 0; j < pf_h; j++)
	for (i = 0; i < pf_w; i++)
	{
	    fixed_t cx = pf_orgx + (i*PF_CELL + PF_CELL/2)*FRACUNIT;
	    fixed_t cy = pf_orgy + (j*PF_CELL + PF_CELL/2)*FRACUNIT;
	    byte    w  = 0;
	    pf_floor[j*pf_w + i] = R_PointInSubsector (cx, cy)->sector->floorheight;
	    if (P_CheckPosition (probe, cx, cy) && (tmceilingz - tmfloorz >= 56*FRACUNIT))
		w = 1;
	    pf_walk[j*pf_w + i] = w;
	}

    // 2) which edges are actually traversable.  A wall/window between two
    // fits-here cells is caught by probing the cell *boundary* (the midpoint
    // between centres): a blocking line there makes the marine not fit, so the
    // edge is closed.  This stops A* routing through walls/windows.
    {
	static const int dx[8] = { 1,1,0,-1,-1,-1,0,1 };
	static const int dy[8] = { 0,1,1,1,0,-1,-1,-1 };
	for (j = 0; j < pf_h; j++)
	  for (i = 0; i < pf_w; i++)
	  {
	    int  c = j*pf_w + i, d;
	    byte e = 0;
	    if (pf_walk[c])
	      for (d = 0; d < 8; d++)
	      {
		int ni = i+dx[d], nj = j+dy[d];
		if (ni<0 || nj<0 || ni>=pf_w || nj>=pf_h) continue;
		if (!pf_walk[nj*pf_w+ni]) continue;
		if (d & 1)				// diagonal: don't clip a corner
		    if (!pf_walk[j*pf_w+ni] || !pf_walk[nj*pf_w+i]) continue;
		{
		    fixed_t mx = pf_orgx + (i*PF_CELL + PF_CELL/2 + dx[d]*PF_CELL/2)*FRACUNIT;
		    fixed_t my = pf_orgy + (j*PF_CELL + PF_CELL/2 + dy[d]*PF_CELL/2)*FRACUNIT;
		    // edge open only if the marine fits at the boundary, has head-room,
		    // and the step up from this cell is climbable (<=24) -- so it won't
		    // try to scale a too-high ledge or hop a window sill.
		    if (P_CheckPosition (probe, mx, my)
			&& (tmceilingz - tmfloorz >= 56*FRACUNIT)
			&& (tmfloorz - pf_floor[c] <= 24*FRACUNIT))
			e |= (1 << d);
		}
	      }
	    pf_edge[c] = e;
	  }
    }
}

static void PF_Ensure (mobj_t* probe)
{
    int lvl = gameepisode*100 + gamemap;
    if (lvl != pf_level || !pf_walk)
    {
	PF_Build (probe);
	pf_level = lvl;
	pf_goal = NULL;
    }
}

static int PF_Cell (fixed_t x, fixed_t y)
{
    int i = ((x - pf_orgx) >> FRACBITS) / PF_CELL;
    int j = ((y - pf_orgy) >> FRACBITS) / PF_CELL;
    if (i < 0 || j < 0 || i >= pf_w || j >= pf_h) return -1;
    return j*pf_w + i;
}

static int PF_Heur (int a, int b)		// octile distance * 10
{
    int dx = abs (a%pf_w - b%pf_w);
    int dy = abs (a/pf_w - b/pf_w);
    return 10*(dx+dy) - 6*(dx < dy ? dx : dy);
}

// A* from (sx,sy) to (dx,dy); set *outx/*outy to a waypoint a few cells ahead.
// Returns 1 if a route was found, 0 otherwise (caller falls back to direct).
static int PF_Next (mobj_t* mo, fixed_t dxf, fixed_t dyf, fixed_t* outx, fixed_t* outy)
{
    int		start, dest, n, on, exp, cur, k;
    static const int dirx[8] = { 1,1,0,-1,-1,-1,0,1 };
    static const int diry[8] = { 0,1,1,1,0,-1,-1,-1 };

    if (!pf_walk) return 0;
    start = PF_Cell (mo->x, mo->y);
    dest  = PF_Cell (dxf, dyf);
    if (start < 0 || dest < 0 || start == dest) return 0;

    n = pf_w * pf_h;
    if (!pf_walk[dest])			// snap goal to a nearby walkable cell
    {
	int di = dest%pf_w, dj = dest/pf_w, r, found = -1;
	for (r = 1; r <= 4 && found < 0; r++)
	{
	    int oi, oj;
	    for (oj = -r; oj <= r && found < 0; oj++)
	      for (oi = -r; oi <= r; oi++)
	      {
		int ii = di+oi, jj = dj+oj;
		if (ii>=0 && jj>=0 && ii<pf_w && jj<pf_h && pf_walk[jj*pf_w+ii])
		    { found = jj*pf_w+ii; break; }
	      }
	}
	if (found < 0) return 0;
	dest = found;
    }
    if (!pf_walk[start]) return 0;

    for (k = 0; k < n; k++) { pf_state[k] = 0; pf_g[k] = 0x7fffffff; }
    pf_g[start] = 0; pf_state[start] = 1;
    pf_olist[0] = start; on = 1; exp = 0;

    while (on > 0 && exp < PF_MAXEXP)
    {
	int bi = 0, bf = 0x7fffffff, d;
	for (k = 0; k < on; k++)			// pop lowest f
	{
	    int f = pf_g[pf_olist[k]] + PF_Heur (pf_olist[k], dest);
	    if (f < bf) { bf = f; bi = k; }
	}
	cur = pf_olist[bi];
	if (cur == dest) break;
	pf_olist[bi] = pf_olist[--on];
	pf_state[cur] = 2;
	exp++;

	for (d = 0; d < 8; d++)
	{
	    int ci = cur%pf_w + dirx[d], cj = cur/pf_w + diry[d], nb, ng;
	    if (!(pf_edge[cur] & (1 << d))) continue;	// edge blocked (wall/window)
	    nb = cj*pf_w + ci;
	    if (pf_state[nb] == 2) continue;
	    ng = pf_g[cur] + ((d & 1) ? 14 : 10);
	    if (ng < pf_g[nb])
	    {
		pf_g[nb] = ng; pf_came[nb] = cur;
		if (pf_state[nb] != 1) { pf_state[nb] = 1; pf_olist[on++] = nb; }
	    }
	}
    }

    if (cur != dest) return 0;				// no route

    // walk back to a cell PF_LOOKAHEAD steps from start
    {
	int path[PF_MAXEXP], len = 0, c = dest, wp;
	while (c != start && len < PF_MAXEXP) { path[len++] = c; c = pf_came[c]; }
	if (len == 0) return 0;
	wp = path[len > PF_LOOKAHEAD ? len - PF_LOOKAHEAD : 0];	// reversed: index from start side
	*outx = pf_orgx + ((wp%pf_w)*PF_CELL + PF_CELL/2)*FRACUNIT;
	*outy = pf_orgy + ((wp/pf_w)*PF_CELL + PF_CELL/2)*FRACUNIT;
	return 1;
    }
}

// Route to a goal mobj: A* waypoints (global) + P_Move stepping (local).  Falls
// back to the plain chase if no route is found.
static void AICoop_PathChase (mobj_t* mo, mobj_t* goal)
{
    PF_Ensure (mo);
    if (goal != pf_goal || --pf_timer <= 0
	|| P_AproxDistance (mo->x - pf_wpx, mo->y - pf_wpy) < PF_CELL*FRACUNIT)
    {
	pf_goal = goal; pf_timer = 12;
	if (!PF_Next (mo, goal->x, goal->y, &pf_wpx, &pf_wpy))
	{
	    pf_goal = NULL;			// no route -> retry next tic
	    AICoop_MoveToPoint (mo, goal->x, goal->y);	// head straight (also opens doors)
	    return;
	}
    }
    AICoop_MoveToPoint (mo, pf_wpx, pf_wpy);
}

void P_AICoop_BuildCmd (void)
{
    player_t*	bot;
    mobj_t*	mo;
    mobj_t*	mon;
    mobj_t*	pl;
    mobj_t*	item;
    ticcmd_t*	cmd;
    fixed_t	dist;
    fixed_t	ml;			// distance moved during the previous tic
    static int	healtic, fleeing, idletic, curspeed;
    static fixed_t lastx, lasty;

    if (!aicoop || !playeringame[1])
	return;

    bot = &players[1];
    cmd = &bot->cmd;

    // Dead: tap Use so co-op reborns the companion at its start.
    if (bot->playerstate == PST_DEAD)
    {
	memset (cmd, 0, sizeof(*cmd));
	cmd->buttons |= BT_USE;
	return;
    }
    if (bot->playerstate != PST_LIVE || !bot->mo)
	return;

    mo = bot->mo;
    memset (cmd, 0, sizeof(*cmd));

    // (6) auto-heal: +1 HP per second up to 100
    if (++healtic >= 35)
    {
	healtic = 0;
	if (bot->health < 100) { bot->health++; mo->health = bot->health; }
    }

    pl  = playeringame[0] ? players[0].mo : NULL;
    mon = AICoop_FindTarget (mo);

    // (7) flee hysteresis: enter below heal_hp, leave once back to defend_hp
    if (bot->health < coop_heal_hp)            fleeing = 1;
    else if (bot->health >= coop_defend_hp)    fleeing = 0;

    coop_state = 0;
    wantmove   = 0;

    // Acceleration ramp: measure how far we moved last tic; if we kept moving,
    // wind P_Move's step size up toward coop_speed, otherwise start slow.  This
    // gives a gradual run-up instead of snapping to full speed in one tic.
    ml = P_AproxDistance (mo->x - lastx, mo->y - lasty);
    lastx = mo->x; lasty = mo->y;
    if (ml > FRACUNIT) { curspeed += 3; if (curspeed > coop_speed) curspeed = coop_speed; }
    else                 curspeed = coop_speed / 3;
    if (curspeed < 4) curspeed = 4;
    mobjinfo[MT_PLAYER].speed = curspeed;

    // (2,5) yield: the human is right on top of us -> step aside (top priority)
    if (pl && P_AproxDistance (pl->x - mo->x, pl->y - mo->y) < YIELD_DIST)
    {
	AICoop_MoveAway (mo, pl->x, pl->y);
	coop_state = 5;
    }
    // "attack" ordered: charge the forced target until it (or the timer) dies
    else if (forceaggro > 0)
    {
	mobj_t* t = forcetarget;
	forceaggro--;
	if (!t || t->health <= 0 || (t->flags & MF_CORPSE) || !(t->flags & MF_SHOOTABLE))
	    t = forcetarget = AICoop_FindTarget (mo);	// target down -> reacquire
	if (t)
	{
	    coop_state = 2;
	    dist = P_AproxDistance (t->x - mo->x, t->y - mo->y);
	    mo->angle = R_PointToAngle2 (mo->x, mo->y, t->x, t->y);
	    if (P_CheckSight (mo, t))
		cmd->buttons |= BT_ATTACK;
	    if (dist > COOP_KEEP)
		AICoop_PathChase (mo, t);			// always close in (forced)
	}
	else
	    forceaggro = 0;				// nothing left to attack
    }
    // "wait/stay" ordered: hold position; still face & fire at anything in sight
    else if (hold)
    {
	coop_state = 0;
	if (mon)
	{
	    coop_state = 2;
	    mo->angle = R_PointToAngle2 (mo->x, mo->y, mon->x, mon->y);
	    if (P_CheckSight (mo, mon))
		cmd->buttons |= BT_ATTACK;
	}
    }
    // (7) flee & hide: run from monsters, regroup on the player when clear
    else if (fleeing)
    {
	coop_state = 3;
	if (mon)
	{
	    AICoop_MoveAway (mo, mon->x, mon->y);
	    mo->angle = R_PointToAngle2 (mon->x, mon->y, mo->x, mo->y);
	}
	else if (pl)
	{
	    AICoop_PathChase (mo, pl);
	    mo->angle = R_PointToAngle2 (mo->x, mo->y, pl->x, pl->y);
	}
    }
    // "come" ordered from the console: run to the player, ignoring fights/items
    else if (summon > 0 && pl)
    {
	summon--;
	AICoop_PathChase (mo, pl);
	mo->angle = R_PointToAngle2 (mo->x, mo->y, pl->x, pl->y);
	coop_state = 1;
    }
    // fight: aim and fire; charge when healthy, kite when low
    else if (mon)
    {
	coop_state = 2;
	int defensive = (bot->health < coop_defend_hp);
	dist = P_AproxDistance (mon->x - mo->x, mon->y - mo->y);
	mo->angle = R_PointToAngle2 (mo->x, mo->y, mon->x, mon->y);
	if (P_CheckSight (mo, mon))
	    cmd->buttons |= BT_ATTACK;
	if (defensive)
	{
	    if (dist < COOP_KEEP) AICoop_MoveAway (mo, mon->x, mon->y);	// back off
	}
	else if (dist > COOP_KEEP)
	    AICoop_PathChase (mo, mon);					// close in
    }
    // (4) no monster in sight: fetch nearby pickups, else follow the human
    else if ((item = AICoop_FindItem (mo)) != NULL)
    {
	AICoop_PathChase (mo, item);
	mo->angle = R_PointToAngle2 (mo->x, mo->y, item->x, item->y);
	coop_state = 4;
    }
    else if (pl)
    {
	coop_state = 1;
	if (P_AproxDistance (pl->x - mo->x, pl->y - mo->y) > COOP_NEAR)
	{
	    AICoop_PathChase (mo, pl);
	    mo->angle = R_PointToAngle2 (mo->x, mo->y, pl->x, pl->y);
	}
    }

    // Walk animation: P_Move drives the mobj directly (forwardmove stays 0), so
    // P_MovePlayer never starts the run frames -- do it here.  Only touch the
    // locomotion states so we don't stomp on attack/pain/death animations.
    {
	statenum_t st = (statenum_t)(mo->state - states);
	if (st == S_PLAY || (st >= S_PLAY_RUN1 && st <= S_PLAY_RUN4))
	{
	    if (wantmove)
	    {
		if (st == S_PLAY) P_SetMobjState (mo, S_PLAY_RUN1);
	    }
	    else if (st != S_PLAY)
		P_SetMobjState (mo, S_PLAY);
	}
    }

    // (3) Anti-stuck only: trying to move but no progress for ~3 s -> shuffle.
    // When there's nothing to do (no goal) we just stand calmly.
    if (wantmove && ml < 2*FRACUNIT) idletic++; else idletic = 0;
    if (idletic >= IDLE_TICS)
    {
	mo->movedir = (mo->movedir + 2) & 7;	// perpendicular step
	P_Move (mo);
	idletic = 0;
    }
}


// Console "where": a one-line spoken-style status answer.
const char* P_AICoop_Report (void)
{
    static char		buf[160];
    static const char*	dirs[8]  = { "east","north-east","north","north-west",
					"west","south-west","south","south-east" };
    static const char*	doing[6] = { "holding position","right behind you","fighting",
					"falling back (low HP)","grabbing supplies",
					"stepping out of your way" };
    player_t*	bot = &players[1];
    mobj_t*	me;
    mobj_t*	pl;
    int		dist = 0, di = 0, st;

    if (!aicoop || !playeringame[1] || bot->playerstate == PST_DEAD || !bot->mo)
    {
	snprintf (buf, sizeof(buf), "[Buddy] (no companion -- launch with -aicoop)");
	return buf;
    }
    me = bot->mo;
    pl = playeringame[0] ? players[0].mo : NULL;
    if (pl)
    {
	dist = P_AproxDistance (me->x - pl->x, me->y - pl->y) >> FRACBITS;
	di   = (int)((R_PointToAngle2 (pl->x, pl->y, me->x, me->y) + (1u<<28)) >> 29) & 7;
    }
    st = (coop_state >= 0 && coop_state <= 5) ? coop_state : 0;
    snprintf (buf, sizeof(buf), "[Buddy] %d units to your %s, %d HP -- %s.",
	      dist, dirs[di], bot->health, doing[st]);
    return buf;
}


// Console "come": order the companion to run to the player.  0 if none.
int P_AICoop_Summon (void)
{
    if (!aicoop || !playeringame[1] || players[1].playerstate == PST_DEAD || !players[1].mo)
	return 0;
    summon = 245;		// ~7 s of coming to the player
    hold = 0;
    return 1;
}

#define COOP_NONE	"[Buddy] (no companion -- launch with -aicoop)"
static int AICoop_Here (void)
{
    return aicoop && playeringame[1]
	&& players[1].playerstate != PST_DEAD && players[1].mo;
}

// Console "wait" / "stay": toggle holding position.
const char* P_AICoop_Wait (void)
{
    if (!AICoop_Here ()) return COOP_NONE;
    hold = !hold;
    forceaggro = 0; summon = 0;		// holding cancels other orders
    return hold ? "[Buddy] Holding position." : "[Buddy] Moving out.";
}

// Console "attack": charge the monster nearest the player.
const char* P_AICoop_Attack (void)
{
    mobj_t* pl;
    if (!AICoop_Here ()) return COOP_NONE;
    pl = playeringame[0] ? players[0].mo : players[1].mo;
    forcetarget = AICoop_NearestMonsterTo (pl->x, pl->y);
    if (!forcetarget) return "[Buddy] No targets around.";
    forceaggro = 350;			// ~10 s
    hold = 0;
    return "[Buddy] Attacking!";
}

// Console "report": ammo / status line.
const char* P_AICoop_StatusReport (void)
{
    static char		buf[160];
    static const char*	wn[NUMWEAPONS] = { "fists","pistol","shotgun","chaingun",
	"rocket launcher","plasma rifle","BFG9000","chainsaw","super shotgun" };
    player_t*	bot = &players[1];
    int		w, at;
    char	ammo[40];

    if (!AICoop_Here ()) return COOP_NONE;

    w = bot->readyweapon;
    switch (w)
    {
      case wp_pistol: case wp_chaingun:      at = am_clip;   break;
      case wp_shotgun: case wp_supershotgun: at = am_shell;  break;
      case wp_missile:                       at = am_misl;   break;
      case wp_plasma: case wp_bfg:           at = am_cell;   break;
      default:                               at = am_noammo; break;
    }
    if (at == am_noammo) snprintf (ammo, sizeof(ammo), "no ammo");
    else                 snprintf (ammo, sizeof(ammo), "%d rounds", bot->ammo[at]);

    if (w < 0 || w >= NUMWEAPONS) w = 0;
    snprintf (buf, sizeof(buf), "[Buddy] %d HP, %d%% armor, %s, %s.",
	      bot->health, bot->armorpoints, wn[w], ammo);
    return buf;
}
