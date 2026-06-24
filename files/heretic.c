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
//	Monsters: Mummy, Sabreclaw, Gargoyle, Knight (melee), + Weredragon, Disciple,
//	Ophidian (ranged w/ projectiles).  Bosses (Maulotaur, Iron Lich, D'Sparil) TODO.
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

#define HITDICE(d)	(((P_Random () & 7) + 1) * (d))

// Weredragon (beast): melee HITDICE(3), else hurl a fireball.
void A_BeastAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    if (P_CheckMeleeRange (actor))
	{ P_DamageMobj (actor->target, actor, actor, HITDICE (3)); return; }
    P_SpawnMissile (actor, actor->target, MT_HBEASTBALL);
}

// Disciple (wizard): melee HITDICE(4), else fire a homing-coloured bolt.
void A_WizardAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    if (P_CheckMeleeRange (actor))
	{ P_DamageMobj (actor->target, actor, actor, HITDICE (4)); return; }
    P_SpawnMissile (actor, actor->target, MT_HWIZFX);
}

// Ophidian (snake): pure ranged -- spit a projectile.
void A_SnakeAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    P_SpawnMissile (actor, actor->target, MT_HSNAKEPRO);
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

    // ====================================================================
    // Weredragon (beast): melee + lobbed fireball
    // ====================================================================
    ST (S_HBEA_LOOK1, SPR_HBEA, 0, 10, (actionf_p1)A_Look,        S_HBEA_LOOK2);
    ST (S_HBEA_LOOK2, SPR_HBEA, 1, 10, (actionf_p1)A_Look,        S_HBEA_LOOK1);
    ST (S_HBEA_WALK1, SPR_HBEA, 0,  3, (actionf_p1)A_Chase,       S_HBEA_WALK2);
    ST (S_HBEA_WALK2, SPR_HBEA, 1,  3, (actionf_p1)A_Chase,       S_HBEA_WALK3);
    ST (S_HBEA_WALK3, SPR_HBEA, 2,  3, (actionf_p1)A_Chase,       S_HBEA_WALK4);
    ST (S_HBEA_WALK4, SPR_HBEA, 3,  3, (actionf_p1)A_Chase,       S_HBEA_WALK5);
    ST (S_HBEA_WALK5, SPR_HBEA, 4,  3, (actionf_p1)A_Chase,       S_HBEA_WALK6);
    ST (S_HBEA_WALK6, SPR_HBEA, 5,  3, (actionf_p1)A_Chase,       S_HBEA_WALK1);
    ST (S_HBEA_ATK1,  SPR_HBEA, 7, 10, (actionf_p1)A_FaceTarget,  S_HBEA_ATK2);
    ST (S_HBEA_ATK2,  SPR_HBEA, 8, 10, (actionf_p1)A_BeastAttack, S_HBEA_WALK1);
    ST (S_HBEA_PAIN1, SPR_HBEA, 6,  3, NULL,                      S_HBEA_PAIN2);
    ST (S_HBEA_PAIN2, SPR_HBEA, 6,  3, (actionf_p1)A_Pain,        S_HBEA_WALK1);
    ST (S_HBEA_DIE1,  SPR_HBEA, 17, 6, NULL,                      S_HBEA_DIE2);
    ST (S_HBEA_DIE2,  SPR_HBEA, 18, 6, (actionf_p1)A_Scream,      S_HBEA_DIE3);
    ST (S_HBEA_DIE3,  SPR_HBEA, 19, 6, NULL,                      S_HBEA_DIE4);
    ST (S_HBEA_DIE4,  SPR_HBEA, 20, 6, NULL,                      S_HBEA_DIE5);
    ST (S_HBEA_DIE5,  SPR_HBEA, 21, 6, NULL,                      S_HBEA_DIE6);
    ST (S_HBEA_DIE6,  SPR_HBEA, 22, 6, (actionf_p1)A_Fall,        S_HBEA_DIE7);
    ST (S_HBEA_DIE7,  SPR_HBEA, 23, 6, NULL,                      S_HBEA_DIE8);
    ST (S_HBEA_DIE8,  SPR_HBEA, 24, 6, NULL,                      S_HBEA_DIE9);
    ST (S_HBEA_DIE9,  SPR_HBEA, 25, -1, NULL,                     S_NULL);
    ST (S_HBEB1, SPR_HBEB, 32768, 2, NULL, S_HBEB2);
    ST (S_HBEB2, SPR_HBEB, 32768, 2, NULL, S_HBEB3);
    ST (S_HBEB3, SPR_HBEB, 32769, 2, NULL, S_HBEB4);
    ST (S_HBEB4, SPR_HBEB, 32769, 2, NULL, S_HBEB5);
    ST (S_HBEB5, SPR_HBEB, 32770, 2, NULL, S_HBEB6);
    ST (S_HBEB6, SPR_HBEB, 32770, 2, NULL, S_HBEB1);
    ST (S_HBEBX1, SPR_HBEB, 32771, 4, NULL, S_HBEBX2);
    ST (S_HBEBX2, SPR_HBEB, 32772, 4, NULL, S_HBEBX3);
    ST (S_HBEBX3, SPR_HBEB, 32773, 4, NULL, S_HBEBX4);
    ST (S_HBEBX4, SPR_HBEB, 32774, 4, NULL, S_HBEBX5);
    ST (S_HBEBX5, SPR_HBEB, 32775, 4, NULL, S_NULL);

    m = &mobjinfo[MT_HBEAST];
    m->doomednum = -1;        m->spawnstate  = S_HBEA_LOOK1; m->spawnhealth = 220;
    m->seestate  = S_HBEA_WALK1; m->seesound  = sfx_bgsit2;  m->reactiontime = 8;
    m->attacksound = sfx_firsht; m->painstate = S_HBEA_PAIN1; m->painchance = 100;
    m->painsound = sfx_popain; m->meleestate = S_HBEA_ATK1;  m->missilestate = S_HBEA_ATK1;
    m->deathstate = S_HBEA_DIE1; m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth2;
    m->speed = 14; m->radius = 32*FRACUNIT; m->height = 74*FRACUNIT; m->mass = 200;
    m->damage = 0; m->activesound = sfx_bgact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL; m->raisestate = S_NULL;

    m = &mobjinfo[MT_HBEASTBALL];
    m->doomednum = -1;        m->spawnstate  = S_HBEB1;      m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;    m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;      m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;     m->missilestate = S_NULL;
    m->deathstate = S_HBEBX1;    m->xdeathstate = S_NULL;    m->deathsound = sfx_firxpl;
    m->speed = 12*FRACUNIT; m->radius = 9*FRACUNIT; m->height = 8*FRACUNIT; m->mass = 100;
    m->damage = 4; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;

    // ====================================================================
    // Disciple (wizard): floating caster, fires bolts
    // ====================================================================
    ST (S_HWIZ_LOOK1, SPR_HWIZ, 0, 10, (actionf_p1)A_Look,         S_HWIZ_LOOK2);
    ST (S_HWIZ_LOOK2, SPR_HWIZ, 1, 10, (actionf_p1)A_Look,         S_HWIZ_LOOK1);
    ST (S_HWIZ_WALK1, SPR_HWIZ, 0,  3, (actionf_p1)A_Chase,        S_HWIZ_WALK2);
    ST (S_HWIZ_WALK2, SPR_HWIZ, 0,  4, (actionf_p1)A_Chase,        S_HWIZ_WALK3);
    ST (S_HWIZ_WALK3, SPR_HWIZ, 0,  3, (actionf_p1)A_Chase,        S_HWIZ_WALK4);
    ST (S_HWIZ_WALK4, SPR_HWIZ, 0,  4, (actionf_p1)A_Chase,        S_HWIZ_WALK5);
    ST (S_HWIZ_WALK5, SPR_HWIZ, 1,  3, (actionf_p1)A_Chase,        S_HWIZ_WALK6);
    ST (S_HWIZ_WALK6, SPR_HWIZ, 1,  4, (actionf_p1)A_Chase,        S_HWIZ_WALK7);
    ST (S_HWIZ_WALK7, SPR_HWIZ, 1,  3, (actionf_p1)A_Chase,        S_HWIZ_WALK8);
    ST (S_HWIZ_WALK8, SPR_HWIZ, 1,  4, (actionf_p1)A_Chase,        S_HWIZ_WALK1);
    ST (S_HWIZ_ATK1,  SPR_HWIZ, 2,  4, (actionf_p1)A_FaceTarget,   S_HWIZ_ATK2);
    ST (S_HWIZ_ATK2,  SPR_HWIZ, 2,  4, (actionf_p1)A_FaceTarget,   S_HWIZ_ATK3);
    ST (S_HWIZ_ATK3,  SPR_HWIZ, 3, 12, (actionf_p1)A_WizardAttack, S_HWIZ_WALK1);
    ST (S_HWIZ_PAIN1, SPR_HWIZ, 4,  3, NULL,                       S_HWIZ_PAIN2);
    ST (S_HWIZ_PAIN2, SPR_HWIZ, 4,  3, (actionf_p1)A_Pain,         S_HWIZ_WALK1);
    ST (S_HWIZ_DIE1,  SPR_HWIZ, 5,  6, NULL,                       S_HWIZ_DIE2);
    ST (S_HWIZ_DIE2,  SPR_HWIZ, 6,  6, (actionf_p1)A_Scream,       S_HWIZ_DIE3);
    ST (S_HWIZ_DIE3,  SPR_HWIZ, 7,  6, NULL,                       S_HWIZ_DIE4);
    ST (S_HWIZ_DIE4,  SPR_HWIZ, 8,  6, NULL,                       S_HWIZ_DIE5);
    ST (S_HWIZ_DIE5,  SPR_HWIZ, 9,  6, (actionf_p1)A_Fall,         S_HWIZ_DIE6);
    ST (S_HWIZ_DIE6,  SPR_HWIZ, 10, 6, NULL,                       S_HWIZ_DIE7);
    ST (S_HWIZ_DIE7,  SPR_HWIZ, 11, 6, NULL,                       S_HWIZ_DIE8);
    ST (S_HWIZ_DIE8,  SPR_HWIZ, 12, -1, NULL,                      S_NULL);
    ST (S_HWIB1, SPR_HWIB, 32768, 6, NULL, S_HWIB2);
    ST (S_HWIB2, SPR_HWIB, 32769, 6, NULL, S_HWIB1);
    ST (S_HWIBX1, SPR_HWIB, 32770, 5, NULL, S_HWIBX2);
    ST (S_HWIBX2, SPR_HWIB, 32771, 5, NULL, S_HWIBX3);
    ST (S_HWIBX3, SPR_HWIB, 32772, 5, NULL, S_HWIBX4);
    ST (S_HWIBX4, SPR_HWIB, 32773, 5, NULL, S_HWIBX5);
    ST (S_HWIBX5, SPR_HWIB, 32774, 5, NULL, S_NULL);

    m = &mobjinfo[MT_HWIZARD];
    m->doomednum = -1;        m->spawnstate  = S_HWIZ_LOOK1; m->spawnhealth = 180;
    m->seestate  = S_HWIZ_WALK1; m->seesound  = sfx_cacsit;  m->reactiontime = 8;
    m->attacksound = sfx_firsht; m->painstate = S_HWIZ_PAIN1; m->painchance = 64;
    m->painsound = sfx_dmpain; m->meleestate = S_NULL;       m->missilestate = S_HWIZ_ATK1;
    m->deathstate = S_HWIZ_DIE1; m->xdeathstate = S_NULL;    m->deathsound = sfx_cacdth;
    m->speed = 12; m->radius = 16*FRACUNIT; m->height = 68*FRACUNIT; m->mass = 100;
    m->damage = 0; m->activesound = sfx_dmact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_FLOAT|MF_NOGRAVITY|MF_COUNTKILL; m->raisestate = S_NULL;

    m = &mobjinfo[MT_HWIZFX];
    m->doomednum = -1;        m->spawnstate  = S_HWIB1;      m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;    m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;      m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;     m->missilestate = S_NULL;
    m->deathstate = S_HWIBX1;    m->xdeathstate = S_NULL;    m->deathsound = sfx_firxpl;
    m->speed = 18*FRACUNIT; m->radius = 10*FRACUNIT; m->height = 6*FRACUNIT; m->mass = 100;
    m->damage = 3; m->activesound = sfx_None;
    m->flags = MF_NOBLOCKMAP|MF_MISSILE|MF_DROPOFF|MF_NOGRAVITY; m->raisestate = S_NULL;

    // ====================================================================
    // Ophidian (snake): stationary-ish ranged spitter
    // ====================================================================
    ST (S_HSNK_LOOK1, SPR_HSNK, 0, 10, (actionf_p1)A_Look,        S_HSNK_LOOK2);
    ST (S_HSNK_LOOK2, SPR_HSNK, 1, 10, (actionf_p1)A_Look,        S_HSNK_LOOK1);
    ST (S_HSNK_WALK1, SPR_HSNK, 0,  4, (actionf_p1)A_Chase,       S_HSNK_WALK2);
    ST (S_HSNK_WALK2, SPR_HSNK, 1,  4, (actionf_p1)A_Chase,       S_HSNK_WALK3);
    ST (S_HSNK_WALK3, SPR_HSNK, 2,  4, (actionf_p1)A_Chase,       S_HSNK_WALK4);
    ST (S_HSNK_WALK4, SPR_HSNK, 3,  4, (actionf_p1)A_Chase,       S_HSNK_WALK1);
    ST (S_HSNK_ATK1,  SPR_HSNK, 5,  5, (actionf_p1)A_FaceTarget,  S_HSNK_ATK2);
    ST (S_HSNK_ATK2,  SPR_HSNK, 5,  5, (actionf_p1)A_FaceTarget,  S_HSNK_ATK3);
    ST (S_HSNK_ATK3,  SPR_HSNK, 5,  4, (actionf_p1)A_SnakeAttack, S_HSNK_ATK4);
    ST (S_HSNK_ATK4,  SPR_HSNK, 5,  4, (actionf_p1)A_SnakeAttack, S_HSNK_ATK5);
    ST (S_HSNK_ATK5,  SPR_HSNK, 5,  4, (actionf_p1)A_SnakeAttack, S_HSNK_WALK1);
    ST (S_HSNK_PAIN1, SPR_HSNK, 4,  3, NULL,                      S_HSNK_PAIN2);
    ST (S_HSNK_PAIN2, SPR_HSNK, 4,  3, (actionf_p1)A_Pain,        S_HSNK_WALK1);
    ST (S_HSNK_DIE1,  SPR_HSNK, 6,  5, NULL,                      S_HSNK_DIE2);
    ST (S_HSNK_DIE2,  SPR_HSNK, 7,  5, (actionf_p1)A_Scream,      S_HSNK_DIE3);
    ST (S_HSNK_DIE3,  SPR_HSNK, 8,  5, NULL,                      S_HSNK_DIE4);
    ST (S_HSNK_DIE4,  SPR_HSNK, 9,  5, NULL,                      S_HSNK_DIE5);
    ST (S_HSNK_DIE5,  SPR_HSNK, 10, 5, NULL,                      S_HSNK_DIE6);
    ST (S_HSNK_DIE6,  SPR_HSNK, 11, 5, NULL,                      S_HSNK_DIE7);
    ST (S_HSNK_DIE7,  SPR_HSNK, 12, 5, (actionf_p1)A_Fall,        S_HSNK_DIE8);
    ST (S_HSNK_DIE8,  SPR_HSNK, 13, 5, NULL,                      S_HSNK_DIE9);
    ST (S_HSNK_DIE9,  SPR_HSNK, 14, 5, NULL,                      S_HSNK_DIE10);
    ST (S_HSNK_DIE10, SPR_HSNK, 15, -1, NULL,                     S_NULL);
    ST (S_HSNB1, SPR_HSNB, 32768, 5, NULL, S_HSNB2);
    ST (S_HSNB2, SPR_HSNB, 32769, 5, NULL, S_HSNB3);
    ST (S_HSNB3, SPR_HSNB, 32770, 5, NULL, S_HSNB4);
    ST (S_HSNB4, SPR_HSNB, 32771, 5, NULL, S_HSNB1);
    ST (S_HSNBX1, SPR_HSNB, 32772, 5, NULL, S_HSNBX2);
    ST (S_HSNBX2, SPR_HSNB, 32773, 5, NULL, S_HSNBX3);
    ST (S_HSNBX3, SPR_HSNB, 32774, 4, NULL, S_HSNBX4);
    ST (S_HSNBX4, SPR_HSNB, 32775, 3, NULL, S_HSNBX5);
    ST (S_HSNBX5, SPR_HSNB, 32776, 3, NULL, S_NULL);

    m = &mobjinfo[MT_HSNAKE];
    m->doomednum = -1;        m->spawnstate  = S_HSNK_LOOK1; m->spawnhealth = 280;
    m->seestate  = S_HSNK_WALK1; m->seesound  = sfx_bgsit1;  m->reactiontime = 8;
    m->attacksound = sfx_firsht; m->painstate = S_HSNK_PAIN1; m->painchance = 48;
    m->painsound = sfx_popain; m->meleestate = S_NULL;       m->missilestate = S_HSNK_ATK1;
    m->deathstate = S_HSNK_DIE1; m->xdeathstate = S_NULL;    m->deathsound = sfx_bgdth1;
    m->speed = 10; m->radius = 22*FRACUNIT; m->height = 70*FRACUNIT; m->mass = 100;
    m->damage = 0; m->activesound = sfx_bgact;
    m->flags = MF_SOLID|MF_SHOOTABLE|MF_COUNTKILL; m->raisestate = S_NULL;

    m = &mobjinfo[MT_HSNAKEPRO];
    m->doomednum = -1;        m->spawnstate  = S_HSNB1;      m->spawnhealth = 1000;
    m->seestate  = S_NULL;       m->seesound  = sfx_None;    m->reactiontime = 8;
    m->attacksound = sfx_None;   m->painstate = S_NULL;      m->painchance = 0;
    m->painsound = sfx_None;     m->meleestate = S_NULL;     m->missilestate = S_NULL;
    m->deathstate = S_HSNBX1;    m->xdeathstate = S_NULL;    m->deathsound = sfx_firxpl;
    m->speed = 14*FRACUNIT; m->radius = 12*FRACUNIT; m->height = 8*FRACUNIT; m->mass = 100;
    m->damage = 3; m->activesound = sfx_None;
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
    if (!strcmp (name, "beast") || !strcmp (name, "weredragon")) return MT_HBEAST;
    if (!strcmp (name, "wizard")|| !strcmp (name, "disciple"))  return MT_HWIZARD;
    if (!strcmp (name, "snake") || !strcmp (name, "ophidian"))  return MT_HSNAKE;
    return -1;
}

mobj_t* Heretic_Spawn (int type, fixed_t x, fixed_t y)
{
    if (!Heretic_Available () || type < 0)
	return NULL;
    return P_SpawnMonsterChecked (x, y, (mobjtype_t)type);	// (C) only if it fits + fits the sector
}
