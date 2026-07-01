// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Freedoom DOOM2-exclusive monsters in the DOOM(1) engine -- the same additive
//	approach as files/heretic.c, but built by CLONING: the DOOM2 actors' states,
//	mobjinfo and A_* action funcs already live in this engine (they just need the
//	doom2 sprites that DOOM1 lacks).  So instead of re-typing ~340 frame rows we:
//
//	  1. take the DOOM2-exclusive monster/projectile sprites from a FREE freedoom2,
//	     RENAMED to F* (SKEL->FSKE ...) by tools/extract_freedoom2.py so they never
//	     collide with / override vanilla DOOM or a loaded doom2stuff.wad, and
//	  2. at startup (Freedoom_Init) deep-copy each DOOM2 actor's reachable state
//	     graph into a reserved S_FD_* block, remapping the sprite to its F* twin and
//	     re-pointing the missile-spawn actions at cloned F* projectiles, then build a
//	     parallel mobjinfo[MT_FD_*].  Sounds keep their DS* names (the clone reuses
//	     the same sfx indices; freedoomstuff carries those lumps).
//
//	Result: MT_FD_UNDEAD (revenant), MT_FD_FATSO (mancubus), MT_FD_VILE (arch-vile),
//	MT_FD_BABY (arachnotron), MT_FD_CHAINGUY, MT_FD_KNIGHT (hell knight),
//	MT_FD_PAIN (pain elemental), MT_FD_WOLFSS, MT_FD_KEEN -- spawnable via the
//	`summon`/`freedoom` console command, free-art and conflict-free.
//
//-----------------------------------------------------------------------------

#include <string.h>

#include "doomdef.h"
#include "info.h"
#include "m_fixed.h"
#include "tables.h"		// finecosine/finesine, ANGLETOFINESHIFT, ANG90
#include "p_mobj.h"
#include "w_wad.h"
#include "r_state.h"		// sprites[] -- presence test by parsed sprite, not lump name
#include "freedoom.h"

#define FATSPREAD	(ANG90/8)

extern state_t *states;
extern mobjinfo_t *mobjinfo;

// engine action funcs we re-point or call (no public header -- declare by hand)
extern void	A_FaceTarget (mobj_t*);
extern void	A_Fire (mobj_t*);
extern void	A_SkelMissile (mobj_t*);
extern void	A_FatAttack1 (mobj_t*);
extern void	A_FatAttack2 (mobj_t*);
extern void	A_FatAttack3 (mobj_t*);
extern void	A_VileTarget (mobj_t*);
extern void	A_BspiAttack (mobj_t*);
extern mobj_t*	P_SpawnMissile (mobj_t* source, mobj_t* dest, mobjtype_t type);
extern mobj_t*	P_SpawnMobj (fixed_t x, fixed_t y, fixed_t z, mobjtype_t type);
extern mobj_t*	P_SpawnMonsterChecked (fixed_t x, fixed_t y, mobjtype_t type);

// ---------------------------------------------------------------------------
// Projectile-spawn wrappers: byte-for-byte the engine's DOOM2 versions, but
// firing the cloned MT_FD_* projectile (which uses the renamed F* sprite) so a
// Freedoom monster never spawns the original doom2 projectile.
// ---------------------------------------------------------------------------
static void A_FD_SkelMissile (mobj_t* actor)
{
    mobj_t* mo;
    if (!actor->target) return;
    A_FaceTarget (actor);
    actor->z += 16*FRACUNIT;
    mo = P_SpawnMissile (actor, actor->target, MT_FD_TRACER);
    actor->z -= 16*FRACUNIT;
    if (mo) { mo->x += mo->momx; mo->y += mo->momy; mo->tracer = actor->target; }
}

static void A_FD_FatAttack1 (mobj_t* actor)
{
    mobj_t* mo; int an;
    A_FaceTarget (actor);
    actor->angle += FATSPREAD;
    P_SpawnMissile (actor, actor->target, MT_FD_FATSHOT);
    mo = P_SpawnMissile (actor, actor->target, MT_FD_FATSHOT);
    if (!mo) return;
    mo->angle += FATSPREAD;
    an = mo->angle >> ANGLETOFINESHIFT;
    mo->momx = FixedMul (mo->info->speed, finecosine[an]);
    mo->momy = FixedMul (mo->info->speed, finesine[an]);
}

