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

// Spawn a Heretic mummy at (x,y) on the floor; NULL if unavailable.
struct mobj_s* Heretic_SpawnMummy (fixed_t x, fixed_t y);

#endif
