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
//	Main loop menu stuff.
//	Default Config File.
//	PCX Screenshots.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_misc.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#include <stdlib.h>
#include <ctype.h>

extern int access(char *file, int mode);

#include "doomdef.h"

#include "z_zone.h"

#include "m_swap.h"
#include "m_argv.h"

#include "w_wad.h"

#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

#include "hu_stuff.h"

// State.
#include "doomstat.h"

// Data.
#include "dstrings.h"

#include "m_misc.h"

//
// M_DrawText
// Returns the final X coordinate
// HU_Init must have been called to init the font
//
extern patch_t*		hu_font[HU_FONTSIZE];

int
M_DrawText
( int		x,
  int		y,
  boolean	direct,
  char*		string )
{
    int 	c;
    int		w;

    while (*string)
    {
	c = toupper(*string) - HU_FONTSTART;
	string++;
	if (c < 0 || c> HU_FONTSIZE)
	{
	    x += 4;
	    continue;
	}
		
	w = SHORT (hu_font[c]->width);
	if (x+w > SCREENWIDTH)
	    break;
	if (direct)
	    V_DrawPatchDirect(x, y, 0, hu_font[c]);
	else
	    V_DrawPatch(x, y, 0, hu_font[c]);
	x+=w;
    }

    return x;
}




//
// M_WriteFile
//
#ifndef O_BINARY
#define O_BINARY 0
#endif

boolean
M_WriteFile
( char const*	name,
  void*		source,
  int		length )
{
    FILE       *handle;
    int		count;
	
    handle = fopen ( name, "wb");

    if (handle == NULL)
	return false;

    count = fwrite (source, 1, length, handle);
    fclose (handle);
	
    if (count < length)
	return false;
		
    return true;
}


//
// M_ReadFile
//
int
M_ReadFile
( char const*	name,
  byte**	buffer )
{
    FILE *handle;
    int count, length;
    byte	*buf;
	
    handle = fopen (name, "rb");
    if (handle == NULL)
	I_Error ("Couldn't read file %s", name);
    fseek(handle, 0, SEEK_END);
    length = ftell(handle);
    rewind(handle);
    buf = Z_Malloc (length, PU_STATIC, NULL);
    count = fread (buf, 1, length, handle);
    fclose (handle);
	
    if (count < length)
	I_Error ("Couldn't read file %s", name);
		
    *buffer = buf;
    return length;
}


//
// DEFAULTS
//
int		usemouse;
int		usejoystick;

extern int	key_right;
extern int	key_left;
extern int	key_up;
extern int	key_down;

extern int	key_strafeleft;
extern int	key_straferight;

extern int	key_fire;
extern int	key_use;
extern int	key_strafe;
extern int	key_speed;
extern int	key_nextweapon;
extern int	key_prevweapon;
extern int	key_jump;
extern int	key_buddy_come;
extern int	key_buddy_attack;
extern int	key_buddy_stay;
extern int	key_buddy_mode;
extern int	key_inv_left;
extern int	key_inv_right;
extern int	key_inv_use;
extern int	key_inv_drop;
extern int	autorun;
extern int	key_console;
extern int	key_spy;
extern int	crosshair;
extern int	scale_mode;
extern int	vsync;
extern int	integer_scale;
extern int	render_backend;
extern int	aspect;		// doomdef.c -- 0=4:3, 1=16:9, 2=16:10

// Monster pack-hunt AI (p_enemy.c)
extern int	monster_pack;
extern int	monster_pack_range;

extern int	mousebfire;
extern int	mousebstrafe;
extern int	mousebforward;

extern int	joybfire;
extern int	joybstrafe;
extern int	joybuse;
extern int	joybspeed;

extern int	viewwidth;
extern int	viewheight;

extern int	mouseSensitivity;
extern int	showMessages;
extern int	show_buddy_hud;	// hu_buddy.c -- companion top-of-screen HUD
extern int	show_inventory_hud;	// hu_buddy.c -- (J) artifact inventory readout

extern int	detailLevel;

extern int	screenblocks;

