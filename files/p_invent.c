// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	(J) DOOM "overflow" inventory.
//
//	When the player picks up a health / armor / ammo item while ALREADY at
//	that item's cap (so vanilla DOOM would refuse the pickup and leave it on
//	the ground), the surplus is pocketed here instead and can be spent later
//	with the inventory use key.
//
//	  - Health/armor artifacts store a COUNT of items; each use applies the
//	    effect once and decrements the count by 1.
//	  - Ammo artifacts store an AMMO AMOUNT (sum of overflowed rounds); each
//	    use transfers as much as fits into the live ammo store and decrements
//	    the held amount by the transferred quantity.
//
//	The held counts/amounts + the selected slot live in player_t
//	(inventory[]/invslot) so they ride along in the savegame automatically.
//
//-----------------------------------------------------------------------------

#include "doomdef.h"
#include "doomstat.h"			// players[], consoleplayer
#include "info.h"
#include "m_fixed.h"
#include "tables.h"
#include "sounds.h"
#include "s_sound.h"
#include "p_local.h"
#include "p_invent.h"
#include "p_inv_heretic.h"		// (H) ApplyHereticArtifact for the Heretic slots

// P_GiveBody / P_GiveArmor (p_inter.c) reuse DOOM's at-cap semantics.
extern boolean	P_GiveBody  (player_t* player, int num);
extern boolean	P_GiveArmor (player_t* player, int armortype);

// maxammo[] (p_inter.c) is the base (no-backpack) ammo cap; the live per-player
// cap is player->maxammo[] (doubled with a backpack).
extern int	maxammo[NUMAMMO];


// Map an ammo artifact slot to its ammotype_t (and back, for the store cap).
static ammotype_t ArtiToAmmo (artitype_t a)
{
    switch (a)
    {
      case arti_ammo_bullets:	return am_clip;
      case arti_ammo_shells:	return am_shell;
      case arti_ammo_rockets:	return am_misl;
      case arti_ammo_cells:	return am_cell;
      default:			return am_noammo;
    }
}


const char* P_ArtifactName (artitype_t a)
{
    switch (a)
    {
      case arti_stimpack:	return "Stimpack";
      case arti_medikit:	return "Medikit";
      case arti_healthbonus:	return "Health Bonus";
      case arti_armorbonus:	return "Armor Bonus";
      case arti_greenarmor:	return "Green Armor";
      case arti_bluearmor:	return "Blue Armor";
      case arti_ammo_bullets:	return "Bullets";
      case arti_ammo_shells:	return "Shells";
      case arti_ammo_rockets:	return "Rockets";
      case arti_ammo_cells:	return "Cells";
      // (H) Heretic artifacts (files/p_inv_heretic.c)
      case h_arti_flask:	return "Quartz Flask";
      case h_arti_urn:		return "Mystic Urn";
      case h_arti_tome:		return "Tome of Power";
      case h_arti_torch:	return "Torch";
      case h_arti_bomb:		return "Time Bomb";
      case h_arti_ring:		return "Ring of Invincibility";
      case h_arti_shadow:	return "Shadowsphere";
      case h_arti_chaos:	return "Chaos Device";
      case h_arti_wings:	return "Wings of Wrath";
      case h_arti_egg:		return "Morph Ovum";
      default:			return "";
    }
}


// ---------------------------------------------------------------------------
// Store overflow into an inventory slot.  `amount` is 1 for the item artifacts
// (stimpack..bluearmor) and the ammo amount for the arti_ammo_* slots.
// Returns false if it can't be stored (item count already at MAXARTICOUNT, or
// ammo store already at its cap) -- the caller then leaves the item on the
// ground.  On success the slot is auto-selected if nothing was selected.
// ---------------------------------------------------------------------------
extern int	P_AICoop_Active (void);		// the DOOM overflow inventory is a buddy-mode feature

boolean P_StoreOverflow (player_t* player, artitype_t a, int amount)
{
    ammotype_t	at;

    if (!P_AICoop_Active ())			// no buddy -> no DOOM inventory; pickup stays vanilla
	return false;
    if (a <= arti_none || a >= NUMARTIFACTS)
	return false;

    at = ArtiToAmmo (a);
    if (at != am_noammo)
    {
	// Ammo artifact: cap the stored amount at this ammo type's max (the live
	// per-player cap, so a backpack lets you stockpile more).
	int cap = player->maxammo[at];
	if (player->inventory[a] >= cap)
	    return false;			// store full
	player->inventory[a] += amount;
	if (player->inventory[a] > cap)
	    player->inventory[a] = cap;
    }
    else
    {
	// Item artifact: a simple carry count.
	if (player->inventory[a] >= MAXARTICOUNT)
	    return false;			// can't hold any more
	player->inventory[a] += amount;
	if (player->inventory[a] > MAXARTICOUNT)
	    player->inventory[a] = MAXARTICOUNT;
    }

    if (player->invslot == arti_none)
	player->invslot = a;
    return true;
}


