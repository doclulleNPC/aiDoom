// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	AI-controlled co-op companion (player 2).  Enabled with -aicoop, which
//	marks playeringame[1] so the engine spawns a second marine at the map's
//	co-op start.  Each tic this builds that player's ticcmd with a small
//	built-in brain: acquire the nearest visible monster, turn to it and
//	fire; with no target, follow the human player.  It drives the player
//	through the normal ticcmd path, so weapons, damage, pickups and reborn
//	all work like a real co-op peer.
//
//	Single-machine only (no real netgame): the cmd is generated locally and
//	never carried over the wire.
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

static int	aicoop;			// -aicoop given

#define COOP_SIGHT	(1280*FRACUNIT)	// monster acquisition range
#define COOP_TURN	1300		// max angleturn per tic (~7 deg)
#define COOP_FACING	1500		// |remaining turn| under which we open fire
#define COOP_NEAR	(256*FRACUNIT)	// follow distance to the human (healthy)
#define COOP_KEEP	(192*FRACUNIT)	// advance toward a monster until this close
#define COOP_RUN	0x32		// forwardmove "run" magnitude
#define COOP_DEFEND_HP	50		// below this: defensive -- don't charge, hang back
#define COOP_HEAL_HP	30		// below this: break off and grab a med-pack
#define COOP_HEAL_RANGE	(1024*FRACUNIT)	// how far to look for one
#define COOP_BEHIND	(110*FRACUNIT)	// defensive: hold this far behind the player
#define COOP_NEAR_DEF	(64*FRACUNIT)	// defensive: stay tight to that spot


void P_AICoop_Init (void)
{
    if (!M_CheckParm ("-aicoop"))
	return;
    aicoop = 1;
    playeringame[1] = true;		// spawn player 2 at the map's co-op start
    printf ("P_AICoop: AI co-op companion (player 2) enabled\n");
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

	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}


void P_AICoop_BuildCmd (void)
{
    player_t*	bot;
    mobj_t*	mo;
    mobj_t*	tgt;
    mobj_t*	heal;
    mobj_t*	follow;
    ticcmd_t*	cmd;
    angle_t	want, delta;
    fixed_t	tx, ty, dist;
    int		rem, turn;

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

    // Health-tiered behaviour:
    //   < 30 HP : break off and grab the nearest med-pack.
    //   < 50 HP : defensive -- still shoots, but doesn't charge; hangs back
    //             behind the player.
    //   healthy : chase the nearest monster, else follow the player.
    int defensive = (bot->health < COOP_DEFEND_HP);

    heal   = (bot->health < COOP_HEAL_HP) ? AICoop_FindHealth (mo) : NULL;
    tgt    = heal ? NULL : AICoop_FindTarget (mo);
    follow = playeringame[0] ? players[0].mo : NULL;

    if (heal)
    {
	tx = heal->x; ty = heal->y;
    }
    else if (tgt)
    {
	tx = tgt->x; ty = tgt->y;	// face the monster (to shoot)
    }
    else if (follow)
    {
	if (defensive)
	{
	    // hold a spot behind the player and stick close to it
	    angle_t pa = follow->angle;
	    tx = follow->x - FixedMul (finecosine[pa>>ANGLETOFINESHIFT], COOP_BEHIND);
	    ty = follow->y - FixedMul (finesine[pa>>ANGLETOFINESHIFT],   COOP_BEHIND);
	}
	else
	{
	    tx = follow->x; ty = follow->y;
	}
    }
    else
	return;

    // turn toward the target, clamped to a sane rate
    want  = R_PointToAngle2 (mo->x, mo->y, tx, ty);
    delta = want - mo->angle;
    rem   = (short)(delta >> 16);		// shortest signed turn (BAM>>16)
    turn  = rem;
    if (turn >  COOP_TURN) turn =  COOP_TURN;
    if (turn < -COOP_TURN) turn = -COOP_TURN;
    cmd->angleturn = (short)turn;

    dist = P_AproxDistance (tx - mo->x, ty - mo->y);

    if (heal)
    {
	if (dist > 16*FRACUNIT)
	    cmd->forwardmove = COOP_RUN;	// walk onto the pickup (no firing)
    }
    else if (tgt)
    {
	if (abs(rem) < COOP_FACING && P_CheckSight (mo, tgt))
	    cmd->buttons |= BT_ATTACK;
	// Healthy: close the distance.  Defensive: hold position and just shoot.
	if (!defensive && dist > COOP_KEEP)
	    cmd->forwardmove = COOP_RUN;
    }
    else
    {
	// follow the player -- tighter and behind when defensive
	if (dist > (defensive ? COOP_NEAR_DEF : COOP_NEAR))
	    cmd->forwardmove = COOP_RUN;
    }

    // Door/switch opener: if we're trying to move but barely getting anywhere,
    // we're probably against a closed door -- pulse Use (on/off, so P_PlayerThink
    // sees fresh presses) to open it.  Harmless against plain walls.
    {
	static fixed_t	lastx, lasty;
	static int	stuck;
	fixed_t		moved = P_AproxDistance (mo->x - lastx, mo->y - lasty);

	if (cmd->forwardmove && moved < 4*FRACUNIT)
	    stuck++;
	else
	    stuck = 0;

	if (stuck >= 3 && !(leveltime & 1))
	    cmd->buttons |= BT_USE;

	lastx = mo->x;
	lasty = mo->y;
    }
}
