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
	    " (scrolling + scaley applied; fire/foreground/flatmapping deferred).\n",
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
