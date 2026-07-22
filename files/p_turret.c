// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.  (fork additions, DOOM Source License)
//
// DESCRIPTION:
//	Deployable auto-firing sentry turret (aiDoom fork).  See p_turret.h.
//
//	Design:
//	- P_TurretDeploy() is called once per `key_turret` press (edge-triggered in
//	  G_Responder, like the buddy hotkeys).  It checks the ammo cost (25 shells
//	  OR 50 bullets), deducts it, and tosses an MT_TURRET in the player's facing
//	  direction with a little arc -- like throwing a medkit.
//	- The turret is a normal shootable mobj (100 HP, MF_SOLID) whose state machine
//	  alternates between an idle "look" loop (A_TurretLook) and a fire loop
//	  (A_TurretFire).  It never moves under its own power (speed 0, no A_Chase);
//	  a very high mass keeps it planted when shot.
//	- Targeting mirrors the AI co-op buddy: nearest live, shootable, visible
//	  monster within TURRET_RANGE (files/p_ai_coop.c AICoop_FindTarget).
//	- Firing mirrors the chaingunner hitscan (A_CPosAttack) but uses the player
//	  chaingun's per-bullet damage DOUBLED.
//
//-----------------------------------------------------------------------------

#include "doomdef.h"
#include "doomstat.h"
#include "m_random.h"
#include "p_local.h"
#include "r_main.h"		// R_PointToAngle2
#include "tables.h"		// finesine / finecosine
#include "s_sound.h"
#include "sounds.h"
#include "info.h"
#include "p_turret.h"
#include "p_ai_coop.h"		// P_AICoop_Active -- turret is a buddy-mode-only perk

// From p_enemy.c -- faces actor->target and sets actor->angle.
void A_FaceTarget (mobj_t* actor);

// --- tunables ---------------------------------------------------------------
#define TURRET_SHELL_COST	25		// shells consumed per turret
#define TURRET_CLIP_COST	50		// or bullets, if no shells
#define TURRET_RANGE		(1280*FRACUNIT)	// acquisition + fire range (== buddy sight)
#define TURRET_THROW		(11*FRACUNIT)	// horizontal toss speed
#define TURRET_ARC		(6*FRACUNIT)	// base upward toss speed

//
// Turret_FindTarget
// Nearest live, shootable, visible enemy within range (never the player or an ally).
// Same thinker-list walk the AI co-op buddy uses.
//
static mobj_t* Turret_FindTarget (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
    {
	mobj_t*	m;
	fixed_t	d;

	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;

	if (m == self)			continue;
	if (m->type == MT_PLAYER)	continue;	// never target a human
	if (m->flags & MF_FRIEND)	continue;	// never target an ally / another turret
	if (!(m->flags & MF_COUNTKILL) && m->info->seestate == S_NULL)	continue;	// monsters incl. lost souls
	if (!(m->flags & MF_SHOOTABLE))	continue;
	if (m->flags & MF_CORPSE)	continue;
	if (m->health <= 0)		continue;

	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > TURRET_RANGE)		continue;
	if (!P_CheckSight (self, m))	continue;

	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}

//
// A_TurretLook
// Idle codepointer: scan for a target; when found, latch it and switch to the
// fire loop (info->missilestate).
//
void A_TurretLook (mobj_t* self)
{
    mobj_t*	targ = Turret_FindTarget (self);

    if (targ)
    {
	self->target = targ;
	P_SetMobjState (self, self->info->missilestate);
    }
}

//
// A_TurretFire
// Fire codepointer: re-validate / re-acquire the target, face it, and hitscan one
// bullet with chaingun spread at double chaingun damage.  Drops back to the idle
// look loop when nothing is left to shoot.
//
void A_TurretFire (mobj_t* self)
{
    mobj_t*	targ = self->target;
    angle_t	angle;
    fixed_t	slope;
    int		damage;

    // Still a valid target?  If not, try to re-acquire, else go back to looking.
    if (!targ || targ->health <= 0 || !(targ->flags & MF_SHOOTABLE)
	|| (targ->flags & MF_CORPSE)
	|| P_AproxDistance (targ->x - self->x, targ->y - self->y) > TURRET_RANGE
	|| !P_CheckSight (self, targ))
    {
	targ = Turret_FindTarget (self);
	if (!targ)
	{
	    self->target = NULL;
	    P_SetMobjState (self, self->info->spawnstate);
	    return;
	}
	self->target = targ;
    }

    // Aim: face the target, take the auto-aim vertical slope, add chaingun spread.
    A_FaceTarget (self);
    angle = self->angle;
    slope = P_AimLineAttack (self, angle, TURRET_RANGE);
    angle += (P_Random () - P_Random ()) << 20;		// chaingun horizontal spread

    // Player chaingun bullet damage is 5*(P_Random()%3+1); the turret does double.
    damage = 10 * (P_Random () % 3 + 1);

    S_StartSound (self, sfx_pistol);			// the chaingun's shot sound
    P_LineAttack (self, angle, TURRET_RANGE, slope, damage);
}

//
// P_TurretDeploy
// Hotkey action: spend ammo and toss a turret out in front of the player.
//
const char* P_TurretDeploy (player_t* player)
{
    mobj_t*	p;
    mobj_t*	t;
    angle_t	ang;
    unsigned	fine;
    fixed_t	dist, x, y, z;
    int		ld;
    boolean	useShell, useClip;

    if (!player || !player->mo)
	return "";

    // Buddy-mode-only perk: the turret is squad kit, not something a solo marine
    // carries.  (Matches how the buddy order keys self-report "(no companion)".)
    if (!P_AICoop_Active ())
	return "[Turret] Buddy mode only";

    // Cost: 50 bullets first, else 25 shells.
    useClip  = player->ammo[am_clip]  >= TURRET_CLIP_COST;
    useShell = player->ammo[am_shell] >= TURRET_SHELL_COST;
    if (!useClip && !useShell)
	return "Turret needs 50 bullets or 25 shells";

    p   = player->mo;
    ang = p->angle;
    fine = ang >> ANGLETOFINESHIFT;

    // Spawn just in front of the player at chest height, then throw it.  Spawn AT the
    // player and step forward with P_TryMove so a wall (or other blocker) between the
    // player and the target spot stops it -- otherwise P_SpawnMobj would happily place
    // the turret straight through a wall you're standing against.
    dist = p->radius + 24*FRACUNIT;
    x = p->x + FixedMul (dist, finecosine[fine]);
    y = p->y + FixedMul (dist, finesine[fine]);
    z = p->z + 24*FRACUNIT;

    t = P_SpawnMobj (p->x, p->y, z, MT_TURRET);
    if (!t)
	return "";
    {   // nudge to the spot in front; if blocked by a wall it stays at the player's feet
	extern boolean P_TryMove (mobj_t* thing, fixed_t x, fixed_t y);
	P_TryMove (t, x, y);
    }

    // Only charge the player once we know the turret actually spawned (bullets first).
    if (useClip) player->ammo[am_clip]  -= TURRET_CLIP_COST;
    else         player->ammo[am_shell] -= TURRET_SHELL_COST;

    t->angle  = ang;
    t->target = NULL;

    // Toss it in the facing direction with a little arc; steeper if looking up.
    t->momx = FixedMul (TURRET_THROW, finecosine[fine]);
    t->momy = FixedMul (TURRET_THROW, finesine[fine]);
    ld = player->lookdir;
    if (ld >  90) ld =  90;
    if (ld < -90) ld = -90;
    t->momz = TURRET_ARC + ld * (FRACUNIT/12);

    S_StartSound (t, sfx_itemup);
    return "Turret deployed";
}
