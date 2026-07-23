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
//
// $Log:$
//
// DESCRIPTION:
//	Switches, buttons. Two-state animation. Exits.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: p_switch.c,v 1.3 1997/01/28 22:08:29 b1 Exp $";


#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"
#include "doomdef.h"
#include "p_local.h"

#include "g_game.h"

#include "s_sound.h"

// Data.
#include "sounds.h"

// State.
#include "doomstat.h"
#include "r_state.h"
#include "r_data.h"


//
// CHANGE THE TEXTURE OF A WALL SWITCH TO ITS OPPOSITE
//
switchlist_t alphSwitchList[] =
{
    // Doom shareware episode 1 switches
    {"SW1BRCOM",	"SW2BRCOM",	1},
    {"SW1BRN1",	"SW2BRN1",	1},
    {"SW1BRN2",	"SW2BRN2",	1},
    {"SW1BRNGN",	"SW2BRNGN",	1},
    {"SW1BROWN",	"SW2BROWN",	1},
    {"SW1COMM",	"SW2COMM",	1},
    {"SW1COMP",	"SW2COMP",	1},
    {"SW1DIRT",	"SW2DIRT",	1},
    {"SW1EXIT",	"SW2EXIT",	1},
    {"SW1GRAY",	"SW2GRAY",	1},
    {"SW1GRAY1",	"SW2GRAY1",	1},
    {"SW1METAL",	"SW2METAL",	1},
    {"SW1PIPE",	"SW2PIPE",	1},
    {"SW1SLAD",	"SW2SLAD",	1},
    {"SW1STARG",	"SW2STARG",	1},
    {"SW1STON1",	"SW2STON1",	1},
    {"SW1STON2",	"SW2STON2",	1},
    {"SW1STONE",	"SW2STONE",	1},
    {"SW1STRTN",	"SW2STRTN",	1},
    
    // Doom registered episodes 2&3 switches
    {"SW1BLUE",	"SW2BLUE",	2},
    {"SW1CMT",		"SW2CMT",	2},
    {"SW1GARG",	"SW2GARG",	2},
    {"SW1GSTON",	"SW2GSTON",	2},
    {"SW1HOT",		"SW2HOT",	2},
    {"SW1LION",	"SW2LION",	2},
    {"SW1SATYR",	"SW2SATYR",	2},
    {"SW1SKIN",	"SW2SKIN",	2},
    {"SW1VINE",	"SW2VINE",	2},
    {"SW1WOOD",	"SW2WOOD",	2},
    
    // Doom II switches
    {"SW1PANEL",	"SW2PANEL",	3},
    {"SW1ROCK",	"SW2ROCK",	3},
    {"SW1MET2",	"SW2MET2",	3},
    {"SW1WDMET",	"SW2WDMET",	3},
    {"SW1BRIK",	"SW2BRIK",	3},
    {"SW1MOD1",	"SW2MOD1",	3},
    {"SW1ZIM",		"SW2ZIM",	3},
    {"SW1STON6",	"SW2STON6",	3},
    {"SW1TEK",		"SW2TEK",	3},
    {"SW1MARB",	"SW2MARB",	3},
    {"SW1SKULL",	"SW2SKULL",	3},
	
    {"\0",		"\0",		0}
};

int		switchlist[MAXSWITCHES * 2];
int		numswitches;
button_t        buttonlist[MAXBUTTONS];

