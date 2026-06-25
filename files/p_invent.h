// (J) Simple Heretic-style artifact inventory -- see files/p_invent.c.
//
// Three collectible/stackable/usable artifacts (Quartz Flask, Chaos Device,
// Torch).  The player holds a count of each (player_t.inventory[]) and a
// currently-selected slot (player_t.invslot); scroll left/right selects, a
// single "use" key consumes one of the selected artifact and applies its
// effect.  Pickup mobjtypes (MT_ARTI_*) are added at runtime via Invent_Init
// (same additive mechanism as files/revmarine.c).
#ifndef __P_INVENT_H__
#define __P_INVENT_H__

#include "d_player.h"		// player_t, artitype_t

void		Invent_Init (void);			// fill MT_ARTI_* states/mobjinfo (D_DoomMain)

// USE the player's currently-selected artifact (or `which` if not arti_none).
// Returns true if one was consumed.  Sets player->message either way.
boolean		P_UseArtifact (player_t* player, artitype_t which);

// Inventory navigation (skips empty slots, wraps).  dir = -1 left, +1 right.
void		P_InvScroll (player_t* player, int dir);

// Pickup hook (called from P_TouchSpecialThing): if `special` is an artifact
// item, pocket it on `player` and return true (caller then removes it).
boolean		P_TouchArtifact (player_t* player, mobj_t* special);

const char*	P_ArtifactName (artitype_t a);		// human-readable name

#endif
