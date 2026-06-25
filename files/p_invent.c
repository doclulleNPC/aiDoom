// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	(J) Simple Heretic-style artifact inventory.
//
//	Three collectible / stackable / usable artifacts:
//	  - Quartz Flask : +25 HP, capped at 100 (like Heretic).
//	  - Chaos Device : teleport back to this level's player-1 start spot,
//	                   with the teleport fog + sound.
//	  - Torch        : temporary infrared-style brightening (reuses the
//	                   pw_infrared powerup mechanism).
//
//	The pickup mobjtypes (MT_ARTI_*) reuse existing DOOM sprites as
//	placeholder icons and are appended to the engine state/mobjinfo tables
//	at runtime (Invent_Init), the same additive mechanism as
//	files/revmarine.c.  Held counts + the selected slot live in player_t
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

extern state_t		states[];
extern mobjinfo_t	mobjinfo[];

// the level's player start spots (p_setup.c); player 1's is index consoleplayer.
extern mapthing_t	playerstarts[MAXPLAYERS];

extern boolean		P_TeleportMove (mobj_t* thing, fixed_t x, fixed_t y);
extern mobj_t*		P_SpawnMobj (fixed_t x, fixed_t y, fixed_t z, mobjtype_t type);

#define ARTI_TORCHTICS	(20*TICRATE)	// ~20s of infrared light per Torch


// One spawnstate per artifact: a static (-1 tic) frame showing a reused DOOM
// sprite.  No animation -- these are simple floor pickups.
static void ST (statenum_t s, spritenum_t spr, int frame, statenum_t next)
{
    states[s].sprite      = spr;
    states[s].frame       = frame;
    states[s].tics        = -1;
    states[s].action.acp1 = NULL;
    states[s].nextstate   = next;
    states[s].misc1 = states[s].misc2 = 0;
}

static void MakeItem (mobjtype_t t, statenum_t spawn)
{
    mobjinfo_t* m = &mobjinfo[t];
    m->doomednum   = -1;			// not map-placed by default; given via console/pickup
    m->spawnstate  = spawn;  m->spawnhealth = 1000;
    m->seestate    = S_NULL; m->seesound    = 0;  m->reactiontime = 8;
    m->attacksound = 0;      m->painstate   = S_NULL; m->painchance = 0;
    m->painsound   = 0;      m->meleestate  = S_NULL; m->missilestate = S_NULL;
    m->deathstate  = S_NULL; m->xdeathstate = S_NULL; m->deathsound = 0;
    m->speed = 0;  m->radius = 20*FRACUNIT;  m->height = 16*FRACUNIT;  m->mass = 100;
    m->damage = 0; m->activesound = 0;
    m->flags = MF_SPECIAL;			// touched -> P_TouchSpecialThing
    m->raisestate = S_NULL;
}

void Invent_Init (void)
{
    // Placeholder icons (Heretic artifact art isn't in any wad): flask -> the
    // stimpack sprite, chaos device -> the invisibility-sphere sprite, torch ->
    // the tall techlamp.  The 32768 bit is FF_FULLBRIGHT.
    ST (S_ARTI_FLASK, SPR_STIM, 0,         S_NULL);
    ST (S_ARTI_CHAOS, SPR_PINS, 32768,     S_NULL);
    ST (S_ARTI_TORCH, SPR_TLMP, 32768,     S_NULL);

    MakeItem (MT_ARTI_FLASK, S_ARTI_FLASK);
    MakeItem (MT_ARTI_CHAOS, S_ARTI_CHAOS);
    MakeItem (MT_ARTI_TORCH, S_ARTI_TORCH);
}


const char* P_ArtifactName (artitype_t a)
{
    switch (a)
    {
      case arti_flask:		return "Quartz Flask";
      case arti_chaosdevice:	return "Chaos Device";
      case arti_torch:		return "Torch";
      default:			return "";
    }
}


