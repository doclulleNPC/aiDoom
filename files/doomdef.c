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
//  DoomDef - basic defines for DOOM, e.g. Version, game mode
//   and skill level, and display parameters.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_bbox.c,v 1.1 1997/02/03 22:45:10 b1 Exp $";


#ifdef __GNUG__
#pragma implementation "doomdef.h"
#endif
#include "doomdef.h"

// Location for any defines turned variables.

// Internal rendering resolution.  Defaults to 320x200 (hires==1); changed at
// runtime by V_SetRes() (see i_video.c) from the Video menu.
int	SCREENWIDTH  = BASE_WIDTH;
int	SCREENHEIGHT = BASE_HEIGHT;
int	hires        = 1;

// Widescreen (Hor+) support, crispy-doom style.  When `widescreen` is on,
// SCREENWIDTH is made wider than the 4:3/16:10 reference NONWIDEWIDTH; the 3D
// projection uses the NONWIDE width so the extra columns show more world at the
// sides (vertical FOV unchanged).  WIDESCREENDELTA is half the extra width in
// BASE (320-wide) coords -- HUD edge elements shift out by it.
int	widescreen    = 0;		// config: widescreen on/off
int	NONWIDEWIDTH  = BASE_WIDTH;	// the 16:10 width for the current hires
int	WIDESCREENDELTA = 0;		// (SCREENWIDTH-NONWIDEWIDTH)/hires/2, BASE coords