static void A_FD_FatAttack2 (mobj_t* actor)
{
    mobj_t* mo; int an;
    A_FaceTarget (actor);
    actor->angle -= FATSPREAD;
    P_SpawnMissile (actor, actor->target, MT_FD_FATSHOT);
    mo = P_SpawnMissile (actor, actor->target, MT_FD_FATSHOT);
    if (!mo) return;
    mo->angle -= FATSPREAD*2;
    an = mo->angle >> ANGLETOFINESHIFT;
    mo->momx = FixedMul (mo->info->speed, finecosine[an]);
    mo->momy = FixedMul (mo->info->speed, finesine[an]);
}

static void A_FD_FatAttack3 (mobj_t* actor)
{
    mobj_t* mo; int an;
    A_FaceTarget (actor);
    mo = P_SpawnMissile (actor, actor->target, MT_FD_FATSHOT);
    if (mo) {
	mo->angle -= FATSPREAD/2;
	an = mo->angle >> ANGLETOFINESHIFT;
	mo->momx = FixedMul (mo->info->speed, finecosine[an]);
	mo->momy = FixedMul (mo->info->speed, finesine[an]);
    }
    mo = P_SpawnMissile (actor, actor->target, MT_FD_FATSHOT);
    if (mo) {
	mo->angle += FATSPREAD/2;
	an = mo->angle >> ANGLETOFINESHIFT;
	mo->momx = FixedMul (mo->info->speed, finecosine[an]);
	mo->momy = FixedMul (mo->info->speed, finesine[an]);
    }
}

static void A_FD_VileTarget (mobj_t* actor)
{
    mobj_t* fog;
    if (!actor->target) return;
    A_FaceTarget (actor);
    fog = P_SpawnMobj (actor->target->x, actor->target->x,	// (x,x,z): vanilla quirk kept
		       actor->target->z, MT_FD_FIRE);
    actor->tracer = fog;
    fog->target = actor;
    fog->tracer = actor->target;
    A_Fire (fog);
}

static void A_FD_BspiAttack (mobj_t* actor)
{
    if (!actor->target) return;
    A_FaceTarget (actor);
    P_SpawnMissile (actor, actor->target, MT_FD_ARACHPLAZ);
}

// ---------------------------------------------------------------------------
// Clone machinery
// ---------------------------------------------------------------------------
// DOOM2 sprite -> Freedoom renamed sprite (must match info.h SPR_F* + extract_freedoom2.py)
static const struct { short from, to; } sprmap[] = {
    { SPR_SKEL, SPR_FSKE }, { SPR_FATT, SPR_FFAT }, { SPR_VILE, SPR_FVIL },
    { SPR_BSPI, SPR_FBSP }, { SPR_CPOS, SPR_FCPO }, { SPR_BOS2, SPR_FBO2 },
    { SPR_PAIN, SPR_FPAI }, { SPR_SSWV, SPR_FSSW }, { SPR_KEEN, SPR_FKEE },
    { SPR_FATB, SPR_FFAB }, { SPR_FBXP, SPR_FFBX }, { SPR_MANF, SPR_FMAN },
    { SPR_FIRE, SPR_FFIR }, { SPR_APLS, SPR_FAPL }, { SPR_APBX, SPR_FAPB },
};

static int RemapSprite (int s)
{
    unsigned i;
    for (i = 0; i < sizeof(sprmap)/sizeof(sprmap[0]); i++)
	if (sprmap[i].from == s) return sprmap[i].to;
    return s;
}

static actionf_p1 RemapAction (actionf_p1 a)
{
    if (a == (actionf_p1)A_SkelMissile) return (actionf_p1)A_FD_SkelMissile;
    if (a == (actionf_p1)A_FatAttack1)  return (actionf_p1)A_FD_FatAttack1;
    if (a == (actionf_p1)A_FatAttack2)  return (actionf_p1)A_FD_FatAttack2;
    if (a == (actionf_p1)A_FatAttack3)  return (actionf_p1)A_FD_FatAttack3;
    if (a == (actionf_p1)A_VileTarget)  return (actionf_p1)A_FD_VileTarget;
    if (a == (actionf_p1)A_BspiAttack)  return (actionf_p1)A_FD_BspiAttack;
    return a;					// every other action is sprite-agnostic
}

static statenum_t	fd_next;		// next free slot in the S_FD_* reserve
static statenum_t	fd_map[NUMSTATES];	// original state -> clone (per-actor, 0 = none)

