// (M) Generic morph subsystem -- see files/p_morph.c.
//
// Temporarily turns a live monster into a "morph-creature" (Heretic uses a
// chicken; Hexen will use a pig; DOOM could use anything), then restores it.
// Reusable + parameterised by the morph mobjtype, so any inventory can drive it.
//
// mobj_t has no spare special1/special2 fields and we must NOT grow the struct
// (savegame layout), so the morph bookkeeping lives in a private side-table keyed
// by mobj_t* -- P_MorphTicker ages it and P_MorphReset clears it on level load.
#ifndef __P_MORPH_H__
#define __P_MORPH_H__

#include "doomtype.h"
#include "info.h"		// mobjtype_t
#include "p_mobj.h"		// mobj_t

// Fill the appended morph-creature (chicken) states/mobjinfo.  Call once at
// startup, next to RevMarine_Init / HereticInv_Init in d_main.c.
void		Morph_Init (void);

// True once the morph-creature sprite (SPR_HCHK from hereticstuff.wad) is loaded.
// The morph still works without it (the chicken just has no art); use this only
// if a caller wants to gate on the wad being present.
int		Morph_Available (void);

// Turn `target` into `morphtype` for `tics` tics.  Returns false (no morph) if the
// target is a boss / unmorphable / the morph-creature itself / a player / already
// morphed.  On success it transforms IN PLACE and records the original type in the
// side-table so P_MorphTicker can restore it later.
boolean		P_MorphMonster (mobj_t* target, mobjtype_t morphtype, int tics);

// Age every morph (call once per tic from P_Ticker).  Drops dead/removed entries;
// when an entry's timer expires it restores the original type if it fits, else
// re-arms a short retry.
void		P_MorphTicker (void);

// Clear the side-table (call from P_SetupLevel so morphs don't leak across levels).
void		P_MorphReset (void);

#endif
