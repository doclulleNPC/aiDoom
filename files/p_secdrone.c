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
boolean P_Move (mobj_t* actor);		// step one move along actor->movedir
void P_NewChaseDir (mobj_t* actor);	// pick a movedir toward actor->target (needs a target!)

// --- tunables ---------------------------------------------------------------
#define DRONE_AGGRO_RANGE	(1600*FRACUNIT)	// acquire ANY enemy within this (all around)
#define DRONE_FIRE_RANGE	(1400*FRACUNIT)	// fire whenever a target is in sight within this
#define DRONE_CHARGE_RANGE	(640*FRACUNIT)	// ram when the target is within this...
#define DRONE_CHARGE_MIN	(128*FRACUNIT)	// ...but not point-blank (just laser instead)
#define DRONE_CHARGE_SPEED	(24*FRACUNIT)	// ram velocity (lost soul is 20) -- very aggressive
#define DRONE_CHARGE_CD		(35*4)		// ~4 s cooldown between charges
#define DRONE_CHARGE_FLYTICS	30		// give up a missed charge after ~0.85 s
#define DRONE_AVOID		(200*FRACUNIT)	// idle: back off if a human is closer than this
#define DRONE_ENEMY_RANGE	(1024*FRACUNIT)	// "surrounded" detection radius
#define DRONE_ENEMY_COUNT	5		// how many ENGAGED enemies counts as "many"
#define DRONE_PAIN_THRESH	25		// damagecount at/above this = "under heavy fire"
#define DRONE_COOLDOWN		(35*12)		// ~12 s between buddy deployments
#define DRONE_MAX_ACTIVE	1		// cap on simultaneous friendly drones
#define DRONE_LOW_HP		30		// buddy HP at/below this = last-resort panic
#define DRONE_MAX_PANIC		2		// cap raised to this while critically hurt
#define DRONE_CLIP_COST		50		// bullets, else...
#define DRONE_SHELL_COST	25		// ...shells

//
// A_SecDroneShot
// Fire one laser projectile at the drone's current target (the volley of 3 comes
// from the three fire states looping through this codepointer).
//
void A_SecDroneShot (mobj_t* self)
{
    mobj_t*	mo;
    int		dist;

    if (!self->target)
	return;
    A_FaceTarget (self);
    mo = P_SpawnMissile (self, self->target, MT_SECDRONESHOT);
    if (!mo)
	return;

    // P_SpawnMissile launches from source->z + 32 (a standing monster's gun height),
    // which sits above this small floating drone body -- so the laser looked like it
    // came from empty air over the drone.  Drop the origin to the drone's centre and
    // re-aim vertically at the target's centre so the shot visibly leaves the drone.
    mo->z = self->z + (self->height >> 1);
    dist = P_AproxDistance (self->target->x - self->x, self->target->y - self->y) / mo->info->speed;
    if (dist < 1)
	dist = 1;
    mo->momz = ((self->target->z + (self->target->height >> 1)) - mo->z) / dist;
}

//
// A monster is "attacking a human" -- i.e. firing on the player (player 1) or the
// buddy (player 2) -- when its current target is a player mobj.  Both the human
// and the buddy carry a ->player back-pointer, so one test covers both.
//
static boolean SecDrone_AttackingHuman (mobj_t* m)
{
    return m->target != NULL && m->target->player != NULL;
}

//
// SecDrone_BestTarget
// Pick a target ALL AROUND (no facing restriction) among live, shootable, visible
// non-friendly monsters within range.  PRIMARY: whoever is actually shooting at the
// player or the buddy (nearest such).  SECONDARY: if nobody is, just the nearest
// enemy in range/LoS.  So the drone defends its humans first, then mops up.
//
static mobj_t* SecDrone_BestTarget (mobj_t* self, fixed_t range)
{
    thinker_t*	th;
    mobj_t*	bestPrim = NULL;	fixed_t bestPrimD = 0;	// attacking a human
    mobj_t*	bestSec  = NULL;	fixed_t bestSecD  = 0;	// any enemy

    for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
    {
	mobj_t*	m;
	fixed_t	d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;
	if (m == self)			continue;
	if (m->player)			continue;	// never target the human OR the buddy
	if (!(m->flags & MF_COUNTKILL) && m->info->seestate == S_NULL)	continue;	// monsters incl. lost souls
	if (m->flags & MF_FRIEND)	continue;	// not our allies
	if (!(m->flags & MF_SHOOTABLE))	continue;
	if (m->flags & MF_CORPSE)	continue;
	if (m->health <= 0)		continue;
	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > range)			continue;
	if (!P_CheckSight (self, m))	continue;
	if (SecDrone_AttackingHuman (m)) {
	    if (!bestPrim || d < bestPrimD) { bestPrim = m; bestPrimD = d; }
	} else {
	    if (!bestSec  || d < bestSecD)  { bestSec  = m; bestSecD  = d; }
	}
    }
    return bestPrim ? bestPrim : bestSec;
}

