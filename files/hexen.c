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
    return -1;
}

mobj_t* Hexen_Spawn (int type, fixed_t x, fixed_t y)
{
    if (!Hexen_Available () || type < 0)
	return NULL;
    return P_SpawnMonsterChecked (x, y, type);
}
