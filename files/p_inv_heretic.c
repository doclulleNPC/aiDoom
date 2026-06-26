// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	(H) Heretic artifact inventory -- effects + pickup actors.
//
//	Eight Heretic artifacts share the generic player_t.inventory[] list (the
//	same array the DOOM "overflow" inventory in p_invent.c drives), but their
//	EFFECTS and on-floor pickup items live here, as a separate additive module
//	(same mechanism as files/revmarine.c / files/hexen.c -- enum slots at the
//	end of statenum_t/mobjtype_t, tables filled at runtime in HereticInv_Init).
//
//	Effect values are ported from crispy-doom's heretic/{p_user.c,p_inter.c}:
//	  flask  +25 HP        urn   +100 HP
//	  tome   -> Berserk    torch  pw_infrared (INFRATICS = 120s)
//	  ring   pw_invulnerability (INVULNTICS = 30s)
//	  shadow pw_invisibility (INVISTICS = 60s) + MF_SHADOW
//	  chaos  teleport to the player start (fog + sfx_telept)
//	  bomb   spawn a fusing Time Bomb (A_Explode, radius 128)
//
//	Sprites come from hereticstuff.wad (PTN1/SPHL/PWBK/TRCH/FBMB/INVU/INVS/ATLP,
//	the 4-char Heretic codes, registered in info.h/info.c).  The EFFECTS work
//	without the wad (console-give); only the on-floor pickup icon needs it, so
//	the pickup actors are gated by HereticInv_Available (sprite presence).
//
//	Deferred: Wings of Wrath (flight) and Morph Ovum (chicken) -- no subsystem.
//
//-----------------------------------------------------------------------------

#include "doomdef.h"
#include "doomstat.h"			// players[], consoleplayer, playerstarts[]
#include "info.h"
#include "m_fixed.h"
#include "tables.h"
#include "sounds.h"
#include "s_sound.h"
#include "p_local.h"
#include "r_state.h"			// sprites[] -- presence test by parsed sprite
#include "p_inv_heretic.h"

extern state_t		states[];
extern mobjinfo_t	mobjinfo[];

// engine pieces we call (declared by hand, like hexen.c/revmarine.c)
extern boolean	P_GiveBody (player_t* player, int num);
extern void	A_Scream (mobj_t*);
extern void	A_Explode (mobj_t*);
extern void	P_SpawnPlayerMissile (mobj_t* source, mobjtype_t type);

#define BRIGHT		32768		// FF_FULLBRIGHT frame bit


