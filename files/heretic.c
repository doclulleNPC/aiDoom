// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Additive Heretic monsters in the DOOM engine (HERETIC_HEXEN.md approach A).
//	Ported from crispy-doom's heretic/info.c (frame tables) + heretic/p_enemy.c
//	(action funcs).  The states/mobjinfo are appended to the engine tables at
//	runtime (Heretic_Init) so info.c's huge generated initializers stay untouched;
//	the enum slots live at the end of statenum_t/mobjtype_t/spritenum_t (info.h).
//	Sprites: hereticstuff.wad (renamed H*).  Sounds: DOOM SFX reused for now.
//
//	Monsters so far: Mummy (golem) -- a melee grunt.  More follow the same pattern.
//
//-----------------------------------------------------------------------------

#include <string.h>

#include "doomdef.h"
#include "info.h"
#include "m_random.h"
#include "m_fixed.h"
#include "tables.h"		// finecosine/finesine, ANGLETOFINESHIFT (imp charge)
#include "sounds.h"
#include "w_wad.h"
#include "p_mobj.h"
#include "heretic.h"

#define ONFLOORZ	MININT		// from p_local.h (avoided: its p_spec.h open/close enums)

extern state_t		states[];
extern mobjinfo_t	mobjinfo[];

// engine helpers (no public header for the p_enemy action funcs -- declare by hand)
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
extern fixed_t	P_AproxDistance (fixed_t dx, fixed_t dy);
extern mobj_t*	P_SpawnMonsterChecked (fixed_t x, fixed_t y, mobjtype_t type);
extern mobj_t*	P_SpawnMissile (mobj_t* source, mobj_t* dest, mobjtype_t type);

// ---------------------------------------------------------------------------
// Action functions (crispy heretic/p_enemy.c, adapted to DOOM's 1-arg signature).
// Heretic's HITDICE(d) melee damage = ((P_Random() & 7) + 1) * d.
// ---------------------------------------------------------------------------
void A_MummyAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    if (P_CheckMeleeRange (actor))
	P_DamageMobj (actor->target, actor, actor, ((P_Random () & 7) + 1) * 2);
}

// Sabreclaw (clink): melee, (P_Random()%7)+3 = 3..9.
void A_ClinkAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    if (P_CheckMeleeRange (actor))
	P_DamageMobj (actor->target, actor, actor, (P_Random () % 7) + 3);
}

// Gargoyle (imp): melee, 5 + (P_Random()&7) = 5..12.
void A_ImpMeAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    if (P_CheckMeleeRange (actor))
	P_DamageMobj (actor->target, actor, actor, 5 + (P_Random () & 7));
}

// Gargoyle dive-bomb: ~25 % of the time launch a skull-fly charge at the target (DOOM's
// MF_SKULLFLY handling damages on contact and reverts to spawnstate on impact); else
// just keep flying.
void A_ImpMsAttack (mobj_t* actor)
{
    mobj_t*	dest;
    angle_t	an;
    int		dist;

    if (!actor->target || P_Random () > 64)
    {
	P_SetMobjState (actor, actor->info->seestate);
	return;
    }
    dest = actor->target;
    actor->flags |= MF_SKULLFLY;
    S_StartSound (actor, actor->info->attacksound);
    A_FaceTarget (actor);
    an = actor->angle >> ANGLETOFINESHIFT;
    actor->momx = FixedMul (12*FRACUNIT, finecosine[an]);
    actor->momy = FixedMul (12*FRACUNIT, finesine[an]);
    dist = P_AproxDistance (dest->x - actor->x, dest->y - actor->y) / (12*FRACUNIT);
    if (dist < 1) dist = 1;
    actor->momz = (dest->z + (dest->height>>1) - actor->z) / dist;
}

// Gargoyle death: drop the float so the corpse falls, and stop blocking.
void A_ImpDeath (mobj_t* actor)
{
    actor->flags &= ~(MF_SOLID | MF_FLOAT | MF_NOGRAVITY);
}

