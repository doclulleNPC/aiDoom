// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Quake-style drop-down developer console.  Toggle with the backquote (`)
//	key; type commands, see output.  Overlays the game (which keeps running).
//
//-----------------------------------------------------------------------------

#ifndef __C_CONSOLE__
#define __C_CONSOLE__

#include "doomtype.h"
#include "d_event.h"

void	C_Init (void);
boolean	C_Responder (event_t* ev);	// true = event consumed
void	C_Drawer (void);		// legacy no-op (SDL overlay draws it now)
void	C_Printf (const char* fmt, ...);
int	C_Active (void);		// non-zero while the console is open
const char* C_GetLine (int row);	// row 0 = input; 1.. = scrollback (i_video overlay)

#endif
