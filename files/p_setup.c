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
//	Do all the WAD I/O, get map description,
//	set up initial state and misc. LUTs.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: p_setup.c,v 1.5 1997/02/03 22:45:12 b1 Exp $";


#include <math.h>
#include <limits.h>

#include "z_zone.h"

#include "m_swap.h"
#include "m_bbox.h"

#include "g_game.h"

#include "i_system.h"
#include "w_wad.h"

#include "doomdef.h"
#include "p_local.h"
#include "p_ai_director.h"
#include "p_ai_llm.h"
#include "p_morph.h"		// (M) P_MorphReset -- clear morphs on level load

#include "s_sound.h"

#include "doomstat.h"


void	P_SpawnMapThing (mapthing_t*	mthing);


//
// MAP related Lookup tables.
// Store VERTEXES, LINEDEFS, SIDEDEFS, etc.
//
int		numvertexes;
vertex_t*	vertexes;

int		numsegs;
seg_t*		segs;

int		numsectors;
sector_t*	sectors;

int		numsubsectors;
subsector_t*	subsectors;

int		numnodes;
node_t*		nodes;

int		numlines;
line_t*		lines;

int		numsides;
side_t*		sides;


// BLOCKMAP
// Created from axis aligned bounding box
// of the map, a rectangular array of
// blocks of size ...
// Used to speed up collision detection
// by spatial subdivision in 2D.
//
// Blockmap size.
int		bmapwidth;
int		bmapheight;	// size in mapblocks
int*		blockmap;	// int: limit-removing blockmap (big maps + >32767 line indices)
// offsets in blockmap are from here
int*		blockmaplump;
// origin of block map
fixed_t		bmaporgx;
fixed_t		bmaporgy;
// for thing chains
mobj_t**	blocklinks;		


// REJECT
// For fast sight rejection.
// Speeds up enemy AI by skipping detailed
//  LineOf Sight calculation.
// Without special effect, this could be
//  used as a PVS lookup as well.
//
byte*		rejectmatrix;


// Maintain single and multi player starting spots.
#define MAX_DEATHMATCH_STARTS	10

mapthing_t	deathmatchstarts[MAX_DEATHMATCH_STARTS];
mapthing_t*	deathmatch_p;
mapthing_t	playerstarts[MAXPLAYERS];





//
// P_LoadVertexes
//
void P_LoadVertexes (int lump)
{
    byte*		data;
    int			i;
    mapvertex_t*	ml;
    vertex_t*		li;

    // Determine number of lumps:
    //  total lump length / vertex record length.
    numvertexes = W_LumpLength (lump) / sizeof(mapvertex_t);

    // Allocate zone memory for buffer.
    vertexes = Z_Malloc (numvertexes*sizeof(vertex_t),PU_LEVEL,0);	

    // Load data into cache.
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    ml = (mapvertex_t *)data;
    li = vertexes;

    // Copy and convert vertex coordinates,
    // internal representation as fixed.
    for (i=0 ; i<numvertexes ; i++, li++, ml++)
    {
	li->x = SHORT(ml->x)<<FRACBITS;
	li->y = SHORT(ml->y)<<FRACBITS;
    }

    // Free buffer memory.
    Z_Free (data);
}



//
// P_LoadSegs
//
void P_LoadSegs (int lump)
{
    byte*		data;
    int			i;
    mapseg_t*		ml;
    seg_t*		li;
    line_t*		ldef;
    int			linedef;
    int			side;
	
    numsegs = W_LumpLength (lump) / sizeof(mapseg_t);
    segs = Z_Malloc (numsegs*sizeof(seg_t),PU_LEVEL,0);	
    memset (segs, 0, numsegs*sizeof(seg_t));
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    ml = (mapseg_t *)data;
    li = segs;
    for (i=0 ; i<numsegs ; i++, li++, ml++)
    {
	li->v1 = &vertexes[SHORT(ml->v1)];
	li->v2 = &vertexes[SHORT(ml->v2)];
					
	li->angle = (SHORT(ml->angle))<<16;
	li->offset = (SHORT(ml->offset))<<16;
	linedef = SHORT(ml->linedef);
	ldef = &lines[linedef];
	li->linedef = ldef;
	side = SHORT(ml->side);
	li->sidedef = &sides[ldef->sidenum[side]];
	li->frontsector = sides[ldef->sidenum[side]].sector;
	if (ldef-> flags & ML_TWOSIDED)
	    li->backsector = sides[ldef->sidenum[side^1]].sector;
	else
	    li->backsector = 0;
    }
	
    Z_Free (data);
}


//
// P_LoadSubsectors
//
void P_LoadSubsectors (int lump)
{
    byte*		data;
    int			i;
    mapsubsector_t*	ms;
    subsector_t*	ss;
	
    numsubsectors = W_LumpLength (lump) / sizeof(mapsubsector_t);
    subsectors = Z_Malloc (numsubsectors*sizeof(subsector_t),PU_LEVEL,0);	
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    ms = (mapsubsector_t *)data;
    memset (subsectors,0, numsubsectors*sizeof(subsector_t));
    ss = subsectors;
    
    for (i=0 ; i<numsubsectors ; i++, ss++, ms++)
    {
	ss->numlines = SHORT(ms->numsegs);
	ss->firstline = SHORT(ms->firstseg);
    }
	
    Z_Free (data);
}



