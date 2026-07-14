// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.  (fork additions, DOOM Source License)
//
// DESCRIPTION:
//	Deployable auto-firing sentry turret (aiDoom fork).
//
//	The player throws a turret in front of himself with the `key_turret` hotkey
//	(default 'q', overridable in the config).  It costs 25 shells OR 50 bullets
//	from the player's inventory.  The turret cannot move; it has 100 HP and
//	hitscans nearby enemies like the chaingun at DOUBLE damage.
//
//-----------------------------------------------------------------------------

#ifndef __P_TURRET__
#define __P_TURRET__

#include "p_mobj.h"
#include "d_player.h"

// Actor codepointers -- referenced by the MT_TURRET states in info.c.
void A_TurretLook (mobj_t* self);	// idle: acquire nearest visible enemy
void A_TurretFire (mobj_t* self);	// firing: track + hitscan the target

// Hotkey entry point.  Tries to deploy a turret for `player`; returns a HUD
// message (success text, or why it failed).  Never returns NULL.  Deducts the
// ammo cost and spawns the turret on success.
const char* P_TurretDeploy (player_t* player);

#endif	// __P_TURRET__
