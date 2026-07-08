// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Light PNG support for UI elements.  Decodes a PNG lump (stb_image) and
//	converts it into a palette-quantised Doom `patch_t` so it can be drawn by
//	the ordinary V_DrawPatch path (HUD / menu graphics) with no renderer
//	changes.  Colours are nearest-matched into the IWAD PLAYPAL; pixels with
//	alpha < 128 become transparent (gaps in the column posts).
//
//	This is deliberately "light": UI-only, paletted output (so it banded like
//	any Doom graphic), small images (height <= 254, the 1-byte topdelta limit).
//	For true-colour in-world art see HD_TEXTURES.md.
//
//	Usage:  patch_t* p = V_CachePNG("RARRA0");  if (p) V_DrawPatch(x,y,0,p);
//	Results are cached by lump name; returns NULL if the lump is missing or
//	isn't a PNG.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "doomtype.h"
#include "z_zone.h"
#include "w_wad.h"
#include "r_defs.h"		// patch_t
#include "v_video.h"

// ---- palette (built once from PLAYPAL) -------------------------------------

static byte	vp_pal[256][3];
static boolean	vp_pal_ready;

static void VP_LoadPalette (void)
{
    const byte*	p = (const byte*) W_CacheLumpName ("PLAYPAL", PU_CACHE);
    int		i;
    for (i = 0; i < 256; i++)
	{ vp_pal[i][0] = p[i*3]; vp_pal[i][1] = p[i*3+1]; vp_pal[i][2] = p[i*3+2]; }
    vp_pal_ready = true;
}

// Nearest palette index for an RGB triple (squared-distance match).
static byte VP_Nearest (int r, int g, int b)
{
    int		i, best = 0;
    long	bestd = 0x7fffffff;
    for (i = 0; i < 256; i++)
    {
	int  dr = r - vp_pal[i][0], dg = g - vp_pal[i][1], db = b - vp_pal[i][2];
	long d  = (long)dr*dr + (long)dg*dg + (long)db*db;
	if (d < bestd) { bestd = d; best = i; if (!d) break; }
    }
    return (byte) best;
}

// ---- health-colour translation tables (green/yellow/red) -------------------
// A "translation" is a 256-entry palette remap applied per pixel by
// V_DrawPatchTranslated.  We build a luminance-preserving colourise for each hue,
// so any source colour (the gray HUD font OR the red status-bar numbers) becomes
// the target hue at the same brightness.

static byte	vp_xlat_grn[256], vp_xlat_yel[256], vp_xlat_red[256], vp_xlat_blu[256];
static boolean	vp_xlat_ready;

static void VP_BuildHealthXlats (void)
{
    int i;
    if (!vp_pal_ready) VP_LoadPalette ();
    for (i = 0; i < 256; i++)
    {
	int r = vp_pal[i][0], g = vp_pal[i][1], b = vp_pal[i][2];
	int L = (r*77 + g*150 + b*29) >> 8;		// luminance 0..255
	// A lighter green (mix some white in) instead of dark fully-saturated pure
	// green, so the healthy-HP readout (buddy + player) reads as a normal green.
	vp_xlat_grn[i] = VP_Nearest (L/2, L, L/2);
	vp_xlat_yel[i] = VP_Nearest (L, L*13/16, 0);	// gold (MBF cr_gold), not pure yellow
	vp_xlat_red[i] = VP_Nearest (L, 0, 0);
	vp_xlat_blu[i] = VP_Nearest (L/2, L/2, L);	// >max readout (mega health/armor)
    }
    vp_xlat_ready = true;
}

// MBF/Boom status-bar colour for a health/armor value: <25 red, <50 gold, <=100 green, else blue.
const byte* V_HealthTrans (int hp)
{
    if (!vp_xlat_ready) VP_BuildHealthXlats ();
    if (hp <  25) return vp_xlat_red;
    if (hp <  50) return vp_xlat_yel;
    if (hp <= 100) return vp_xlat_grn;
    return vp_xlat_blu;
}

// ---- RGBA -> patch_t --------------------------------------------------------

#define VP_ALPHA_CUT	128		// alpha below this -> transparent
#define VP_MAXH		254		// 1-byte topdelta cap (UI images are small)

