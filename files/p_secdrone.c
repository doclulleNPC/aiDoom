// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.  (fork additions, DOOM Source License)
//
// DESCRIPTION:
//	Security Drone (aiDoom fork).  See p_secdrone.h.
//
//	The MT_SECDRONE actor reuses the stock monster AI (A_Look / A_Chase /
//	A_FaceTarget / A_Pain / A_Scream / A_Fall) for movement and targeting; only the
//	laser attack (A_SecDroneShot) is custom.  Spawned MF_FRIEND by the buddy, it
//	hunts monsters via the engine's friendly-monster AI (P_LookForPlayers ->
//	P_FriendNearestEnemy) and can't be hurt by (or hurt) the player/buddy
//	(see the MF_FRIEND guard in P_DamageMobj, files/p_inter.c).
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
#include "p_secdrone.h"

// From p_enemy.c -- stock monster AI reused for the drone's movement/aim.
void A_FaceTarget (mobj_t* actor);
void A_Chase (mobj_t* actor);

// --- tunables ---------------------------------------------------------------
#define DRONE_AGGRO_RANGE	(1600*FRACUNIT)	// acquire ANY enemy within this (all around)
#define DRONE_FIRE_RANGE	(1400*FRACUNIT)	// fire whenever a target is in sight within this
#define DRONE_ENEMY_RANGE	(1024*FRACUNIT)	// "surrounded" detection radius
#define DRONE_ENEMY_COUNT	5		// how many enemies counts as "many"
#define DRONE_COOLDOWN		(35*12)		// ~12 s between buddy deployments
#define DRONE_MAX_ACTIVE	1		// cap on simultaneous friendly drones
#define DRONE_CLIP_COST		50		// bullets, else...
#define DRONE_SHELL_COST	25		// ...shells

//
// A_SecDroneShot
// Fire one laser projectile at the drone's current target (the volley of 3 comes
// from the three fire states looping through this codepointer).
//
void A_SecDroneShot (mobj_t* self)
{
    if (!self->target)
	return;
    A_FaceTarget (self);
    P_SpawnMissile (self, self->target, MT_SECDRONESHOT);
}

//
// SecDrone_NearestEnemy
// Nearest live, shootable, visible non-friendly monster within range -- ALL AROUND
// (no facing restriction), so the drone engages anything nearby.
//
static mobj_t* SecDrone_NearestEnemy (mobj_t* self, fixed_t range)
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
	if (!(m->flags & MF_COUNTKILL))	continue;	// real monsters only
	if (m->flags & MF_FRIEND)	continue;	// not our allies
	if (!(m->flags & MF_SHOOTABLE))	continue;
	if (m->flags & MF_CORPSE)	continue;
	if (m->health <= 0)		continue;
	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > range)			continue;
	if (!P_CheckSight (self, m))	continue;
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}

//
// A_SecDroneLook
// Aggressive idle: sweep ALL AROUND for the nearest enemy and engage immediately.
//
void A_SecDroneLook (mobj_t* self)
{
    mobj_t*	e = SecDrone_NearestEnemy (self, DRONE_AGGRO_RANGE);
    if (e)
    {
	self->target = e;
	if (self->info->seesound)
	    S_StartSound (self, self->info->seesound);
	P_SetMobjState (self, self->info->seestate);
    }
}

//
// A_SecDroneChase
// Aggressive engage: keep a live target (re-acquire the nearest enemy the instant the
// current one dies or leaves), and FIRE whenever the target is in sight and in range
// -- no probabilistic gate.  Otherwise close in using the stock chase movement.
//
void A_SecDroneChase (mobj_t* self)
{
    mobj_t*	t = self->target;

    if (!t || t->health <= 0 || !(t->flags & MF_SHOOTABLE) || (t->flags & MF_CORPSE))
    {
	t = SecDrone_NearestEnemy (self, DRONE_AGGRO_RANGE);
	self->target = t;
	if (!t)
	{
	    P_SetMobjState (self, self->info->spawnstate);
	    return;
	}
    }

    A_FaceTarget (self);
    if (P_CheckSight (self, t)
	&& P_AproxDistance (t->x - self->x, t->y - self->y) <= DRONE_FIRE_RANGE)
    {
	P_SetMobjState (self, self->info->missilestate);	// attack now
	return;
    }
    A_Chase (self);						// else close the distance
}

//
// Count live, shootable, non-friendly monsters within `range` of `origin`.
//
static int SecDrone_CountEnemies (mobj_t* origin, fixed_t range)
{
    thinker_t*	th;
    int		n = 0;

    for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
    {
	mobj_t*	m;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;
	if (!(m->flags & MF_COUNTKILL))	continue;	// real monsters only
	if (m->flags & MF_FRIEND)	continue;	// not our own allies
	if (!(m->flags & MF_SHOOTABLE))	continue;
	if (m->flags & MF_CORPSE)	continue;
	if (m->health <= 0)		continue;
	if (P_AproxDistance (m->x - origin->x, m->y - origin->y) > range)
	    continue;
	n++;
    }
    return n;
}

//
// Count friendly Security Drones currently alive (deploy cap).
//
static int SecDrone_CountActive (void)
{
    thinker_t*	th;
    int		n = 0;

    for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
    {
	mobj_t*	m;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;
	if (m->type == MT_SECDRONE && m->health > 0)
	    n++;
    }
    return n;
}

//
// P_AICoop_MaybeSpawnDrone
// Called each tic while the buddy is alive.  When surrounded by many enemies and
// able to pay, deploy a friendly drone in front of / above the buddy.
//
void P_AICoop_MaybeSpawnDrone (player_t* bot)
{
    static int	cooldown = 0;
    mobj_t*	b;
    mobj_t*	d;
    boolean	useClip, useShell;
    angle_t	ang;
    unsigned	fine;
    fixed_t	x, y, z;

    if (!bot || !bot->mo || bot->playerstate != PST_LIVE)
	return;
    if (cooldown > 0) { cooldown--; return; }

    // Affordable?  (Bullets first, else shells -- same order as the turret.)
    useClip  = bot->ammo[am_clip]  >= DRONE_CLIP_COST;
    useShell = bot->ammo[am_shell] >= DRONE_SHELL_COST;
    if (!useClip && !useShell)
	return;

    b = bot->mo;
    if (SecDrone_CountEnemies (b, DRONE_ENEMY_RANGE) < DRONE_ENEMY_COUNT)
	return;					// not "many enemies" yet
    if (SecDrone_CountActive () >= DRONE_MAX_ACTIVE)
	return;					// already have enough out

    // Spawn just in front of and above the buddy.
    ang  = b->angle;
    fine = ang >> ANGLETOFINESHIFT;
    x = b->x + FixedMul (b->radius + 32*FRACUNIT, finecosine[fine]);
    y = b->y + FixedMul (b->radius + 32*FRACUNIT, finesine[fine]);
    z = b->z + 48*FRACUNIT;

    d = P_SpawnMobj (x, y, z, MT_SECDRONE);
    if (!d)
	return;
    d->angle  = ang;
    d->flags |= MF_FRIEND;			// hunt monsters, spare the player/buddy

    if (useClip) bot->ammo[am_clip]  -= DRONE_CLIP_COST;
    else         bot->ammo[am_shell] -= DRONE_SHELL_COST;
    cooldown = DRONE_COOLDOWN;

    players[consoleplayer].message = "[Buddy] Deploying security drone!";
}
