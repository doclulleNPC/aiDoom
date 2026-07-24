// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.  (BuddyDoom fork additions)
//
// DESCRIPTION:
//	In-game key-bindings screen ("Options -> Controls").  Owns the rebind
//	STATE + input; the crisp TrueType-atlas OVERLAY is drawn by i_video.c
//	(I_DrawControlsOverlay), the same split the developer console uses.
//
//-----------------------------------------------------------------------------
#ifndef __M_CONTROLS__
#define __M_CONTROLS__

#include "doomtype.h"
#include "d_event.h"

// Enter the controls screen (called from the Options menu).
void	M_Controls_Open (void);

// True while the screen is up -- i_video.c draws the overlay, D_ProcessEvents
// routes input here.
boolean	M_Controls_Active (void);

// Handle one event; returns true if it was consumed (screen is up).
boolean	M_Controls_Responder (event_t* ev);

// ---- read-only accessors for the SDL overlay drawer (i_video.c) -------------
int		M_Controls_Count (void);		// number of rebindable actions
const char*	M_Controls_Label (int i);		// action label
void		M_Controls_KeyName (int i, char* out, int n);	// current key as text ("" = unbound)
int		M_Controls_Sel (void);			// highlighted row
boolean		M_Controls_Capturing (void);		// waiting for a keypress?

#endif