// Undead warrior (knight): melee HITDICE(3) = 3..24, else hurl a spinning axe.
void A_KnightAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    if (P_CheckMeleeRange (actor))
    {
	P_DamageMobj (actor->target, actor, actor, ((P_Random () & 7) + 1) * 3);
	return;
    }
    S_StartSound (actor, actor->info->attacksound);
    P_SpawnMissile (actor, actor->target, MT_HKNIGHTAXE);	// throw the axe
}

// Looping whoosh while the thrown axe spins through the air.
void A_ContMobjSound (mobj_t* actor)
{
    S_StartSound (actor, sfx_firsht);
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

void Heretic_Init (void)
{
    mobjinfo_t*	m;

    // ---- Mummy states (crispy S_MUMMY_*; soul-on-death omitted for now) ----
    ST (S_HMUM_LOOK1, SPR_HMUM,  0, 10, (actionf_p1)A_Look,        S_HMUM_LOOK2);
    ST (S_HMUM_LOOK2, SPR_HMUM,  1, 10, (actionf_p1)A_Look,        S_HMUM_LOOK1);
    ST (S_HMUM_WALK1, SPR_HMUM,  0,  4, (actionf_p1)A_Chase,       S_HMUM_WALK2);
    ST (S_HMUM_WALK2, SPR_HMUM,  1,  4, (actionf_p1)A_Chase,       S_HMUM_WALK3);
    ST (S_HMUM_WALK3, SPR_HMUM,  2,  4, (actionf_p1)A_Chase,       S_HMUM_WALK4);
    ST (S_HMUM_WALK4, SPR_HMUM,  3,  4, (actionf_p1)A_Chase,       S_HMUM_WALK1);
    ST (S_HMUM_ATK1,  SPR_HMUM,  4,  6, (actionf_p1)A_FaceTarget,  S_HMUM_ATK2);
    ST (S_HMUM_ATK2,  SPR_HMUM,  5,  6, (actionf_p1)A_MummyAttack, S_HMUM_ATK3);
    ST (S_HMUM_ATK3,  SPR_HMUM,  6,  6, (actionf_p1)A_FaceTarget,  S_HMUM_WALK1);
    ST (S_HMUM_PAIN1, SPR_HMUM,  7,  4, NULL,                      S_HMUM_PAIN2);
    ST (S_HMUM_PAIN2, SPR_HMUM,  7,  4, (actionf_p1)A_Pain,        S_HMUM_WALK1);
    ST (S_HMUM_DIE1,  SPR_HMUM,  8,  5, NULL,                      S_HMUM_DIE2);
    ST (S_HMUM_DIE2,  SPR_HMUM,  9,  5, (actionf_p1)A_Scream,      S_HMUM_DIE3);
    ST (S_HMUM_DIE3,  SPR_HMUM, 10,  5, NULL,                      S_HMUM_DIE4);
    ST (S_HMUM_DIE4,  SPR_HMUM, 11,  5, NULL,                      S_HMUM_DIE5);
    ST (S_HMUM_DIE5,  SPR_HMUM, 12,  5, (actionf_p1)A_Fall,        S_HMUM_DIE6);
    ST (S_HMUM_DIE6,  SPR_HMUM, 13,  5, NULL,                      S_HMUM_DIE7);
    ST (S_HMUM_DIE7,  SPR_HMUM, 14,  5, NULL,                      S_HMUM_DIE8);
    ST (S_HMUM_DIE8,  SPR_HMUM, 15, -1, NULL,                      S_NULL);

    // ---- Mummy mobjinfo (crispy MT_MUMMY; DOOM sounds reused) ----
    m = &mobjinfo[MT_HMUMMY];
    m->doomednum    = -1;			// director-spawned only (no map ednum clash)
    m->spawnstate   = S_HMUM_LOOK1;
    m->spawnhealth  = 80;
    m->seestate     = S_HMUM_WALK1;
    m->seesound     = sfx_bgsit1;
    m->reactiontime = 8;
    m->attacksound  = sfx_claw;
    m->painstate    = S_HMUM_PAIN1;
    m->painchance   = 128;
    m->painsound    = sfx_popain;
    m->meleestate   = S_HMUM_ATK1;
    m->missilestate = S_NULL;
    m->deathstate   = S_HMUM_DIE1;
    m->xdeathstate  = S_NULL;
    m->deathsound   = sfx_bgdth1;
    m->speed        = 12;
    m->radius       = 22 * FRACUNIT;
    m->height       = 62 * FRACUNIT;
    m->mass         = 75;
    m->damage       = 0;
    m->activesound  = sfx_bgact;
    m->flags        = MF_SOLID | MF_SHOOTABLE | MF_COUNTKILL;
    m->raisestate   = S_NULL;

    // ---- Sabreclaw (clink): pure melee, no blood ----
    ST (S_HCLK_LOOK1, SPR_HCLK,  0, 10, (actionf_p1)A_Look,        S_HCLK_LOOK2);
    ST (S_HCLK_LOOK2, SPR_HCLK,  1, 10, (actionf_p1)A_Look,        S_HCLK_LOOK1);
    ST (S_HCLK_WALK1, SPR_HCLK,  0,  3, (actionf_p1)A_Chase,       S_HCLK_WALK2);
    ST (S_HCLK_WALK2, SPR_HCLK,  1,  3, (actionf_p1)A_Chase,       S_HCLK_WALK3);
    ST (S_HCLK_WALK3, SPR_HCLK,  2,  3, (actionf_p1)A_Chase,       S_HCLK_WALK4);
    ST (S_HCLK_WALK4, SPR_HCLK,  3,  3, (actionf_p1)A_Chase,       S_HCLK_WALK1);
    ST (S_HCLK_ATK1,  SPR_HCLK,  4,  5, (actionf_p1)A_FaceTarget,  S_HCLK_ATK2);
    ST (S_HCLK_ATK2,  SPR_HCLK,  5,  4, (actionf_p1)A_FaceTarget,  S_HCLK_ATK3);
    ST (S_HCLK_ATK3,  SPR_HCLK,  6,  7, (actionf_p1)A_ClinkAttack, S_HCLK_WALK1);
    ST (S_HCLK_PAIN1, SPR_HCLK,  7,  3, NULL,                      S_HCLK_PAIN2);
    ST (S_HCLK_PAIN2, SPR_HCLK,  7,  3, (actionf_p1)A_Pain,        S_HCLK_WALK1);
    ST (S_HCLK_DIE1,  SPR_HCLK,  8,  6, NULL,                      S_HCLK_DIE2);
    ST (S_HCLK_DIE2,  SPR_HCLK,  9,  6, NULL,                      S_HCLK_DIE3);
    ST (S_HCLK_DIE3,  SPR_HCLK, 10,  5, (actionf_p1)A_Scream,      S_HCLK_DIE4);
    ST (S_HCLK_DIE4,  SPR_HCLK, 11,  5, (actionf_p1)A_Fall,        S_HCLK_DIE5);
    ST (S_HCLK_DIE5,  SPR_HCLK, 12,  5, NULL,                      S_HCLK_DIE6);
    ST (S_HCLK_DIE6,  SPR_HCLK, 13,  5, NULL,                      S_HCLK_DIE7);
    ST (S_HCLK_DIE7,  SPR_HCLK, 14, -1, NULL,                      S_NULL);

    m = &mobjinfo[MT_HCLINK];
    m->doomednum = -1;        m->spawnstate  = S_HCLK_LOOK1; m->spawnhealth = 150;
    m->seestate  = S_HCLK_WALK1; m->seesound  = sfx_sgtsit;  m->reactiontime = 8;
    m->attacksound = sfx_sgtatk; m->painstate = S_HCLK_PAIN1; m->painchance = 32;
    m->painsound = sfx_dmpain; m->meleestate = S_HCLK_ATK1;  m->missilestate = S_NULL;
    m->deathstate = S_HCLK_DIE1; m->xdeathstate = S_NULL;    m->deathsound = sfx_sgtdth;
    m->speed = 14; m->radius = 20*FRACUNIT; m->height = 64*FRACUNIT; m->mass = 75;
    m->damage = 0; m->activesound = sfx_dmact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL|MF_NOBLOOD; m->raisestate = S_NULL;

    // ---- Gargoyle (imp): flies; melee + skull-fly dive ----
    ST (S_HIMP_LOOK1,  SPR_HIMP, 0, 10, (actionf_p1)A_Look,        S_HIMP_LOOK2);
    ST (S_HIMP_LOOK2,  SPR_HIMP, 1, 10, (actionf_p1)A_Look,        S_HIMP_LOOK3);
    ST (S_HIMP_LOOK3,  SPR_HIMP, 2, 10, (actionf_p1)A_Look,        S_HIMP_LOOK4);
    ST (S_HIMP_LOOK4,  SPR_HIMP, 1, 10, (actionf_p1)A_Look,        S_HIMP_LOOK1);
    ST (S_HIMP_FLY1,   SPR_HIMP, 0,  3, (actionf_p1)A_Chase,       S_HIMP_FLY2);
    ST (S_HIMP_FLY2,   SPR_HIMP, 0,  3, (actionf_p1)A_Chase,       S_HIMP_FLY3);
    ST (S_HIMP_FLY3,   SPR_HIMP, 1,  3, (actionf_p1)A_Chase,       S_HIMP_FLY4);
    ST (S_HIMP_FLY4,   SPR_HIMP, 1,  3, (actionf_p1)A_Chase,       S_HIMP_FLY5);
    ST (S_HIMP_FLY5,   SPR_HIMP, 2,  3, (actionf_p1)A_Chase,       S_HIMP_FLY6);
    ST (S_HIMP_FLY6,   SPR_HIMP, 2,  3, (actionf_p1)A_Chase,       S_HIMP_FLY7);
    ST (S_HIMP_FLY7,   SPR_HIMP, 1,  3, (actionf_p1)A_Chase,       S_HIMP_FLY8);
    ST (S_HIMP_FLY8,   SPR_HIMP, 1,  3, (actionf_p1)A_Chase,       S_HIMP_FLY1);
    ST (S_HIMP_MEATK1, SPR_HIMP, 3,  6, (actionf_p1)A_FaceTarget,  S_HIMP_MEATK2);
    ST (S_HIMP_MEATK2, SPR_HIMP, 4,  6, (actionf_p1)A_FaceTarget,  S_HIMP_MEATK3);
    ST (S_HIMP_MEATK3, SPR_HIMP, 5,  6, (actionf_p1)A_ImpMeAttack, S_HIMP_FLY1);
    ST (S_HIMP_MSATK1, SPR_HIMP, 0, 10, (actionf_p1)A_FaceTarget,  S_HIMP_MSATK2);
    ST (S_HIMP_MSATK2, SPR_HIMP, 1,  6, (actionf_p1)A_ImpMsAttack, S_HIMP_MSATK3);
    ST (S_HIMP_MSATK3, SPR_HIMP, 2,  6, NULL,                      S_HIMP_MSATK4);
    ST (S_HIMP_MSATK4, SPR_HIMP, 1,  6, NULL,                      S_HIMP_MSATK5);
    ST (S_HIMP_MSATK5, SPR_HIMP, 0,  6, NULL,                      S_HIMP_MSATK6);
    ST (S_HIMP_MSATK6, SPR_HIMP, 1,  6, NULL,                      S_HIMP_MSATK3);
    ST (S_HIMP_PAIN1,  SPR_HIMP, 6,  3, NULL,                      S_HIMP_PAIN2);
    ST (S_HIMP_PAIN2,  SPR_HIMP, 6,  3, (actionf_p1)A_Pain,        S_HIMP_FLY1);
    ST (S_HIMP_DIE1,   SPR_HIMP,  6,  4, (actionf_p1)A_ImpDeath,   S_HIMP_DIE2);
    ST (S_HIMP_DIE2,   SPR_HIMP,  7,  5, (actionf_p1)A_Scream,     S_HIMP_DIE3);
    ST (S_HIMP_DIE3,   SPR_HIMP,  8,  6, (actionf_p1)A_Fall,       S_HIMP_DIE4);	// crash frames
    ST (S_HIMP_DIE4,   SPR_HIMP,  9,  5, NULL,                     S_HIMP_DIE5);
    ST (S_HIMP_DIE5,   SPR_HIMP, 10,  5, NULL,                     S_HIMP_DIE6);
    ST (S_HIMP_DIE6,   SPR_HIMP, 11, -1, NULL,                     S_NULL);

    m = &mobjinfo[MT_HIMP];
    m->doomednum = -1;        m->spawnstate  = S_HIMP_LOOK1; m->spawnhealth = 40;
    m->seestate  = S_HIMP_FLY1; m->seesound   = sfx_sgtsit;  m->reactiontime = 8;
    m->attacksound = sfx_sklatk; m->painstate = S_HIMP_PAIN1; m->painchance = 200;
    m->painsound = sfx_dmpain; m->meleestate = S_HIMP_MEATK1; m->missilestate = S_HIMP_MSATK1;
    m->deathstate = S_HIMP_DIE1; m->xdeathstate = S_NULL;    m->deathsound = sfx_skldth;
    m->speed = 10; m->radius = 16*FRACUNIT; m->height = 36*FRACUNIT; m->mass = 50;
    m->damage = 0; m->activesound = sfx_dmact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_FLOAT|MF_NOGRAVITY|MF_COUNTKILL; m->raisestate = S_NULL;

    // ---- Undead warrior (knight): melee + thrown spinning axe ----
    ST (S_HKNI_STND1, SPR_HKNI,  0, 10, (actionf_p1)A_Look,        S_HKNI_STND2);
    ST (S_HKNI_STND2, SPR_HKNI,  1, 10, (actionf_p1)A_Look,        S_HKNI_STND1);
    ST (S_HKNI_WALK1, SPR_HKNI,  0,  4, (actionf_p1)A_Chase,       S_HKNI_WALK2);
    ST (S_HKNI_WALK2, SPR_HKNI,  1,  4, (actionf_p1)A_Chase,       S_HKNI_WALK3);
    ST (S_HKNI_WALK3, SPR_HKNI,  2,  4, (actionf_p1)A_Chase,       S_HKNI_WALK4);
    ST (S_HKNI_WALK4, SPR_HKNI,  3,  4, (actionf_p1)A_Chase,       S_HKNI_WALK1);
    ST (S_HKNI_ATK1,  SPR_HKNI,  4, 10, (actionf_p1)A_FaceTarget,  S_HKNI_ATK2);
    ST (S_HKNI_ATK2,  SPR_HKNI,  5,  8, (actionf_p1)A_FaceTarget,  S_HKNI_ATK3);
    ST (S_HKNI_ATK3,  SPR_HKNI,  6,  8, (actionf_p1)A_KnightAttack,S_HKNI_ATK4);
    ST (S_HKNI_ATK4,  SPR_HKNI,  4, 10, (actionf_p1)A_FaceTarget,  S_HKNI_ATK5);
    ST (S_HKNI_ATK5,  SPR_HKNI,  5,  8, (actionf_p1)A_FaceTarget,  S_HKNI_ATK6);
    ST (S_HKNI_ATK6,  SPR_HKNI,  6,  8, (actionf_p1)A_KnightAttack,S_HKNI_WALK1);
    ST (S_HKNI_PAIN1, SPR_HKNI,  7,  3, NULL,                      S_HKNI_PAIN2);
    ST (S_HKNI_PAIN2, SPR_HKNI,  7,  3, (actionf_p1)A_Pain,        S_HKNI_WALK1);
    ST (S_HKNI_DIE1,  SPR_HKNI,  8,  6, NULL,                      S_HKNI_DIE2);
    ST (S_HKNI_DIE2,  SPR_HKNI,  9,  6, (actionf_p1)A_Scream,      S_HKNI_DIE3);
    ST (S_HKNI_DIE3,  SPR_HKNI, 10,  6, NULL,                      S_HKNI_DIE4);
    ST (S_HKNI_DIE4,  SPR_HKNI, 11,  6, (actionf_p1)A_Fall,        S_HKNI_DIE5);
    ST (S_HKNI_DIE5,  SPR_HKNI, 12,  6, NULL,                      S_HKNI_DIE6);
    ST (S_HKNI_DIE6,  SPR_HKNI, 13,  6, NULL,                      S_HKNI_DIE7);
    ST (S_HKNI_DIE7,  SPR_HKNI, 14, -1, NULL,                      S_NULL);

    // thrown axe projectile (sprites full-bright: frame|32768); spin loops, then explode
    ST (S_HKAX1,  SPR_HKAX, 32768, 3, (actionf_p1)A_ContMobjSound, S_HKAX2);
    ST (S_HKAX2,  SPR_HKAX, 32769, 3, NULL,                        S_HKAX3);
    ST (S_HKAX3,  SPR_HKAX, 32770, 3, NULL,                        S_HKAX1);
    ST (S_HKAXX1, SPR_HKAX, 32771, 6, NULL,                        S_HKAXX2);
    ST (S_HKAXX2, SPR_HKAX, 32772, 6, NULL,                        S_HKAXX3);
    ST (S_HKAXX3, SPR_HKAX, 32773, 6, NULL,                        S_NULL);

    m = &mobjinfo[MT_HKNIGHT];
    m->doomednum = -1;        m->spawnstate  = S_HKNI_STND1; m->spawnhealth = 200;
    m->seestate  = S_HKNI_WALK1; m->seesound  = sfx_kntsit;  m->reactiontime = 8;
    m->attacksound = sfx_firsht; m->painstate = S_HKNI_PAIN1; m->painchance = 100;
    m->painsound = sfx_dmpain; m->meleestate = S_HKNI_ATK1;  m->missilestate = S_HKNI_ATK1;
    m->deathstate = S_HKNI_DIE1; m->xdeathstate = S_NULL;    m->deathsound = sfx_kntdth;
    m->speed = 12; m->radius = 24*FRACUNIT; m->height = 78*FRACUNIT; m->mass = 150;
    m->damage = 0; m->activesound = sfx_dmact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL; m->raisestate = S_NULL;

    m = &mobjinfo[MT_HKNIGHTAXE];
    m->doomednum = -1;        m->spawnstate  = S_HKAX1;      m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;    m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;      m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;     m->missilestate = S_NULL;
    m->deathstate = S_HKAXX1;    m->xdeathstate = S_NULL;    m->deathsound = sfx_firxpl;
    m->speed = 9*FRACUNIT; m->radius = 10*FRACUNIT; m->height = 8*FRACUNIT; m->mass = 100;
    m->damage = 2; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;
}

// hereticstuff.wad sprites loaded?  (the mummy's first frame lump)
int Heretic_Available (void)
{
    return W_CheckNumForName ("HMUMA1") >= 0;
}

// Map a name to a Heretic mobjtype, or -1 if unknown (default "" -> mummy).
int Heretic_TypeByName (const char* name)
{
    if (!name || !name[0] || !strcmp (name, "mummy"))           return MT_HMUMMY;
    if (!strcmp (name, "clink") || !strcmp (name, "sabreclaw")) return MT_HCLINK;
    if (!strcmp (name, "imp")   || !strcmp (name, "gargoyle"))  return MT_HIMP;
    if (!strcmp (name, "knight")|| !strcmp (name, "undead"))    return MT_HKNIGHT;
    return -1;
}

mobj_t* Heretic_Spawn (int type, fixed_t x, fixed_t y)
{
    if (!Heretic_Available () || type < 0)
	return NULL;
    return P_SpawnMonsterChecked (x, y, (mobjtype_t)type);	// (C) only if it fits + fits the sector
}