extern int	showMessages;
extern int	show_buddy_hud;	// hu_buddy.c -- companion top-of-screen HUD

// machine-independent sound params
extern	int	numChannels;

// Video options (i_video.c / doomdef.c).
extern	int	hires;			// resolution scale, 1..6
extern	int	fullscreen_mode;


extern char*	chat_macros[];



typedef struct
{
    char*	name;
    int*	location;
    int		defaultvalue;
    int		scantranslate;		// PC scan code hack
    int		untranslated;		// lousy hack
} default_t;

default_t	defaults[] =
{
    {"mouse_sensitivity",&mouseSensitivity, 5},
    {"sfx_volume",&snd_SfxVolume, 8},
    {"music_volume",&snd_MusicVolume, 8},
    {"show_messages",&showMessages, 1},
    {"show_buddy_hud",&show_buddy_hud, 1},
    {"show_inventory_hud",&show_inventory_hud, 1},	// (J) artifact inventory readout
    

    {"key_right",&key_right, KEY_RIGHTARROW},
    {"key_left",&key_left, KEY_LEFTARROW},
    {"key_up",&key_up, 'w'},
    {"key_down",&key_down, 's'},
    {"key_strafeleft",&key_strafeleft, 'a'},
    {"key_straferight",&key_straferight, 'd'},

    // Co-op buddy order keys -- one bind each, rebind here.  Defaults ',' '.' '-'
    // take over those keys (',' / '.' are also the strafe defaults; the buddy bind
    // wins, and '-' is taken from the screen-size shortcut while bound here).
    {"key_buddy_come",&key_buddy_come, ','},
    {"key_buddy_attack",&key_buddy_attack, '.'},
    {"key_buddy_stay",&key_buddy_stay, KEY_MINUS},
    {"key_buddy_mode",&key_buddy_mode, -1},	// unbound
    {"key_inv_left",&key_inv_left, KEY_LEFTARROW},	// (J) inventory: select prev artifact
    {"key_inv_right",&key_inv_right, KEY_RIGHTARROW},	// (J) inventory: select next artifact
    {"key_inv_use",&key_inv_use, KEY_UPARROW},		// (J) inventory: use selected artifact
    {"key_inv_drop",&key_inv_drop, KEY_DOWNARROW},	// (J) inventory: drop selected artifact

    {"key_fire",&key_fire, KEY_RCTRL},
    {"key_use",&key_use, 'e'},
    {"key_strafe",&key_strafe, KEY_RALT},
    {"key_speed",&key_speed, KEY_RSHIFT},
    {"key_nextweapon",&key_nextweapon, KEY_MWHEELUP},
    {"key_prevweapon",&key_prevweapon, KEY_MWHEELDOWN},
    {"key_jump",&key_jump, ' '},
    {"autorun",&autorun, 1},
    {"key_console",&key_console, KEY_BACKQUOTE},
    {"key_spy",&key_spy, KEY_F12},	// spy mode (view the AI buddy); default F12
    {"crosshair",&crosshair, 0},

    {"monster_pack",&monster_pack, 0},		// default OFF -> vanilla 1993 monster AI
    {"monster_pack_range",&monster_pack_range, 2048},

    {"use_mouse",&usemouse, 1},
    {"mouseb_fire",&mousebfire,0},
    {"mouseb_strafe",&mousebstrafe,-1},
    {"mouseb_forward",&mousebforward,-1},	// unbound by default (was the RIGHT button -> moved you forward)

    {"use_joystick",&usejoystick, 0},
    {"joyb_fire",&joybfire,0},
    {"joyb_strafe",&joybstrafe,1},
    {"joyb_use",&joybuse,3},
    {"joyb_speed",&joybspeed,2},

    {"screenblocks",&screenblocks, 10},
    {"detaillevel",&detailLevel, 0},

    // Video: internal resolution scale (1 = 320x200 ... 6 = 1920x1200) and
    // fullscreen flag.  Applied by i_video.c at startup.
    {"screen_resolution",&hires, 3},
    {"fullscreen",&fullscreen_mode, 0},
    {"scale_mode",&scale_mode, 0},
    {"vsync",&vsync, 1},
    {"integer_scale",&integer_scale, 0},
    {"render_backend",&render_backend, 0},
    {"aspect",&aspect, 0},			// default 4:3 (0=4:3, 1=16:9, 2=16:10)

    {"snd_channels",&numChannels, 3},



    {"usegamma",&usegamma, 0},

/*#ifndef __BEOS__*/
/*    {"chatmacro0", (int *) &chat_macros[0], (int) HUSTR_CHATMACRO0 },*/
/*    {"chatmacro1", (int *) &chat_macros[1], (int) HUSTR_CHATMACRO1 },*/
/*    {"chatmacro2", (int *) &chat_macros[2], (int) HUSTR_CHATMACRO2 },*/
/*    {"chatmacro3", (int *) &chat_macros[3], (int) HUSTR_CHATMACRO3 },*/
/*    {"chatmacro4", (int *) &chat_macros[4], (int) HUSTR_CHATMACRO4 },*/
/*    {"chatmacro5", (int *) &chat_macros[5], (int) HUSTR_CHATMACRO5 },*/
/*    {"chatmacro6", (int *) &chat_macros[6], (int) HUSTR_CHATMACRO6 },*/
/*    {"chatmacro7", (int *) &chat_macros[7], (int) HUSTR_CHATMACRO7 },*/
/*    {"chatmacro8", (int *) &chat_macros[8], (int) HUSTR_CHATMACRO8 },*/
/*    {"chatmacro9", (int *) &chat_macros[9], (int) HUSTR_CHATMACRO9 }*/
/*#endif*/

};

