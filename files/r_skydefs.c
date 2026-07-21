// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	SKYDEFS (ID24) parser + lookup.  See r_skydefs.h.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "doomtype.h"
#include "w_wad.h"
#include "z_zone.h"
#include "w_json.h"
#include "r_skydefs.h"

extern int	R_CheckTextureNumForName (char* name);

static skydef_t*	skydefs;
static int		numskydefs;

typedef struct { char flat[9]; char sky[9]; } flatmap_t;
static flatmap_t*	flatmaps;
static int		numflatmaps;

static void CopyName8 (char* dst, const char* src)
{
    int i;
    for (i = 0; i < 8 && src[i]; i++) dst[i] = (char)toupper ((unsigned char)src[i]);
    for (; i < 9; i++) dst[i] = 0;
}

void R_LoadSkyDefs (void)
{
    int		lump = W_CheckNumForName ("SKYDEFS");
    json_t*	root;
    json_t*	data;
    json_t*	skies;
    json_t*	fmaps;
    int		i;

    if (lump < 0)
	return;

    root = JSON_Parse ((const char*) W_CacheLumpNum (lump, PU_CACHE), W_LumpLength (lump));
    if (!root)
    {
	fprintf (stderr, "SKYDEFS: JSON parse error -- ignored.\n");
	return;
    }

    data  = JSON_Get (root, "data");
    skies = data ? JSON_Get (data, "skies") : NULL;
    fmaps = data ? JSON_Get (data, "flatmapping") : NULL;

    if (skies && skies->type == JSON_ARR && skies->n > 0)
    {
	skydefs = malloc (skies->n * sizeof *skydefs);
	for (i = 0; i < skies->n; i++)
	{
	    json_t*	s  = JSON_Index (skies, i);
	    skydef_t*	sd = &skydefs[numskydefs];
	    memset (sd, 0, sizeof *sd);
	    CopyName8 (sd->name, JSON_Str (JSON_Get (s, "name")));
	    sd->type    = (int) JSON_Num (JSON_Get (s, "type"),   0);
	    sd->mid     =       JSON_Num (JSON_Get (s, "mid"),    0);
	    sd->scrollx =       JSON_Num (JSON_Get (s, "scrollx"),0);
	    sd->scrolly =       JSON_Num (JSON_Get (s, "scrolly"),0);
	    sd->scalex  =       JSON_Num (JSON_Get (s, "scalex"), 1);
	    sd->scaley  =       JSON_Num (JSON_Get (s, "scaley"), 1);
	    if (sd->scalex == 0) sd->scalex = 1;
	    if (sd->scaley == 0) sd->scaley = 1;

	    // fire sky (type 1): palette ramp + updatetime
	    if (sd->type == 1)
	    {
		json_t* fire = JSON_Get (s, "fire");
		json_t* pal  = fire ? JSON_Get (fire, "palette") : NULL;
		double  ut   = fire ? JSON_Num (JSON_Get (fire, "updatetime"), 0.05) : 0.05;
		if (pal && pal->type == JSON_ARR && pal->n > 0)
		{
		    int k;
		    sd->firepaln = pal->n;
		    sd->firepal  = malloc (pal->n);
		    for (k = 0; k < pal->n; k++)
			sd->firepal[k] = (unsigned char)(int) JSON_Num (JSON_Index (pal, k), 0);
		    sd->fireupdatetics = (int)(ut * 35.0); if (sd->fireupdatetics < 1) sd->fireupdatetics = 1;
		    sd->firew = 256; sd->fireh = 128;
		    sd->firebuf = calloc (sd->firew * sd->fireh, 1);
		    for (k = 0; k < sd->firew; k++)		// bottom row = brightest (fire source)
			sd->firebuf[(sd->fireh-1)*sd->firew + k] = (unsigned char)(sd->firepaln - 1);
		    sd->firelasttic = -1;
		}
	    }
	    if (sd->name[0]) numskydefs++;
	}
    }

    if (fmaps && fmaps->type == JSON_ARR && fmaps->n > 0)
    {
	flatmaps = malloc (fmaps->n * sizeof *flatmaps);
	for (i = 0; i < fmaps->n; i++)
	{
	    json_t*	f = JSON_Index (fmaps, i);
	    CopyName8 (flatmaps[numflatmaps].flat, JSON_Str (JSON_Get (f, "flat")));
	    CopyName8 (flatmaps[numflatmaps].sky,  JSON_Str (JSON_Get (f, "sky")));
	    if (flatmaps[numflatmaps].flat[0]) numflatmaps++;
	}
    }

    JSON_Free (root);
    printf ("SKYDEFS: %d sky(s), %d flat mapping(s)"
	    " (scrolling + scaley + fire applied; foreground/flatmapping deferred).\n",
	    numskydefs, numflatmaps);
}

skydef_t* R_SkyDefForTexNum (int texnum)
{
    int i;
    for (i = 0; i < numskydefs; i++)
	if (skydefs[i].name[0] && R_CheckTextureNumForName (skydefs[i].name) == texnum)
	    return &skydefs[i];
    return NULL;
}

// One Fabien-Sanglard doom-fire spread step over the whole buffer, throttled to
// the sky's updatetime.  Renderer-only (visual), so a plain LCG rng is fine.
static unsigned int fire_rng = 0x1234567u;
static int FireRand (void) { fire_rng = fire_rng*1103515245u + 12345u; return (fire_rng >> 16) & 0x7fff; }

void R_SkyFireUpdate (skydef_t* sd)
{
    extern int leveltime;
    int x, y, W, H;
    if (!sd || sd->type != 1 || !sd->firebuf) return;
    if (sd->firelasttic >= 0 && leveltime - sd->firelasttic < sd->fireupdatetics) return;
    sd->firelasttic = leveltime;

    W = sd->firew; H = sd->fireh;
    for (x = 0; x < W; x++)
	for (y = 1; y < H; y++)
	{
	    int src = y*W + x;
	    int v   = sd->firebuf[src];
	    if (v == 0)
		sd->firebuf[src - W] = 0;
	    else
	    {
		int r  = FireRand () & 3;
		int nx = (x - (r & 1) + 1) & (W - 1);		// horizontal wobble (wraps)
		sd->firebuf[(y-1)*W + nx] = (unsigned char)(v - (r & 1));
	    }
	}
}

// A game-palette column (fireh tall) for a sky column angle, sampled from the fire.
unsigned char* R_SkyFireColumn (skydef_t* sd, int col_angle)
{
    static unsigned char col[256];
    int y, c;
    if (!sd || !sd->firebuf) return NULL;
    c = col_angle & (sd->firew - 1);
    for (y = 0; y < sd->fireh && y < 256; y++)
	col[y] = sd->firepal[sd->firebuf[y*sd->firew + c]];
    return col;
}