//
// P_LoadSectors
//
void P_LoadSectors (int lump)
{
    byte*		data;
    int			i;
    mapsector_t*	ms;
    sector_t*		ss;
	
    numsectors = W_LumpLength (lump) / sizeof(mapsector_t);
    sectors = Z_Malloc (numsectors*sizeof(sector_t),PU_LEVEL,0);	
    memset (sectors, 0, numsectors*sizeof(sector_t));
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    ms = (mapsector_t *)data;
    ss = sectors;
    for (i=0 ; i<numsectors ; i++, ss++, ms++)
    {
	ss->floorheight = SHORT(ms->floorheight)<<FRACBITS;
	ss->ceilingheight = SHORT(ms->ceilingheight)<<FRACBITS;
	ss->floorpic = R_FlatNumForName(ms->floorpic);
	ss->ceilingpic = R_FlatNumForName(ms->ceilingpic);
	ss->lightlevel = SHORT(ms->lightlevel);
	ss->special = SHORT(ms->special);
	ss->tag = SHORT(ms->tag);
	ss->thinglist = NULL;
	ss->heightsec = -1;	// Boom 242: no transfer-height control sector unless set below
	ss->floorlightsec = ss->ceilinglightsec = -1;	// Boom 213/261 light transfer: none
	ss->sky = 0;		// Boom 271/272 sky transfer: none
    }
	
    Z_Free (data);
}


// ZDBSP / Boom "extended nodes": when a map is built with extended nodes the NODES lump
// starts with a 4-byte magic (SSECTORS + SEGS lumps are empty) and carries new vertices,
// subsectors, segs and 32-bit nodes itself.  We handle the plain "XNOD" format and the
// zlib-compressed "ZNOD" variant (ZDBSP `-z`, common in modern WADs) -- ZNOD is just XNOD
// deflated, so we inflate then run the same parser.  (GL-node variants XGL*/ZGL* have a
// different seg layout and are still not handled.)  Returns true if it consumed the lump.
extern angle_t R_PointToAngle2 (fixed_t, fixed_t, fixed_t, fixed_t);

static fixed_t P_SegOffset (fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
    double dx = (double)(x1 - x2) / FRACUNIT;
    double dy = (double)(y1 - y2) / FRACUNIT;
    return (fixed_t)(sqrt (dx*dx + dy*dy) * FRACUNIT);
}

// zlib inflate lives in w_inflate.c -- <zlib.h> can't be included here (it pulls in a
// POSIX close() that clashes with p_spec.h's `close` enum).  Returns a malloc'd buffer.
extern byte* W_InflateZlib (byte* src, unsigned srclen, unsigned* outlen);