// ---------------------------------------------------------------------------
// Effects (ApplyHereticArtifact): return true if the artifact was consumed.
// ---------------------------------------------------------------------------
boolean ApplyHereticArtifact (player_t* player, artitype_t a)
{
    mobj_t*	mo = player->mo;

    switch (a)
    {
      case h_arti_flask:		// Quartz Flask: +25 HP, refuse at full.
	if (!P_GiveBody (player, 25))
	{
	    player->message = "CANNOT USE QUARTZ FLASK (FULL HEALTH)";
	    return false;
	}
	player->message = "USED QUARTZ FLASK";
	return true;

      case h_arti_urn:			// Mystic Urn: +100 HP, refuse at full.
	if (!P_GiveBody (player, 100))
	{
	    player->message = "CANNOT USE MYSTIC URN (FULL HEALTH)";
	    return false;
	}
	player->message = "USED MYSTIC URN";
	return true;

      case h_arti_tome:
	// Tome of Power: DOOM has no powered weapons, so the analog is a Berserk
	// pack -- give pw_strength + heal to 100 (mirrors the DOOM SPR_PSTR case).
	// (Simplification; a true Tome would temporarily upgrade weapons.)
	player->powers[pw_strength] = 1;
	P_GiveBody (player, 100);
	if (player->readyweapon != wp_fist)
	    player->pendingweapon = wp_fist;
	player->message = "USED TOME OF POWER (BERSERK)";
	return true;

      case h_arti_torch:		// Torch: light-amp goggles for 120s.
	player->powers[pw_infrared] = INFRATICS;
	player->message = "USED TORCH";
	return true;

      case h_arti_ring:			// Ring of Invincibility (30s).
	player->powers[pw_invulnerability] = INVULNTICS;
	player->message = "USED RING OF INVINCIBILITY";
	return true;

      case h_arti_shadow:		// Shadowsphere: invisibility (60s) + MF_SHADOW.
	player->powers[pw_invisibility] = INVISTICS;
	if (mo) mo->flags |= MF_SHADOW;
	player->message = "USED SHADOWSPHERE";
	return true;

      case h_arti_chaos:
      {
	// Chaos Device: teleport to this player's map start, with teleport fog +
	// sound at both ends and zeroed momentum (reimplemented from p_invent.c
	// history; mirrors the engine teleporter in p_telept.c).
	mapthing_t* start;
	fixed_t	    oldx, oldy, oldz;
	mobj_t*	    fog;
	unsigned    an;

	if (!mo) { player->message = "CANNOT USE CHAOS DEVICE"; return false; }
	start = &playerstarts[consoleplayer];
	oldx = mo->x; oldy = mo->y; oldz = mo->z;
	if (!P_TeleportMove (mo, start->x << FRACBITS, start->y << FRACBITS))
	{
	    player->message = "CANNOT USE CHAOS DEVICE (BLOCKED)";
	    return false;
	}
	mo->z = mo->floorz;
	player->viewz = mo->z + player->viewheight;

	fog = P_SpawnMobj (oldx, oldy, oldz, MT_TFOG);		// departure
	S_StartSound (fog, sfx_telept);
	an  = (ANG45 * (start->angle / 45)) >> ANGLETOFINESHIFT;
	fog = P_SpawnMobj (mo->x + 20*finecosine[an],		// arrival
			   mo->y + 20*finesine[an], mo->z, MT_TFOG);
	S_StartSound (fog, sfx_telept);

	mo->angle = ANG45 * (start->angle / 45);
	mo->momx = mo->momy = mo->momz = 0;
	player->message = "USED CHAOS DEVICE";
	return true;
      }

      case h_arti_bomb:
      {
	// Time Bomb of the Ancients: drop a fusing bomb at the player's feet.  Its
	// ->target is the player so the kills (and the splash damage) attribute the
	// same as a real bomb / rocket blast.
	mobj_t* bomb;
	if (!mo) { player->message = "CANNOT USE TIME BOMB"; return false; }
	bomb = P_SpawnMobj (mo->x, mo->y, mo->z, MT_HFIREBOMB);
	if (bomb)
	    bomb->target = mo;
	player->message = "USED TIME BOMB OF THE ANCIENTS";
	return true;
      }

      case h_arti_wings:
	// Wings of Wrath: grant the GENERIC timed flight power (p_user.c does the
	// float/climb/descend; this just sets the timer, so any inventory can do it).
	player->powers[pw_flight] = FLIGHTTICS;
	if (mo) mo->flags |= MF_NOGRAVITY;	// take off immediately
	player->message = "USED WINGS OF WRATH";
	return true;

      case h_arti_egg:
	// Morph Ovum: fire an egg missile (MT_HEGGFX).  On impact P_DamageMobj
	// morphs the struck monster into a chicken (generic morph subsystem).
	// P_SpawnPlayerMissile is void here, so we fire the single straight shot
	// (crispy's 4 spread eggs need a P_SPMAngle we don't have; one is fine).
	if (!mo) { player->message = "CANNOT USE MORPH OVUM"; return false; }
	P_SpawnPlayerMissile (mo, MT_HEGGFX);
	player->message = "USED MORPH OVUM";
	return true;

      default:
	return false;
    }
}


// ---------------------------------------------------------------------------
// Table fill
// ---------------------------------------------------------------------------
static void ST (statenum_t s, spritenum_t spr, int frame, int tics,
		actionf_p1 act, statenum_t next)
{
    states[s].sprite      = spr;
    states[s].frame       = frame;
    states[s].tics        = tics;
    states[s].action.acp1 = act;
    states[s].nextstate   = next;
    states[s].misc1 = states[s].misc2 = 0;
}

// Fill a console-give-only pickup actor: a single looping spinning-icon state.
static void PickupItem (mobjtype_t mt, statenum_t spawn)
{
    mobjinfo_t*	m = &mobjinfo[mt];
    m->doomednum   = -1;			// NOT map-placed (would collide with DOOM)
    m->spawnstate  = spawn;  m->spawnhealth = 1000;
    m->seestate    = S_NULL; m->seesound    = sfx_None; m->reactiontime = 8;
    m->attacksound = sfx_None; m->painstate = S_NULL;   m->painchance = 0;
    m->painsound   = sfx_None; m->meleestate = S_NULL;  m->missilestate = S_NULL;
    m->deathstate  = S_NULL; m->xdeathstate = S_NULL;   m->deathsound = sfx_None;
    m->speed = 0; m->radius = 16*FRACUNIT; m->height = 16*FRACUNIT; m->mass = 100;
    m->damage = 0; m->activesound = sfx_None;
    m->flags = MF_SPECIAL;			// picked up on touch
    m->raisestate = S_NULL;
}