int	numdefaults;
char*	defaultfile;


//
// M_SaveDefaults
//
void M_SaveDefaults (void)
{
    int		i;
    int		v;
    FILE*	f;
    // Preserve config lines we don't manage (e.g. ollama_* written by the SDL3
    // config app) so quitting the game doesn't wipe them from aidoom.cfg.
    char	keep[64][256];
    int		nkeep = 0;

    f = fopen (defaultfile, "r");
    if (f)
    {
	char line[256], name[80];
	while (nkeep < 64 && fgets(line, sizeof(line), f))
	{
	    int known = 0;
	    if (sscanf(line, " %79s", name) != 1)
		continue;
	    for (i=0 ; i<numdefaults ; i++)
		if (!strcmp(defaults[i].name, name)) { known = 1; break; }
	    if (!known) { strncpy(keep[nkeep], line, 255); keep[nkeep][255]=0; nkeep++; }
	}
	fclose(f);
    }

    f = fopen (defaultfile, "w");
    if (!f)
	return; // can't write the file, but don't complain

    for (i=0 ; i<numdefaults ; i++)
    {
	if (defaults[i].defaultvalue > -0xfff
	    && defaults[i].defaultvalue < 0xfff)
	{
	    v = *defaults[i].location;
	    fprintf (f,"%s\t\t%i\n",defaults[i].name,v);
	} else {
	    fprintf (f,"%s\t\t\"%s\"\n",defaults[i].name,
		     * (char **) (defaults[i].location));
	}
    }
    for (i=0 ; i<nkeep ; i++)
	fputs (keep[i], f);

    fclose (f);
}


//
// M_LoadDefaults
//
extern byte	scantokey[128];