boolean P_LoadNodes_Extended (int lump)
{
    byte*   data = W_CacheLumpNum (lump, PU_STATIC);
    byte*   ibuf = NULL;		// inflated body for the compressed (ZNOD) variant
    byte*   p;
    unsigned origv, newv, i;
    unsigned nsub, nseg, nnod;

    if (W_LumpLength (lump) < 8)
    { Z_Free (data); return false; }

    if (memcmp (data, "XNOD", 4) == 0)
    {
	p = data + 4;				// uncompressed: parse in place
    }
    else if (memcmp (data, "ZNOD", 4) == 0)
    {
	unsigned ilen = 0;			// zlib-compressed: inflate then parse
	ibuf = W_InflateZlib (data + 4, W_LumpLength (lump) - 4, &ilen);
	if (!ibuf || ilen < 8) { free (ibuf); Z_Free (data); return false; }
	p = ibuf;
    }
    else
    { Z_Free (data); return false; }		// XNOD/ZNOD only (GL variants unhandled)
    #define RD32() ( p += 4, (unsigned)(p[-4] | (p[-3]<<8) | (p[-2]<<16) | ((unsigned)p[-1]<<24)) )
    #define RD16() ( p += 2, (unsigned short)(p[-2] | (p[-1]<<8)) )

    origv = RD32();  newv = RD32();

    // --- vertices: keep the originals (already loaded), append the new ones ---
    {
        vertex_t* nv = Z_Malloc ((origv + newv) * sizeof(vertex_t), PU_LEVEL, 0);
        memcpy (nv, vertexes, origv * sizeof(vertex_t));
        for (i = 0; i < newv; i++) { nv[origv+i].x = (fixed_t) RD32(); nv[origv+i].y = (fixed_t) RD32(); }
        vertexes = nv; numvertexes = origv + newv;
    }

    // --- subsectors: each stores only a seg count; firstline is the running total ---
    nsub = RD32();
    numsubsectors = nsub;
    subsectors = Z_Malloc (nsub * sizeof(subsector_t), PU_LEVEL, 0);
    memset (subsectors, 0, nsub * sizeof(subsector_t));
    { int first = 0;
      for (i = 0; i < nsub; i++)
      { int cnt = (int) RD32(); subsectors[i].firstline = first; subsectors[i].numlines = cnt; first += cnt; } }

    // --- segs: v1,v2 (32-bit), linedef (16), side (8); angle+offset are computed ---
    nseg = RD32();
    numsegs = nseg;
    segs = Z_Malloc (nseg * sizeof(seg_t), PU_LEVEL, 0);
    memset (segs, 0, nseg * sizeof(seg_t));
    for (i = 0; i < nseg; i++)
    {
        seg_t* li = &segs[i];
        unsigned v1 = RD32(), v2 = RD32();
        int ld = RD16();
        int side = *p++;
        line_t* ldef = &lines[ld];
        li->v1 = &vertexes[v1];
        li->v2 = &vertexes[v2];
        li->linedef = ldef;
        li->sidedef = &sides[ldef->sidenum[side]];
        li->frontsector = sides[ldef->sidenum[side]].sector;
        li->backsector = (ldef->flags & ML_TWOSIDED) ? sides[ldef->sidenum[side^1]].sector : 0;
        li->angle = R_PointToAngle2 (li->v1->x, li->v1->y, li->v2->x, li->v2->y);
        { vertex_t* vv = side ? ldef->v2 : ldef->v1;
          li->offset = P_SegOffset (li->v1->x, li->v1->y, vv->x, vv->y); }
    }

    // --- nodes: 16-bit geometry, 32-bit children (bit 31 = subsector) ---
    nnod = RD32();
    numnodes = nnod;
    nodes = Z_Malloc (nnod * sizeof(node_t), PU_LEVEL, 0);
    for (i = 0; i < nnod; i++)
    {
        node_t* no = &nodes[i]; int j, k;
        no->x = ((short) RD16()) << FRACBITS; no->y = ((short) RD16()) << FRACBITS;
        no->dx = ((short) RD16()) << FRACBITS; no->dy = ((short) RD16()) << FRACBITS;
        for (j = 0; j < 2; j++) for (k = 0; k < 4; k++) no->bbox[j][k] = ((short) RD16()) << FRACBITS;
        for (j = 0; j < 2; j++) no->children[j] = (int) RD32();   // already bit-31 = subsector
    }
    #undef RD32
    #undef RD16
    free (ibuf);			// NULL for the uncompressed XNOD path -- free(NULL) is fine
    Z_Free (data);
    printf ("P_LoadNodes: extended (%s) nodes -- %u verts, %u ssectors, %u segs, %u nodes\n",
            ibuf ? "ZNOD" : "XNOD", origv + newv, nsub, nseg, nnod);
    return true;
}

//
// P_LoadNodes
//
void P_LoadNodes (int lump)
{
    byte*	data;
    int		i;
    int		j;
    int		k;
    mapnode_t*	mn;
    node_t*	no;
	
    numnodes = W_LumpLength (lump) / sizeof(mapnode_t);
    nodes = Z_Malloc (numnodes*sizeof(node_t),PU_LEVEL,0);	
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    mn = (mapnode_t *)data;
    no = nodes;
    
    for (i=0 ; i<numnodes ; i++, no++, mn++)
    {
	no->x = SHORT(mn->x)<<FRACBITS;
	no->y = SHORT(mn->y)<<FRACBITS;
	no->dx = SHORT(mn->dx)<<FRACBITS;
	no->dy = SHORT(mn->dy)<<FRACBITS;
	for (j=0 ; j<2 ; j++)
	{
	    unsigned short raw = (unsigned short) SHORT(mn->children[j]);
	    no->children[j] = (raw & NF_SUBSECTOR_16)
			     ? (NF_SUBSECTOR | (raw & ~NF_SUBSECTOR_16)) : raw;
	    for (k=0 ; k<4 ; k++)
		no->bbox[j][k] = SHORT(mn->bbox[j][k])<<FRACBITS;
	}
    }
	
    Z_Free (data);
}


//
// P_LoadThings
//
void P_LoadThings (int lump)
{
    byte*		data;
    int			i;
    mapthing_t*		mt;
    int			numthings;
    boolean		spawn;
	
    data = W_CacheLumpNum (lump,PU_STATIC);
    numthings = W_LumpLength (lump) / sizeof(mapthing_t);
	
    mt = (mapthing_t *)data;
    for (i=0 ; i<numthings ; i++, mt++)
    {
	spawn = true;

	// Do not spawn cool, new monsters if !commercial.  (Skipped in heretic_mode:
	// Heretic doomednums 64/66/68 are the Knight/Gargoyle/Mummy, NOT DOOM2 monsters,
	// and this gate's `break` would otherwise abort the whole THINGS loop.)
	if ( gamemode != commercial && !heretic_mode)
	{
	    switch(mt->type)
	    {
	      case 68:	// Arachnotron
	      case 64:	// Archvile
	      case 88:	// Boss Brain
	      case 89:	// Boss Shooter
	      case 69:	// Hell Knight
	      case 67:	// Mancubus
	      case 71:	// Pain Elemental
	      case 65:	// Former Human Commando
	      case 66:	// Revenant
	      case 84:	// Wolf SS
		spawn = false;
		break;
	    }
	}
	if (spawn == false)
	    break;

	// Do spawn all other stuff. 
	mt->x = SHORT(mt->x);
	mt->y = SHORT(mt->y);
	mt->angle = SHORT(mt->angle);
	mt->type = SHORT(mt->type);
	mt->options = SHORT(mt->options);
	
	P_SpawnMapThing (mt);
    }
	
    Z_Free (data);
}