// Deep-copy a state and everything reachable via nextstate into the FD reserve,
// remapping sprite + missile-spawn action.  Memoised so loops terminate.
static statenum_t CloneState (statenum_t s)
{
    statenum_t fd;
    if (s == S_NULL) return S_NULL;
    if (fd_map[s])   return fd_map[s];
    if (fd_next > S_FD_LAST) return s;		// reserve exhausted -> reuse original (safe)
    fd = fd_next++;
    fd_map[s] = fd;
    states[fd] = states[s];
    states[fd].sprite      = RemapSprite (states[s].sprite);
    states[fd].action.acp1 = RemapAction (states[s].action.acp1);
    states[fd].nextstate   = CloneState (states[s].nextstate);
    return fd;
}

static void CloneMobj (mobjtype_t from, mobjtype_t to)
{
    mobjinfo_t* m;
    memset (fd_map, 0, sizeof(fd_map));		// each actor gets an independent graph
    mobjinfo[to] = mobjinfo[from];
    m = &mobjinfo[to];
    m->doomednum    = -1;			// summon-only, no map ednum clash
    m->spawnstate   = CloneState (mobjinfo[from].spawnstate);
    m->seestate     = CloneState (mobjinfo[from].seestate);
    m->painstate    = CloneState (mobjinfo[from].painstate);
    m->meleestate   = CloneState (mobjinfo[from].meleestate);
    m->missilestate = CloneState (mobjinfo[from].missilestate);
    m->deathstate   = CloneState (mobjinfo[from].deathstate);
    m->xdeathstate  = CloneState (mobjinfo[from].xdeathstate);
    m->raisestate   = CloneState (mobjinfo[from].raisestate);
}

void Freedoom_Init (void)
{
    fd_next = S_FD_FIRST;
    // projectiles first (the wrappers spawn these at runtime)
    CloneMobj (MT_TRACER,    MT_FD_TRACER);
    CloneMobj (MT_FATSHOT,   MT_FD_FATSHOT);
    CloneMobj (MT_FIRE,      MT_FD_FIRE);
    CloneMobj (MT_ARACHPLAZ, MT_FD_ARACHPLAZ);
    // monsters
    CloneMobj (MT_UNDEAD,   MT_FD_UNDEAD);
    CloneMobj (MT_FATSO,    MT_FD_FATSO);
    CloneMobj (MT_VILE,     MT_FD_VILE);
    CloneMobj (MT_BABY,     MT_FD_BABY);
    CloneMobj (MT_CHAINGUY, MT_FD_CHAINGUY);
    CloneMobj (MT_KNIGHT,   MT_FD_KNIGHT);
    CloneMobj (MT_PAIN,     MT_FD_PAIN);
    CloneMobj (MT_WOLFSS,   MT_FD_WOLFSS);
    CloneMobj (MT_KEEN,     MT_FD_KEEN);
}

// freedoomstuff.wad's renamed sprites loaded?  Test the PARSED sprite (numframes), not a
// guessed lump name: the revenant's frame-A lumps are mirror-packed ("FSKEA1D1", not
// "FSKEA1"), so W_CheckNumForName("FSKEA1") wrongly returns -1 and blocked every summon.
int Freedoom_Available (void)
{
    return numsprites > SPR_FSKE && sprites[SPR_FSKE].numframes > 0;
}

int Freedoom_TypeByName (const char* name)
{
    if (!name || !name[0]) return -1;
    if (!strcmp (name, "revenant") || !strcmp (name, "skeleton")) return MT_FD_UNDEAD;
    if (!strcmp (name, "mancubus") || !strcmp (name, "fatso"))    return MT_FD_FATSO;
    if (!strcmp (name, "archvile") || !strcmp (name, "vile"))     return MT_FD_VILE;
    if (!strcmp (name, "arachnotron") || !strcmp (name, "baby"))  return MT_FD_BABY;
    if (!strcmp (name, "chaingunner") || !strcmp (name, "chainguy")) return MT_FD_CHAINGUY;
    if (!strcmp (name, "hellknight") || !strcmp (name, "knight2")) return MT_FD_KNIGHT;
    if (!strcmp (name, "painelemental") || !strcmp (name, "pain")) return MT_FD_PAIN;
    if (!strcmp (name, "ss") || !strcmp (name, "wolfss"))         return MT_FD_WOLFSS;
    if (!strcmp (name, "keen"))                                   return MT_FD_KEEN;
    return -1;
}

mobj_t* Freedoom_Spawn (int type, fixed_t x, fixed_t y)
{
    if (!Freedoom_Available () || type < 0)
	return NULL;
    return P_SpawnMonsterChecked (x, y, (mobjtype_t)type);
}
