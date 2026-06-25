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
    m->speed = 20*FRACUNIT; m->radius = 10*FRACUNIT; m->height = 8*FRACUNIT; m->mass = 100;
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
    return -1;
}

mobj_t* Hexen_Spawn (int type, fixed_t x, fixed_t y)
{
    if (!Hexen_Available () || type < 0)
	return NULL;
    return P_SpawnMonsterChecked (x, y, type);
}
