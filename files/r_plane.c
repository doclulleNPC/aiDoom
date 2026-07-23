// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	Here is a core component: drawing the floors and ceilings,
//	 while maintaining a per column clipping list only.
//	Moreover, the sky areas have to be determined.
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: r_plane.c,v 1.4 1997/02/03 16:47:55 b1 Exp $";

#include <stdlib.h>

#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"

#include "doomdef.h"
#include "doomstat.h"
#include "r_skydefs.h"	// ID24 SKYDEFS: scrolling / scaled skies

#include "r_local.h"
#include "r_sky.h"



planefunction_t		floorfunc;
planefunction_t		ceilingfunc;

//
// opening
//

// Here comes the obnoxious "visplane".
//
// TRULY dynamic now (killough's hash, ported from MBF/Nugget): MAXVISPLANES is no
// longer a cap on the number of visplanes -- only the number of hash *buckets*.
// Each visplane_t is allocated individually (Z_Malloc, PU_STATIC) and reused via a
// free list, so the visplane_t* the BSP renderer holds across the frame stay put
// (no realloc dangling -- the reason the old code used a fixed 1024 array + I_Error).
// Overflowing is now impossible short of running out of RAM: complex Boom/limit-
// removing maps (or the hi-res renderer, which splits more spans) can't crash.
#define MAXVISPLANES	128		/* hash buckets -- must be a power of two */
static visplane_t*	visplanes[MAXVISPLANES];	// killough: hash table of chains
static visplane_t*	freetail;			// killough: head of the free list
static visplane_t**	freehead = &freetail;		// killough: tail insert pointer
visplane_t*		floorplane;
visplane_t*		ceilingplane;

// killough: hash a visplane's defining fields to a bucket index
#define visplane_hash(picnum,lightlevel,height) \
  (((unsigned)(picnum)*3 + (unsigned)(lightlevel) + (unsigned)(height)*7) & (MAXVISPLANES-1))

// ? (sized for the maximum internal resolution)
#define MAXOPENINGS	MAXWIDTH*64
int			openings[MAXOPENINGS];
int*			lastopening;


//
// Clip values are the solid pixel bounding the range.
//  floorclip starts out SCREENHEIGHT
//  ceilingclip starts out -1
//
int			floorclip[MAXWIDTH];
int			ceilingclip[MAXWIDTH];

//
// spanstart holds the start of a plane span
// initialized to 0 at start
//
int			spanstart[MAXHEIGHT];
int			spanstop[MAXHEIGHT];

//
// texture mapping
//
lighttable_t**		planezlight;
fixed_t			planeheight;
fixed_t			ds_planexoffs;	// Boom flat scroll: offset added in R_MapPlane
fixed_t			ds_planeyoffs;

fixed_t			yslope[MAXHEIGHT];
fixed_t			distscale[MAXWIDTH];
fixed_t			basexscale;
fixed_t			baseyscale;

fixed_t			cachedheight[MAXHEIGHT];
fixed_t			cacheddistance[MAXHEIGHT];
fixed_t			cachedxstep[MAXHEIGHT];
fixed_t			cachedystep[MAXHEIGHT];



//
// R_InitPlanes
// Only at game startup.
//
void R_InitPlanes (void)
{
  // Doh!
}


//
// R_MapPlane
//
// Uses global vars:
//  planeheight
//  ds_source
//  basexscale
//  baseyscale
//  viewx
//  viewy
//
// BASIC PRIMITIVE
//
extern fixed_t	viewsin, viewcos;	// set in R_SetupFrame
extern int	centerx;

