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
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_bbox.c,v 1.1 1997/02/03 22:45:10 b1 Exp $";


#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <SDL3/SDL.h>

#include "doomdef.h"
#include "m_misc.h"
#include "i_video.h"
#include "i_sound.h"
#include "i_voice.h"

#include "d_net.h"
#include "g_game.h"

#ifdef __GNUG__
#pragma implementation "i_system.h"
#endif
#include "i_system.h"




// Zone heap size (MB). 6 was the 1996 320x200 value; the hi-res screen wipe
// transposes a SCREENWIDTH*SCREENHEIGHT buffer (~1MB at 1280x800, ~2.3MB at
// 1920x1200) on top of level data, which overflowed a 6MB zone at map end.
int	mb_used = 48;	// zone heap; bumped for the larger screen-wipe buffer at 2560x1440


int I_strncasecmp(char *str1, char *str2, int len)
{
	// Compare exactly `len` bytes (or until both end at the same NUL).  The old loop
	// `while (*str1 && *str2 && len--)` stopped at the FIRST NUL in EITHER string and then
	// returned "equal", so a 7-char name like "242TEXT" wrongly matched the 8-char "242TEXTA"
	// (it is a prefix) -- and whichever came first in TEXTURE1 won.  8-byte WAD names must
	// compare all 8 bytes, treating the NUL pad as a real character.
	while ( len-- ) {
		char c1 = *str1++, c2 = *str2++;
		if ( toupper((unsigned char)c1) != toupper((unsigned char)c2) )
			return(1);
		if ( !c1 )		// both equal and NUL -> the names ended together
			return(0);
	}
	return(0);
}

void
I_Tactile
( int	on,
  int	off,
  int	total )
{
  // UNUSED.
  on = off = total = 0;
}

ticcmd_t	emptycmd;
ticcmd_t*	I_BaseTiccmd(void)
{
    return &emptycmd;
}


int  I_GetHeapSize (void)
{
    return mb_used*1024*1024;
}

byte* I_ZoneBase (int*	size)
{
    *size = mb_used*1024*1024;
    return (byte *) malloc (*size);
}



//
// I_GetTime
// returns time in 1/35 second tics
//
int  I_GetTime (void)
{
    // SDL_GetTicks() returns Uint32; multiplying by TICRATE (35) can wrap a
    // 32-bit int, so use Uint64 for the intermediate, then cast back.
    Uint64 ms = (Uint64)SDL_GetTicks ();
    return (int)((ms * TICRATE) / 1000);
}



//
// I_Init
//
void I_Init (void)
{
    if ( !SDL_Init(SDL_INIT_AUDIO|SDL_INIT_VIDEO) )
        I_Error("Could not initialize SDL: %s", SDL_GetError());

    I_InitSound();
    //  I_InitGraphics();
    I_Voice_Init ();
}

//
// I_Quit
//
void I_Quit (void)
{
    D_QuitNetGame ();
    I_ShutdownSound();
    I_ShutdownMusic();
    I_Voice_Shutdown ();
    M_SaveDefaults ();
    I_ShutdownGraphics();
    exit(0);
}

void I_WaitVBL(int count)
{
    SDL_Delay((count*1000)/70);
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte*	I_AllocLow(int length)
{
    byte*	mem;
        
    mem = (byte *)malloc (length);
    memset (mem,0,length);
    return mem;
}


//
// I_Error
//
extern boolean demorecording;

void I_Error (char *error, ...)
{
    va_list	argptr;
    char	buf[1024];

    // Message first.
    va_start (argptr,error);
    vsnprintf (buf, sizeof(buf), error, argptr);
    va_end (argptr);

    fprintf (stderr, "Error: %s\n", buf);
    fflush( stderr );

    // Show native message box to make errors user-friendly
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BuddyDoom Error", buf, NULL);

    // Shutdown. Here might be other errors.
    if (demorecording)
	G_CheckDemoStatus();

    D_QuitNetGame ();
    I_ShutdownGraphics();
    
    exit(-1);
}