//
// P_InitSwitchList
// Only called at game initialization.
//
void P_InitSwitchList(void)
{
    int		i;
    int		index;
    int		episode;

    // Heretic (phase 1) has its own switch textures (SW1* names differ) -- the DOOM
    // alphSwitchList references textures heretic.wad lacks, and R_TextureNumForName
    // I_Errors on a miss.  Skip building the switch list until Heretic switches are
    // ported (a later phase); animated switches just won't toggle for now.
    if (heretic_mode)
    {
	numswitches = 0;
	switchlist[0] = -1;
	return;
    }

    episode = 1;

    if (gamemode == registered)
	episode = 2;
    else
	if ( gamemode == commercial )
	    episode = 3;

    // Boom: use the WAD's SWITCHES lump if present (packed: char[9] name1, char[9] name2, int16
    // episode; terminated by episode==0), else the vanilla alphSwitchList[] table.
    {
	int swlen = (W_CheckNumForName("SWITCHES") >= 0) ? W_LumpLength(W_GetNumForName("SWITCHES")) : -1;
	byte* sw = (swlen >= 20) ? W_CacheLumpName("SWITCHES", PU_STATIC) : NULL;
	if (sw)
	{
	    char n1[9], n2[9]; int ep;
	    for (index = 0, i = 0; (i+1)*20 <= swlen; i++)
	    {
		byte* p = sw + i*20;
		memcpy (n1, p, 9); n1[8]=0; memcpy (n2, p+9, 9); n2[8]=0;
		ep = (short)(p[18] | (p[19]<<8));
		if (!ep) break;
		if (index >= MAXSWITCHES*2 - 2) break;
		if (ep <= episode && R_CheckTextureNumForName(n1) >= 0 && R_CheckTextureNumForName(n2) >= 0)
		{
		    switchlist[index++] = R_TextureNumForName(n1);
		    switchlist[index++] = R_TextureNumForName(n2);
		}
	    }
	    numswitches = index/2;
	    switchlist[index] = -1;
	    Z_ChangeTag (sw, PU_CACHE);
	    return;
	}
    }

    for (index = 0,i = 0;i < MAXSWITCHES;i++)
    {
	if (!alphSwitchList[i].episode)
	{
	    numswitches = index/2;
	    switchlist[index] = -1;
	    break;
	}
		
	if (alphSwitchList[i].episode <= episode)
	{
#if 0	// UNUSED - debug?
	    int		value;
			
	    if (R_CheckTextureNumForName(alphSwitchList[i].name1) < 0)
	    {
		I_Error("Can't find switch texture '%s'!",
			alphSwitchList[i].name1);
		continue;
	    }
	    
	    value = R_TextureNumForName(alphSwitchList[i].name1);
#endif
	    switchlist[index++] = R_TextureNumForName(alphSwitchList[i].name1);
	    switchlist[index++] = R_TextureNumForName(alphSwitchList[i].name2);
	}
    }
}


//
// Start a button counting down till it turns off.
//
void
P_StartButton
( line_t*	line,
  bwhere_e	w,
  int		texture,
  int		time )
{
    int		i;
    
    // See if button is already pressed
    for (i = 0;i < MAXBUTTONS;i++)
    {
	if (buttonlist[i].btimer
	    && buttonlist[i].line == line)
	{
	    
	    return;
	}
    }
    

    
    for (i = 0;i < MAXBUTTONS;i++)
    {
	if (!buttonlist[i].btimer)
	{
	    buttonlist[i].line = line;
	    buttonlist[i].where = w;
	    buttonlist[i].btexture = texture;
	    buttonlist[i].btimer = time;
	    buttonlist[i].soundorg = (mobj_t *)&line->frontsector->soundorg;
	    return;
	}
    }
    
    I_Error("P_StartButton: no button slots left!");
}