void
R_MapPlane
( int		y,
  int		x1,
  int		x2 )
{
    fixed_t	distance;
    unsigned	index;
    int		dx, dy;

#ifdef RANGECHECK
    if (x2 < x1
	|| x1<0
	|| x2>=viewwidth
	|| (unsigned)y>viewheight)
    {
	I_Error ("R_MapPlane: %i, %i at %i",x1,x2,y);
    }
#endif

    // crispy/prboom+ linear flat mapping (world-locked even in Hor+ widescreen).  The vanilla
    // xtoviewangle/distscale form sheared floors/ceilings toward the wide screen edges (the flat
    // "wandered" with the view angle); this derives the world coord from the pixel offset off the
    // TRUE view centre (dx = x1 - centerx) instead, so it stays fixed to the world.
    if (centery == y)
	return;				// horizon row: infinite distance

    dy = (abs(centery - y) << FRACBITS) + (y < centery ? -FRACUNIT : FRACUNIT) / 2;

    if (planeheight != cachedheight[y])
    {
	cachedheight[y] = planeheight;
	distance = cacheddistance[y] = FixedMul (planeheight, yslope[y]);
	ds_xstep = cachedxstep[y] = FixedDiv (FixedMul (viewsin, planeheight), dy) << detailshift;
	ds_ystep = cachedystep[y] = FixedDiv (FixedMul (viewcos, planeheight), dy) << detailshift;
    }
    else
    {
	distance = cacheddistance[y];
	ds_xstep = cachedxstep[y];
	ds_ystep = cachedystep[y];
    }

    dx = x1 - centerx;
    // ds_planexoffs/yoffs carry the Boom flat-scroll offset of the current visplane (0 normally).
    ds_xfrac =  viewx + FixedMul (viewcos, distance) + dx * ds_xstep + ds_planexoffs;
    ds_yfrac = -viewy - FixedMul (viewsin, distance) + dx * ds_ystep + ds_planeyoffs;

    if (fixedcolormap)
	ds_colormap = fixedcolormap;
    else
    {
	index = distance >> LIGHTZSHIFT;
	
	if (index >= MAXLIGHTZ )
	    index = MAXLIGHTZ-1;

	if (r_dither_on) {
	    int fine = planezlight_fine[index], lvl = fine>>4;
	    ds_colormap  = colormaps + lvl*256;
	    ds_colormap2 = colormaps + (lvl < NUMCOLORMAPS-1 ? lvl+1 : lvl)*256;
	    ds_litfrac   = fine & 15;
	} else
	    ds_colormap = planezlight[index];
    }
	
    ds_y = y;
    ds_x1 = x1;
    ds_x2 = x2;

    // high or low detail
    if (r_dither_on && !fixedcolormap) R_DrawSpanDither(); else spanfunc ();	
}


//
// R_ClearPlanes
// At begining of frame.
//
void R_ClearPlanes (void)
{
    int		i;
    angle_t	angle;
    
    // opening / clipping determination
    for (i=0 ; i<viewwidth ; i++)
    {
	floorclip[i] = viewheight;
	ceilingclip[i] = -1;
    }

    // killough: move all visplanes from the hash chains onto the free list for reuse
    for (i=0 ; i<MAXVISPLANES ; i++)
	for (*freehead = visplanes[i], visplanes[i] = NULL ; *freehead ; )
	    freehead = &(*freehead)->next;

    lastopening = openings;

    // texture calculation
    memset (cachedheight, 0, sizeof(cachedheight));

    // left to right mapping
    angle = (viewangle-ANG90)>>ANGLETOFINESHIFT;

    // NOTE: basexscale/baseyscale are legacy -- the vanilla angle-table flat mapping they fed was
    // replaced by the crispy/prboom+ linear mapping in R_MapPlane (which sheared in widescreen).
    // Kept only so the globals stay defined; R_MapPlane no longer reads them.
    basexscale = FixedDiv (finecosine[angle],centerxfrac);
    baseyscale = -FixedDiv (finesine[angle],centerxfrac);
}




//
// new_visplane
// killough: pull a visplane_t off the free list (allocating one if the list is
// empty) and link it into hash bucket `hash`.  Individually allocated -> the
// pointer is stable for the life of the frame (never moved by a realloc).
//
static visplane_t* new_visplane (unsigned hash)
{
    visplane_t*	check = freetail;

    if (!check)
	check = Z_Malloc (sizeof(*check), PU_STATIC, 0);	// grow: no fixed cap
    else if (!(freetail = freetail->next))
	freehead = &freetail;

    check->next = visplanes[hash];
    visplanes[hash] = check;
    return check;
}