// ---------------------------------------------------------------------------
// Scroll the selection left (-1) or right (+1), skipping empty slots, wrapping.
// ---------------------------------------------------------------------------
void P_InvScroll (player_t* player, int dir)
{
    int slot = player->invslot;
    int i;

    if (dir == 0) return;
    dir = (dir < 0) ? -1 : 1;

    // Step through real artifact slots (1 .. NUMARTIFACTS-1) until one is held.
    for (i = 0; i < NUMARTIFACTS - 1; i++)
    {
	slot += dir;
	if (slot >= NUMARTIFACTS) slot = arti_none + 1;
	if (slot <= arti_none)    slot = NUMARTIFACTS - 1;
	if (player->inventory[slot] > 0)
	{
	    player->invslot = slot;
	    return;
	}
    }
    // Nothing held anywhere: leave selection as the empty sentinel.
    if (player->invslot != arti_none && player->inventory[player->invslot] <= 0)
	player->invslot = arti_none;
}


// ---------------------------------------------------------------------------
// Apply an artifact's effect to the player.  Returns the amount consumed: for
// item artifacts that's 1 on success / 0 on refusal; for ammo artifacts it's
// the number of rounds transferred (0 if already full).  Sets player->message.
// ---------------------------------------------------------------------------
static int ApplyArtifact (player_t* player, artitype_t a)
{
    mobj_t*	mo = player->mo;

    // (H) Heretic artifacts: effects live in files/p_inv_heretic.c.  They consume
    // exactly one on success (it sets ->message either way).
    if (a >= h_arti_flask)
	return ApplyHereticArtifact (player, a) ? 1 : 0;

    switch (a)
    {
      case arti_stimpack:
	if (!P_GiveBody (player, 10))
	{
	    player->message = "ALREADY AT FULL HEALTH";
	    return 0;
	}
	player->message = "USED STIMPACK";
	return 1;

      case arti_medikit:
	if (!P_GiveBody (player, 25))
	{
	    player->message = "ALREADY AT FULL HEALTH";
	    return 0;
	}
	player->message = "USED MEDIKIT";
	return 1;

      case arti_healthbonus:
	if (player->health >= 200)
	{
	    player->message = "ALREADY AT FULL HEALTH";
	    return 0;
	}
	player->health++;
	if (mo) mo->health = player->health;
	player->message = "USED HEALTH BONUS";
	return 1;

      case arti_armorbonus:
	if (player->armorpoints >= 200)
	{
	    player->message = "ALREADY AT FULL ARMOR";
	    return 0;
	}
	player->armorpoints++;
	if (!player->armortype)
	    player->armortype = 1;
	player->message = "USED ARMOR BONUS";
	return 1;

      case arti_greenarmor:
	if (!P_GiveArmor (player, 1))
	{
	    player->message = "ALREADY HAVE BETTER ARMOR";
	    return 0;
	}
	player->message = "USED GREEN ARMOR";
	return 1;

      case arti_bluearmor:
	if (!P_GiveArmor (player, 2))
	{
	    player->message = "ALREADY HAVE BETTER ARMOR";
	    return 0;
	}
	player->message = "USED BLUE ARMOR";
	return 1;

      case arti_ammo_bullets:
      case arti_ammo_shells:
      case arti_ammo_rockets:
      case arti_ammo_cells:
      {
	ammotype_t at = ArtiToAmmo (a);
	int room  = player->maxammo[at] - player->ammo[at];
	int give;
	if (room <= 0)
	{
	    player->message = "AMMO ALREADY FULL";
	    return 0;
	}
	give = player->inventory[a];
	if (give > room) give = room;
	player->ammo[at] += give;
	player->message = "TRANSFERRED AMMO";
	return give;				// consume the transferred amount
      }

      default:
	return 0;
    }
}


// ---------------------------------------------------------------------------
// USE the selected artifact (or `which` when given explicitly, e.g. for tests).
// Consumes per ApplyArtifact's semantics on success.  Always sets ->message.
// ---------------------------------------------------------------------------
boolean P_UseArtifact (player_t* player, artitype_t which)
{
    artitype_t	a = (which != arti_none) ? which : (artitype_t) player->invslot;
    int		used;

    if (a <= arti_none || a >= NUMARTIFACTS || player->inventory[a] <= 0)
    {
	player->message = "YOU HAVE NO ARTIFACT TO USE";
	return false;
    }

    used = ApplyArtifact (player, a);
    if (used <= 0)
	return false;				// effect refused (message already set)

    player->inventory[a] -= used;
    if (player->inventory[a] < 0)
	player->inventory[a] = 0;
    // If that emptied the slot, hop to another held artifact (or empty).
    if (player->inventory[a] <= 0)
	P_InvScroll (player, +1);
    return true;
}


// ---------------------------------------------------------------------------
// DROP the currently selected artifact: spawn its pickup item on the ground a
// little in front of the player (tossed, so they don't instantly re-grab it)
// and take it out of the inventory.  Works for DOOM overflow items and the
// Heretic artifacts alike.
// ---------------------------------------------------------------------------
extern mobj_t*	P_SpawnMobj (fixed_t x, fixed_t y, fixed_t z, mobjtype_t type);
extern boolean	P_TryMove (mobj_t* thing, fixed_t x, fixed_t y);

