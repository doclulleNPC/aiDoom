// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	AI-controlled co-op companion (player 2).  Enabled with -aicoop, which
//	marks playeringame[1] so the engine spawns a second marine at the map's
//	co-op start.  Each tic this builds that player's ticcmd with a small
//	built-in brain: acquire the nearest visible monster, turn to it and fire;
//	when hurt grab health; when idle collect nearby items; otherwise follow
//	the human.  It drives the player through the normal ticcmd path, so
//	weapons, damage, pickups and reborn all work like a real co-op peer.
//
//	Console commands (c_console.c) let the human direct it: where / come /
//	wait / attack / report.
//
//	Single-machine only (no real netgame): the cmd is generated locally and
//	never carried over the wire.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_items.h"
#include "m_argv.h"
#include "p_local.h"
#include "p_mobj.h"
#include "info.h"
#include "r_main.h"
#include "m_fixed.h"

#include "p_ai_coop.h"

static int	aicoop;			// -aicoop given
static int	coop_state;		// 0 follow 1 fight 2 heal 3 hold 4 come 5 items
static int	summon;			// "come": tics left running to the player
static int	hold;			// "wait/stay": hold position
static int	forceaggro;		// "attack": tics left charging forcetarget
static mobj_t*	forcetarget;		// the forced attack target

#define COOP_SIGHT	(1280*FRACUNIT)	// monster acquisition range
#define COOP_TURN	1300		// max angleturn per tic (~7 deg)
#define COOP_FACING	1500		// |remaining turn| under which we open fire
#define COOP_NEAR	(256*FRACUNIT)	// follow distance to the human
#define COOP_KEEP	(192*FRACUNIT)	// advance toward a monster until this close
#define COOP_RUN	0x32		// forwardmove "run" magnitude
#define COOP_HEAL_HP	50		// seek a med-pack below this health
#define COOP_HEAL_RANGE	(1024*FRACUNIT)	// how far to look for one
#define COOP_ITEM_RANGE	(1024*FRACUNIT)	// how far to look for an idle pickup
#define COOP_SUMMON_TICS (7*TICRATE)	// "come" runs to you for this long
#define COOP_ATTACK_TICS (10*TICRATE)	// "attack" charges the target for this long


void P_AICoop_Init (void)
{
    if (!M_CheckParm ("-aicoop"))
	return;
    aicoop = 1;
    playeringame[1] = true;		// spawn player 2 at the map's co-op start
    printf ("P_AICoop: AI co-op companion (player 2) enabled\n");
}


// The live companion mobj, or NULL if there isn't one right now.
static mobj_t* AICoop_Mo (void)
{
    if (!aicoop || !playeringame[1])		return NULL;
    if (players[1].playerstate != PST_LIVE)	return NULL;
    return players[1].mo;
}


//
// AICoop_FindTarget
// Nearest live, shootable, visible monster within range (never the human).
//
static mobj_t* AICoop_FindTarget (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m;
	fixed_t	d;

	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;

	if (m == self)			continue;
	if (m->type == MT_PLAYER)	continue;	// never target a human
	if (!(m->flags & MF_COUNTKILL))	continue;	// monsters only (skip barrels etc.)
	if (!(m->flags & MF_SHOOTABLE))	continue;
	if (m->flags & MF_CORPSE)	continue;
	if (m->health <= 0)		continue;

	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > COOP_SIGHT)		continue;
	if (!P_CheckSight (self, m))	continue;

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
	mobj_t*	m;
	fixed_t	d;

	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;

	if (m->type == MT_PLAYER)	continue;
	if (!(m->flags & MF_COUNTKILL))	continue;
	if (!(m->flags & MF_SHOOTABLE))	continue;
	if (m->flags & MF_CORPSE)	continue;
	if (m->health <= 0)		continue;

	d = P_AproxDistance (m->x - x, m->y - y);
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}


//
// AICoop_CanReach
// Can the companion walk in a straight line from its feet to a pickup?  We
// march the segment in ~24-unit steps and at each point require that the marine
// fits (P_CheckPosition: no wall/obstacle, head-room) and that the floor never
// rises more than a 24-unit step.  Rejects items behind a wall or up a ledge so
// the bot doesn't run face-first into geometry trying to fetch them.
//
static boolean AICoop_CanReach (mobj_t* self, mobj_t* it)
{
    fixed_t	dx = it->x - self->x;
    fixed_t	dy = it->y - self->y;
    fixed_t	dist = P_AproxDistance (dx, dy);
    fixed_t	fz = self->z;			// start at the buddy's feet
    int		steps, i;

    if (dist < 24*FRACUNIT)
	return true;				// practically on it
    steps = dist / (24*FRACUNIT);
    if (steps > 64)
	return false;				// too far -- don't bother (bounds cost)

    for (i = 1; i <= steps; i++)
    {
	fixed_t	frac = (i << 16) / steps;	// i/steps as 16.16
	fixed_t	px   = self->x + FixedMul (dx, frac);
	fixed_t	py   = self->y + FixedMul (dy, frac);

	if (!P_CheckPosition (self, px, py))		return false;	// wall/obstacle
	if (tmceilingz - tmfloorz < 56*FRACUNIT)	return false;	// won't fit
	if (tmfloorz - fz > 24*FRACUNIT)		return false;	// step up too high
	fz = tmfloorz;
    }
    return true;
}