void HereticInv_Init (void)
{
    mobjinfo_t*	m;

    // ---- Pickup spinning-icon states (FULLBRIGHT, looping single frame) ----
    ST (S_HARTI_FLASK,  SPR_PTN1, BRIGHT|0, 6, NULL, S_HARTI_FLASK);
    ST (S_HARTI_URN,    SPR_SPHL, BRIGHT|0, 6, NULL, S_HARTI_URN);
    ST (S_HARTI_TOME,   SPR_PWBK, BRIGHT|0, 6, NULL, S_HARTI_TOME);
    ST (S_HARTI_TORCH,  SPR_TRCH, BRIGHT|0, 6, NULL, S_HARTI_TORCH);
    ST (S_HARTI_BOMB,   SPR_FBMB, BRIGHT|0, 6, NULL, S_HARTI_BOMB);
    ST (S_HARTI_RING,   SPR_INVU, BRIGHT|0, 6, NULL, S_HARTI_RING);
    ST (S_HARTI_SHADOW, SPR_INVS, BRIGHT|0, 6, NULL, S_HARTI_SHADOW);
    ST (S_HARTI_CHAOS,  SPR_ATLP, BRIGHT|0, 6, NULL, S_HARTI_CHAOS);
    ST (S_HARTI_WINGS,  SPR_SOAR, BRIGHT|0, 6, NULL, S_HARTI_WINGS);
    ST (S_HARTI_EGG,    SPR_EGGC, BRIGHT|0, 6, NULL, S_HARTI_EGG);

    PickupItem (MT_HARTI_FLASK,  S_HARTI_FLASK);
    PickupItem (MT_HARTI_URN,    S_HARTI_URN);
    PickupItem (MT_HARTI_TOME,   S_HARTI_TOME);
    PickupItem (MT_HARTI_TORCH,  S_HARTI_TORCH);
    PickupItem (MT_HARTI_BOMB,   S_HARTI_BOMB);
    PickupItem (MT_HARTI_RING,   S_HARTI_RING);
    PickupItem (MT_HARTI_SHADOW, S_HARTI_SHADOW);
    PickupItem (MT_HARTI_CHAOS,  S_HARTI_CHAOS);
    PickupItem (MT_HARTI_WINGS,  S_HARTI_WINGS);
    PickupItem (MT_HARTI_EGG,    S_HARTI_EGG);

    // ---- Time Bomb of the Ancients actor (crispy MT_FIREBOMB) ----
    // A few FBMB fuse frames (A_Scream just before the boom), then A_Explode
    // (engine A_Explode = P_RadiusAttack(thing, thing->target, 128)).  SPR_XPL1
    // isn't extracted, so the boom reuses the DOOM barrel-blast sprite SPR_BEXP.
    ST (S_HFIREBOMB1,  SPR_FBMB, 0,        10, NULL,                    S_HFIREBOMB2);
    ST (S_HFIREBOMB2,  SPR_FBMB, 1,        10, NULL,                    S_HFIREBOMB3);
    ST (S_HFIREBOMB3,  SPR_FBMB, 2,        10, NULL,                    S_HFIREBOMB4);
    ST (S_HFIREBOMB4,  SPR_FBMB, 3,        10, NULL,                    S_HFIREBOMB5);
    ST (S_HFIREBOMB5,  SPR_FBMB, 4,         6, (actionf_p1)A_Scream,    S_HFIREBOMB6);
    ST (S_HFIREBOMB6,  SPR_BEXP, BRIGHT|0,  4, (actionf_p1)A_Explode,   S_HFIREBOMB7);
    ST (S_HFIREBOMB7,  SPR_BEXP, BRIGHT|1,  4, NULL,                    S_HFIREBOMB8);
    ST (S_HFIREBOMB8,  SPR_BEXP, BRIGHT|2,  4, NULL,                    S_HFIREBOMB9);
    ST (S_HFIREBOMB9,  SPR_BEXP, BRIGHT|3,  4, NULL,                    S_HFIREBOMB10);
    ST (S_HFIREBOMB10, SPR_BEXP, BRIGHT|4,  4, NULL,                    S_NULL);

    m = &mobjinfo[MT_HFIREBOMB];
    m->doomednum   = -1;
    m->spawnstate  = S_HFIREBOMB1; m->spawnhealth = 1000;
    m->seestate    = S_NULL;  m->seesound    = sfx_None; m->reactiontime = 8;
    m->attacksound = sfx_None; m->painstate  = S_NULL;   m->painchance = 0;
    m->painsound   = sfx_None; m->meleestate = S_NULL;   m->missilestate = S_NULL;
    m->deathstate  = S_NULL;  m->xdeathstate = S_NULL;   m->deathsound = sfx_None;
    m->speed = 0; m->radius = 8*FRACUNIT; m->height = 16*FRACUNIT; m->mass = 100;
    m->damage = 0; m->activesound = sfx_None;
    m->flags = MF_NOGRAVITY;			// sits where dropped, harmless until A_Explode
    m->raisestate = S_NULL;

    // ---- Morph Ovum egg projectile (crispy MT_EGGFX) ----
    // A small no-gravity missile that does 0 damage; the morph happens in
    // P_DamageMobj when inflictor->type == MT_HEGGFX (p_inter.c).  Reuses the
    // EGGC pickup sprite for the in-flight frames; no special boom (explodes to
    // S_NULL, which P_ExplodeMissile chains to harmlessly).
    ST (S_HEGGFX1, SPR_EGGC, BRIGHT|0, 4, NULL, S_HEGGFX2);
    ST (S_HEGGFX2, SPR_EGGC, BRIGHT|0, 4, NULL, S_HEGGFX3);
    ST (S_HEGGFX3, SPR_EGGC, BRIGHT|0, 4, NULL, S_HEGGFX4);
    ST (S_HEGGFX4, SPR_EGGC, BRIGHT|0, 4, NULL, S_HEGGFX5);
    ST (S_HEGGFX5, SPR_EGGC, BRIGHT|0, 4, NULL, S_HEGGFX1);

    m = &mobjinfo[MT_HEGGFX];
    m->doomednum   = -1;
    m->spawnstate  = S_HEGGFX1; m->spawnhealth = 1000;
    m->seestate    = S_NULL;  m->seesound    = sfx_None; m->reactiontime = 8;
    m->attacksound = sfx_None; m->painstate  = S_NULL;   m->painchance = 0;
    m->painsound   = sfx_None; m->meleestate = S_NULL;   m->missilestate = S_NULL;
    m->deathstate  = S_NULL;  m->xdeathstate = S_NULL;   m->deathsound = sfx_None;
    m->speed = 18*FRACUNIT; m->radius = 8*FRACUNIT; m->height = 8*FRACUNIT; m->mass = 100;
    m->damage = 0; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY;
    m->raisestate = S_NULL;
}


