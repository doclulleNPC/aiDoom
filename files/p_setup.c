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
short*		blockmap;	// int for larger maps
// offsets in blockmap are from here
short*		blockmaplump;		
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
    }
	
    Z_Free (data);
}


// ZDBSP / Boom "extended nodes": when a map is built with uncompressed extended nodes the NODES
// lump starts with "XNOD" (SSECTORS + SEGS lumps are empty) and carries new vertices, subsectors,
// segs and 32-bit nodes itself.  Detect + load the whole thing here.  (ZNOD = zlib-compressed and
// the GL variants are not handled yet.)  Returns true if it consumed the lump.
extern angle_t R_PointToAngle2 (fixed_t, fixed_t, fixed_t, fixed_t);

static fixed_t P_SegOffset (fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
    double dx = (double)(x1 - x2) / FRACUNIT;
    double dy = (double)(y1 - y2) / FRACUNIT;
    return (fixed_t)(sqrt (dx*dx + dy*dy) * FRACUNIT);
}

boolean P_LoadNodes_Extended (int lump)
{
    byte*   data = W_CacheLumpNum (lump, PU_STATIC);
    byte*   p;
    unsigned origv, newv, i;
    unsigned nsub, nseg, nnod;

    if (W_LumpLength (lump) < 8 || memcmp (data, "XNOD", 4) != 0)
    { Z_Free (data); return false; }

    p = data + 4;
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
    Z_Free (data);
    printf ("P_LoadNodes: extended (XNOD) nodes -- %u verts, %u ssectors, %u segs, %u nodes\n",
            origv + newv, nsub, nseg, nnod);
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

	ld->sidenum[0] = SHORT(mld->sidenum[0]);
	ld->sidenum[1] = SHORT(mld->sidenum[1]);

	if (ld->sidenum[0] != -1)
	    ld->frontsector = sides[ld->sidenum[0]].sector;
	else
	    ld->frontsector = 0;

	if (ld->sidenum[1] != -1)
	    ld->backsector = sides[ld->sidenum[1]].sector;
	else
	    ld->backsector = 0;

	ld->tranlump = -1;	// Boom 260: opaque unless a 260 special says otherwise (below)
    }

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
    }
	
    Z_Free (data);
}


//
// P_LoadBlockMap
//
void P_LoadBlockMap (int lump)
{
    int		i;
    int		count;
	
    blockmaplump = W_CacheLumpNum (lump,PU_LEVEL);
    blockmap = blockmaplump+4;
    count = W_LumpLength (lump)/2;

    for (i=0 ; i<count ; i++)
	blockmaplump[i] = SHORT(blockmaplump[i]);
		
    bmaporgx = blockmaplump[0]<<FRACBITS;
    bmaporgy = blockmaplump[1]<<FRACBITS;
    bmapwidth = blockmaplump[2];
    bmapheight = blockmaplump[3];
	
    // clear out mobj chains
    count = sizeof(*blocklinks)* bmapwidth*bmapheight;
    blocklinks = Z_Malloc (count,PU_LEVEL, 0);
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
    P_LoadBlockMap (lumpnum+ML_BLOCKMAP);
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



