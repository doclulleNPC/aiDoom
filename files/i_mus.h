// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	From-scratch MUS music player (MOD addition).
//	Parses DOOM's MUS lumps and renders them with a small 2-operator FM
//	synth driven by the IWAD's GENMIDI instrument patches -- an OPL-style
//	(Adlib) sound, without ZMusic or any external dependency.  Output is fed
//	into the same SDL audio mix as the SFX and the OGG path (see i_sound.c).
//
//-----------------------------------------------------------------------------

#ifndef __I_MUS__
#define __I_MUS__

#include "doomtype.h"

// Load GENMIDI from the WAD (once).  Returns false if absent.
boolean	MUS_Init (void);

// Parse a MUS lump.  Returns false if it is not MUS or GENMIDI is missing.
boolean	MUS_Register (const void* data, int length);

// Begin/stop playback (Start may be called again to restart).
void	MUS_Start (int looping);
void	MUS_Stop (void);

// Render `frames` stereo S16 samples at 11025 Hz; advances the sequencer.
void	MUS_Render (short* out, int frames);

#endif
