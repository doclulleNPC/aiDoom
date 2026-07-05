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
//	Teleportation.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: p_telept.c,v 1.3 1997/01/28 22:08:29 b1 Exp $";



#include "doomdef.h"

#include "s_sound.h"

#include "p_local.h"
#include "p_spec.h"
#include "tables.h"
#include "m_fixed.h"


// Data.
#include "sounds.h"

// State.
#include "r_state.h"

int P_FindLineFromLineTag (const line_t *line, int start);   // p_boomsp.c
void P_CalcHeight (player_t* player);                        // p_user.c



//
// TELEPORTATION
//
int
EV_Teleport
( line_t*	line,
  int		side,
  mobj_t*	thing )
{
    int		i;
    int		tag;
    mobj_t*	m;
    mobj_t*	fog;
    unsigned	an;
    thinker_t*	thinker;
    sector_t*	sector;
    fixed_t	oldx;
    fixed_t	oldy;
    fixed_t	oldz;

    // don't teleport missiles
    if (thing->flags & MF_MISSILE)
	return 0;		

    // Don't teleport if hit back of line,
    //  so you can get out of teleporter.
    if (side == 1)		
	return 0;	

    
    tag = line->tag;
    for (i = 0; i < numsectors; i++)
    {
	if (sectors[ i ].tag == tag )
	{
	    thinker = thinkercap.next;
	    for (thinker = thinkercap.next;
		 thinker != &thinkercap;
		 thinker = thinker->next)
	    {
		// not a mobj
		if (thinker->function.acp1 != (actionf_p1)P_MobjThinker)
		    continue;	

		m = (mobj_t *)thinker;
		
		// not a teleportman
		if (m->type != MT_TELEPORTMAN )
		    continue;		

		sector = m->subsector->sector;
		// wrong sector
		if (sector-sectors != i )
		    continue;	

		oldx = thing->x;
		oldy = thing->y;
		oldz = thing->z;
				
		if (!P_TeleportMove (thing, m->x, m->y))
		    return 0;
		
		thing->z = thing->floorz;  //fixme: not needed?
		if (thing->player)
		    thing->player->viewz = thing->z+thing->player->viewheight;
				
		// spawn teleport fog at source and destination
		fog = P_SpawnMobj (oldx, oldy, oldz, MT_TFOG);
		S_StartSound (fog, sfx_telept);
		an = m->angle >> ANGLETOFINESHIFT;
		fog = P_SpawnMobj (m->x+20*finecosine[an], m->y+20*finesine[an]
				   , thing->z, MT_TFOG);

		// emit sound, where?
		S_StartSound (fog, sfx_telept);
		
		// don't move for a bit
		if (thing->player)
		    thing->reactiontime = 18;	

		thing->angle = m->angle;
		thing->momx = thing->momy = thing->momz = 0;
		return 1;
	    }	
	}
    }
    return 0;
}



//
// Boom SILENT teleporters (Lee Killough).  No fog, no sound, angle/height preserved and momentum
// rotated so movement is continuous -- this is what lets a conveyor (252) carry candles into a
// teleporter and have them re-emerge still moving at the start of the belt.
//