//
// AICoop_FindHealth
// Nearest health pickup still lying in the world (stimpack/medikit/soul/mega).
//
static mobj_t* AICoop_FindHealth (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m;
	fixed_t	d;

	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;

	if (!(m->flags & MF_SPECIAL))	continue;	// not a pickup (or already taken)
	switch (m->sprite)
	{
	  case SPR_STIM: case SPR_MEDI:
	  case SPR_SOUL: case SPR_MEGA:
	    break;
	  default:
	    continue;
	}

	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > COOP_HEAL_RANGE)	continue;
	if (best && d >= bestd)		continue;	// not closer -> skip the trace
	if (!AICoop_CanReach (self, m))	continue;	// can't actually walk there

	best = m; bestd = d;
    }
    return best;
}


//
// AICoop_FindItem
// Nearest worth-grabbing pickup: health, bonuses, armor, ammo, weapons,
// backpack.  Deliberately skips keys (the human may need them in co-op).
//
static mobj_t* AICoop_FindItem (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m;
	fixed_t	d;

	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;

	if (!(m->flags & MF_SPECIAL))	continue;	// a pickup still in the world
	switch (m->sprite)
	{
	  case SPR_STIM: case SPR_MEDI: case SPR_SOUL: case SPR_MEGA:	// health
	  case SPR_BON1: case SPR_BON2:					// bonuses
	  case SPR_ARM1: case SPR_ARM2:					// armor
	  case SPR_CLIP: case SPR_AMMO: case SPR_SHEL: case SPR_SBOX:	// ammo
	  case SPR_ROCK: case SPR_BROK: case SPR_CELL: case SPR_CELP:
	  case SPR_BPAK:						// backpack
	  case SPR_SHOT: case SPR_SGN2: case SPR_MGUN: case SPR_LAUN:	// weapons
	  case SPR_PLAS: case SPR_BFUG: case SPR_CSAW:
	    break;
	  default:
	    continue;						// keys & everything else
	}

	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > COOP_ITEM_RANGE)	continue;
	if (best && d >= bestd)		continue;	// not closer -> skip the trace
	if (!AICoop_CanReach (self, m))	continue;	// can't actually walk there

	best = m; bestd = d;
    }
    return best;
}


// ----------------------------------------------------------------- commands
// Replies are short strings the console prints; they start with "[Buddy] ".

const char* P_AICoop_Report (void)
{
    static char		buf[120];
    static const char*	what[]    = { "following you", "fighting", "getting health",
				      "holding position", "coming to you", "grabbing an item" };
    static const char*	compass[8] = { "east","north-east","north","north-west",
				       "west","south-west","south","south-east" };
    mobj_t*	mo = AICoop_Mo ();
    mobj_t*	pl = playeringame[0] ? players[0].mo : NULL;

    if (!mo)
	return "[Buddy] (no companion -- launch with -aicoop)";

    if (pl)
    {
	int	units = (int)(P_AproxDistance (mo->x - pl->x, mo->y - pl->y) >> FRACBITS);
	angle_t	a     = R_PointToAngle2 (pl->x, pl->y, mo->x, mo->y);
	int	oct   = (int)((a + (1u<<28)) >> 29) & 7;
	snprintf (buf, sizeof(buf), "[Buddy] %d units to your %s, %d HP -- %s.",
		  units, compass[oct], players[1].health, what[coop_state]);
    }
    else
	snprintf (buf, sizeof(buf), "[Buddy] %d HP -- %s.", players[1].health, what[coop_state]);

    return buf;
}

int P_AICoop_Summon (void)
{
    if (!AICoop_Mo ())
	return 0;
    summon = COOP_SUMMON_TICS;
    hold   = 0;
    return 1;
}

const char* P_AICoop_Wait (void)
{
    if (!AICoop_Mo ())
	return "[Buddy] (no companion -- launch with -aicoop)";
    hold = !hold;
    if (hold) summon = 0;
    return hold ? "[Buddy] Holding position." : "[Buddy] Moving out.";
}

const char* P_AICoop_Attack (void)
{
    mobj_t*	mo = AICoop_Mo ();
    mobj_t*	pl = playeringame[0] ? players[0].mo : NULL;
    mobj_t*	t;

    if (!mo)
	return "[Buddy] (no companion -- launch with -aicoop)";
    t = AICoop_NearestMonsterTo (pl ? pl->x : mo->x, pl ? pl->y : mo->y);
    if (!t)
	return "[Buddy] No targets around.";
    forcetarget = t;
    forceaggro  = COOP_ATTACK_TICS;
    hold = 0;
    return "[Buddy] Attacking!";
}