//
// Nearest live human to escort toward -- the player (player 1) or the buddy
// (player 2).  Enemies cluster around them, so heading their way is the most
// productive place for an idle drone to go looking for a fight.
//
static mobj_t* SecDrone_NearestHuman (mobj_t* self)
{
    int		i;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (i = 0; i < MAXPLAYERS; i++)
    {
	mobj_t*	pm;
	fixed_t	d;
	if (!playeringame[i])		continue;
	pm = players[i].mo;
	if (!pm || pm->health <= 0)	continue;
	d = P_AproxDistance (pm->x - self->x, pm->y - self->y);
	if (!best || d < bestd) { best = pm; bestd = d; }
    }
    return best;
}

//
// SecDrone_Roam
// Idle hunting: with no enemy in sight, don't just hover.  Head toward the nearest
// human (where the fighting is), scanning all-around every idle tic as it travels --
// but if it gets too close, step directly AWAY so it never crowds or blocks the
// player/buddy.  Net effect: it patrols at arm's length, always moving, until an
// enemy shows up.  (With no human on the map it wanders randomly.)
//
static void SecDrone_Roam (mobj_t* self)
{
    mobj_t*	h = SecDrone_NearestHuman (self);
    fixed_t	hd;

    self->target = NULL;			// idle -> no combat target

    if (!h)					// nobody to escort: random wander
    {
	if (--self->movecount < 0 || !P_Move (self))
	{
	    self->movedir   = P_Random () & 7;
	    self->movecount = 8 + (P_Random () & 15);
	}
	return;
    }

    hd = P_AproxDistance (h->x - self->x, h->y - self->y);
    if (hd < DRONE_AVOID)
    {
	// Too close -> flee straight away from the human so we stop blocking them.
	angle_t	away = R_PointToAngle2 (h->x, h->y, self->x, self->y);
	self->movedir   = ((unsigned)(away + ANG45/2) / ANG45) & 7;
	self->movecount = 4;
	P_Move (self);
	return;
    }

    // Otherwise close toward the human (where the fight is).  P_NewChaseDir needs a
    // target for its direction pick; borrow the human for pathing only, then drop it
    // so the idle state never holds a "combat" target.
    self->target = h;
    if (--self->movecount < 0 || !P_Move (self))
	P_NewChaseDir (self);
    self->target = NULL;
}

//
// A_SecDroneLook
// Aggressive idle: sweep ALL AROUND for the nearest enemy and engage immediately;
// if none is in sight, actively roam toward the fight to go find one.
//
void A_SecDroneLook (mobj_t* self)
{
    mobj_t*	e = SecDrone_BestTarget (self, DRONE_AGGRO_RANGE);
    if (e)
    {
	self->target = e;
	if (self->info->seesound)
	    S_StartSound (self, self->info->seesound);
	P_SetMobjState (self, self->info->seestate);
	return;
    }
    SecDrone_Roam (self);			// nothing in sight -> go hunting
}