//
// Raw sidedef top/bottom texture names, kept from P_LoadSideDefs so P_LoadLineDefs
// can resolve ID24 music-change lines (whose texture name is a music lump, not a
// real texture).  Allocated PU_LEVEL; consumed at the end of P_LoadLineDefs.
// (P_LoadSideDefs runs before P_LoadLineDefs in P_SetupLevel.)
static char (*sd_raw_top)[9];
static char (*sd_raw_bot)[9];

// P_LoadLineDefs
// Also counts secret lines for intermissions.
//
void P_LoadLineDefs (int lump)
{
    byte*		data;
    int			i;
    maplinedef_t*	mld;
    line_t*		ld;
    vertex_t*		v1;
    vertex_t*		v2;
	
    numlines = W_LumpLength (lump) / sizeof(maplinedef_t);
    lines = Z_Malloc (numlines*sizeof(line_t),PU_LEVEL,0);	
    memset (lines, 0, numlines*sizeof(line_t));
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    mld = (maplinedef_t *)data;
    ld = lines;
    for (i=0 ; i<numlines ; i++, mld++, ld++)
    {
	ld->flags = SHORT(mld->flags);
	ld->special = SHORT(mld->special);
	ld->tag = SHORT(mld->tag);
	v1 = ld->v1 = &vertexes[SHORT(mld->v1)];
	v2 = ld->v2 = &vertexes[SHORT(mld->v2)];
	ld->dx = v2->x - v1->x;
	ld->dy = v2->y - v1->y;
	
	if (!ld->dx)
	    ld->slopetype = ST_VERTICAL;
	else if (!ld->dy)
	    ld->slopetype = ST_HORIZONTAL;
	else
	{
	    if (FixedDiv (ld->dy , ld->dx) > 0)
		ld->slopetype = ST_POSITIVE;
	    else
		ld->slopetype = ST_NEGATIVE;
	}
		
	if (v1->x < v2->x)
	{
	    ld->bbox[BOXLEFT] = v1->x;
	    ld->bbox[BOXRIGHT] = v2->x;
	}
	else
	{
	    ld->bbox[BOXLEFT] = v2->x;
	    ld->bbox[BOXRIGHT] = v1->x;
	}

	if (v1->y < v2->y)
	{
	    ld->bbox[BOXBOTTOM] = v1->y;
	    ld->bbox[BOXTOP] = v2->y;
	}
	else
	{
	    ld->bbox[BOXBOTTOM] = v2->y;
	    ld->bbox[BOXTOP] = v1->y;
	}

	// Read sidenum UNSIGNED: 0xFFFF is the "no sidedef" sentinel (== -1); every other value
	// is a valid index.  Reading it as a signed short (the old SHORT()) broke maps with more
	// than 32767 sidedefs -- a high index came out NEGATIVE, slipped past the "== -1" test,
	// and indexed sides[] out of bounds -> a NULL/garbage frontsector and a crash in
	// P_GroupLines.  Common in ZDBSP/extended-node maps (e.g. Legacy of Rust MAP13).
	{ unsigned short s0 = (unsigned short) SHORT(mld->sidenum[0]);
	  unsigned short s1 = (unsigned short) SHORT(mld->sidenum[1]);
	  ld->sidenum[0] = (s0 == 0xFFFF || s0 >= numsides) ? -1 : (int) s0;
	  ld->sidenum[1] = (s1 == 0xFFFF || s1 >= numsides) ? -1 : (int) s1; }

	if (ld->sidenum[0] == -1)
	    ld->sidenum[0] = 0;  // Substitute dummy sidedef for missing right side

	if (ld->sidenum[0] != -1)
	    ld->frontsector = sides[ld->sidenum[0]].sector;
	else
	    ld->frontsector = 0;

	if (ld->sidenum[1] != -1)
	    ld->backsector = sides[ld->sidenum[1]].sector;
	else
	    ld->backsector = 0;

	ld->tranlump = -1;	// Boom 260: opaque unless a 260 special says otherwise (below)
	ld->frontmusic = ld->backmusic = -1;	// ID24 music-change: resolved below
    }

    // ID24 music-change lines (2057-2068, 2087-2098): the sidedef's texture NAME is a
    // music lump (front = top of front side, back = bottom of back side).  Resolve it
    // to a lump number now and clear the fake top texture so it doesn't render.
    if (sd_raw_top)
    {
	line_t* ld = lines;
	int i;
	for (i = 0; i < numlines; i++, ld++)
	{
	    int sp = ld->special;
	    if (!((sp >= 2057 && sp <= 2068) || (sp >= 2087 && sp <= 2098)))
		continue;
	    if (ld->sidenum[0] >= 0 && ld->sidenum[0] < numsides)
	    {
		ld->frontmusic = W_CheckNumForName (sd_raw_top[ld->sidenum[0]]);
		sides[ld->sidenum[0]].toptexture = 0;
	    }
	    if (ld->sidenum[1] >= 0 && ld->sidenum[1] < numsides)
		ld->backmusic = W_CheckNumForName (sd_raw_bot[ld->sidenum[1]]);
	}
    }
    sd_raw_top = sd_raw_bot = NULL;	// PU_LEVEL memory is reclaimed at the next level load

    // Boom 260 (killough 4/11/98): translucent 2S middle textures.  tag 0 -> just this linedef;
    // tag N -> every linedef with that tag.  aiDoom uses the generated main_tranmap (tranlump 0).
    {
	line_t* ld = lines;
	int i, j;
	for (i = 0; i < numlines; i++, ld++)
	    if (ld->special == 260)
	    {
		if (!ld->tag)
		    ld->tranlump = 0;
		else
		    for (j = 0; j < numlines; j++)
			if (lines[j].tag == ld->tag)
			    lines[j].tranlump = 0;
	    }
    }

    Z_Free (data);
}