const char* P_AICoop_StatusReport (void)
{
    static char		buf[120];
    static const char*	wn[NUMWEAPONS] = { "fists","pistol","shotgun","chaingun",
				"rocket launcher","plasma rifle","BFG9000","chainsaw","super shotgun" };
    player_t*	bot = &players[1];
    int		w, am;

    if (!AICoop_Mo ())
	return "[Buddy] (no companion -- launch with -aicoop)";
    w  = bot->readyweapon;
    am = (weaponinfo[w].ammo < NUMAMMO) ? bot->ammo[weaponinfo[w].ammo] : -1;
    if (am >= 0)
	snprintf (buf, sizeof(buf), "[Buddy] %d HP, %d%% armor, %s, %d rounds.",
		  bot->health, bot->armorpoints, wn[w], am);
    else
	snprintf (buf, sizeof(buf), "[Buddy] %d HP, %d%% armor, %s.",
		  bot->health, bot->armorpoints, wn[w]);
    return buf;
}


void P_AICoop_BuildCmd (void)
{
    player_t*	bot;
    mobj_t*	mo;
    mobj_t*	tgt;
    mobj_t*	heal;
    mobj_t*	item;
    mobj_t*	pl;
    mobj_t*	aimmon = NULL;		// the monster we're firing at (for the sight test)
    ticcmd_t*	cmd;
    angle_t	want, delta;
    fixed_t	tx = 0, ty = 0, dist;
    fixed_t	movethresh = -1;	// move when dist > this; -1 = stand still
    int		rem, turn, haveaim = 0, fire = 0;

    if (!aicoop || !playeringame[1])
	return;

    bot = &players[1];
    cmd = &bot->cmd;

    // Dead: tap "use" so co-op reborns the companion at its start.
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

    pl   = playeringame[0] ? players[0].mo : NULL;
    tgt  = AICoop_FindTarget (mo);
    heal = (bot->health < COOP_HEAL_HP) ? AICoop_FindHealth (mo) : NULL;

    if (summon > 0)     summon--;
    if (forceaggro > 0) forceaggro--;

    coop_state = 0;

    // (attack) ordered: charge the forced target until it (or the timer) dies
    if (forceaggro > 0)
    {
	if (!forcetarget || forcetarget->health <= 0
	    || (forcetarget->flags & MF_CORPSE) || !(forcetarget->flags & MF_SHOOTABLE))
	    forcetarget = AICoop_FindTarget (mo);		// reacquire
	if (forcetarget)
	{
	    coop_state = 1; haveaim = 1; fire = 1; aimmon = forcetarget;
	    movethresh = COOP_KEEP;
	    tx = forcetarget->x; ty = forcetarget->y;
	}
	else
	    forceaggro = 0;
    }

    if (!haveaim)
    {
	// (wait/stay) ordered: hold position; still face & fire at a monster
	if (hold)
	{
	    coop_state = 3;
	    if (tgt) { coop_state = 1; haveaim = 1; fire = 1; aimmon = tgt; tx = tgt->x; ty = tgt->y; }
	    // else: no aim -> stand still
	}
	// (come) ordered: run to the player, ignoring fights/items
	else if (summon > 0 && pl)
	{
	    coop_state = 4; haveaim = 1; movethresh = COOP_NEAR/2;
	    tx = pl->x; ty = pl->y;
	}
	// hurt: break off and grab the nearest med-pack
	else if (heal)
	{
	    coop_state = 2; haveaim = 1; movethresh = 16*FRACUNIT;
	    tx = heal->x; ty = heal->y;
	}
	// fight the nearest monster
	else if (tgt)
	{
	    coop_state = 1; haveaim = 1; fire = 1; aimmon = tgt;
	    movethresh = COOP_KEEP;
	    tx = tgt->x; ty = tgt->y;
	}
	// idle: collect a nearby item, else follow the human
	else if ((item = AICoop_FindItem (mo)) != NULL)
	{
	    coop_state = 5; haveaim = 1; movethresh = 16*FRACUNIT;
	    tx = item->x; ty = item->y;
	}
	else if (pl)
	{
	    coop_state = 0; haveaim = 1; movethresh = COOP_NEAR;
	    tx = pl->x; ty = pl->y;
	}
    }

    if (!haveaim)
	return;					// nothing to do -> stand still

    // turn toward the aim point, clamped to a sane rate
    want  = R_PointToAngle2 (mo->x, mo->y, tx, ty);
    delta = want - mo->angle;
    rem   = (short)(delta >> 16);		// shortest signed turn (BAM>>16)
    turn  = rem;
    if (turn >  COOP_TURN) turn =  COOP_TURN;
    if (turn < -COOP_TURN) turn = -COOP_TURN;
    cmd->angleturn = (short)turn;

    dist = P_AproxDistance (tx - mo->x, ty - mo->y);

    if (fire && aimmon && abs(rem) < COOP_FACING && P_CheckSight (mo, aimmon))
	cmd->buttons |= BT_ATTACK;

    if (movethresh >= 0 && dist > movethresh)
	cmd->forwardmove = COOP_RUN;
}