//
// A_SecDroneChase
// Aggressive engage: keep a live target (re-acquire the best target the instant the
// current one dies or leaves -- see SecDrone_BestTarget for the priority), then, in
// order: CHARGE (lost-soul ram, off cooldown, target at mid-range) > FIRE (in sight,
// in range, no probabilistic gate) > close in with the stock chase movement.
//
// The per-drone charge cooldown lives in self->lastlook: a plain int the drone's AI
// never uses for anything else (A_SecDroneLook/Chase drive targeting directly and
// never call P_LookForPlayers, the only other reader of lastlook), so it is a free,
// savegame-persisted timer -- no mobj_t field or side-table needed.
//
void A_SecDroneChase (mobj_t* self)
{
    mobj_t*	t = self->target;
    fixed_t	dist;

    // If a hit knocked us out of a charge into this state, cancel the ram cleanly so
    // we don't chase around still flagged MF_SKULLFLY with leftover momentum.
    if (self->flags & MF_SKULLFLY)
    {
	self->flags &= ~MF_SKULLFLY;
	self->momx = self->momy = self->momz = 0;
    }

    if (self->lastlook > 0)					// tick the charge cooldown
	self->lastlook--;

    if (!t || t->health <= 0 || !(t->flags & MF_SHOOTABLE) || (t->flags & MF_CORPSE))
    {
	t = SecDrone_BestTarget (self, DRONE_AGGRO_RANGE);
	self->target = t;
	if (!t)
	{
	    P_SetMobjState (self, self->info->spawnstate);
	    return;
	}
    }

    A_FaceTarget (self);
    dist = P_AproxDistance (t->x - self->x, t->y - self->y);

    // Charge: ram the target lost-soul style when it's at mid-range and the cooldown
    // is up.  (Point-blank we just laser; far away we close in first.)
    if (self->lastlook == 0
	&& dist <= DRONE_CHARGE_RANGE && dist >= DRONE_CHARGE_MIN
	&& P_CheckSight (self, t))
    {
	A_SecDroneCharge (self);
	self->lastlook = DRONE_CHARGE_CD;
	P_SetMobjState (self, S_SECDR_CHG1);
	return;
    }

    if (P_CheckSight (self, t) && dist <= DRONE_FIRE_RANGE)
    {
	P_SetMobjState (self, self->info->missilestate);	// attack now
	return;
    }
    A_Chase (self);						// else close the distance
}

//
// A_SecDroneCharge
// Lost-soul-style ram: fling the drone at its target as an MF_SKULLFLY missile.  The
// impact damage happens in PIT_CheckThing (info->damage); a FRIEND charger passes
// harmlessly through the player/buddy/allies (guarded in p_map.c).  self->movecount
// doubles as the flight timeout (A_Chase, which owns movecount, isn't running while
// we're charging).
//
void A_SecDroneCharge (mobj_t* self)
{
    mobj_t*	dest = self->target;
    angle_t	an;
    int		dist;

    if (!dest || dest->health <= 0)
	return;

    self->flags |= MF_SKULLFLY;
    if (self->info->attacksound)
	S_StartSound (self, self->info->attacksound);

    A_FaceTarget (self);
    an = self->angle >> ANGLETOFINESHIFT;
    self->momx = FixedMul (DRONE_CHARGE_SPEED, finecosine[an]);
    self->momy = FixedMul (DRONE_CHARGE_SPEED, finesine[an]);
    dist = P_AproxDistance (dest->x - self->x, dest->y - self->y) / DRONE_CHARGE_SPEED;
    if (dist < 1)
	dist = 1;
    self->momz = (dest->z + (dest->height>>1) - self->z) / dist;
    self->movecount = DRONE_CHARGE_FLYTICS;
}

//
// A_SecDroneChargeTick
// Runs each frame of the ram flight.  A real hit (thing or wall) is handled by the
// engine, which clears MF_SKULLFLY and resets us to spawnstate -- so if we're still
// here and flagged, we're mid-flight.  Bail out (resume chasing) when the flight
// times out or the target is gone, so a missed charge doesn't slide forever.
//
void A_SecDroneChargeTick (mobj_t* self)
{
    if (!(self->flags & MF_SKULLFLY))
    {
	P_SetMobjState (self, self->info->seestate);
	return;
    }
    if (--self->movecount <= 0 || !self->target || self->target->health <= 0)
    {
	self->flags &= ~MF_SKULLFLY;
	self->momx = self->momy = self->momz = 0;
	P_SetMobjState (self, self->info->seestate);		// resume chasing
    }
}

