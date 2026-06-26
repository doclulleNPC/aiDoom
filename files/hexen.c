// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Additive Hexen monsters in the DOOM engine (HERETIC_HEXEN.md approach A) --
//	the same mechanism as files/heretic.c.  States/mobjinfo are appended to the
//	engine tables at runtime (Hexen_Init) so info.c's generated initializers stay
//	untouched; the enum slots live at the end of statenum_t/mobjtype_t/spritenum_t
//	(info.h).  Sprites: hexenstuff.wad (renamed X*, by tools/extract_hexen.py;
//	see tools/hexen_sprite_map.txt).  Sounds: DOOM SFX reused for now.
//
//	First monster: Ettin (a club-wielding melee brute).  More follow the pattern.
//
//-----------------------------------------------------------------------------

#include <string.h>

#include "doomdef.h"
#include "info.h"
#include "m_random.h"
#include "m_fixed.h"
#include "sounds.h"
#include "w_wad.h"
#include "p_mobj.h"
#include "r_state.h"		// sprites[] -- presence test by parsed sprite, not lump name
#include "hexen.h"

extern state_t		states[];
extern mobjinfo_t	mobjinfo[];

// engine action funcs we call (no public header -- declare by hand)
extern void	A_Look (mobj_t*);
extern void	A_Chase (mobj_t*);
extern void	A_FaceTarget (mobj_t*);
extern void	A_Pain (mobj_t*);
extern void	A_Scream (mobj_t*);
extern void	A_Fall (mobj_t*);
extern boolean	P_CheckMeleeRange (mobj_t*);
extern void	P_DamageMobj (mobj_t* target, mobj_t* inflictor, mobj_t* source, int damage);
extern void	S_StartSound (void* origin, int sfx_id);
extern mobj_t*	P_SpawnMobj (fixed_t x, fixed_t y, fixed_t z, mobjtype_t type);
extern boolean	P_SetMobjState (mobj_t* mobj, statenum_t state);
extern mobj_t*	P_SpawnMonsterChecked (fixed_t x, fixed_t y, mobjtype_t type);
extern mobj_t*	P_SpawnMissile (mobj_t* source, mobj_t* dest, mobjtype_t type);

// Hexen's HITDICE(d) melee damage = ((P_Random() & 7) + 1) * d.
#define HITDICE(d)	(((P_Random () & 7) + 1) * (d))

// ---------------------------------------------------------------------------
// Action functions (crispy hexen/p_enemy.c, adapted to DOOM's 1-arg signature).
// ---------------------------------------------------------------------------

// Ettin: pure melee, HITDICE(2) = 2..16.
void A_EttinAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    if (P_CheckMeleeRange (actor))
	P_DamageMobj (actor->target, actor, actor, HITDICE (2));
}

// Centaur: pure melee swipe, P_Random()%7 + 3 = 3..9 (crispy A_CentaurAttack).
void A_CentaurAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    if (P_CheckMeleeRange (actor))
	P_DamageMobj (actor->target, actor, actor, P_Random () % 7 + 3);
}

// Slaughtaur ranged: lob a (here non-reflecting, simplified) bolt
// (crispy A_CentaurAttack2 fires MT_CENTAUR_FX).
void A_CentaurAttack2 (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    P_SpawnMissile (actor, actor->target, MT_XCENTAUR_FX);
}

// Chaos Serpent melee (crispy A_DemonAttack1), HITDICE(2) = 2..16.
void A_DemonAttack1 (mobj_t* actor)
{
    if (!actor->target)
	return;
    if (P_CheckMeleeRange (actor))
	P_DamageMobj (actor->target, actor, actor, HITDICE (2));
}

// Chaos Serpent fire-breath (crispy A_DemonAttack2); spawn the fireball a bit high.
void A_DemonAttack2 (mobj_t* actor)
{
    mobj_t* mo;
    if (!actor->target)
	return;
    mo = P_SpawnMissile (actor, actor->target, MT_XDEMON_FX);
    if (mo)
    {
	mo->z += 30*FRACUNIT;
	S_StartSound (actor, actor->info->attacksound);
    }
}

// Fire Demon / Afrit ranged fireball (crispy A_FiredAttack; rock-throw variants
// simplified away -- it just lobs the MT_XFIREDEMON_FX missile).
void A_FiredAttack (mobj_t* actor)
{
    mobj_t* mo;
    if (!actor->target)
	return;
    mo = P_SpawnMissile (actor, actor->target, MT_XFIREDEMON_FX);
    if (mo)
	S_StartSound (actor, actor->info->attacksound);
}

// Reiver / Wraith melee: drains health (crispy A_WraithMelee).  HITDICE(2)=2..16.
void A_WraithMelee (mobj_t* actor)
{
    int amount;
    if (!actor->target)
	return;
    if (P_CheckMeleeRange (actor) && (P_Random () < 220))
    {
	amount = HITDICE (2);
	P_DamageMobj (actor->target, actor, actor, amount);
	actor->health += amount;	// steal life
    }
}

// Reiver / Wraith ranged bolt (crispy A_WraithMissile).
void A_WraithMissile (mobj_t* actor)
{
    if (!actor->target)
	return;
    if (P_SpawnMissile (actor, actor->target, MT_XWRAITH_FX))
	S_StartSound (actor, actor->info->attacksound);
}

// Dark Bishop attack (crispy A_BishopAttack/A_BishopAttack2 merged + simplified:
// no special1 burst counter, no homing -- melee swing in range, else a plain
// (non-seeking) missile).  HITDICE(4) = 4..32 in melee.
void A_BishopAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    if (P_CheckMeleeRange (actor))
    {
	P_DamageMobj (actor->target, actor, actor, HITDICE (4));
	return;
    }
    P_SpawnMissile (actor, actor->target, MT_XBISHOP_FX);
}

// Wendigo / Ice Guy ranged ice shard (crispy A_IceGuyAttack; the symmetric
// dual-missile fan + wisp spawns are simplified to a single straight shard).
void A_IceGuyAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    if (P_SpawnMissile (actor, actor->target, MT_XICEGUY_FX))
	S_StartSound (actor, actor->info->attacksound);
}

// Ice Guy death = the Hexen ICE SHATTER (crispy A_IceGuyDie/A_FreezeDeathChunks, simplified):
// the floater bursts into a scatter of the shard's ice-shatter puffs and then vanishes (it
// leaves no corpse).  Fixes the death that used to freeze on one frame floating mid-air.
void A_IceGuyShatter (mobj_t* actor)
{
    int		i;
    int		hsteps = (actor->height >> FRACBITS) > 1 ? (actor->height >> FRACBITS) : 1;
    actor->momx = actor->momy = actor->momz = 0;
    for (i = 0; i < 8; i++)
    {
	fixed_t	dx = (P_Random () - 128) * (actor->radius >> 7);
	fixed_t	dy = (P_Random () - 128) * (actor->radius >> 7);
	fixed_t	dz = (fixed_t)(P_Random () % hsteps) << FRACBITS;
	mobj_t*	sh = P_SpawnMobj (actor->x + dx, actor->y + dy, actor->z + dz, MT_XICEGUY_FX);
	if (sh)
	{
	    sh->momx = sh->momy = sh->momz = 0;
	    sh->flags &= ~MF_MISSILE;			// harmless ice debris, not an attack
	    P_SetMobjState (sh, S_XICP_BOOM1);		// the ice-shatter puff frames
	}
    }
}

