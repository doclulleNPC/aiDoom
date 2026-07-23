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
// DESCRIPTION:
//	Items: key cards, artifacts, weapon, ammunition.
//
//-----------------------------------------------------------------------------


#ifndef __D_ITEMS__
#define __D_ITEMS__

#include "doomdef.h"

#ifdef __GNUG__
#pragma interface
#endif


// MBF21 weapon flags (weaponinfo_t.flags), set via DEHACKED "MBF21 Bits".
#define WPF_NOTHRUST		0x00000001	// its attacks don't thrust things
#define WPF_SILENT		0x00000002	// silent -- firing doesn't alert monsters
#define WPF_NOAUTOFIRE		0x00000004	// won't autofire when swapped to with fire held
#define WPF_FLEEMELEE		0x00000008	// monsters treat it as a melee weapon (AI only)
#define WPF_AUTOSWITCHFROM	0x00000010	// auto-switch away from when ammo is picked up
#define WPF_NOAUTOSWITCHTO	0x00000020	// never auto-switched to when ammo is picked up

// Weapon info: sprite frames, ammunition use.
typedef struct
{
    ammotype_t	ammo;
    int		upstate;
    int		downstate;
    int		readystate;
    int		atkstate;
    int		flashstate;
    int		flags;		// mbf21 WPF_* weapon flags
    int		ammopershot;	// mbf21: ammo per shot (0 = vanilla per-weapon amount)

} weaponinfo_t;

extern  weaponinfo_t    weaponinfo[NUMWEAPONS];

#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