// hereticstuff.wad's artifact sprites loaded?  Test the PARSED sprite, not a lump.
int HereticInv_Available (void)
{
    return numsprites > SPR_PTN1 && sprites[SPR_PTN1].numframes > 0;
}


// ---------------------------------------------------------------------------
// Pocket a placed/spawned MT_HARTI_* artifact (called from P_TouchSpecialThing).
// ---------------------------------------------------------------------------
boolean P_TouchHereticArtifact (player_t* player, mobj_t* special)
{
    artitype_t	a;

    switch (special->type)
    {
      case MT_HARTI_FLASK:  a = h_arti_flask;  player->message = "PICKED UP A QUARTZ FLASK";  break;
      case MT_HARTI_URN:    a = h_arti_urn;    player->message = "PICKED UP A MYSTIC URN";    break;
      case MT_HARTI_TOME:   a = h_arti_tome;   player->message = "PICKED UP A TOME OF POWER"; break;
      case MT_HARTI_TORCH:  a = h_arti_torch;  player->message = "PICKED UP A TORCH";         break;
      case MT_HARTI_BOMB:   a = h_arti_bomb;   player->message = "PICKED UP A TIME BOMB";     break;
      case MT_HARTI_RING:   a = h_arti_ring;   player->message = "PICKED UP A RING OF INVINCIBILITY"; break;
      case MT_HARTI_SHADOW: a = h_arti_shadow; player->message = "PICKED UP A SHADOWSPHERE";  break;
      case MT_HARTI_CHAOS:  a = h_arti_chaos;  player->message = "PICKED UP A CHAOS DEVICE";  break;
      case MT_HARTI_WINGS:  a = h_arti_wings;  player->message = "PICKED UP THE WINGS OF WRATH"; break;
      case MT_HARTI_EGG:    a = h_arti_egg;    player->message = "PICKED UP A MORPH OVUM";    break;
      default:
	return false;				// not ours
    }

    if (player->inventory[a] < MAXARTICOUNT)
	player->inventory[a]++;
    if (player->invslot == arti_none)
	player->invslot = a;
    return true;
}