// Stalker / Serpent melee (crispy A_SerpentMeleeAttack; the re-check-for-attack
// chain dropped).  HITDICE(5) = 5..40.
void A_StalkerMelee (mobj_t* actor)
{
    if (!actor->target)
	return;
    if (P_CheckMeleeRange (actor))
    {
	P_DamageMobj (actor->target, actor, actor, HITDICE (5));
	S_StartSound (actor, actor->info->attacksound);
    }
}

// Stalker / Serpent spit (crispy A_SerpentMissileAttack).
void A_StalkerMissile (mobj_t* actor)
{
    if (!actor->target)
	return;
    if (P_SpawnMissile (actor, actor->target, MT_XSTALKER_FX))
	S_StartSound (actor, actor->info->attacksound);
}

// Death Wyvern / Dragon fireball (crispy A_DragonAttack; the homing FX2 trails
// are simplified to a single straight fireball).
void A_DragonAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    if (P_SpawnMissile (actor, actor->target, MT_XDRAGON_FX))
	S_StartSound (actor, actor->info->attacksound);
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

void Hexen_Init (void)
{
    mobjinfo_t*	m;

    // ---- Ettin (crispy S_ETTIN_*; one death sequence, ice/mace deaths omitted) ----
    ST (S_XETT_LOOK1,  SPR_XETT,  0, 10, (actionf_p1)A_Look,        S_XETT_LOOK2);
    ST (S_XETT_LOOK2,  SPR_XETT,  0, 10, (actionf_p1)A_Look,        S_XETT_LOOK1);
    ST (S_XETT_CHASE1, SPR_XETT,  0,  5, (actionf_p1)A_Chase,       S_XETT_CHASE2);
    ST (S_XETT_CHASE2, SPR_XETT,  1,  5, (actionf_p1)A_Chase,       S_XETT_CHASE3);
    ST (S_XETT_CHASE3, SPR_XETT,  2,  5, (actionf_p1)A_Chase,       S_XETT_CHASE4);
    ST (S_XETT_CHASE4, SPR_XETT,  3,  5, (actionf_p1)A_Chase,       S_XETT_CHASE1);
    ST (S_XETT_PAIN1,  SPR_XETT,  7,  7, (actionf_p1)A_Pain,        S_XETT_CHASE1);
    ST (S_XETT_ATK1,   SPR_XETT,  4,  6, (actionf_p1)A_FaceTarget,  S_XETT_ATK2);
    ST (S_XETT_ATK2,   SPR_XETT,  5,  6, (actionf_p1)A_FaceTarget,  S_XETT_ATK3);
    ST (S_XETT_ATK3,   SPR_XETT,  6,  8, (actionf_p1)A_EttinAttack, S_XETT_CHASE1);
    ST (S_XETT_DIE1,   SPR_XETT,  8,  4, NULL,                      S_XETT_DIE2);
    ST (S_XETT_DIE2,   SPR_XETT,  9,  4, NULL,                      S_XETT_DIE3);
    ST (S_XETT_DIE3,   SPR_XETT, 10,  4, (actionf_p1)A_Scream,      S_XETT_DIE4);
    ST (S_XETT_DIE4,   SPR_XETT, 11,  4, (actionf_p1)A_Fall,        S_XETT_DIE5);
    ST (S_XETT_DIE5,   SPR_XETT, 12,  4, NULL,                      S_XETT_DIE6);
    ST (S_XETT_DIE6,   SPR_XETT, 13,  4, NULL,                      S_XETT_DIE7);
    ST (S_XETT_DIE7,   SPR_XETT, 14,  4, NULL,                      S_XETT_DIE8);
    ST (S_XETT_DIE8,   SPR_XETT, 15,  4, NULL,                      S_XETT_DIE9);
    ST (S_XETT_DIE9,   SPR_XETT, 16, -1, NULL,                      S_NULL);

    m = &mobjinfo[MT_XETTIN];
    m->doomednum = -1;        m->spawnstate  = S_XETT_LOOK1; m->spawnhealth = 175;
    m->seestate  = S_XETT_CHASE1; m->seesound  = sfx_bgsit1; m->reactiontime = 8;
    m->attacksound = sfx_claw;    m->painstate = S_XETT_PAIN1; m->painchance = 60;
    m->painsound = sfx_popain;    m->meleestate = S_XETT_ATK1; m->missilestate = S_NULL;
    m->deathstate = S_XETT_DIE1;  m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth1;
    m->speed = 13; m->radius = 25*FRACUNIT; m->height = 68*FRACUNIT; m->mass = 175;
    m->damage = 0; m->activesound = sfx_bgact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL; m->raisestate = S_NULL;

    // ---- Centaur / Slaughtaur (crispy S_CENTAUR_*; reflect-shield + ice/sword
    //      deaths simplified away -- one plain death sequence, like the Ettin) ----
    ST (S_XCEN_LOOK1, SPR_XCEN,  0, 10, (actionf_p1)A_Look,         S_XCEN_LOOK2);
    ST (S_XCEN_LOOK2, SPR_XCEN,  1, 10, (actionf_p1)A_Look,         S_XCEN_LOOK1);
    ST (S_XCEN_WALK1, SPR_XCEN,  0,  4, (actionf_p1)A_Chase,        S_XCEN_WALK2);
    ST (S_XCEN_WALK2, SPR_XCEN,  1,  4, (actionf_p1)A_Chase,        S_XCEN_WALK3);
    ST (S_XCEN_WALK3, SPR_XCEN,  2,  4, (actionf_p1)A_Chase,        S_XCEN_WALK4);
    ST (S_XCEN_WALK4, SPR_XCEN,  3,  4, (actionf_p1)A_Chase,        S_XCEN_WALK1);
    ST (S_XCEN_ATK1,  SPR_XCEN,  7,  5, (actionf_p1)A_FaceTarget,   S_XCEN_ATK2);
    ST (S_XCEN_ATK2,  SPR_XCEN,  8,  4, (actionf_p1)A_FaceTarget,   S_XCEN_ATK3);
    ST (S_XCEN_ATK3,  SPR_XCEN,  9,  7, (actionf_p1)A_CentaurAttack,S_XCEN_WALK1);
    ST (S_XCEN_MIS1,  SPR_XCEN,  4, 10, (actionf_p1)A_FaceTarget,   S_XCEN_MIS2);
    ST (S_XCEN_MIS2,  SPR_XCEN, 32773, 8,(actionf_p1)A_CentaurAttack2,S_XCEN_MIS3);
    ST (S_XCEN_MIS3,  SPR_XCEN,  4, 10, (actionf_p1)A_FaceTarget,   S_XCEN_MIS4);
    ST (S_XCEN_MIS4,  SPR_XCEN, 32773, 8,(actionf_p1)A_CentaurAttack2,S_XCEN_WALK1);
    ST (S_XCEN_PAIN1, SPR_XCEN,  6,  6, NULL,                       S_XCEN_PAIN2);
    ST (S_XCEN_PAIN2, SPR_XCEN,  6,  6, (actionf_p1)A_Pain,         S_XCEN_WALK1);
    ST (S_XCEN_DIE1,  SPR_XCEN, 10,  4, NULL,                       S_XCEN_DIE2);
    ST (S_XCEN_DIE2,  SPR_XCEN, 11,  4, (actionf_p1)A_Scream,       S_XCEN_DIE3);
    ST (S_XCEN_DIE3,  SPR_XCEN, 12,  4, NULL,                       S_XCEN_DIE4);
    ST (S_XCEN_DIE4,  SPR_XCEN, 13,  4, NULL,                       S_XCEN_DIE5);
    ST (S_XCEN_DIE5,  SPR_XCEN, 14,  4, (actionf_p1)A_Fall,         S_XCEN_DIE6);
    ST (S_XCEN_DIE6,  SPR_XCEN, 15,  4, NULL,                       S_XCEN_DIE7);
    ST (S_XCEN_DIE7,  SPR_XCEN, 16,  4, NULL,                       S_XCEN_DIE8);
    ST (S_XCEN_DIE8,  SPR_XCEN, 17,  4, NULL,                       S_XCEN_DIE9);
    ST (S_XCEN_DIE9,  SPR_XCEN, 18,  4, NULL,                       S_XCEN_DIE10);
    ST (S_XCEN_DIE10, SPR_XCEN, 19, -1, NULL,                       S_NULL);

    // Slaughtaur bolt projectile (crispy S_CENTAUR_FX*; reflection dropped).
    ST (S_XCTF_MOVE1, SPR_XCTF, 32768, -1, NULL,                    S_NULL);
    ST (S_XCTF_X1,    SPR_XCTF, 32769, 4, NULL,                     S_XCTF_X2);
    ST (S_XCTF_X2,    SPR_XCTF, 32770, 3, NULL,                     S_XCTF_X3);
    ST (S_XCTF_X3,    SPR_XCTF, 32771, 4, NULL,                     S_XCTF_X4);
    ST (S_XCTF_X4,    SPR_XCTF, 32772, 3, NULL,                     S_XCTF_X5);
    ST (S_XCTF_X5,    SPR_XCTF, 32773, 2, NULL,                     S_NULL);

    // Centaur: pure melee.  doomednum 107 in Hexen but -1 here (summon-only).
    m = &mobjinfo[MT_XCENTAUR];
    m->doomednum = -1;        m->spawnstate  = S_XCEN_LOOK1; m->spawnhealth = 200;
    m->seestate  = S_XCEN_WALK1; m->seesound  = sfx_bgsit2; m->reactiontime = 8;
    m->attacksound = sfx_claw;   m->painstate = S_XCEN_PAIN1; m->painchance = 135;
    m->painsound = sfx_popain;   m->meleestate = S_XCEN_ATK1; m->missilestate = S_NULL;
    m->deathstate = S_XCEN_DIE1; m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth2;
    m->speed = 13; m->radius = 20*FRACUNIT; m->height = 64*FRACUNIT; m->mass = 120;
    m->damage = 0; m->activesound = sfx_bgact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL; m->raisestate = S_NULL;

    // Slaughtaur: tougher, also lobs a bolt at range.
    m = &mobjinfo[MT_XSLAUGHTAUR];
    m->doomednum = -1;        m->spawnstate  = S_XCEN_LOOK1; m->spawnhealth = 250;
    m->seestate  = S_XCEN_WALK1; m->seesound  = sfx_bgsit2; m->reactiontime = 8;
    m->attacksound = sfx_claw;   m->painstate = S_XCEN_PAIN1; m->painchance = 96;
    m->painsound = sfx_popain;   m->meleestate = S_XCEN_ATK1; m->missilestate = S_XCEN_MIS1;
    m->deathstate = S_XCEN_DIE1; m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth2;
    m->speed = 10; m->radius = 20*FRACUNIT; m->height = 64*FRACUNIT; m->mass = 120;
    m->damage = 0; m->activesound = sfx_bgact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL; m->raisestate = S_NULL;

    // Slaughtaur bolt.
    m = &mobjinfo[MT_XCENTAUR_FX];
    m->doomednum = -1;        m->spawnstate  = S_XCTF_MOVE1; m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;  m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;    m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;   m->missilestate = S_NULL;
    m->deathstate = S_XCTF_X1;   m->xdeathstate = S_NULL;  m->deathsound = sfx_firxpl;
    m->speed = 20*FRACUNIT; m->radius = 20*FRACUNIT; m->height = 16*FRACUNIT; m->mass = 100;
    m->damage = 4; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;

    // ---- Chaos Serpent / Demon (crispy S_DEMN_*; gib XDeath simplified to the
    //      plain death so no chunk actors are needed) ----
    ST (S_XDEM_LOOK1,  SPR_XDEM,  0, 10, (actionf_p1)A_Look,        S_XDEM_LOOK2);
    ST (S_XDEM_LOOK2,  SPR_XDEM,  0, 10, (actionf_p1)A_Look,        S_XDEM_LOOK1);
    ST (S_XDEM_CHASE1, SPR_XDEM,  0,  4, (actionf_p1)A_Chase,       S_XDEM_CHASE2);
    ST (S_XDEM_CHASE2, SPR_XDEM,  1,  4, (actionf_p1)A_Chase,       S_XDEM_CHASE3);
    ST (S_XDEM_CHASE3, SPR_XDEM,  2,  4, (actionf_p1)A_Chase,       S_XDEM_CHASE4);
    ST (S_XDEM_CHASE4, SPR_XDEM,  3,  4, (actionf_p1)A_Chase,       S_XDEM_CHASE1);
    ST (S_XDEM_ATK1_1, SPR_XDEM,  4,  6, (actionf_p1)A_FaceTarget,  S_XDEM_ATK1_2);
    ST (S_XDEM_ATK1_2, SPR_XDEM,  5,  8, (actionf_p1)A_FaceTarget,  S_XDEM_ATK1_3);
    ST (S_XDEM_ATK1_3, SPR_XDEM,  6,  6, (actionf_p1)A_DemonAttack1,S_XDEM_CHASE1);
    ST (S_XDEM_ATK2_1, SPR_XDEM,  4,  5, (actionf_p1)A_FaceTarget,  S_XDEM_ATK2_2);
    ST (S_XDEM_ATK2_2, SPR_XDEM,  5,  6, (actionf_p1)A_FaceTarget,  S_XDEM_ATK2_3);
    ST (S_XDEM_ATK2_3, SPR_XDEM,  6,  5, (actionf_p1)A_DemonAttack2,S_XDEM_CHASE1);
    ST (S_XDEM_PAIN1,  SPR_XDEM,  4,  4, NULL,                      S_XDEM_PAIN2);
    ST (S_XDEM_PAIN2,  SPR_XDEM,  4,  4, (actionf_p1)A_Pain,        S_XDEM_CHASE1);
    ST (S_XDEM_DIE1,   SPR_XDEM,  7,  6, NULL,                      S_XDEM_DIE2);
    ST (S_XDEM_DIE2,   SPR_XDEM,  8,  6, NULL,                      S_XDEM_DIE3);
    ST (S_XDEM_DIE3,   SPR_XDEM,  9,  6, (actionf_p1)A_Scream,      S_XDEM_DIE4);
    ST (S_XDEM_DIE4,   SPR_XDEM, 10,  6, (actionf_p1)A_Fall,        S_XDEM_DIE5);
    ST (S_XDEM_DIE5,   SPR_XDEM, 11,  6, NULL,                      S_XDEM_DIE6);
    ST (S_XDEM_DIE6,   SPR_XDEM, 12,  6, NULL,                      S_XDEM_DIE7);
    ST (S_XDEM_DIE7,   SPR_XDEM, 13,  6, NULL,                      S_XDEM_DIE8);
    ST (S_XDEM_DIE8,   SPR_XDEM, 14,  6, NULL,                      S_XDEM_DIE9);
    ST (S_XDEM_DIE9,   SPR_XDEM, 15, -1, NULL,                      S_NULL);

    // Chaos Serpent fireball (crispy S_DEMONFX_*).
    ST (S_XDMF_MOVE1, SPR_XDMF, 32768, 4, NULL,                     S_XDMF_MOVE2);
    ST (S_XDMF_MOVE2, SPR_XDMF, 32769, 4, NULL,                     S_XDMF_MOVE3);
    ST (S_XDMF_MOVE3, SPR_XDMF, 32770, 4, NULL,                     S_XDMF_MOVE1);
    ST (S_XDMF_BOOM1, SPR_XDMF, 32771, 4, NULL,                     S_XDMF_BOOM2);
    ST (S_XDMF_BOOM2, SPR_XDMF, 32772, 4, NULL,                     S_XDMF_BOOM3);
    ST (S_XDMF_BOOM3, SPR_XDMF, 32773, 3, NULL,                     S_XDMF_BOOM4);
    ST (S_XDMF_BOOM4, SPR_XDMF, 32774, 3, NULL,                     S_XDMF_BOOM5);
    ST (S_XDMF_BOOM5, SPR_XDMF, 32775, 3, NULL,                     S_NULL);

    m = &mobjinfo[MT_XDEMON];
    m->doomednum = -1;        m->spawnstate  = S_XDEM_LOOK1; m->spawnhealth = 250;
    m->seestate  = S_XDEM_CHASE1; m->seesound = sfx_bgsit1; m->reactiontime = 8;
    m->attacksound = sfx_claw;   m->painstate = S_XDEM_PAIN1; m->painchance = 50;
    m->painsound = sfx_dmpain;   m->meleestate = S_XDEM_ATK1_1; m->missilestate = S_XDEM_ATK2_1;
    m->deathstate = S_XDEM_DIE1; m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth1;
    m->speed = 13; m->radius = 32*FRACUNIT; m->height = 64*FRACUNIT; m->mass = 220;
    m->damage = 0; m->activesound = sfx_dmact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL; m->raisestate = S_NULL;

    m = &mobjinfo[MT_XDEMON_FX];
    m->doomednum = -1;        m->spawnstate  = S_XDMF_MOVE1; m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;  m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;    m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;   m->missilestate = S_NULL;
    m->deathstate = S_XDMF_BOOM1; m->xdeathstate = S_NULL; m->deathsound = sfx_firxpl;
    m->speed = 15*FRACUNIT; m->radius = 10*FRACUNIT; m->height = 6*FRACUNIT; m->mass = 100;
    m->damage = 5; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;

    // ---- Fire Demon / Afrit (crispy S_FIRED_*; the multi-stage spawn/look ritual
    //      and rock-throw "split" deaths are simplified to a plain flyer that lobs
    //      a fireball, like a flying Slaughtaur).  Sprite frames are fullbright. ----
    ST (S_XFDM_LOOK1, SPR_XFDM, 32768, 10, (actionf_p1)A_Look,        S_XFDM_LOOK2);
    ST (S_XFDM_LOOK2, SPR_XFDM, 32769, 10, (actionf_p1)A_Look,        S_XFDM_LOOK3);
    ST (S_XFDM_LOOK3, SPR_XFDM, 32770, 10, (actionf_p1)A_Look,        S_XFDM_LOOK1);
    ST (S_XFDM_WALK1, SPR_XFDM, 32768,  5, (actionf_p1)A_Chase,       S_XFDM_WALK2);
    ST (S_XFDM_WALK2, SPR_XFDM, 32769,  5, (actionf_p1)A_Chase,       S_XFDM_WALK3);
    ST (S_XFDM_WALK3, SPR_XFDM, 32770,  5, (actionf_p1)A_Chase,       S_XFDM_WALK1);
    ST (S_XFDM_ATK1,  SPR_XFDM, 32778,  3, (actionf_p1)A_FaceTarget,  S_XFDM_ATK2);
    ST (S_XFDM_ATK2,  SPR_XFDM, 32778,  5, (actionf_p1)A_FiredAttack, S_XFDM_ATK3);
    ST (S_XFDM_ATK3,  SPR_XFDM, 32778,  5, (actionf_p1)A_FiredAttack, S_XFDM_ATK4);
    ST (S_XFDM_ATK4,  SPR_XFDM, 32778,  5, (actionf_p1)A_FiredAttack, S_XFDM_WALK1);
    ST (S_XFDM_PAIN1, SPR_XFDM, 32771,  6, (actionf_p1)A_Pain,        S_XFDM_WALK1);
    ST (S_XFDM_DIE1,  SPR_XFDM, 32771,  4, (actionf_p1)A_FaceTarget,  S_XFDM_DIE2);
    ST (S_XFDM_DIE2,  SPR_XFDM, 32779,  4, (actionf_p1)A_Scream,      S_XFDM_DIE3);
    ST (S_XFDM_DIE3,  SPR_XFDM, 32779,  4, (actionf_p1)A_Fall,        S_XFDM_DIE4);
    ST (S_XFDM_DIE4,  SPR_XFDM, 32779, -1, NULL,                      S_NULL);

    // Fire Demon fireball (crispy S_FIRED_FX6_*).
    ST (S_XFDB_MOVE1, SPR_XFDB, 32768, 5, NULL,                       S_XFDB_MOVE2);
    ST (S_XFDB_MOVE2, SPR_XFDB, 32768, 5, NULL,                       S_XFDB_MOVE3);
    ST (S_XFDB_MOVE3, SPR_XFDB, 32768, 5, NULL,                       S_XFDB_MOVE1);
    ST (S_XFDB_BOOM1, SPR_XFDB, 32769, 4, NULL,                       S_XFDB_BOOM2);
    ST (S_XFDB_BOOM2, SPR_XFDB, 32770, 4, NULL,                       S_XFDB_BOOM3);
    ST (S_XFDB_BOOM3, SPR_XFDB, 32771, 4, NULL,                       S_XFDB_BOOM4);
    ST (S_XFDB_BOOM4, SPR_XFDB, 32772, 4, NULL,                       S_XFDB_BOOM5);
    ST (S_XFDB_BOOM5, SPR_XFDB, 32772, 3, NULL,                       S_NULL);

    m = &mobjinfo[MT_XFIREDEMON];
    m->doomednum = -1;        m->spawnstate  = S_XFDM_LOOK1; m->spawnhealth = 80;
    m->seestate  = S_XFDM_WALK1; m->seesound  = sfx_bgsit1; m->reactiontime = 8;
    m->attacksound = sfx_firsht;  m->painstate = S_XFDM_PAIN1; m->painchance = 1;
    m->painsound = sfx_popain;    m->meleestate = S_NULL;     m->missilestate = S_XFDM_ATK1;
    m->deathstate = S_XFDM_DIE1;  m->xdeathstate = S_NULL;    m->deathsound = sfx_firxpl;
    m->speed = 13; m->radius = 20*FRACUNIT; m->height = 68*FRACUNIT; m->mass = 75;
    m->damage = 1; m->activesound = sfx_bgact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL|MF_FLOAT|MF_NOGRAVITY; m->raisestate = S_NULL;

    m = &mobjinfo[MT_XFIREDEMON_FX];
    m->doomednum = -1;        m->spawnstate  = S_XFDB_MOVE1; m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;  m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;    m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;   m->missilestate = S_NULL;
    m->deathstate = S_XFDB_BOOM1; m->xdeathstate = S_NULL; m->deathsound = sfx_firxpl;
    m->speed = 10*FRACUNIT; m->radius = 10*FRACUNIT; m->height = 6*FRACUNIT; m->mass = 15;
    m->damage = 4; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;

    // ---- Reiver / Wraith (crispy S_WRAITH_*; the rise-from-ground init and ice
    //      death are simplified away).  Floating undead: melee drains health, also
    //      lobs a bolt at range.  Sprite: XWRT (A-D walk, E-G attack, H pain, I-R die). ----
    ST (S_XWRT_LOOK1,  SPR_XWRT,  0, 15, (actionf_p1)A_Look,         S_XWRT_LOOK2);
    ST (S_XWRT_LOOK2,  SPR_XWRT,  1, 15, (actionf_p1)A_Look,         S_XWRT_LOOK1);
    ST (S_XWRT_CHASE1, SPR_XWRT,  0,  4, (actionf_p1)A_Chase,        S_XWRT_CHASE2);
    ST (S_XWRT_CHASE2, SPR_XWRT,  1,  4, (actionf_p1)A_Chase,        S_XWRT_CHASE3);
    ST (S_XWRT_CHASE3, SPR_XWRT,  2,  4, (actionf_p1)A_Chase,        S_XWRT_CHASE4);
    ST (S_XWRT_CHASE4, SPR_XWRT,  3,  4, (actionf_p1)A_Chase,        S_XWRT_CHASE1);
    ST (S_XWRT_ATK1_1, SPR_XWRT,  4,  6, (actionf_p1)A_FaceTarget,   S_XWRT_ATK1_2);
    ST (S_XWRT_ATK1_2, SPR_XWRT,  5,  6, (actionf_p1)A_FaceTarget,   S_XWRT_ATK1_3);
    ST (S_XWRT_ATK1_3, SPR_XWRT,  6,  6, (actionf_p1)A_WraithMelee,  S_XWRT_CHASE1);
    ST (S_XWRT_ATK2_1, SPR_XWRT,  4,  6, (actionf_p1)A_FaceTarget,   S_XWRT_ATK2_2);
    ST (S_XWRT_ATK2_2, SPR_XWRT,  5,  6, (actionf_p1)A_FaceTarget,   S_XWRT_ATK2_3);
    ST (S_XWRT_ATK2_3, SPR_XWRT,  6,  6, (actionf_p1)A_WraithMissile,S_XWRT_CHASE1);
    ST (S_XWRT_PAIN1,  SPR_XWRT,  7,  2, NULL,                       S_XWRT_PAIN2);
    ST (S_XWRT_PAIN2,  SPR_XWRT,  7,  6, (actionf_p1)A_Pain,         S_XWRT_CHASE1);
    ST (S_XWRT_DIE1,   SPR_XWRT,  8,  4, NULL,                       S_XWRT_DIE2);
    ST (S_XWRT_DIE2,   SPR_XWRT,  9,  4, (actionf_p1)A_Scream,       S_XWRT_DIE3);
    ST (S_XWRT_DIE3,   SPR_XWRT, 10,  4, NULL,                       S_XWRT_DIE4);
    ST (S_XWRT_DIE4,   SPR_XWRT, 11,  4, (actionf_p1)A_Fall,         S_XWRT_DIE5);
    ST (S_XWRT_DIE5,   SPR_XWRT, 12,  4, NULL,                       S_XWRT_DIE6);
    ST (S_XWRT_DIE6,   SPR_XWRT, 13,  4, NULL,                       S_XWRT_DIE7);
    ST (S_XWRT_DIE7,   SPR_XWRT, 14,  4, NULL,                       S_XWRT_DIE8);
    ST (S_XWRT_DIE8,   SPR_XWRT, 15,  5, NULL,                       S_XWRT_DIE9);
    ST (S_XWRT_DIE9,   SPR_XWRT, 16,  5, NULL,                       S_XWRT_DIE10);
    ST (S_XWRT_DIE10,  SPR_XWRT, 17, -1, NULL,                       S_NULL);

    // Reiver bolt (crispy S_WRTHFX_MOVE*/BOOM*).
    ST (S_XWRB_MOVE1, SPR_XWRB, 32768, 3, NULL,                      S_XWRB_MOVE2);
    ST (S_XWRB_MOVE2, SPR_XWRB, 32769, 3, NULL,                      S_XWRB_MOVE3);
    ST (S_XWRB_MOVE3, SPR_XWRB, 32770, 3, NULL,                      S_XWRB_MOVE1);
    ST (S_XWRB_BOOM1, SPR_XWRB, 32771, 4, NULL,                      S_XWRB_BOOM2);
    ST (S_XWRB_BOOM2, SPR_XWRB, 32772, 4, NULL,                      S_XWRB_BOOM3);
    ST (S_XWRB_BOOM3, SPR_XWRB, 32773, 4, NULL,                      S_XWRB_BOOM4);
    ST (S_XWRB_BOOM4, SPR_XWRB, 32774, 3, NULL,                      S_XWRB_BOOM5);
    ST (S_XWRB_BOOM5, SPR_XWRB, 32775, 3, NULL,                      S_XWRB_BOOM6);
    ST (S_XWRB_BOOM6, SPR_XWRB, 32776, 3, NULL,                      S_NULL);

    m = &mobjinfo[MT_XWRAITH];
    m->doomednum = -1;        m->spawnstate  = S_XWRT_LOOK1; m->spawnhealth = 150;
    m->seestate  = S_XWRT_CHASE1; m->seesound  = sfx_bgsit1; m->reactiontime = 8;
    m->attacksound = sfx_firsht;  m->painstate = S_XWRT_PAIN1; m->painchance = 25;
    m->painsound = sfx_popain;    m->meleestate = S_XWRT_ATK1_1; m->missilestate = S_XWRT_ATK2_1;
    m->deathstate = S_XWRT_DIE1;  m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth1;
    m->speed = 11; m->radius = 20*FRACUNIT; m->height = 55*FRACUNIT; m->mass = 75;
    m->damage = 0; m->activesound = sfx_bgact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL|MF_FLOAT|MF_NOGRAVITY; m->raisestate = S_NULL;

    m = &mobjinfo[MT_XWRAITH_FX];
    m->doomednum = -1;        m->spawnstate  = S_XWRB_MOVE1; m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;  m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;    m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;   m->missilestate = S_NULL;
    m->deathstate = S_XWRB_BOOM1; m->xdeathstate = S_NULL; m->deathsound = sfx_firxpl;
    m->speed = 14*FRACUNIT; m->radius = 10*FRACUNIT; m->height = 6*FRACUNIT; m->mass = 5;
    m->damage = 5; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;

    // ---- Dark Bishop (crispy S_BISHOP_*; the teleport-blur evasion + homing
    //      missile + special1 burst counter are simplified away).  Floating caster:
    //      melee swing in range, else a plain missile.  Sprite XBIS (A-F frames
    //      0-5 rotated, attack/death frames fullbright). ----
    ST (S_XBIS_LOOK1, SPR_XBIS,  0, 10, (actionf_p1)A_Look,          S_XBIS_LOOK1);
    ST (S_XBIS_WALK1, SPR_XBIS,  0,  3, (actionf_p1)A_Chase,         S_XBIS_WALK2);
    ST (S_XBIS_WALK2, SPR_XBIS,  1,  3, (actionf_p1)A_Chase,         S_XBIS_WALK3);
    ST (S_XBIS_WALK3, SPR_XBIS,  2,  3, (actionf_p1)A_Chase,         S_XBIS_WALK1);
    ST (S_XBIS_ATK1,  SPR_XBIS,  0,  3, (actionf_p1)A_FaceTarget,    S_XBIS_ATK2);
    ST (S_XBIS_ATK2,  SPR_XBIS, 32771, 3, (actionf_p1)A_FaceTarget,  S_XBIS_ATK3);
    ST (S_XBIS_ATK3,  SPR_XBIS, 32772, 3, (actionf_p1)A_FaceTarget,  S_XBIS_ATK4);
    ST (S_XBIS_ATK4,  SPR_XBIS, 32773, 3, (actionf_p1)A_BishopAttack,S_XBIS_ATK5);
    ST (S_XBIS_ATK5,  SPR_XBIS, 32773, 5, (actionf_p1)A_FaceTarget,  S_XBIS_WALK1);
    ST (S_XBIS_PAIN1, SPR_XBIS,  2,  6, (actionf_p1)A_Pain,          S_XBIS_WALK1);
    ST (S_XBIS_DIE1,  SPR_XBIS,  6,  6, NULL,                        S_XBIS_DIE2);
    ST (S_XBIS_DIE2,  SPR_XBIS, 32775, 6, (actionf_p1)A_Scream,      S_XBIS_DIE3);
    ST (S_XBIS_DIE3,  SPR_XBIS, 32776, 5, (actionf_p1)A_Fall,        S_XBIS_DIE4);
    ST (S_XBIS_DIE4,  SPR_XBIS, 32777, 5, NULL,                      S_XBIS_DIE5);
    ST (S_XBIS_DIE5,  SPR_XBIS, 32778, 5, NULL,                      S_XBIS_DIE6);
    ST (S_XBIS_DIE6,  SPR_XBIS, 32779, 4, NULL,                      S_XBIS_DIE7);
    ST (S_XBIS_DIE7,  SPR_XBIS, 32780, 4, NULL,                      S_NULL);

    // Dark Bishop missile (crispy S_BISHFX*; seeking dropped -- straight flight).
    ST (S_XBPF_MOVE1, SPR_XBPF, 32768, 2, NULL,                      S_XBPF_MOVE2);
    ST (S_XBPF_MOVE2, SPR_XBPF, 32769, 2, NULL,                      S_XBPF_MOVE1);
    ST (S_XBPF_BOOM1, SPR_XBPF, 32770, 4, NULL,                      S_XBPF_BOOM2);
    ST (S_XBPF_BOOM2, SPR_XBPF, 32771, 4, NULL,                      S_XBPF_BOOM3);
    ST (S_XBPF_BOOM3, SPR_XBPF, 32772, 4, NULL,                      S_XBPF_BOOM4);
    ST (S_XBPF_BOOM4, SPR_XBPF, 32773, 3, NULL,                      S_XBPF_BOOM5);
    ST (S_XBPF_BOOM5, SPR_XBPF, 32774, 3, NULL,                      S_NULL);

    m = &mobjinfo[MT_XBISHOP];
    m->doomednum = -1;        m->spawnstate  = S_XBIS_LOOK1; m->spawnhealth = 130;
    m->seestate  = S_XBIS_WALK1; m->seesound  = sfx_bgsit2; m->reactiontime = 8;
    m->attacksound = sfx_firsht;  m->painstate = S_XBIS_PAIN1; m->painchance = 110;
    m->painsound = sfx_popain;    m->meleestate = S_NULL;     m->missilestate = S_XBIS_ATK1;
    m->deathstate = S_XBIS_DIE1;  m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth2;
    m->speed = 10; m->radius = 22*FRACUNIT; m->height = 65*FRACUNIT; m->mass = 100;
    m->damage = 0; m->activesound = sfx_bgact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL|MF_FLOAT|MF_NOGRAVITY|MF_NOBLOOD; m->raisestate = S_NULL;

    m = &mobjinfo[MT_XBISHOP_FX];
    m->doomednum = -1;        m->spawnstate  = S_XBPF_MOVE1; m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;  m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;    m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;   m->missilestate = S_NULL;
    m->deathstate = S_XBPF_BOOM1; m->xdeathstate = S_NULL; m->deathsound = sfx_firxpl;
    m->speed = 10*FRACUNIT; m->radius = 10*FRACUNIT; m->height = 6*FRACUNIT; m->mass = 100;
    m->damage = 4; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;

    // ---- Wendigo / Ice Guy (crispy S_ICEGUY_*; the dormant spawn, wisp/bit
    //      spawns and dual-missile fan are simplified to a plain floating caster
    //      that lobs one straight ice shard).  Sprite XICE (A-D walk, E-G attack
    //      fullbright).  Death = the ice SHATTER (frame A like crispy, then burst into
    //      ice-shatter puffs and vanish -- A_IceGuyShatter). ----
    ST (S_XICE_LOOK1, SPR_XICE,  0, 10, (actionf_p1)A_Look,         S_XICE_LOOK2);
    ST (S_XICE_LOOK2, SPR_XICE,  0, 10, (actionf_p1)A_Look,         S_XICE_LOOK1);
    ST (S_XICE_WALK1, SPR_XICE,  0,  4, (actionf_p1)A_Chase,        S_XICE_WALK2);
    ST (S_XICE_WALK2, SPR_XICE,  1,  4, (actionf_p1)A_Chase,        S_XICE_WALK3);
    ST (S_XICE_WALK3, SPR_XICE,  2,  4, (actionf_p1)A_Chase,        S_XICE_WALK4);
    ST (S_XICE_WALK4, SPR_XICE,  3,  4, (actionf_p1)A_Chase,        S_XICE_WALK1);
    ST (S_XICE_ATK1,  SPR_XICE,  4,  3, (actionf_p1)A_FaceTarget,   S_XICE_ATK2);
    ST (S_XICE_ATK2,  SPR_XICE,  5,  3, (actionf_p1)A_FaceTarget,   S_XICE_ATK3);
    ST (S_XICE_ATK3,  SPR_XICE, 32774, 8,(actionf_p1)A_IceGuyAttack,S_XICE_ATK4);
    ST (S_XICE_ATK4,  SPR_XICE,  5,  4, (actionf_p1)A_FaceTarget,   S_XICE_WALK1);
    ST (S_XICE_PAIN1, SPR_XICE,  0,  2, (actionf_p1)A_Pain,         S_XICE_WALK1);
    ST (S_XICE_DIE1,  SPR_XICE,  0,  5, (actionf_p1)A_Scream,         S_XICE_DIE2);
    ST (S_XICE_DIE2,  SPR_XICE,  0,  5, (actionf_p1)A_Fall,           S_XICE_DIE3);
    ST (S_XICE_DIE3,  SPR_XICE,  0,  3, (actionf_p1)A_IceGuyShatter,  S_NULL);

    // Wendigo ice shard (crispy S_ICEGUY_FX*/FX_X*).
    ST (S_XICP_MOVE1, SPR_XICP, 32768, 3, NULL,                     S_XICP_MOVE2);
    ST (S_XICP_MOVE2, SPR_XICP, 32769, 3, NULL,                     S_XICP_MOVE3);
    ST (S_XICP_MOVE3, SPR_XICP, 32770, 3, NULL,                     S_XICP_MOVE1);
    ST (S_XICP_BOOM1, SPR_XICP, 32771, 4, NULL,                     S_XICP_BOOM2);
    ST (S_XICP_BOOM2, SPR_XICP, 32772, 4, NULL,                     S_XICP_BOOM3);
    ST (S_XICP_BOOM3, SPR_XICP, 32773, 4, NULL,                     S_XICP_BOOM4);
    ST (S_XICP_BOOM4, SPR_XICP, 32774, 4, NULL,                     S_XICP_BOOM5);
    ST (S_XICP_BOOM5, SPR_XICP, 32775, 3, NULL,                     S_NULL);

    m = &mobjinfo[MT_XICEGUY];
    m->doomednum = -1;        m->spawnstate  = S_XICE_LOOK1; m->spawnhealth = 120;
    m->seestate  = S_XICE_WALK1; m->seesound  = sfx_bgsit2; m->reactiontime = 8;
    m->attacksound = sfx_firsht;  m->painstate = S_XICE_PAIN1; m->painchance = 144;
    m->painsound = sfx_popain;    m->meleestate = S_NULL;     m->missilestate = S_XICE_ATK1;
    m->deathstate = S_XICE_DIE1;  m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth2;
    m->speed = 14; m->radius = 22*FRACUNIT; m->height = 75*FRACUNIT; m->mass = 150;
    m->damage = 0; m->activesound = sfx_bgact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL|MF_FLOAT|MF_NOGRAVITY|MF_NOBLOOD; m->raisestate = S_NULL;

    m = &mobjinfo[MT_XICEGUY_FX];
    m->doomednum = -1;        m->spawnstate  = S_XICP_MOVE1; m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;  m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;    m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;   m->missilestate = S_NULL;
    m->deathstate = S_XICP_BOOM1; m->xdeathstate = S_NULL; m->deathsound = sfx_firxpl;
    m->speed = 14*FRACUNIT; m->radius = 8*FRACUNIT; m->height = 10*FRACUNIT; m->mass = 100;
    m->damage = 3; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;

    // ---- Stalker / Serpent (crispy S_SERPENT_*; the underwater hide/dive/surface
    //      ritual is simplified away -- a plain ground ambusher that chases, then
    //      swings in melee or spits at range).  Sprite XSSP (8-9 walk, 10-13 attack,
    //      14-25 die). ----
    ST (S_XSSP_LOOK1, SPR_XSSP,  7, 10, (actionf_p1)A_Look,         S_XSSP_LOOK2);
    ST (S_XSSP_LOOK2, SPR_XSSP,  7, 10, (actionf_p1)A_Look,         S_XSSP_LOOK1);
    ST (S_XSSP_WALK1, SPR_XSSP,  8,  5, (actionf_p1)A_Chase,        S_XSSP_WALK2);
    ST (S_XSSP_WALK2, SPR_XSSP,  9,  5, (actionf_p1)A_Chase,        S_XSSP_WALK3);
    ST (S_XSSP_WALK3, SPR_XSSP,  8,  5, (actionf_p1)A_Chase,        S_XSSP_WALK4);
    ST (S_XSSP_WALK4, SPR_XSSP,  9,  5, (actionf_p1)A_Chase,        S_XSSP_WALK1);
    ST (S_XSSP_ATK1,  SPR_XSSP, 10,  6, (actionf_p1)A_FaceTarget,   S_XSSP_ATK2);
    ST (S_XSSP_ATK2,  SPR_XSSP, 11,  5, (actionf_p1)A_FaceTarget,   S_XSSP_MEL1);
    ST (S_XSSP_MEL1,  SPR_XSSP, 13,  5, (actionf_p1)A_StalkerMelee, S_XSSP_WALK1);
    ST (S_XSSP_MIS1,  SPR_XSSP, 13,  5, (actionf_p1)A_StalkerMissile,S_XSSP_WALK1);
    ST (S_XSSP_PAIN1, SPR_XSSP, 11,  5, NULL,                       S_XSSP_PAIN2);
    ST (S_XSSP_PAIN2, SPR_XSSP, 11,  5, (actionf_p1)A_Pain,         S_XSSP_WALK1);
    ST (S_XSSP_DIE1,  SPR_XSSP, 14,  4, NULL,                       S_XSSP_DIE2);
    ST (S_XSSP_DIE2,  SPR_XSSP, 15,  4, (actionf_p1)A_Scream,       S_XSSP_DIE3);
    ST (S_XSSP_DIE3,  SPR_XSSP, 16,  4, (actionf_p1)A_Fall,         S_XSSP_DIE4);
    ST (S_XSSP_DIE4,  SPR_XSSP, 17,  4, NULL,                       S_XSSP_DIE5);
    ST (S_XSSP_DIE5,  SPR_XSSP, 18,  4, NULL,                       S_XSSP_DIE6);
    ST (S_XSSP_DIE6,  SPR_XSSP, 19,  4, NULL,                       S_XSSP_DIE7);
    ST (S_XSSP_DIE7,  SPR_XSSP, 20,  4, NULL,                       S_XSSP_DIE8);
    ST (S_XSSP_DIE8,  SPR_XSSP, 21,  4, NULL,                       S_XSSP_DIE9);
    ST (S_XSSP_DIE9,  SPR_XSSP, 22, -1, NULL,                       S_NULL);

    // Stalker spit (crispy S_SERPENT_FX*/FX_X*).
    ST (S_XSSF_MOVE1, SPR_XSSF, 32768, 3, NULL,                     S_XSSF_MOVE2);
    ST (S_XSSF_MOVE2, SPR_XSSF, 32769, 3, NULL,                     S_XSSF_MOVE3);
    ST (S_XSSF_MOVE3, SPR_XSSF, 32768, 3, NULL,                     S_XSSF_MOVE4);
    ST (S_XSSF_MOVE4, SPR_XSSF, 32769, 3, NULL,                     S_XSSF_MOVE1);
    ST (S_XSSF_BOOM1, SPR_XSSF, 32770, 4, NULL,                     S_XSSF_BOOM2);
    ST (S_XSSF_BOOM2, SPR_XSSF, 32771, 4, NULL,                     S_XSSF_BOOM3);
    ST (S_XSSF_BOOM3, SPR_XSSF, 32772, 4, NULL,                     S_XSSF_BOOM4);
    ST (S_XSSF_BOOM4, SPR_XSSF, 32773, 4, NULL,                     S_XSSF_BOOM5);
    ST (S_XSSF_BOOM5, SPR_XSSF, 32774, 4, NULL,                     S_XSSF_BOOM6);
    ST (S_XSSF_BOOM6, SPR_XSSF, 32775, 4, NULL,                     S_NULL);

    m = &mobjinfo[MT_XSTALKER];
    m->doomednum = -1;        m->spawnstate  = S_XSSP_LOOK1; m->spawnhealth = 90;
    m->seestate  = S_XSSP_WALK1; m->seesound  = sfx_bgsit1; m->reactiontime = 8;
    m->attacksound = sfx_claw;    m->painstate = S_XSSP_PAIN1; m->painchance = 96;
    m->painsound = sfx_popain;    m->meleestate = S_XSSP_ATK1; m->missilestate = S_XSSP_MIS1;
    m->deathstate = S_XSSP_DIE1;  m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth1;
    m->speed = 12; m->radius = 32*FRACUNIT; m->height = 70*FRACUNIT; m->mass = 200;
    m->damage = 0; m->activesound = sfx_bgact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL; m->raisestate = S_NULL;

    m = &mobjinfo[MT_XSTALKER_FX];
    m->doomednum = -1;        m->spawnstate  = S_XSSF_MOVE1; m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;  m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;    m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;   m->missilestate = S_NULL;
    m->deathstate = S_XSSF_BOOM1; m->xdeathstate = S_NULL; m->deathsound = sfx_firxpl;
    m->speed = 15*FRACUNIT; m->radius = 8*FRACUNIT; m->height = 10*FRACUNIT; m->mass = 100;
    m->damage = 4; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;

    // ---- Death Wyvern / Dragon (crispy S_DRAGON_*; the take-off flight ritual,
    //      A_DragonFlight steering and crash sequence are simplified to a plain
    //      flying boss that lobs a straight fireball).  Sprite XDRA (0-3 flight,
    //      4 attack, 5 pain, 6-12 die).  Big, high HP. ----
    ST (S_XDRA_LOOK1, SPR_XDRA,  3, 10, (actionf_p1)A_Look,         S_XDRA_LOOK2);
    ST (S_XDRA_LOOK2, SPR_XDRA,  3, 10, (actionf_p1)A_Look,         S_XDRA_LOOK1);
    ST (S_XDRA_WALK1, SPR_XDRA,  0,  3, (actionf_p1)A_Chase,        S_XDRA_WALK2);
    ST (S_XDRA_WALK2, SPR_XDRA,  1,  3, (actionf_p1)A_Chase,        S_XDRA_WALK3);
    ST (S_XDRA_WALK3, SPR_XDRA,  2,  3, (actionf_p1)A_Chase,        S_XDRA_WALK4);
    ST (S_XDRA_WALK4, SPR_XDRA,  3,  3, (actionf_p1)A_Chase,        S_XDRA_WALK1);
    ST (S_XDRA_ATK1,  SPR_XDRA,  4,  8, (actionf_p1)A_DragonAttack, S_XDRA_WALK1);
    ST (S_XDRA_PAIN1, SPR_XDRA,  5, 10, (actionf_p1)A_Pain,         S_XDRA_WALK1);
    ST (S_XDRA_DIE1,  SPR_XDRA,  6,  5, (actionf_p1)A_Scream,       S_XDRA_DIE2);
    ST (S_XDRA_DIE2,  SPR_XDRA,  7,  4, (actionf_p1)A_Fall,         S_XDRA_DIE3);
    ST (S_XDRA_DIE3,  SPR_XDRA,  8,  4, NULL,                       S_XDRA_DIE4);
    ST (S_XDRA_DIE4,  SPR_XDRA,  9,  4, NULL,                       S_XDRA_DIE5);
    ST (S_XDRA_DIE5,  SPR_XDRA, 10, -1, NULL,                       S_NULL);

    // Dragon fireball (crispy S_DRAGON_FX1_*).
    ST (S_XDRF_MOVE1, SPR_XDRF, 32768, 4, NULL,                     S_XDRF_MOVE2);
    ST (S_XDRF_MOVE2, SPR_XDRF, 32769, 4, NULL,                     S_XDRF_MOVE3);
    ST (S_XDRF_MOVE3, SPR_XDRF, 32770, 4, NULL,                     S_XDRF_MOVE4);
    ST (S_XDRF_MOVE4, SPR_XDRF, 32771, 4, NULL,                     S_XDRF_MOVE5);
    ST (S_XDRF_MOVE5, SPR_XDRF, 32772, 4, NULL,                     S_XDRF_MOVE6);
    ST (S_XDRF_MOVE6, SPR_XDRF, 32773, 4, NULL,                     S_XDRF_MOVE1);
    ST (S_XDRF_BOOM1, SPR_XDRF, 32774, 4, NULL,                     S_XDRF_BOOM2);
    ST (S_XDRF_BOOM2, SPR_XDRF, 32775, 4, NULL,                     S_XDRF_BOOM3);
    ST (S_XDRF_BOOM3, SPR_XDRF, 32776, 4, NULL,                     S_XDRF_BOOM4);
    ST (S_XDRF_BOOM4, SPR_XDRF, 32777, 4, NULL,                     S_XDRF_BOOM5);
    ST (S_XDRF_BOOM5, SPR_XDRF, 32778, 3, NULL,                     S_XDRF_BOOM6);
    ST (S_XDRF_BOOM6, SPR_XDRF, 32779, 3, NULL,                     S_NULL);

    m = &mobjinfo[MT_XDRAGON];
    m->doomednum = -1;        m->spawnstate  = S_XDRA_LOOK1; m->spawnhealth = 640;
    m->seestate  = S_XDRA_WALK1; m->seesound  = sfx_bgsit1; m->reactiontime = 8;
    m->attacksound = sfx_firsht;  m->painstate = S_XDRA_PAIN1; m->painchance = 128;
    m->painsound = sfx_dmpain;    m->meleestate = S_NULL;     m->missilestate = S_XDRA_ATK1;
    m->deathstate = S_XDRA_DIE1;  m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth1;
    m->speed = 10; m->radius = 20*FRACUNIT; m->height = 65*FRACUNIT; m->mass = 1000;
    m->damage = 0; m->activesound = sfx_dmact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL|MF_FLOAT|MF_NOGRAVITY|MF_NOBLOOD; m->raisestate = S_NULL;

    m = &mobjinfo[MT_XDRAGON_FX];
    m->doomednum = -1;        m->spawnstate  = S_XDRF_MOVE1; m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;  m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;    m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;   m->missilestate = S_NULL;
    m->deathstate = S_XDRF_BOOM1; m->xdeathstate = S_NULL; m->deathsound = sfx_firxpl;
    m->speed = 24*FRACUNIT; m->radius = 12*FRACUNIT; m->height = 10*FRACUNIT; m->mass = 100;
    m->damage = 6; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;
}