//
// Function that changes wall texture.
// Tell it if switch is ok to use again (1=yes, it's a button).
//
void
P_ChangeSwitchTexture
( line_t*	line,
  int 		useAgain )
{
    int     texTop;
    int     texMid;
    int     texBot;
    int     i;
    int     sound;
	
    if (!useAgain)
	line->special = 0;

    texTop = sides[line->sidenum[0]].toptexture;
    texMid = sides[line->sidenum[0]].midtexture;
    texBot = sides[line->sidenum[0]].bottomtexture;

    sound = sfx_swtchn;

    // EXIT SWITCH?
    if (line->special == 11)                
	sound = sfx_swtchx;
	
    for (i = 0;i < numswitches*2;i++)
    {
	if (switchlist[i] == texTop)
	{
	    S_StartSound(buttonlist->soundorg,sound);
	    sides[line->sidenum[0]].toptexture = switchlist[i^1];

	    if (useAgain)
		P_StartButton(line,top,switchlist[i],BUTTONTIME);

	    return;
	}
	else
	{
	    if (switchlist[i] == texMid)
	    {
		S_StartSound(buttonlist->soundorg,sound);
		sides[line->sidenum[0]].midtexture = switchlist[i^1];

		if (useAgain)
		    P_StartButton(line, middle,switchlist[i],BUTTONTIME);

		return;
	    }
	    else
	    {
		if (switchlist[i] == texBot)
		{
		    S_StartSound(buttonlist->soundorg,sound);
		    sides[line->sidenum[0]].bottomtexture = switchlist[i^1];

		    if (useAgain)
			P_StartButton(line, bottom,switchlist[i],BUTTONTIME);

		    return;
		}
	    }
	}
    }
}






