// (J) DOOM "overflow" inventory -- see files/p_invent.c.
//
// When the player picks up a health/armor/ammo item while already at that
// item's cap (so vanilla DOOM would refuse it), the surplus is stored on the
// player (player_t.inventory[]) and can be spent later with the use key.  The
// player holds a count/amount per artifact and a currently-selected slot
// (player_t.invslot); scroll left/right selects, a single "use" key spends the
// selected artifact and applies its effect.
#ifndef __P_INVENT_H__
#define __P_INVENT_H__

#include "d_player.h"		// player_t, artitype_t

// USE the player's currently-selected artifact (or `which` if not arti_none).
// Returns true if something was consumed.  Sets player->message either way.
boolean		P_UseArtifact (player_t* player, artitype_t which);

// Inventory navigation (skips empty slots, wraps).  dir = -1 left, +1 right.
void		P_InvScroll (player_t* player, int dir);

// Store overflow into an inventory slot.  `amount` is 1 for the item artifacts
// (stimpack..bluearmor) and the ammo amount for the arti_ammo_* slots.  Returns
// false if it can't be stored (item count at MAXARTICOUNT / ammo store at cap),
// in which case the caller leaves the item on the ground.
boolean		P_StoreOverflow (player_t* player, artitype_t a, int amount);

const char*	P_ArtifactName (artitype_t a);		// human-readable name

#endif