//
// P_LoadSideDefs
//
void P_LoadSideDefs (int lump)
{
    byte*		data;
    int			i;
    mapsidedef_t*	msd;
    side_t*		sd;

    numsides = W_LumpLength (lump) / sizeof(mapsidedef_t);
    sides = Z_Malloc (numsides*sizeof(side_t),PU_LEVEL,0);
    memset (sides, 0, numsides*sizeof(side_t));
    data = W_CacheLumpNum (lump,PU_STATIC);

    sd_raw_top = Z_Malloc (numsides * 9, PU_LEVEL, 0);
    sd_raw_bot = Z_Malloc (numsides * 9, PU_LEVEL, 0);

    msd = (mapsidedef_t *)data;
    sd = sides;
    for (i=0 ; i<numsides ; i++, msd++, sd++)
    {
	sd->textureoffset = SHORT(msd->textureoffset)<<FRACBITS;
	sd->rowoffset = SHORT(msd->rowoffset)<<FRACBITS;
	sd->toptexture = R_TextureNumForName(msd->toptexture);
	sd->bottomtexture = R_TextureNumForName(msd->bottomtexture);
	sd->midtexture = R_TextureNumForName(msd->midtexture);
	sd->sector = &sectors[SHORT(msd->sector)];
	// keep the raw names for the ID24 music-line resolution pass
	memcpy (sd_raw_top[i], msd->toptexture, 8);    sd_raw_top[i][8] = 0;
	memcpy (sd_raw_bot[i], msd->bottomtexture, 8); sd_raw_bot[i][8] = 0;
    }

    Z_Free (data);
}


// P_CreateBlockMap (jff/killough, via MBF/PrBoom/Woof): build a fresh blockmap from the
// linedefs.  Needed when the WAD blockmap is absent or too big for the 16-bit on-disk
// offsets (a blockmap over 65535 shorts, e.g. Legacy of Rust MAP13's 76134, can't be
// addressed, which crashed collision).  Produces an int blockmaplump (no size limit).
#define blkshift  7			// cell size = 128 map units
#define blkmask   ((1<<blkshift)-1)
#define blkmargin 0

typedef struct linelist_s { long num; struct linelist_s* next; } linelist_t;

static void AddBlockLine (linelist_t** lists, int* count, int* done, int blockno, long lineno)
{
    linelist_t* l;
    if (done[blockno]) return;
    l = Z_Malloc (sizeof(linelist_t), PU_STATIC, 0);
    l->num = lineno;
    l->next = lists[blockno];
    lists[blockno] = l;
    count[blockno]++;
    done[blockno] = 1;
}

