// Additive Freedoom DOOM2 monsters for aiDoom: runtime clones of the engine's DOOM2
// actors (revenant, mancubus, arch-vile, ...) with sprites renamed to F* (from
// freedoom2stuff.wad) so they never collide with / override DOOM or doom2stuff.
// See files/freedoom.c.
#ifndef __FREEDOOM__
#define __FREEDOOM__

#include "m_fixed.h"
struct mobj_s;

// Build the MT_FD_* clones (states in the reserved S_FD_* block).  Call once at
// startup, after the info tables exist and before any Freedoom monster spawns.
void Freedoom_Init (void);

// True if freedoom2stuff.wad's renamed sprites are loaded (else they'd render blank).
int  Freedoom_Available (void);

// Map a name ("revenant"/"mancubus"/...) to an MT_FD_* type, or -1 if unknown.
int  Freedoom_TypeByName (const char* name);

// Spawn a Freedoom monster (validated placement) at (x,y); NULL if unavailable.
struct mobj_s* Freedoom_Spawn (int type, fixed_t x, fixed_t y);

#endif
