// Additive Hexen content for BuddyDoom (approach A in HERETIC_HEXEN.md, same as heretic.c):
// Hexen monsters ported from crispy-doom's hexen/info.c + p_enemy.c, appended to the
// engine's states[]/mobjinfo[] tables at runtime.  Sprites come from hexenstuff.wad (the
// renamed X* sprites built by tools/extract_hexen.py); sounds reuse DOOM SFX for now.
// Activated only when those sprites are present.
#ifndef __HEXEN_H__
#define __HEXEN_H__

#include "m_fixed.h"
struct mobj_s;

// Fill the Hexen slots appended to states[]/mobjinfo[].  Call once at startup
// (after the info tables exist, before any Hexen monster spawns).
void Hexen_Init (void);

// True if hexenstuff.wad's sprites are loaded -- spawn Hexen monsters only then, else
// they'd render as a blank (0-frame) sprite.
int  Hexen_Available (void);

// Map a name ("ettin"/...) to a Hexen mobjtype, or -1 if unknown.
int  Hexen_TypeByName (const char* name);

// Spawn a Hexen monster at (x,y) on the floor; NULL if unavailable or type<0.
struct mobj_s* Hexen_Spawn (int type, fixed_t x, fixed_t y);

#endif