//
// P_UseSpecialLine
// Called when a thing uses a special line.
// Only the front sides of lines are usable.
//
boolean
P_UseSpecialLine
( mobj_t*	thing,
  line_t*	line,
  int		side )
{
    if (!side && P_DoGenLineSpecial (line, thing, 1)) return true;   // Boom generalized (switch)

    // Err...
    // Use the back sides of VERY SPECIAL lines...
    if (side)
    {
	switch(line->special)
	{
	  case 124:
	    // Sliding door open&close
	    // UNUSED?
	    break;

	  default:
	    return false;
	    break;
	}
    }

    
    // Switches that other things can activate.
    if (!thing->player)
    {
	// never open secret doors
	if (line->flags & ML_SECRET)
	    return false;
	
	switch(line->special)
	{
	  case 1: 	// MANUAL DOOR RAISE
	  case 32:	// MANUAL BLUE
	  case 33:	// MANUAL RED
	  case 34:	// MANUAL YELLOW
	    break;
	    
	  default:
	    return false;
	    break;
	}
    }

    
    // do something  
    switch (line->special)
    {
	// MANUALS
      case 1:		// Vertical Door
      case 26:		// Blue Door/Locked
      case 27:		// Yellow Door /Locked
      case 28:		// Red Door /Locked

      case 31:		// Manual door open
      case 32:		// Blue locked door open
      case 33:		// Red locked door open
      case 34:		// Yellow locked door open

      case 117:		// Blazing door raise
      case 118:		// Blazing door open
	EV_VerticalDoor (line, thing);
	break;
	
	//UNUSED - Door Slide Open&Close
	// case 124:
	// EV_SlidingDoor (line, thing);
	// break;

	// SWITCHES
      case 7:
	// Build Stairs
	if (EV_BuildStairs(line,build8))
	    P_ChangeSwitchTexture(line,0);
	break;

      case 9:
	// Change Donut
	if (EV_DoDonut(line))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 11:
	// Exit level
	P_ChangeSwitchTexture(line,0);
	G_ExitLevel ();
	break;

      case 2070:
	// ID24: S1 Exit + reset inventory (normal)
	P_ChangeSwitchTexture(line,0);
	id24_reset_inventory = true;
	G_ExitLevel ();
	break;

      // ID24 music-change (switch): S1 / SR, looping / once, plain / reset.  No
      // switch-texture flip -- the "texture" name is the music lump.
      case 2059: case 2060: case 2065: case 2066:
      case 2089: case 2090: case 2095: case 2096:
	EV_ChangeMusic (line, side);
	break;

      case 14:
	// Raise Floor 32 and change texture
	if (EV_DoPlat(line,raiseAndChange,32))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 15:
	// Raise Floor 24 and change texture
	if (EV_DoPlat(line,raiseAndChange,24))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 18:
	// Raise Floor to next highest floor
	if (EV_DoFloor(line, raiseFloorToNearest))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 20:
	// Raise Plat next highest floor and change texture
	if (EV_DoPlat(line,raiseToNearestAndChange,0))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 21:
	// PlatDownWaitUpStay
	if (EV_DoPlat(line,downWaitUpStay,0))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 23:
	// Lower Floor to Lowest
	if (EV_DoFloor(line,lowerFloorToLowest))
	    P_ChangeSwitchTexture(line,0);
	break;

      // --- Boom extended S1 (switch, once) floor/ceiling types used by Legacy of Rust ---
      case 159:
	// Boom S1: Floor Lower to Lowest, Change Texture and Type (the S1 form of
	// classic 37; aiDoom's lowerAndChange uses the trigger model rather than
	// Boom's numeric model, but the floor motion + texture/type change match).
	if (EV_DoFloor(line,lowerAndChange))
	    P_ChangeSwitchTexture(line,0);
	break;

      case 164:
	// Boom S1: Start Crusher, Fast/large damage
	if (EV_DoCeiling(line,fastCrushAndRaise))
	    P_ChangeSwitchTexture(line,0);
	break;

      case 166:
	// Boom S1: Ceiling Raise to Highest surrounding ceiling
	if (EV_DoCeiling(line,raiseToHighest))
	    P_ChangeSwitchTexture(line,0);
	break;

      case 221:
	// Boom S1: Floor Lower to Next Lower Neighbour Floor
	if (EV_DoFloor(line,lowerFloorToNearest))
	    P_ChangeSwitchTexture(line,0);
	break;

      case 229:		// Boom S1: Elevator up to next floor
	if (EV_DoElevator(line,elevateUp))     P_ChangeSwitchTexture(line,0);
	break;
      case 233:		// Boom S1: Elevator down to next floor
	if (EV_DoElevator(line,elevateDown))   P_ChangeSwitchTexture(line,0);
	break;
      case 237:		// Boom S1: Elevator to current (triggering) floor
	if (EV_DoElevator(line,elevateCurrent)) P_ChangeSwitchTexture(line,0);
	break;

      case 29:
	// Raise Door
	if (EV_DoDoor(line,normal))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 41:
	// Lower Ceiling to Floor
	if (EV_DoCeiling(line,lowerToFloor))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 71:
	// Turbo Lower Floor
	if (EV_DoFloor(line,turboLower))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 49:
	// Ceiling Crush And Raise
	if (EV_DoCeiling(line,crushAndRaise))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 50:
	// Close Door
	if (EV_DoDoor(line,close))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 51:
	// Secret EXIT
	P_ChangeSwitchTexture(line,0);
	G_SecretExitLevel ();
	break;

      case 2073:
	// ID24: S1 Exit + reset inventory (secret)
	P_ChangeSwitchTexture(line,0);
	id24_reset_inventory = true;
	G_SecretExitLevel ();
	break;

      case 55:
	// Raise Floor Crush
	if (EV_DoFloor(line,raiseFloorCrush))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 101:
	// Raise Floor
	if (EV_DoFloor(line,raiseFloor))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 102:
	// Lower Floor to Surrounding floor height
	if (EV_DoFloor(line,lowerFloor))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 103:
	// Open Door
	if (EV_DoDoor(line,open))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 111:
	// Blazing Door Raise (faster than TURBO!)
	if (EV_DoDoor (line,blazeRaise))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 112:
	// Blazing Door Open (faster than TURBO!)
	if (EV_DoDoor (line,blazeOpen))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 113:
	// Blazing Door Close (faster than TURBO!)
	if (EV_DoDoor (line,blazeClose))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 122:
	// Blazing PlatDownWaitUpStay
	if (EV_DoPlat(line,blazeDWUS,0))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 127:
	// Build Stairs Turbo 16
	if (EV_BuildStairs(line,turbo16))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 131:
	// Raise Floor Turbo
	if (EV_DoFloor(line,raiseFloorTurbo))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 133:
	// BlzOpenDoor BLUE
      case 135:
	// BlzOpenDoor RED
      case 137:
	// BlzOpenDoor YELLOW
	if (EV_DoLockedDoor (line,blazeOpen,thing))
	    P_ChangeSwitchTexture(line,0);
	break;
	
      case 140:
	// Raise Floor 512
	if (EV_DoFloor(line,raiseFloor512))
	    P_ChangeSwitchTexture(line,0);
	break;
	
	// BUTTONS
      case 42:
	// Close Door
	if (EV_DoDoor(line,close))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 43:
	// Lower Ceiling to Floor
	if (EV_DoCeiling(line,lowerToFloor))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 45:
	// Lower Floor to Surrounding floor height
	if (EV_DoFloor(line,lowerFloor))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 60:
	// Lower Floor to Lowest
	if (EV_DoFloor(line,lowerFloorToLowest))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 61:
	// Open Door
	if (EV_DoDoor(line,open))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 62:
	// PlatDownWaitUpStay
	if (EV_DoPlat(line,downWaitUpStay,1))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 63:
	// Raise Door
	if (EV_DoDoor(line,normal))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 64:
	// Raise Floor to ceiling
	if (EV_DoFloor(line,raiseFloor))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 66:
	// Raise Floor 24 and change texture
	if (EV_DoPlat(line,raiseAndChange,24))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 67:
	// Raise Floor 32 and change texture
	if (EV_DoPlat(line,raiseAndChange,32))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 65:
	// Raise Floor Crush
	if (EV_DoFloor(line,raiseFloorCrush))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 68:
	// Raise Plat to next highest floor and change texture
	if (EV_DoPlat(line,raiseToNearestAndChange,0))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 69:
	// Raise Floor to next highest floor
	if (EV_DoFloor(line, raiseFloorToNearest))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 70:
	// Turbo Lower Floor
	if (EV_DoFloor(line,turboLower))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 114:
	// Blazing Door Raise (faster than TURBO!)
	if (EV_DoDoor (line,blazeRaise))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 115:
	// Blazing Door Open (faster than TURBO!)
	if (EV_DoDoor (line,blazeOpen))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 116:
	// Blazing Door Close (faster than TURBO!)
	if (EV_DoDoor (line,blazeClose))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 123:
	// Blazing PlatDownWaitUpStay
	if (EV_DoPlat(line,blazeDWUS,0))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 132:
	// Raise Floor Turbo
	if (EV_DoFloor(line,raiseFloorTurbo))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 99:
	// BlzOpenDoor BLUE
      case 134:
	// BlzOpenDoor RED
      case 136:
	// BlzOpenDoor YELLOW
	if (EV_DoLockedDoor (line,blazeOpen,thing))
	    P_ChangeSwitchTexture(line,1);
	break;
	
      case 138:
	// Light Turn On
	EV_LightTurnOn(line,255);
	P_ChangeSwitchTexture(line,1);
	break;
	
      case 139:
	// Light Turn Off
	EV_LightTurnOn(line,35);
	P_ChangeSwitchTexture(line,1);
	break;

      case 195:
	// Boom SR: silent Teleport (preserves angle/relative position)
	if (EV_SilentTeleport(line,side,thing))
	    P_ChangeSwitchTexture(line,1);
	break;

      case 230:		// Boom SR: Elevator up to next floor
	if (EV_DoElevator(line,elevateUp))      P_ChangeSwitchTexture(line,1);
	break;
      case 234:		// Boom SR: Elevator down to next floor
	if (EV_DoElevator(line,elevateDown))    P_ChangeSwitchTexture(line,1);
	break;
      case 238:		// Boom SR: Elevator to current (triggering) floor
	if (EV_DoElevator(line,elevateCurrent)) P_ChangeSwitchTexture(line,1);
	break;

    }

    return true;
}

