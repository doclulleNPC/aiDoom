// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	(M) Generic morph subsystem (reusable across DOOM / Heretic / Hexen).
//
//	"Turn a live monster into a temporary morph-creature, then restore it."
//	The morph mobjtype is a parameter -- Heretic passes MT_CHICKEN, Hexen will
//	pass a pig, DOOM could pass anything -- so the core is engine-agnostic.
//
//	Crispy-doom's heretic/{p_inter.c P_ChickenMorph, p_enemy.c P_UpdateChicken}
//	spawn a NEW chicken mobj and free the old one, stashing the original type in
//	the chicken's mobj_t.special2.  This fork's mobj_t has no special1/special2
//	and we are NOT allowed to grow the struct (savegames memcpy it), so instead:
//
//	  - the morph is done IN PLACE (same mobj_t pointer, just retype it), and
//	  - the original type + remaining timer live in a private SIDE-TABLE keyed
//	    by mobj_t*, aged once per tic by P_MorphTicker and cleared on level load
//	    by P_MorphReset.
//
//	The chicken actor itself is appended additively (same mechanism as
//	files/revmarine.c / files/hexen.c): enum slots at the end of
//	statenum_t/mobjtype_t/spritenum_t (info.h), tables filled at runtime here.
//
//-----------------------------------------------------------------------------

#include "doomdef.h"
#include "doomstat.h"
#include "info.h"
#include "m_fixed.h"
#include "m_random.h"
#include "sounds.h"
#include "s_sound.h"
#include "p_local.h"
#include "r_state.h"		// sprites[] -- presence test by parsed sprite
#include "p_morph.h"

extern state_t		states[];
extern mobjinfo_t	mobjinfo[];

// engine pieces we call (declared by hand, like hexen.c / revmarine.c)
extern void	A_Look (mobj_t*);
extern void	A_Chase (mobj_t*);
extern void	A_FaceTarget (mobj_t*);
extern void	A_Pain (mobj_t*);
extern void	A_Scream (mobj_t*);
extern void	A_Fall (mobj_t*);
extern boolean	P_CheckMeleeRange (mobj_t*);
extern void	P_DamageMobj (mobj_t* target, mobj_t* inflictor, mobj_t* source, int damage);
extern boolean	P_SetMobjState (mobj_t* mobj, statenum_t state);
extern mobj_t*	P_SpawnMobj (fixed_t x, fixed_t y, fixed_t z, mobjtype_t type);
extern boolean	P_CheckPosition (mobj_t* thing, fixed_t x, fixed_t y);
extern void	S_StartSound (void* origin, int sfx_id);

#define BRIGHT		32768			// FF_FULLBRIGHT frame bit
#define MORPH_RETRY	(TICRATE)		// re-try a blocked restore in ~1s

// ---------------------------------------------------------------------------
// The chicken's peck: weak melee (crispy A_ChicAttack = 1 + (P_Random()&1)).
// In crispy A_ChicAttack also drives P_UpdateChicken; that timing is owned by
// P_MorphTicker here, so this is purely the attack.
// ---------------------------------------------------------------------------
void A_ChicAttack (mobj_t* actor)
{
    if (!actor->target)
	return;
    S_StartSound (actor, actor->info->attacksound);
    if (P_CheckMeleeRange (actor))
	P_DamageMobj (actor->target, actor, actor, 1 + (P_Random () & 1));
}

// ---------------------------------------------------------------------------
// Side-table: one entry per active morph.  Keyed by the live mobj_t*.
// ---------------------------------------------------------------------------
#define MAXMORPHS	64

typedef struct
{
    mobj_t*	mo;		// the morphed actor (NULL = free slot)
    mobjtype_t	origtype;	// what it was before the morph
    int		tics;		// tics left until we try to restore
} morph_t;

static morph_t	morphs[MAXMORPHS];

static morph_t* Morph_Find (mobj_t* mo)
{
    int i;
    for (i = 0; i < MAXMORPHS; i++)
	if (morphs[i].mo == mo)
	    return &morphs[i];
    return NULL;
}

static morph_t* Morph_Alloc (void)
{
    int i;
    for (i = 0; i < MAXMORPHS; i++)
	if (!morphs[i].mo)
	    return &morphs[i];
    return NULL;
}