static void P_CreateBlockMap (void)
{
    int		xorg, yorg, nrows, ncols, NBlocks, i, j;
    long	linetotal = 0;
    linelist_t** blocklists;
    int*	blockcount;
    int*	blockdone;
    int		map_minx = INT_MAX, map_miny = INT_MAX, map_maxx = INT_MIN, map_maxy = INT_MIN;

    if (numvertexes)
    { map_minx = map_maxx = vertexes[0].x; map_miny = map_maxy = vertexes[0].y; }
    for (i = 0; i < numvertexes; i++)
    {
	fixed_t t;
	if ((t = vertexes[i].x) < map_minx) map_minx = t; else if (t > map_maxx) map_maxx = t;
	if ((t = vertexes[i].y) < map_miny) map_miny = t; else if (t > map_maxy) map_maxy = t;
    }
    map_minx >>= FRACBITS; map_maxx >>= FRACBITS; map_miny >>= FRACBITS; map_maxy >>= FRACBITS;

    xorg = map_minx - blkmargin;
    yorg = map_miny - blkmargin;
    ncols = (map_maxx + blkmargin - xorg + 1 + blkmask) >> blkshift;
    nrows = (map_maxy + blkmargin - yorg + 1 + blkmask) >> blkshift;
    NBlocks = ncols * nrows;

    blocklists = Z_Malloc (NBlocks * sizeof(*blocklists), PU_STATIC, 0);
    blockcount = Z_Malloc (NBlocks * sizeof(*blockcount), PU_STATIC, 0);
    blockdone  = Z_Malloc (NBlocks * sizeof(*blockdone),  PU_STATIC, 0);
    memset (blocklists, 0, NBlocks * sizeof(*blocklists));
    memset (blockcount, 0, NBlocks * sizeof(*blockcount));

    for (i = 0; i < NBlocks; i++)
    { blocklists[i] = Z_Malloc (sizeof(linelist_t), PU_STATIC, 0);
      blocklists[i]->num = -1; blocklists[i]->next = NULL; blockcount[i]++; }

    for (i = 0; i < numlines; i++)
    {
	int x1 = lines[i].v1->x >> FRACBITS, y1 = lines[i].v1->y >> FRACBITS;
	int x2 = lines[i].v2->x >> FRACBITS, y2 = lines[i].v2->y >> FRACBITS;
	int dx = x2 - x1, dy = y2 - y1;
	int vert = !dx, horiz = !dy;
	int spos = (dx ^ dy) > 0, sneg = (dx ^ dy) < 0;
	int bx, by;
	int minx = x1 > x2 ? x2 : x1, maxx = x1 > x2 ? x1 : x2;
	int miny = y1 > y2 ? y2 : y1, maxy = y1 > y2 ? y1 : y2;

	memset (blockdone, 0, NBlocks * sizeof(int));

	bx = (x1 - xorg) >> blkshift; by = (y1 - yorg) >> blkshift;
	AddBlockLine (blocklists, blockcount, blockdone, by * ncols + bx, i);
	bx = (x2 - xorg) >> blkshift; by = (y2 - yorg) >> blkshift;
	AddBlockLine (blocklists, blockcount, blockdone, by * ncols + bx, i);

	if (!vert)		// intersect with each column's left edge
	  for (j = 0; j < ncols; j++)
	  {
	    int x = xorg + (j << blkshift);
	    int y = (dy * (x - x1)) / dx + y1;
	    int yb = (y - yorg) >> blkshift, yp = (y - yorg) & blkmask;
	    if (yb < 0 || yb > nrows - 1 || x < minx || x > maxx) continue;
	    AddBlockLine (blocklists, blockcount, blockdone, ncols * yb + j, i);
	    if (yp == 0)
	    {
	      if (sneg)      { if (yb>0 && miny<y) AddBlockLine(blocklists,blockcount,blockdone,ncols*(yb-1)+j,i);
			       if (j>0 && minx<x)  AddBlockLine(blocklists,blockcount,blockdone,ncols*yb+j-1,i); }
	      else if (horiz){ if (yb>0 && miny<y) AddBlockLine(blocklists,blockcount,blockdone,ncols*(yb-1)+j,i); }
	      else if (spos) { if (yb>0 && j>0 && miny<y) AddBlockLine(blocklists,blockcount,blockdone,ncols*(yb-1)+j-1,i); }
	    }
	    else if (j>0 && minx<x) AddBlockLine (blocklists, blockcount, blockdone, ncols*yb+j-1, i);
	  }

	if (!horiz)		// intersect with each row's bottom edge
	  for (j = 0; j < nrows; j++)
	  {
	    int y = yorg + (j << blkshift);
	    int x = (dx * (y - y1)) / dy + x1;
	    int xb = (x - xorg) >> blkshift, xp = (x - xorg) & blkmask;
	    if (xb < 0 || xb > ncols - 1 || y < miny || y > maxy) continue;
	    AddBlockLine (blocklists, blockcount, blockdone, ncols * j + xb, i);
	    if (xp == 0)
	    {
	      if (sneg)      { if (j>0 && miny<y)  AddBlockLine(blocklists,blockcount,blockdone,ncols*(j-1)+xb,i);
			       if (xb>0 && minx<x) AddBlockLine(blocklists,blockcount,blockdone,ncols*j+xb-1,i); }
	      else if (vert) { if (j>0 && miny<y)  AddBlockLine(blocklists,blockcount,blockdone,ncols*(j-1)+xb,i); }
	      else if (spos) { if (xb>0 && j>0 && miny<y) AddBlockLine(blocklists,blockcount,blockdone,ncols*(j-1)+xb-1,i); }
	    }
	    else if (j>0 && miny<y) AddBlockLine (blocklists, blockcount, blockdone, ncols*(j-1)+xb, i);
	  }
    }

    memset (blockdone, 0, NBlocks * sizeof(int));
    for (i = 0, linetotal = 0; i < NBlocks; i++)
    { AddBlockLine (blocklists, blockcount, blockdone, i, 0); linetotal += blockcount[i]; }

    blockmaplump = Z_Malloc (sizeof(*blockmaplump) * (4 + NBlocks + linetotal), PU_LEVEL, 0);
    blockmaplump[0] = bmaporgx = xorg << FRACBITS;
    blockmaplump[1] = bmaporgy = yorg << FRACBITS;
    blockmaplump[2] = bmapwidth  = ncols;
    blockmaplump[3] = bmapheight = nrows;

    for (i = 0; i < NBlocks; i++)
    {
	linelist_t* bl = blocklists[i];
	long offs = blockmaplump[4+i] = (i ? blockmaplump[4+i-1] : 4 + NBlocks) + (i ? blockcount[i-1] : 0);
	while (bl) { linelist_t* tmp = bl->next; blockmaplump[offs++] = bl->num; Z_Free (bl); bl = tmp; }
    }
    blockmap = blockmaplump + 4;

    Z_Free (blocklists); Z_Free (blockcount); Z_Free (blockdone);
    printf ("P_LoadBlockMap: rebuilt blockmap (%d x %d cells)\n", ncols, nrows);
}