void M_LoadDefaults (void)
{
    int		i;
    int		len;
    FILE*	f;
    char	def[80];
    char	strparm[100];
    char*	newstring;
    int		parm;
    boolean	isstring;
    
    // set everything to base values
    numdefaults = sizeof(defaults)/sizeof(defaults[0]);
    for (i=0 ; i<numdefaults ; i++)
	*defaults[i].location = defaults[i].defaultvalue;
    
    // check for a custom default file
    i = M_CheckParm ("-config");
    if (i && i<myargc-1)
    {
	defaultfile = myargv[i+1];
	printf ("	default file: %s\n",defaultfile);
    }
    else
	defaultfile = basedefault;
    
    // read the file in, overriding any set defaults
    f = fopen (defaultfile, "r");
    if (f)
    {
	while (!feof(f))
	{
	    isstring = false;
	    if (fscanf (f, "%79s %[^\n]\n", def, strparm) == 2)
	    {
		if (strparm[0] == '"')
		{
		    // get a string default
		    isstring = true;
		    len = strlen(strparm);
		    newstring = (char *) malloc(len);
		    strparm[len-1] = 0;
		    strcpy(newstring, strparm+1);
		}
		else if (strparm[0] == '0' && strparm[1] == 'x')
		    sscanf(strparm+2, "%x", &parm);
		else
		    sscanf(strparm, "%i", &parm);
		for (i=0 ; i<numdefaults ; i++)
		    if (!strcmp(def, defaults[i].name))
		    {
			if (!isstring)
			    *defaults[i].location = parm;
			else
			    *defaults[i].location =
				(int) newstring;
			break;
		    }
	    }
	}
		
	fclose (f);
    }
}


//
// SCREEN SHOTS
//


typedef struct
{
    char		manufacturer;
    char		version;
    char		encoding;
    char		bits_per_pixel;

    unsigned short	xmin;
    unsigned short	ymin;
    unsigned short	xmax;
    unsigned short	ymax;
    
    unsigned short	hres;
    unsigned short	vres;

    unsigned char	palette[48];
    
    char		reserved;
    char		color_planes;
    unsigned short	bytes_per_line;
    unsigned short	palette_type;
    
    char		filler[58];
    unsigned char	data;		// unbounded
} pcx_t;


//
// WritePCXfile
//
void
WritePCXfile
( char*		filename,
  byte*		data,
  int		width,
  int		height,
  byte*		palette )
{
    int		i;
    int		length;
    pcx_t*	pcx;
    byte*	pack;
	
    pcx = Z_Malloc (width*height*2+1000, PU_STATIC, NULL);

    pcx->manufacturer = 0x0a;		// PCX id
    pcx->version = 5;			// 256 color
    pcx->encoding = 1;			// uncompressed
    pcx->bits_per_pixel = 8;		// 256 color
    pcx->xmin = 0;
    pcx->ymin = 0;
    pcx->xmax = SHORT(width-1);
    pcx->ymax = SHORT(height-1);
    pcx->hres = SHORT(width);
    pcx->vres = SHORT(height);
    memset (pcx->palette,0,sizeof(pcx->palette));
    pcx->color_planes = 1;		// chunky image
    pcx->bytes_per_line = SHORT(width);
    pcx->palette_type = SHORT(2);	// not a grey scale
    memset (pcx->filler,0,sizeof(pcx->filler));


    // pack the image
    pack = &pcx->data;
	
    for (i=0 ; i<width*height ; i++)
    {
	if ( (*data & 0xc0) != 0xc0)
	    *pack++ = *data++;
	else
	{
	    *pack++ = 0xc1;
	    *pack++ = *data++;
	}
    }
    
    // write the palette
    *pack++ = 0x0c;	// palette ID byte
    for (i=0 ; i<768 ; i++)
	*pack++ = *palette++;
    
    // write output file
    length = pack - (byte *)pcx;
    M_WriteFile (filename, pcx, length);

    Z_Free (pcx);
}


//
// M_ScreenShot
//
void M_ScreenShot (void)
{
    int		i;
    byte*	linear;
    char	lbmname[12];
    
    // munge planar buffer to linear
    linear = screens[2];
    I_ReadScreen (linear);
    
    // find a file name to save it to
    strcpy(lbmname,"DOOM00.pcx");
		
    for (i=0 ; i<=99 ; i++)
    {
	lbmname[4] = i/10 + '0';
	lbmname[5] = i%10 + '0';
	if (access(lbmname,0) == -1)
	    break;	// file doesn't exist
    }
    if (i==100)
	I_Error ("M_ScreenShot: Couldn't create a PCX");
    
    // save the pcx file
    WritePCXfile (lbmname, linear,
		  SCREENWIDTH, SCREENHEIGHT,
		  W_CacheLumpName ("PLAYPAL",PU_CACHE));
	
    players[consoleplayer].message = "screen shot";
}