// Is `mt` something we refuse to morph (a boss / the morph creature itself)?
// There is no MF_NOMORPH flag, so this is an explicit blocklist of the big
// uniques + the chicken (so a chicken can't be re-morphed into another chicken).
static boolean Morph_Immune (mobjtype_t mt)
{
    switch (mt)
    {
      case MT_CHICKEN:		// already a morph creature
      case MT_CYBORG:		// cyberdemon
      case MT_SPIDER:		// spider mastermind
	return true;
      default:
	return false;
    }
}

// ---------------------------------------------------------------------------
// P_MorphMonster: transform `target` into `morphtype` for `tics` tics, in place.
// ---------------------------------------------------------------------------
boolean P_MorphMonster (mobj_t* target, mobjtype_t morphtype, int tics)
{
    morph_t*	e;
    mobjinfo_t*	mi;

    if (!target || target->player)		// players are not handled here
	return false;
    if (target->health <= 0)
	return false;
    if (!(target->flags & MF_SHOOTABLE))	// only live actors
	return false;
    if (Morph_Immune (target->type))
	return false;
    if (Morph_Find (target))			// already morphed
	return false;

    e = Morph_Alloc ();
    if (!e)
	return false;				// table full -- refuse rather than leak

    e->mo       = target;
    e->origtype = target->type;
    e->tics     = tics;

    // Morph fog + sound where it stood (the crispy MT_TFOG + sfx_telept tell).
    S_StartSound (P_SpawnMobj (target->x, target->y, target->z, MT_TFOG), sfx_telept);

    // ---- Retype IN PLACE ----
    mi = &mobjinfo[morphtype];
    target->type   = morphtype;
    target->info   = mi;
    target->health = mi->spawnhealth;
    target->radius = mi->radius;
    target->height = mi->height;
    // A normal monster: solid, shootable, counts as a kill, but clear leftover
    // attack/charge bits so it behaves like a fresh spawn.
    target->flags  = MF_SOLID | MF_SHOOTABLE | MF_COUNTKILL;
    target->target = NULL;
    target->tracer = NULL;
    P_SetMobjState (target, mi->seestate ? mi->seestate : mi->spawnstate);
    return true;
}

// ---------------------------------------------------------------------------
// Restore a morph entry to its original type, but only if the original body
// fits where the morph stands.  Returns true if restored (entry should be
// freed), false if blocked (caller re-arms a retry).
// ---------------------------------------------------------------------------
static boolean Morph_Restore (morph_t* e)
{
    mobj_t*	mo = e->mo;
    mobjinfo_t*	mi = &mobjinfo[e->origtype];
    fixed_t	morphr = mo->radius;
    fixed_t	morphh = mo->height;

    // Try the position with the ORIGINAL radius/height (P_CheckPosition reads
    // them off the thing); if it can't fit, leave it as the chicken and retry.
    mo->radius = mi->radius;
    mo->height = mi->height;
    if (!P_CheckPosition (mo, mo->x, mo->y))
    {
	mo->radius = morphr;			// revert -- still a chicken
	mo->height = morphh;
	return false;
    }

    // Fits: become the original again.
    mo->type   = e->origtype;
    mo->info   = mi;
    mo->health = mi->spawnhealth;
    mo->flags  = mi->flags;
    P_SetMobjState (mo, mi->spawnstate);

    S_StartSound (P_SpawnMobj (mo->x, mo->y, mo->z, MT_TFOG), sfx_telept);
    return true;
}

// ---------------------------------------------------------------------------
// P_MorphTicker: age every morph once per tic.
// ---------------------------------------------------------------------------
void P_MorphTicker (void)
{
    int		i;

    for (i = 0; i < MAXMORPHS; i++)
    {
	morph_t* e = &morphs[i];
	if (!e->mo)
	    continue;

	// Removed (state went S_NULL -> P_RemoveMobj sets state to NULL) or dead?
	// A dead morph stays dead as the morph-creature; just drop the entry.
	if (!e->mo->state || e->mo->type != MT_CHICKEN || e->mo->health <= 0)
	{
	    // type != MT_CHICKEN guards against a freed mobj_t being reused for a
	    // different actor type under the same pointer.
	    e->mo = NULL;
	    continue;
	}

	if (--e->tics > 0)
	    continue;

	if (Morph_Restore (e))
	    e->mo = NULL;			// restored -- free the slot
	else
	    e->tics = MORPH_RETRY;		// blocked -- try again shortly
    }
}