//
// P_LoadBlockMap
//
void P_LoadBlockMap (int lump)
{
    int		i;
    int		count = (lump >= 0) ? W_LumpLength (lump)/2 : 0;	// shorts in the on-disk lump

    // Rebuild if the WAD blockmap is missing, degenerate, or too big for its 16-bit offsets
    // (over 65535 shorts -> some list offset can't be represented, so collision is broken).
    if (lump < 0 || count < 4 || count > 0x10000 || M_CheckParm ("-blockmap"))
    {
	P_CreateBlockMap ();
    }
    else
    {
	short* wad = W_CacheLumpNum (lump, PU_LEVEL);
	blockmaplump = Z_Malloc (count * sizeof(*blockmaplump), PU_LEVEL, 0);
	blockmaplump[0] = SHORT (wad[0]);			// origin x (signed)
	blockmaplump[1] = SHORT (wad[1]);			// origin y (signed)
	blockmaplump[2] = (int)(unsigned short) SHORT (wad[2]);	// width  (unsigned)
	blockmaplump[3] = (int)(unsigned short) SHORT (wad[3]);	// height (unsigned)
	for (i = 4; i < count; i++)
	{
	    short t = SHORT (wad[i]);				// offsets + line indices are UNSIGNED;
	    blockmaplump[i] = (t == -1) ? -1 : (int)(unsigned short) t;	// keep the 0xFFFF list terminator as -1
	}
	Z_Free (wad);
	blockmap = blockmaplump + 4;
	bmaporgx = blockmaplump[0] << FRACBITS;
	bmaporgy = blockmaplump[1] << FRACBITS;
	bmapwidth  = blockmaplump[2];
	bmapheight = blockmaplump[3];
    }

    // clear out mobj chains
    count = sizeof(*blocklinks) * bmapwidth * bmapheight;
    blocklinks = Z_Malloc (count, PU_LEVEL, 0);
    memset (blocklinks, 0, count);
}



