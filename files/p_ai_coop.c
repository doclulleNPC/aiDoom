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

#include "p_ai_coop.h"

static int	aicoop;			// -aicoop given

#define COOP_SIGHT	(1280*FRACUNIT)	// monster acquisition range
#define COOP_TURN	1300		// max angleturn per tic (~7 deg)
#define COOP_FACING	1500		// |remaining turn| under which we open fire
#define COOP_NEAR	(256*FRACUNIT)	// follow distance to the human
#define COOP_KEEP	(192*FRACUNIT)	// advance toward a monster until this close
#define COOP_RUN	0x32		// forwardmove "run" magnitude


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


void P_AICoop_BuildCmd (void)
{
    player_t*	bot;
    mobj_t*	mo;
    mobj_t*	tgt;
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

    tgt    = AICoop_FindTarget (mo);
    follow = playeringame[0] ? players[0].mo : NULL;

    if (tgt)		{ tx = tgt->x;    ty = tgt->y; }
    else if (follow)	{ tx = follow->x; ty = follow->y; }
    else		return;

    // turn toward the target, clamped to a sane rate
    want  = R_PointToAngle2 (mo->x, mo->y, tx, ty);
    delta = want - mo->angle;
    rem   = (short)(delta >> 16);		// shortest signed turn (BAM>>16)
    turn  = rem;
    if (turn >  COOP_TURN) turn =  COOP_TURN;
    if (turn < -COOP_TURN) turn = -COOP_TURN;
    cmd->angleturn = (short)turn;

    dist = P_AproxDistance (tx - mo->x, ty - mo->y);

    if (tgt)
    {
	if (abs(rem) < COOP_FACING && P_CheckSight (mo, tgt))
	    cmd->buttons |= BT_ATTACK;
	if (dist > COOP_KEEP)
	    cmd->forwardmove = COOP_RUN;	// close in
    }
    else
    {
	if (dist > COOP_NEAR)
	    cmd->forwardmove = COOP_RUN;	// follow the human
    }
}