//
// R_FindPlane
//
visplane_t*
R_FindPlane
( fixed_t	height,
  int		picnum,
  int		lightlevel,
  fixed_t	xoffs,
  fixed_t	yoffs )
{
    visplane_t*	check;
    unsigned	hash;			// killough

    if (picnum == skyflatnum || (picnum & PL_SKYFLAT))
    {
	height = 0;			// all skys map together
	lightlevel = 0;
	xoffs = yoffs = 0;
    }

    // New visplane algorithm uses hash table -- killough
    hash = visplane_hash (picnum, lightlevel, height);

    for (check=visplanes[hash] ; check ; check=check->next)
	if (height == check->height
	    && picnum == check->picnum
	    && lightlevel == check->lightlevel
	    && xoffs == check->xoffs		// Boom: differently-scrolled flats
	    && yoffs == check->yoffs)		// must not merge into one visplane
	    return check;

    check = new_visplane (hash);

    check->height = height;
    check->picnum = picnum;
    check->lightlevel = lightlevel;
    check->xoffs = xoffs;
    check->yoffs = yoffs;
    check->minx = SCREENWIDTH;
    check->maxx = -1;

    memset (check->top,0xff,sizeof(check->top));

    return check;
}


//
// R_CheckPlane
//
visplane_t*
R_CheckPlane
( visplane_t*	pl,
  int		start,
  int		stop )
{
    int		intrl;
    int		intrh;
    int		unionl;
    int		unionh;
    int		x;
	
    if (start < pl->minx)
    {
	intrl = pl->minx;
	unionl = start;
    }
    else
    {
	unionl = pl->minx;
	intrl = start;
    }
	
    if (stop > pl->maxx)
    {
	intrh = pl->maxx;
	unionh = stop;
    }
    else
    {
	unionh = pl->maxx;
	intrh = stop;
    }

    for (x=intrl ; x<= intrh ; x++)
	if (pl->top[x] != 0xffff)
	    break;

    if (x > intrh)
    {
	pl->minx = unionl;
	pl->maxx = unionh;

	// use the same one
	return pl;
    }

    // make a new visplane (killough: dynamically allocated, hashed like the source)
    {
	unsigned	hash = visplane_hash (pl->picnum, pl->lightlevel, pl->height);
	visplane_t*	newpl = new_visplane (hash);

	newpl->height = pl->height;
	newpl->picnum = pl->picnum;
	newpl->lightlevel = pl->lightlevel;
	newpl->xoffs = pl->xoffs;
	newpl->yoffs = pl->yoffs;

	pl = newpl;
	pl->minx = start;
	pl->maxx = stop;

	memset (pl->top,0xff,sizeof(pl->top));
    }

    return pl;
}


//
// R_MakeSpans
//
void
R_MakeSpans
( int		x,
  int		t1,
  int		b1,
  int		t2,
  int		b2 )
{
    while (t1 < t2 && t1<=b1)
    {
	R_MapPlane (t1,spanstart[t1],x-1);
	t1++;
    }
    while (b1 > b2 && b1>=t1)
    {
	R_MapPlane (b1,spanstart[b1],x-1);
	b1--;
    }
	
    while (t2 < t1 && t2<=b2)
    {
	spanstart[t2] = x;
	t2++;
    }
    while (b2 > b1 && b2>=t2)
    {
	spanstart[b2] = x;
	b2--;
    }
}



