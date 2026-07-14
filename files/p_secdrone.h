// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.  (fork additions, DOOM Source License)
//
// DESCRIPTION:
//	Security Drone (aiDoom fork) -- a vanilla-C port of the ZDoom DECORATE actor
//	from SecurityDrone.pk3 (Code: Gothic).  A small flying laser drone.  Here it is
//	a FRIENDLY ally: the AI co-op buddy spawns one (costing the buddy 50 bullets or
//	25 shells) when it is surrounded by many enemies; the drone then hunts and lasers
//	them.  See files/p_secdrone.c.
//
//-----------------------------------------------------------------------------

#ifndef __P_SECDRONE__
#define __P_SECDRONE__

#include "p_mobj.h"
#include "d_player.h"

// Actor codepointers (referenced by the MT_SECDRONE states in info.c).
void A_SecDroneLook  (mobj_t* self);	// idle: acquire the nearest enemy ALL AROUND
void A_SecDroneChase (mobj_t* self);	// engage: close in + fire whenever a target is in sight
void A_SecDroneShot  (mobj_t* self);	// fire one laser (MT_SECDRONESHOT) at the target

// Buddy hook, called each tic while the buddy is alive: if it is surrounded by enough
// enemies and can afford it, deploy a friendly Security Drone.  Throttled + capped
// internally.
void P_AICoop_MaybeSpawnDrone (player_t* bot);

#endif	// __P_SECDRONE__