// EV_SilentTeleport -- thing-exit kind (specials 207/208, monster-only 268/269).
int EV_SilentTeleport (line_t* line, int side, mobj_t* thing)
{
    int		i;
    mobj_t*	m;
    thinker_t*	th;

    if (side || (thing->flags & MF_MISSILE))
	return 0;

    for (i = -1; (i = P_FindSectorFromLineTag (line, i)) >= 0; )
	for (th = thinkercap.next; th != &thinkercap; th = th->next)
	    if (th->function.acp1 == (actionf_p1)P_MobjThinker &&
		(m = (mobj_t *)th)->type == MT_TELEPORTMAN &&
		m->subsector->sector - sectors == i)
	    {
		fixed_t z = thing->z - thing->floorz;   // height above ground
		// rotate 90 deg so crossing perpendicular exits along the exit thing's facing
		angle_t angle = R_PointToAngle2 (0, 0, line->dx, line->dy) - m->angle + ANG90;
		fixed_t s = finesine[angle >> ANGLETOFINESHIFT];
		fixed_t c = finecosine[angle >> ANGLETOFINESHIFT];
		fixed_t momx = thing->momx, momy = thing->momy;
		player_t* player = thing->player;

		if (!P_TeleportMove (thing, m->x, m->y))
		    return 0;

		thing->angle += angle;
		thing->z = z + thing->floorz;
		thing->momx = FixedMul (momx, c) - FixedMul (momy, s);
		thing->momy = FixedMul (momy, c) + FixedMul (momx, s);

		if (player && player->mo == thing)
		{
		    fixed_t dvh = player->deltaviewheight;
		    player->deltaviewheight = 0;
		    P_CalcHeight (player);
		    player->deltaviewheight = dvh;
		}
		return 1;
	    }
    return 0;
}

// EV_SilentLineTeleport -- linedef-to-linedef kind (243/244, reversed 262/263, monster 264-267).
// Moves the thing to the other linedef sharing this line's tag, keeping its offset along the line
// and rotating orientation/momentum by the angle between the two linedefs.
#define FUDGEFACTOR 10
int EV_SilentLineTeleport (line_t* line, int side, mobj_t* thing, boolean reverse)
{
    int		i;
    line_t*	l;

    if (side || (thing->flags & MF_MISSILE))
	return 0;

    for (i = -1; (i = P_FindLineFromLineTag (line, i)) >= 0; )
	if ((l = lines + i) != line && l->backsector)
	{
	    // thing's fractional position along the source linedef
	    fixed_t pos = abs(line->dx) > abs(line->dy) ?
		FixedDiv (thing->x - line->v1->x, line->dx) :
		FixedDiv (thing->y - line->v1->y, line->dy);

	    // angle between the two linedefs (rotate 180, and flip pos, if reversed)
	    angle_t angle = (reverse ? (pos = FRACUNIT - pos, 0) : ANG180) +
		R_PointToAngle2 (0, 0, l->dx, l->dy) -
		R_PointToAngle2 (0, 0, line->dx, line->dy);

	    fixed_t x = l->v2->x - FixedMul (pos, l->dx);
	    fixed_t y = l->v2->y - FixedMul (pos, l->dy);
	    fixed_t s = finesine[angle >> ANGLETOFINESHIFT];
	    fixed_t c = finecosine[angle >> ANGLETOFINESHIFT];
	    int	    fudge = FUDGEFACTOR;
	    player_t* player = (thing->player && thing->player->mo == thing) ? thing->player : NULL;
	    int	    stepdown = l->frontsector->floorheight < l->backsector->floorheight;
	    fixed_t z = thing->z - thing->floorz;
	    int	    exitside = reverse || (player && stepdown);

	    // nudge onto the correct side of the exit linedef (roundoff safety)
	    while (P_PointOnLineSide (x, y, l) != exitside && --fudge >= 0)
		if (abs(l->dx) > abs(l->dy))
		    y -= ((l->dx < 0) != exitside) ? -1 : 1;
		else
		    x += ((l->dy < 0) != exitside) ? -1 : 1;

	    if (!P_TeleportMove (thing, x, y))
		return 0;

	    thing->z = z + sides[l->sidenum[stepdown]].sector->floorheight;
	    thing->angle += angle;

	    x = thing->momx;
	    y = thing->momy;
	    thing->momx = FixedMul (x, c) - FixedMul (y, s);
	    thing->momy = FixedMul (y, c) + FixedMul (x, s);

	    if (player)
	    {
		fixed_t dvh = player->deltaviewheight;
		player->deltaviewheight = 0;
		P_CalcHeight (player);
		player->deltaviewheight = dvh;
	    }
	    return 1;
	}
    return 0;
}