// ---------------------------------------------------------------------------
// Pickup: map a touched item's mobjtype to an artifact slot and pocket it.
// Returns true if it was an artifact (caller removes the mobj + plays sound).
// ---------------------------------------------------------------------------
boolean P_TouchArtifact (player_t* player, mobj_t* special)
{
    artitype_t a;

    switch (special->type)
    {
      case MT_ARTI_FLASK:	a = arti_flask;       break;
      case MT_ARTI_CHAOS:	a = arti_chaosdevice; break;
      case MT_ARTI_TORCH:	a = arti_torch;       break;
      default:			return false;
    }

    if (player->inventory[a] < MAXARTICOUNT)
	player->inventory[a]++;
    // Auto-select the first artifact picked up so the use key works right away.
    if (player->invslot == arti_none)
	player->invslot = a;

    {
	static char msg[64];
	snprintf (msg, sizeof msg, "PICKED UP %s", P_ArtifactName (a));
	player->message = msg;
    }
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
// Apply an artifact's effect to the player.  Returns true if it was consumed.
// ---------------------------------------------------------------------------
static boolean ApplyArtifact (player_t* player, artitype_t a)
{
    mobj_t*	mo = player->mo;

    switch (a)
    {
      case arti_flask:
	if (player->health >= MAXHEALTH)
	{
	    player->message = "YOU ARE ALREADY AT FULL HEALTH";
	    return false;			// don't waste it
	}
	player->health += 25;
	if (player->health > MAXHEALTH) player->health = MAXHEALTH;
	if (mo) mo->health = player->health;
	player->message = "QUARTZ FLASK -- HEALED";
	return true;

      case arti_chaosdevice:
      {
	mapthing_t*	start = &playerstarts[consoleplayer];
	fixed_t		nx = start->x << FRACBITS;
	fixed_t		ny = start->y << FRACBITS;
	fixed_t		ox, oy, oz;
	angle_t		an;
	if (!mo) return false;
	ox = mo->x; oy = mo->y; oz = mo->z;
	if (!P_TeleportMove (mo, nx, ny))
	{
	    player->message = "CHAOS DEVICE -- START BLOCKED";
	    return false;
	}
	// Teleport fog + sound at the departure and arrival spots (like p_telept.c).
	S_StartSound (P_SpawnMobj (ox, oy, oz, MT_TFOG), sfx_telept);
	an = ANG45 * (start->angle / 45);
	mo->angle = an;
	an >>= ANGLETOFINESHIFT;
	S_StartSound (P_SpawnMobj (mo->x + 20*finecosine[an],
				   mo->y + 20*finesine[an], mo->z, MT_TFOG),
		      sfx_telept);
	mo->momx = mo->momy = mo->momz = 0;
	player->message = "CHAOS DEVICE -- RECALLED TO START";
	return true;
      }

      case arti_torch:
	player->powers[pw_infrared] = ARTI_TORCHTICS;	// drives fixedcolormap in P_PlayerThink
	player->message = "TORCH -- LIT";
	return true;

      default:
	return false;
    }
}


// ---------------------------------------------------------------------------
// USE the selected artifact (or `which` when given explicitly, e.g. for tests).
// Consumes one of it on success.  Always sets player->message.
// ---------------------------------------------------------------------------
boolean P_UseArtifact (player_t* player, artitype_t which)
{
    artitype_t a = (which != arti_none) ? which : (artitype_t) player->invslot;

    if (a <= arti_none || a >= NUMARTIFACTS || player->inventory[a] <= 0)
    {
	player->message = "YOU HAVE NO ARTIFACT TO USE";
	return false;
    }

    if (!ApplyArtifact (player, a))
	return false;				// effect refused (message already set)

    player->inventory[a]--;
    // If that was the last one, hop to another held artifact (or empty).
    if (player->inventory[a] <= 0)
	P_InvScroll (player, +1);
    return true;
}