// ---------------------------------------------------------------------------
// Spawn helpers (console "summon" + director)
// ---------------------------------------------------------------------------
// hexenstuff.wad's renamed sprites loaded?  Test the PARSED sprite, not a lump name.
int Hexen_Available (void)
{
    return numsprites > SPR_XETT && sprites[SPR_XETT].numframes > 0;
}

int Hexen_TypeByName (const char* name)
{
    if (!name || !name[0]) return -1;
    if (!strcmp (name, "ettin")) return MT_XETTIN;
    if (!strcmp (name, "centaur")) return MT_XCENTAUR;
    if (!strcmp (name, "slaughtaur")) return MT_XSLAUGHTAUR;
    // "demon" stays the DOOM pinky (resolved earlier in C_MobjByName); use "serpent".
    if (!strcmp (name, "serpent") || !strcmp (name, "chaosserpent")) return MT_XDEMON;
    if (!strcmp (name, "afrit") || !strcmp (name, "firedemon")) return MT_XFIREDEMON;
    if (!strcmp (name, "reiver") || !strcmp (name, "wraith")) return MT_XWRAITH;
    if (!strcmp (name, "bishop") || !strcmp (name, "darkbishop")) return MT_XBISHOP;
    if (!strcmp (name, "wendigo") || !strcmp (name, "iceguy")) return MT_XICEGUY;
    if (!strcmp (name, "stalker")) return MT_XSTALKER;
    if (!strcmp (name, "wyvern") || !strcmp (name, "dragon") || !strcmp (name, "deathwyvern")) return MT_XDRAGON;
    return -1;
}

mobj_t* Hexen_Spawn (int type, fixed_t x, fixed_t y)
{
    if (!Hexen_Available () || type < 0)
	return NULL;
    return P_SpawnMonsterChecked (x, y, type);
}
