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

#include "doomdef.h"
#include "info.h"
#include "m_random.h"
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
}

// hereticstuff.wad sprites loaded?  (the mummy's first frame lump)
int Heretic_Available (void)
{
    return W_CheckNumForName ("HMUMA1") >= 0;
}

mobj_t* Heretic_SpawnMummy (fixed_t x, fixed_t y)
{
    if (!Heretic_Available ())
	return NULL;
    return P_SpawnMobj (x, y, ONFLOORZ, MT_HMUMMY);
}