boolean P_DropArtifact (player_t* player)
{
    artitype_t	a = (artitype_t) player->invslot;
    mobjtype_t	t;
    int		amount = 1;		// inventory units consumed by the drop
    mobj_t*	drop;
    mobj_t*	mo = player->mo;
    unsigned	an;
    fixed_t	dx, dy;

    if (a <= arti_none || a >= NUMARTIFACTS || player->inventory[a] <= 0 || !mo)
    {
	player->message = "NOTHING TO DROP";
	return false;
    }

    if (a >= h_arti_flask)		// Heretic artifacts map 1:1 to MT_HARTI_*
	t = (mobjtype_t)(MT_HARTI_FLASK + (a - h_arti_flask));
    else switch (a)			// DOOM overflow items -> their stock pickups
    {
      case arti_stimpack:     t = MT_MISC10; break;	// stimpack
      case arti_medikit:      t = MT_MISC11; break;	// medikit
      case arti_healthbonus:  t = MT_MISC2;  break;	// health bonus
      case arti_armorbonus:   t = MT_MISC3;  break;	// armor bonus
      case arti_greenarmor:   t = MT_MISC0;  break;	// green armor
      case arti_bluearmor:    t = MT_MISC1;  break;	// blue armor
      case arti_ammo_bullets: t = MT_CLIP;   amount = 10; break;	// a clip
      case arti_ammo_shells:  t = MT_MISC22; amount = 4;  break;	// 4 shells
      case arti_ammo_rockets: t = MT_MISC18; amount = 1;  break;	// 1 rocket
      case arti_ammo_cells:   t = MT_MISC20; amount = 20; break;	// a cell
      default: player->message = "CAN'T DROP THAT"; return false;
    }

    // Place it ~40u ahead so it lands in front; if that's inside a wall, drop at the feet.
    an = mo->angle >> ANGLETOFINESHIFT;
    dx = mo->x + FixedMul (48*FRACUNIT, finecosine[an]);
    dy = mo->y + FixedMul (48*FRACUNIT, finesine[an]);
    drop = P_SpawnMobj (mo->x, mo->y, mo->z, t);	// spawn at the feet...
    P_TryMove (drop, dx, dy);				// ...slide it ahead (relinks; stays put if a wall blocks)
    drop->flags |= MF_DROPPED;				// a dropped item (no deathmatch respawn)
    // Toss it along the player's facing (an = mo->angle), harder than before so it clears the
    // player and lands well ahead instead of dropping at the feet to be re-grabbed instantly.
    // The in-flight pickup guard in P_TouchSpecialThing keeps the dropper off it until it lands.
    drop->momx = FixedMul (10*FRACUNIT, finecosine[an]);
    drop->momy = FixedMul (10*FRACUNIT, finesine[an]);
    drop->momz = 5*FRACUNIT;

    player->inventory[a] -= amount;
    if (player->inventory[a] < 0) player->inventory[a] = 0;
    if (player->inventory[a] <= 0) P_InvScroll (player, +1);	// emptied -> reselect

    player->message = "DROPPED ITEM";
    return true;
}


// ---------------------------------------------------------------------------
// (buddy mode) Self-revive: a DOWNED human spends a stored medikit/stimpack to get
// back up himself (NOT automatic -- triggered by the inventory-use key while dead).
// Mirrors the buddy's revive: un-corpse the body, restore health to the item's heal
// value, stand up, raise the weapon.  Returns true if it had an item and revived.
// ---------------------------------------------------------------------------
boolean P_InventorySelfRevive (player_t* player)
{
    mobj_t*	mo = player->mo;
    int		heal;
    if (!mo) return false;
    if      (player->inventory[arti_medikit]  > 0) { player->inventory[arti_medikit]--;  heal = 25; }
    else if (player->inventory[arti_stimpack] > 0) { player->inventory[arti_stimpack]--; heal = 10; }
    else return false;

    mo->flags |=  (MF_SOLID | MF_SHOOTABLE);
    mo->flags &= ~MF_CORPSE;			// un-corpse; KEEP MF_DROPOFF -- the player
						// natively has it (lets it drop off ledges); clearing
						// it gave the player the monster "no dropoff" block.
    mo->height = mo->info->height;		// un-squash the corpse
    mo->health = heal;
    player->health = heal;
    player->playerstate = PST_LIVE;
    player->damagecount = 0;
    player->attacker = NULL;
    player->viewheight = VIEWHEIGHT;
    player->deltaviewheight = 0;
    P_SetMobjState (mo, mo->info->spawnstate);	// stand up (S_PLAY)
    player->pendingweapon = player->readyweapon;	// raise the weapon again
    player->message = "PATCHED YOURSELF UP -- back in the fight!";
    if (player->invslot != arti_none && player->inventory[player->invslot] <= 0)
	P_InvScroll (player, +1);
    return true;
}