// Build a column-format patch_t (Z_Malloc'd, PU_STATIC) from RGBA pixels.
static patch_t* VP_BuildPatch (const unsigned char* rgba, int w, int h)
{
    int		x, y, total, off;
    int*	colsize;
    byte*	base;
    patch_t*	patch;

    if (h > VP_MAXH) h = VP_MAXH;		// clamp (UI only)
    colsize = (int*) malloc (w * sizeof(int));
    if (!colsize) return NULL;

    // pass 1: size each column's post data (topdelta+len+pad + data + pad, then 0xff).
    total = 8 + w*4;				// header + columnofs[w]
    for (x = 0; x < w; x++)
    {
	int sz = 0;
	for (y = 0; y < h; )
	{
	    int top, run;
	    while (y < h && rgba[(y*w+x)*4+3] < VP_ALPHA_CUT) y++;	// skip clear
	    if (y >= h) break;
	    top = y; run = 0;
	    while (y < h && rgba[(y*w+x)*4+3] >= VP_ALPHA_CUT && (y-top) < 254) { run++; y++; }
	    sz += 4 + run;
	}
	sz += 1;				// end-of-column marker
	colsize[x] = sz;
	total += sz;
    }

    patch = (patch_t*) Z_Malloc (total, PU_STATIC, 0);
    base  = (byte*) patch;
    // header (host-endian == LE; the engine reads via SHORT/LONG which are no-ops on LE)
    patch->width = (short)w; patch->height = (short)h;
    patch->leftoffset = 0; patch->topoffset = 0;

    off = 8 + w*4;
    for (x = 0; x < w; x++)
    {
	byte* d = base + off;
	*(int*)(base + 8 + x*4) = off;		// columnofs[x]
	for (y = 0; y < h; )
	{
	    int top, run, k;
	    while (y < h && rgba[(y*w+x)*4+3] < VP_ALPHA_CUT) y++;
	    if (y >= h) break;
	    top = y; run = 0;
	    while (y < h && rgba[(y*w+x)*4+3] >= VP_ALPHA_CUT && (y-top) < 254) { run++; y++; }
	    *d++ = (byte)top;			// topdelta
	    *d++ = (byte)run;			// length
	    *d++ = 0;				// pad (V_DrawPatch skips it)
	    for (k = 0; k < run; k++)
	    {
		const unsigned char* px = &rgba[((top+k)*w+x)*4];
		*d++ = VP_Nearest (px[0], px[1], px[2]);
	    }
	    *d++ = 0;				// trailing pad
	}
	*d++ = 0xff;				// end of column
	off += colsize[x];
    }

    free (colsize);
    return patch;
}

// ---- public: cache a PNG lump as a patch -----------------------------------

#define VP_CACHE_MAX	64
static struct { char name[9]; patch_t* patch; } vp_cache[VP_CACHE_MAX];
static int vp_cache_n;

patch_t* V_CachePNG (const char* name)
{
    int			i, lump, len, w, h, comp;
    const byte*		raw;
    unsigned char*	rgba;
    patch_t*		patch;
    char		nm[9];

    if (!name) return NULL;
    strncpy (nm, name, 8); nm[8] = 0;

    for (i = 0; i < vp_cache_n; i++)
	if (!strncmp (vp_cache[i].name, nm, 8)) return vp_cache[i].patch;

    lump = W_CheckNumForName (nm);
    if (lump < 0) return NULL;
    len = W_LumpLength (lump);
    raw = (const byte*) W_CacheLumpNum (lump, PU_CACHE);
    if (len < 8 || raw[0] != 0x89 || raw[1] != 'P' || raw[2] != 'N' || raw[3] != 'G')
	return NULL;				// not a PNG -> caller handles as a normal lump

    rgba = stbi_load_from_memory (raw, len, &w, &h, &comp, 4);
    if (!rgba) return NULL;
    if (!vp_pal_ready) VP_LoadPalette ();
    patch = VP_BuildPatch (rgba, w, h);
    stbi_image_free (rgba);
    if (!patch) return NULL;

    if (vp_cache_n < VP_CACHE_MAX)
	{ strncpy (vp_cache[vp_cache_n].name, nm, 8); vp_cache[vp_cache_n].name[8] = 0;
	  vp_cache[vp_cache_n].patch = patch; vp_cache_n++; }
    return patch;
}