//
// P_GroupLines
// Builds sector line lists and subsector sector numbers.
// Finds block bounding boxes for sectors.
//
void P_GroupLines (void)
{
    line_t**		linebuffer;
    int			i;
    int			j;
    int			total;
    line_t*		li;
    sector_t*		sector;
    subsector_t*	ss;
    seg_t*		seg;
    fixed_t		bbox[4];
    int			block;
	
    // look up sector number for each subsector
    ss = subsectors;
    for (i=0 ; i<numsubsectors ; i++, ss++)
    {
	seg = &segs[ss->firstline];
	ss->sector = seg->sidedef->sector;
    }

    // count number of lines in each sector
    li = lines;
    total = 0;
    for (i=0 ; i<numlines ; i++, li++)
    {
	total++;
	li->frontsector->linecount++;

	if (li->backsector && li->backsector != li->frontsector)
	{
	    li->backsector->linecount++;
	    total++;
	}
    }
	
    // build line tables for each sector
    // (one line_t* per entry -- sizeof(*linebuffer), not 4, on 64-bit)
    linebuffer = Z_Malloc (total*sizeof(*linebuffer), PU_LEVEL, 0);
    sector = sectors;
    for (i=0 ; i<numsectors ; i++, sector++)
    {
	M_ClearBox (bbox);
	sector->lines = linebuffer;
	li = lines;
	for (j=0 ; j<numlines ; j++, li++)
	{
	    if (li->frontsector == sector || li->backsector == sector)
	    {
		*linebuffer++ = li;
		M_AddToBox (bbox, li->v1->x, li->v1->y);
		M_AddToBox (bbox, li->v2->x, li->v2->y);
	    }
	}
	if (linebuffer - sector->lines != sector->linecount)
	    I_Error ("P_GroupLines: miscounted");
			
	// set the degenmobj_t to the middle of the bounding box
	sector->soundorg.x = (bbox[BOXRIGHT]+bbox[BOXLEFT])/2;
	sector->soundorg.y = (bbox[BOXTOP]+bbox[BOXBOTTOM])/2;
		
	// adjust bounding box to map blocks
	block = (bbox[BOXTOP]-bmaporgy+MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block >= bmapheight ? bmapheight-1 : block;
	sector->blockbox[BOXTOP]=block;

	block = (bbox[BOXBOTTOM]-bmaporgy-MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block < 0 ? 0 : block;
	sector->blockbox[BOXBOTTOM]=block;

	block = (bbox[BOXRIGHT]-bmaporgx+MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block >= bmapwidth ? bmapwidth-1 : block;
	sector->blockbox[BOXRIGHT]=block;

	block = (bbox[BOXLEFT]-bmaporgx-MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block < 0 ? 0 : block;
	sector->blockbox[BOXLEFT]=block;
    }
	
}


//
// P_SetupLevel
//
void
P_SetupLevel
( int		episode,
  int		map,
  int		playermask,
  skill_t	skill)
{
    int		i;
    char	lumpname[9];
    int		lumpnum;

    totalkills = totalitems = totalsecret = wminfo.maxfrags = 0;
    wminfo.partime = 180;
    for (i=0 ; i<MAXPLAYERS ; i++)
    {
	players[i].killcount = players[i].secretcount 
	    = players[i].itemcount = 0;
    }

    // Initial height of PointOfView
    // will be set by player think.
    players[consoleplayer].viewz = 1; 

    // Make sure all sounds are stopped before Z_FreeTags.
    S_Start ();			

    
#if 0 // UNUSED
    if (debugfile)
    {
	Z_FreeTags (PU_LEVEL, MAXINT);
	Z_FileDumpHeap (debugfile);
    }
    else
#endif
	Z_FreeTags (PU_LEVEL, PU_PURGELEVEL-1);


    // UNUSED W_Profile ();
    P_InitThinkers ();

    // if working with a devlopment map, reload it
    W_Reload ();			
	   
    // find map name
    if ( gamemode == commercial)
    {
	if (map<10)
	    sprintf (lumpname,"map0%i", map);
	else
	    sprintf (lumpname,"map%i", map);
    }
    else
    {
	lumpname[0] = 'E';
	lumpname[1] = '0' + episode;
	lumpname[2] = 'M';
	lumpname[3] = '0' + map;
	lumpname[4] = 0;
    }

    lumpnum = W_GetNumForName (lumpname);
	
    leveltime = 0;
	
    // note: most of this ordering is important
    P_LoadVertexes (lumpnum+ML_VERTEXES);
    P_LoadSectors (lumpnum+ML_SECTORS);
    P_LoadSideDefs (lumpnum+ML_SIDEDEFS);

    P_LoadLineDefs (lumpnum+ML_LINEDEFS);
    if (!P_LoadNodes_Extended (lumpnum+ML_NODES))   // Boom/ZDBSP XNOD -> loads verts+ssectors+segs+nodes
    {
	P_LoadSubsectors (lumpnum+ML_SSECTORS);
	P_LoadNodes (lumpnum+ML_NODES);
	P_LoadSegs (lumpnum+ML_SEGS);
    }

    // AFTER vertexes/linedefs (+ extended nodes): P_LoadBlockMap may need to REBUILD the
    // blockmap from the linedef geometry (WAD blockmap missing or too big), which reads
    // lines[]/vertexes[] -- so it can't run first like the stock order did.
    P_LoadBlockMap (lumpnum+ML_BLOCKMAP);

    rejectmatrix = W_CacheLumpNum (lumpnum+ML_REJECT,PU_LEVEL);
    P_GroupLines ();

    bodyqueslot = 0;
    deathmatch_p = deathmatchstarts;

    // Co-op buddy: before P_LoadThings reads THINGS, drop any stale mobj/flags
    // for the buddy slot.  Without this, playerstarts[] / players[].mo retain
    // values from the previous map's load (PWAD overlay of an E?M? keeps the
    // IWAD's Player_2_Start intact even though the PWAD has no P2 thing), which
    // would mask a missing P2_Start.  Reset here so P_AICoop_VerifySpawn can
    // reliably distinguish "this map had a P2_Start" (mo != NULL after spawn)
    // from "this map had no P2_Start" (mo == NULL because we just nulled it
    // and P_LoadThings didn't set it).
    P_AICoop_ResetSlot ();

    P_LoadThings (lumpnum+ML_THINGS);
    
    // if deathmatch, randomly spawn the active players
    if (deathmatch)
    {
	for (i=0 ; i<MAXPLAYERS ; i++)
	    if (playeringame[i])
	    {
		players[i].mo = NULL;
		G_DeathMatchSpawnPlayer (i);
	    }
			
    }

    // clear special respawning que
    iquehead = iquetail = 0;		
	
    // set up world state
    P_SpawnSpecials ();

    // LLM AI Director: drop any monster directives from the previous level
    P_AI_Reset ();
    P_Director_Reset ();		// reset L4D intensity/FSM for the new level
    P_MorphReset ();			// (M) drop any morphs from the previous level

    // Co-op buddy: -coop/-aicoop requested Player 2 to spawn on this map.
    // If the map has no Player_2_Start (only Player_1_Start), the buddy has
    // nowhere to spawn -- emit a one-shot warning and disable it for this
    // level instead of silently failing.
    P_AICoop_VerifySpawn ();
	
    // build subsector connect matrix
    //	UNUSED P_ConnectSubsectors ();

    // preload graphics
    if (precache)
	R_PrecacheLevel ();

    //printf ("free memory: 0x%x\n", Z_FreeMemory());

}



//
// P_Init
//
void P_Init (void)
{
    P_InitSwitchList ();
    P_InitPicAnims ();
    R_InitSprites (sprnames);
}