//
// R_DrawPlanes
// At the end of each frame.
//
void R_DrawPlanes (void)
{
    visplane_t*		pl;
    int			light;
    int			x;
    int			stop;
    int			angle;
    int			i;

#ifdef RANGECHECK
    if ((unsigned)(ds_p - drawsegs) > maxdrawsegs)
	I_Error ("R_DrawPlanes: drawsegs overflow (%i)",
		 ds_p - drawsegs);

    if (lastopening - openings > MAXOPENINGS)
	I_Error ("R_DrawPlanes: opening overflow (%i)",
		 lastopening - openings);
#endif

    // killough: walk every hash bucket's chain of visplanes
    for (i=0 ; i<MAXVISPLANES ; i++)
    for (pl = visplanes[i] ; pl ; pl = pl->next)
    {
	if (pl->minx > pl->maxx)
	    continue;


	// sky flat -- plus ID24 SKYDEFS flatmapping (an arbitrary flat drawn as a sky)
	int mappedsky = R_SkyForFlat (pl->picnum);	// -1 unless flatmapped
	if (pl->picnum == skyflatnum || (pl->picnum & PL_SKYFLAT) || mappedsky >= 0)
	{
	    int texture;
	    angle_t an, flip;
	    skydef_t* sd = NULL;	// ID24 SKYDEFS entry for this sky, if any

	    if (pl->picnum & PL_SKYFLAT)
	    {
		// Sky Linedef
		const line_t *l = &lines[pl->picnum & ~PL_SKYFLAT];

		// Sky transferred from first sidedef
		const side_t *s = *l->sidenum + sides;

		// Texture comes from upper texture of reference sidedef
		texture = texturetranslation[s->toptexture];

		// Horizontal offset is turned into an angle offset,
		// to allow sky rotation as well as careful positioning.
		an = viewangle + s->textureoffset;

		// Vertical offset allows careful sky positioning.
		dc_texturemid = s->rowoffset - 28*FRACUNIT;

		// We sometimes flip the picture horizontally.
		flip = l->special == 272 ? 0u : ~0u;
	    }
	    else
	    {
		dc_texturemid = skytexturemid;
		texture = (mappedsky >= 0) ? mappedsky : skytexture;	// flatmapped sky
		an = viewangle;
		flip = 0;

		// ID24 SKYDEFS: if this sky texture has a definition, apply its
		// horizontal/vertical scroll (texels/sec) as a time-based offset.
		sd = R_SkyDefForTexNum (texture);
		if (sd)
		{
		    double t = (double) leveltime / 35.0;	// seconds since level start
		    if (sd->scrollx != 0)
			an += (angle_t)(sd->scrollx * t * (double)(1u << ANGLETOSKYSHIFT));
		    if (sd->scrolly != 0)
			dc_texturemid += (fixed_t)(sd->scrolly * t * (double)FRACUNIT);
		    if (sd->type == 1 && sd->firebuf) R_SkyFireUpdate (sd);	// ID24: advance fire sim
		}
	    }

	    // Map sky texture rows 0..100 over screen-top..horizon.  Use the UNPITCHED half-view
	    // height (viewheight/2), NOT the freelook-adjusted centery: the drawer already places the
	    // horizon at centery via (y - centery), so the scale must stay fixed -- keying it on the
	    // pitched centery stretched the sky when looking up/down (centery grows -> iscale shrinks).
	    { int basecy = viewheight/2;
	      dc_iscale = (basecy > 0 ? (100*FRACUNIT / basecy) : pspriteiscale) >> detailshift;
	      dc_iscale = (dc_iscale * 5) / 8; }

	    // SKYDEFS scaley: vertical scale of 100*(1/scaley) -> divide the inverse scale.
	    if (sd && sd->scaley != 1.0 && sd->scaley > 0)
		dc_iscale = (fixed_t)(dc_iscale / sd->scaley);

	    // Set the height of the current sky texture for column clamping.
	    { extern int dc_skyheight;
	      dc_skyheight = (sd && sd->type == 1 && sd->firebuf) ? sd->fireh
			   : (textureheight[texture] >> FRACBITS); }

	    // Sky is allways drawn full bright,
	    //  i.e. colormaps[0] is used.
	    // Because of this hack, sky is not affected
	    //  by INVUL inverse mapping.
	    dc_colormap = colormaps;
	    for (x=pl->minx ; x <= pl->maxx ; x++)
	    {
		dc_yl = pl->top[x];
		dc_yh = pl->bottom[x];

		if (dc_yl <= dc_yh)
		{
		    angle_t col_angle = ((an + xtoviewangle[x]) ^ flip) >> ANGLETOSKYSHIFT;
		    dc_x = x;
		    dc_source = (sd && sd->type == 1 && sd->firebuf)
			      ? R_SkyFireColumn (sd, col_angle >> 2)	// ID24 fire sky
			      : R_GetColumn (texture, col_angle);
		    R_DrawSkyColumn ();
		}
	    }

	    // ID24 SKYDEFS foreground (type 2): overlay a second texture with
	    // palette-0 transparency over the background sky just drawn.
	    if (sd && sd->type == 2 && sd->fgname[0])
	    {
		extern int R_CheckTextureNumForName (char*);
		extern void R_DrawSkyColumnMasked (void);
		extern int dc_skyheight;
		int fgtex = R_CheckTextureNumForName (sd->fgname);
		if (fgtex >= 0)
		{
		    double  t   = (double) leveltime / 35.0;
		    angle_t fan = viewangle + (angle_t)(sd->fgscrollx * t * (double)(1u << ANGLETOSKYSHIFT));
		    dc_texturemid = skytexturemid + (fixed_t)(sd->fgmid * FRACUNIT)
				  + (fixed_t)(sd->fgscrolly * t * (double)FRACUNIT);
		    dc_skyheight  = textureheight[fgtex] >> FRACBITS;
		    dc_colormap   = colormaps;
		    for (x = pl->minx; x <= pl->maxx; x++)
		    {
			dc_yl = pl->top[x]; dc_yh = pl->bottom[x];
			if (dc_yl <= dc_yh)
			{
			    angle_t ca = ((fan + xtoviewangle[x]) ^ flip) >> ANGLETOSKYSHIFT;
			    dc_x = x; dc_source = R_GetColumn (fgtex, ca);
			    R_DrawSkyColumnMasked ();
			}
		    }
		}
	    }
	    continue;
	}
	
	// (mod) Guard a malformed/unsupported picnum: a PWAD using sky transfers (SIGIL II) or
	// other Boom flat features can leave pl->picnum outside the flat range -> the
	// flattranslation[pl->picnum] read goes OOB and feeds a garbage lump to W_CacheLumpNum.
	{ extern int numflats;
	  if ((pl->picnum < 0 || pl->picnum >= numflats) && !(pl->picnum & PL_SKYFLAT))
	  {
	      static int warned; if (!warned) { warned=1; fprintf(stderr,"R_DrawPlanes: skipping visplane with out-of-range flat %d (numflats=%d) -- malformed PWAD?\n", pl->picnum, numflats); }
	      continue;	// skip rather than feed a garbage lump to W_CacheLumpNum
	  }
	}
	// regular flat (flatlumps[] maps the dense flat index -> lump number)
	ds_source = W_CacheLumpNum(flatlumps[flattranslation[pl->picnum]],
				   PU_STATIC);
	
	planeheight = abs(pl->height-viewz);
	ds_planexoffs = pl->xoffs;	// Boom flat scroll for this visplane
	ds_planeyoffs = pl->yoffs;
	light = (pl->lightlevel >> LIGHTSEGSHIFT)+extralight;

	if (light >= LIGHTLEVELS)
	    light = LIGHTLEVELS-1;

	if (light < 0)
	    light = 0;

	planezlight = zlight[light]; planezlight_fine = zlight_fine[light];

	pl->top[pl->maxx+1] = 0xffff;
	pl->top[pl->minx-1] = 0xffff;
		
	stop = pl->maxx + 1;

	for (x=pl->minx ; x<= stop ; x++)
	{
	    R_MakeSpans(x,pl->top[x-1],
			pl->bottom[x-1],
			pl->top[x],
			pl->bottom[x]);
	}
	
	Z_ChangeTag (ds_source, PU_CACHE);
    }
}
