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

static int	aicoop;			// -aicoop given

#define COOP_SPEED	17		// move speed (u/tic) for P_Move; matches the
					// player's run terminal speed (~16.7 u/tic)
#define COOP_KEEP	(192*FRACUNIT)	// fight: hold this far from a monster
#define YIELD_DIST	(48*FRACUNIT)	// step aside when the human is this close
#define IDLE_TICS	105		// ~3 s: never stand still longer than this

// --- Behaviour knobs (loaded from aidoom.cfg via m_misc.c, edited by aicoop_config) ---
int	coop_defend_hp  = 35;		// below this HP: don't charge / kite
int	coop_heal_hp    = 20;		// below this HP: flee & hide until coop_defend_hp
int	coop_sight      = 1280;		// monster acquisition range (map units)
int	coop_follow     = 256;		// follow distance to the human (map units)
int	coop_heal_range = 1024;		// pickup search range (map units)
#define COOP_SIGHT	((fixed_t)coop_sight      * FRACUNIT)
#define COOP_NEAR	((fixed_t)coop_follow     * FRACUNIT)
#define COOP_ITEM_RANGE	((fixed_t)coop_heal_range * FRACUNIT)


void P_AICoop_Init (void)
{
    if (!M_CheckParm ("-aicoop"))
	return;
    aicoop = 1;
    playeringame[1] = true;		// spawn player 2 at the map's co-op start
    mobjinfo[MT_PLAYER].speed = COOP_SPEED;	// give P_Move something to work with
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

// Move toward a goal mobj using the monster pathing (walls + doors handled).
static void AICoop_Chase (mobj_t* mo, mobj_t* goal)
{
    mo->target = goal;
    if (--mo->movecount < 0 || !P_Move (mo))
	P_NewChaseDir (mo);
}

// Step away from (fx,fy): pick the 8-dir leading away, try it then turned dirs.
static void AICoop_MoveAway (mobj_t* mo, fixed_t fx, fixed_t fy)
{
    angle_t	a = R_PointToAngle2 (fx, fy, mo->x, mo->y);	// from threat -> us
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


void P_AICoop_BuildCmd (void)
{
    player_t*	bot;
    mobj_t*	mo;
    mobj_t*	mon;
    mobj_t*	pl;
    mobj_t*	item;
    ticcmd_t*	cmd;
    fixed_t	dist;
    static int	healtic, fleeing, idletic;
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

    // (2,5) yield: the human is right on top of us -> step aside (top priority)
    if (pl && P_AproxDistance (pl->x - mo->x, pl->y - mo->y) < YIELD_DIST)
    {
	AICoop_MoveAway (mo, pl->x, pl->y);
    }
    // (7) flee & hide: run from monsters, regroup on the player when clear
    else if (fleeing)
    {
	if (mon)
	{
	    AICoop_MoveAway (mo, mon->x, mon->y);
	    mo->angle = R_PointToAngle2 (mon->x, mon->y, mo->x, mo->y);
	}
	else if (pl)
	{
	    AICoop_Chase (mo, pl);
	    mo->angle = R_PointToAngle2 (mo->x, mo->y, pl->x, pl->y);
	}
    }
    // fight: aim and fire; charge when healthy, kite when low
    else if (mon)
    {
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
	    AICoop_Chase (mo, mon);					// close in
    }
    // (4) no monster in sight: fetch nearby pickups, else follow the human
    else if ((item = AICoop_FindItem (mo)) != NULL)
    {
	AICoop_Chase (mo, item);
	mo->angle = R_PointToAngle2 (mo->x, mo->y, item->x, item->y);
    }
    else if (pl)
    {
	if (P_AproxDistance (pl->x - mo->x, pl->y - mo->y) > COOP_NEAR)
	{
	    AICoop_Chase (mo, pl);
	    mo->angle = R_PointToAngle2 (mo->x, mo->y, pl->x, pl->y);
	}
    }

    // (3) never stand still > ~3 s: if we didn't move, shuffle sideways
    {
	fixed_t moved = P_AproxDistance (mo->x - lastx, mo->y - lasty);
	if (moved < 2*FRACUNIT) idletic++; else idletic = 0;
	if (idletic >= IDLE_TICS)
	{
	    mo->movedir = (mo->movedir + 2) & 7;	// perpendicular step
	    P_Move (mo);
	    idletic = 0;
	}
	lastx = mo->x; lasty = mo->y;
    }
}