void P_MorphReset (void)
{
    int i;
    for (i = 0; i < MAXMORPHS; i++)
	morphs[i].mo = NULL;
}

// ---------------------------------------------------------------------------
// Table fill: the morph creature (Heretic chicken, SPR_HCHK / "HCHK*" lumps).
// Ported simple from crispy heretic/info.c S_CHICKEN_* (look/walk/peck/pain/die).
// ---------------------------------------------------------------------------
static void ST (statenum_t s, int frame, int tics, actionf_p1 act, statenum_t next)
{
    states[s].sprite      = SPR_HCHK;
    states[s].frame       = frame;
    states[s].tics        = tics;
    states[s].action.acp1 = act;
    states[s].nextstate   = next;
    states[s].misc1 = states[s].misc2 = 0;
}

void Morph_Init (void)
{
    mobjinfo_t*	m;

    P_MorphReset ();

    ST (S_CHIC_LOOK1, 0, 10, (actionf_p1)A_Look,       S_CHIC_LOOK2);
    ST (S_CHIC_LOOK2, 1, 10, (actionf_p1)A_Look,       S_CHIC_LOOK1);
    ST (S_CHIC_WALK1, 0,  3, (actionf_p1)A_Chase,      S_CHIC_WALK2);
    ST (S_CHIC_WALK2, 1,  3, (actionf_p1)A_Chase,      S_CHIC_WALK1);
    ST (S_CHIC_ATK1,  0,  8, (actionf_p1)A_FaceTarget, S_CHIC_ATK2);
    ST (S_CHIC_ATK2,  2, 10, (actionf_p1)A_ChicAttack, S_CHIC_WALK1);
    ST (S_CHIC_PAIN1, 3,  5, NULL,                     S_CHIC_PAIN2);
    ST (S_CHIC_PAIN2, 2,  5, (actionf_p1)A_Pain,       S_CHIC_WALK1);
    ST (S_CHIC_DIE1,  4,  6, (actionf_p1)A_Scream,     S_CHIC_DIE2);
    ST (S_CHIC_DIE2,  5,  6, (actionf_p1)A_Fall,       S_CHIC_DIE3);
    ST (S_CHIC_DIE3,  6,  6, NULL,                     S_CHIC_DIE4);
    ST (S_CHIC_DIE4,  7,  6, NULL,                     S_CHIC_DIE5);
    ST (S_CHIC_DIE5,  8, -1, NULL,                     S_NULL);

    // crispy MT_CHICKEN: 10 HP, slow (speed 4), small (9x22), pecks for 1..2.
    m = &mobjinfo[MT_CHICKEN];
    m->doomednum   = -1;            m->spawnstate  = S_CHIC_LOOK1; m->spawnhealth = 10;
    m->seestate    = S_CHIC_WALK1;  m->seesound    = sfx_None;  m->reactiontime = 8;
    m->attacksound = sfx_None;      m->painstate   = S_CHIC_PAIN1; m->painchance = 200;
    m->painsound   = sfx_None;      m->meleestate  = S_CHIC_ATK1; m->missilestate = S_NULL;
    m->deathstate  = S_CHIC_DIE1;   m->xdeathstate = S_NULL;    m->deathsound = sfx_None;
    m->speed = 4; m->radius = 9*FRACUNIT; m->height = 22*FRACUNIT; m->mass = 40;
    m->damage = 0; m->activesound = sfx_None;
    m->flags = MF_SOLID | MF_SHOOTABLE | MF_COUNTKILL | MF_DROPOFF;
    m->raisestate = S_NULL;
}

// hereticstuff.wad's chicken sprite loaded?  Test the PARSED sprite, not a lump.
int Morph_Available (void)
{
    return numsprites > SPR_HCHK && sprites[SPR_HCHK].numframes > 0;
}
