// Additive Heretic content for aiDoom (approach A in HERETIC_HEXEN.md): a handful of
// Heretic monsters ported from crispy-doom's heretic/info.c + p_enemy.c, appended to
// the engine's states[]/mobjinfo[] tables.  Sprites come from hereticstuff.wad (the
// renamed H* sprites built by tools/extract_heretic_monsters.py); sounds reuse DOOM
// SFX for now.  Activated only when those sprites are present.
#ifndef __HERETIC__
#define __HERETIC__

#include "m_fixed.h"
struct mobj_s;

// Fill the Heretic slots appended to states[]/mobjinfo[].  Call once at startup
// (after the info tables exist, before any Heretic monster spawns).
void Heretic_Init (void);

// True if hereticstuff.wad's sprites are loaded -- spawn Heretic monsters only then,
// else they'd render as a blank (0-frame) sprite.
int  Heretic_Available (void);

// Map a name ("mummy"/"clink"/"imp"/...) to a Heretic mobjtype, or -1 if unknown
// (empty/NULL -> mummy).
int  Heretic_TypeByName (const char* name);

// Spawn a Heretic monster (mobjtype from Heretic_TypeByName) at (x,y) on the floor;
// NULL if unavailable or type<0.
struct mobj_s* Heretic_Spawn (int type, fixed_t x, fixed_t y);

// Phase 1 map loading: map a real Heretic map-thing doomednum (crispy heretic/info.c)
// to the corresponding aiDoom mobjtype (MT_H* monsters + MT_HARTI_* artifacts, plus a
// few trivial pickup substitutions), or -1 for an unported Heretic thing (skip it).
int  P_HereticThingType (int doomednum);

#endif