//
// Count live, shootable, non-friendly monsters within `range` of `origin` that are
// genuinely ENGAGING one of the humans -- i.e. their target is a player or the buddy
// AND they have line of sight to `origin`.  Mere PRESENCE is not enough: monsters that
// are asleep, infighting each other, or shooting a turret/drone (target isn't a player)
// don't count, and neither does a threat hidden behind a wall.  So "surrounded" means
// surrounded by things actually coming for the squad, not by nearby furniture.
//
static int SecDrone_CountThreats (mobj_t* origin, fixed_t range)
{
    thinker_t*	th;
    int		n = 0;

    for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
    {
	mobj_t*	m;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;
	if (m->player)			continue;	// never the human or the buddy
	if (!(m->flags & MF_COUNTKILL) && m->info->seestate == S_NULL)	continue;	// monsters incl. lost souls
	if (m->flags & MF_FRIEND)	continue;	// not our own allies
	if (!(m->flags & MF_SHOOTABLE))	continue;
	if (m->flags & MF_CORPSE)	continue;
	if (m->health <= 0)		continue;
	if (!SecDrone_AttackingHuman (m))	continue;	// target must be a player/buddy
	if (P_AproxDistance (m->x - origin->x, m->y - origin->y) > range)
	    continue;
	if (!P_CheckSight (origin, m))	continue;	// actually visible to us, not behind a wall
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
// Called each tic while the buddy is alive.  The drone is an EMERGENCY asset:
// deploy it only when the buddy is actually in danger -- taking heavy fire, or
// genuinely surrounded by engaged enemies.  (It used to fire the instant a level
// loaded, because the buddy often spawns standing next to a pack of still-asleep
// monsters; those no longer count -- see SecDrone_CountThreats.)
// Exception: if an ammo pool is maxed out, deploy anyway to burn the overflow --
// a free drone beats letting pickups go to waste.
// Last resort: when the buddy is critically low on HP AND under attack, the drone
// may bypass BOTH the cooldown and the normal 1-drone cap (up to DRONE_MAX_PANIC)
// in a bid to keep it alive.
//
void P_AICoop_MaybeSpawnDrone (player_t* bot)
{
    static int	cooldown = 0;
    mobj_t*	b;
    mobj_t*	d;
    boolean	useClip, useShell;
    boolean	heavyFire, surrounded, lowHP;
    boolean	clipCapped, shellCapped, atCap;
    int		threats, maxActive;
    angle_t	ang;
    unsigned	fine;
    fixed_t	x, y, z;

    if (!bot || !bot->mo || bot->playerstate != PST_LIVE)
	return;
    b = bot->mo;

    // Cheap gates BEFORE the per-tic threat scan (SecDrone_CountThreats is a full
    // thinker walk + a P_CheckSight per monster -- previously paid every single tic
    // even with a drone already out).  Only a critically hurt buddy (health <=
    // DRONE_LOW_HP) may redeploy mid-cooldown, and only it needs the threat count:
    //  1) on cooldown and healthy -> nothing to decide, just tick the cooldown down;
    //  2) healthy and off cooldown -> a drone deploy isn't a 35 Hz decision, so
    //     evaluate only a few times a second.  Both skip the scan; low-HP falls through.
    if (bot->health > DRONE_LOW_HP)
    {
	if (cooldown > 0)  { cooldown--; return; }
	if (gametic & 7)   return;			// ~4-5 Hz deploy evaluation
    }

    // Critically hurt AND something is actually shooting at us -> last-resort
    // mode: worth a drone even mid-cooldown / past the usual single-drone cap.
    threats = SecDrone_CountThreats (b, DRONE_ENEMY_RANGE);
    lowHP   = bot->health > 0 && bot->health <= DRONE_LOW_HP && threats >= 1;

    if (cooldown > 0 && !lowHP) { cooldown--; return; }

    // Affordable?  (Bullets first, else shells -- same order as the turret.)
    useClip  = bot->ammo[am_clip]  >= DRONE_CLIP_COST;
    useShell = bot->ammo[am_shell] >= DRONE_SHELL_COST;
    if (!useClip && !useShell)
	return;

    // --- deploy only when it's actually warranted ---------------------------
    heavyFire   = bot->damagecount >= DRONE_PAIN_THRESH;			// under heavy fire
    surrounded  = threats >= DRONE_ENEMY_COUNT;
    // Ammo-cap exception: only meaningful for a pool we can actually spend.
    clipCapped  = useClip  && bot->maxammo[am_clip]  > 0 && bot->ammo[am_clip]  >= bot->maxammo[am_clip];
    shellCapped = useShell && bot->maxammo[am_shell] > 0 && bot->ammo[am_shell] >= bot->maxammo[am_shell];
    atCap       = clipCapped || shellCapped;

    if (!heavyFire && !surrounded && !atCap && !lowHP)
	return;					// safe and not overflowing -> hold

    // When deploying purely to burn overflow (no danger), spend the pool that is
    // actually capped so we relieve the overflow.
    if (atCap && !heavyFire && !surrounded && !lowHP)
	useClip = clipCapped;			// clip-capped -> spend clip; else fall to shell

    // Hard rule: at most ONE drone active at a time (no exceptions -- a low-HP buddy
    // still only ever has one out; lowHP just lets it skip the cooldown to redeploy the
    // instant its drone dies).
    maxActive = DRONE_MAX_ACTIVE;
    if (SecDrone_CountActive () >= maxActive)
	return;					// already have one out

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
