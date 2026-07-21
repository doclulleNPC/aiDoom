// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	SKYDEFS (ID24) -- JSON sky definitions.  https://doomwiki.org/wiki/SKYDEFS
//	Parses the SKYDEFS lump into a sky table + flat->sky mapping.  Applied so far:
//	scrolling (scrollx/scrolly) and vertical scale (scaley) of the active sky.
//	Fire skies (type 1), foreground layers (type 2), scalex, and flat->sky
//	rendering are parsed but not yet rendered (documented deferral).
//
//-----------------------------------------------------------------------------
#ifndef __R_SKYDEFS__
#define __R_SKYDEFS__

#include "doomtype.h"

typedef struct
{
    char	name[9];	// sky texture (TEXTURE1/2)
    int		type;		// 0 = normal, 1 = fire, 2 = with foreground
    double	mid;		// texel row at screen centre (unused yet)
    double	scrollx;	// texels/sec, horizontal
    double	scrolly;	// texels/sec, vertical
    double	scalex;		// (unused yet)
    double	scaley;		// vertical scale divisor (100*(1/scaley))

    // fire sky (type 1): a Fabien-Sanglard doom-fire simulation.
    unsigned char* firepal;	// game-palette indices, decay(0)..bright(N-1)
    int		firepaln;
    int		fireupdatetics;	// sim step interval (updatetime * TICRATE)
    unsigned char* firebuf;	// firew*fireh fire-palette indices
    int		firew, fireh;
    int		firelasttic;
} skydef_t;

void		R_LoadSkyDefs (void);			// parse the SKYDEFS lump if present
skydef_t*	R_SkyDefForTexNum (int texnum);		// active skydef for a texture, or NULL
void		R_SkyFireUpdate (skydef_t* sd);		// advance the fire sim (once/frame)
unsigned char*	R_SkyFireColumn (skydef_t* sd, int col_angle);	// one game-palette column

#endif	// __R_SKYDEFS__
