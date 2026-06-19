// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//   System interface for the AI co-op buddy's spoken voice lines (offline).
//
//   The buddy's lines are pre-rendered into a Doom PWAD (buddy.wad, ~37 OGG
//   lumps named DSCONTAC1 / DSHURT01 / DSSTPISTP / ...).  At startup
//   I_Voice_Init() locates the WAD via W_AddFile() and indexes every
//   DS*-prefixed lump.  When the buddy wants to speak, the playsim calls
//   I_Voice_Say(lumpname); the dedicated SDL audio stream below decodes the
//   OGG via stb_vorbis (single-header, public domain, vendored as
//   files/stb_vorbis.c) and pushes PCM straight to the default playback
//   device -- completely independent of the 8-channel SFX mixer in i_sound.c
//   so speech and gameplay SFX never fight for channels.
//
//   Why a separate stream: the SFX mixer is 8-channel / mono-8 / 11025 Hz
//   and Doom-specific; speech is 22050-44100 Hz / 16-bit and benefits from
//   not being resampled into the SFX pipeline.
//
//-----------------------------------------------------------------------------

#ifndef __I_VOICE__
#define __I_VOICE__


// Speak the lump with the given 8-char name (case-insensitive).  The lump is
// loaded from the buddy.wad PWAD added by D_DoomMain.  Returns silently if the
// lump is unknown, the WAD isn't loaded, or voice init failed.  Voice init
// failures are non-fatal -- the buddy stays silent rather than crashing.
void I_Voice_SayByName (const char* lumpname);

// Speak the buddy's canonical phrase by tag: "contact", "hurt", "clear",
// "state:<what>", "summon_ok", "wait_hold", "wait_move", "attack_ok",
// "attack_none", "status:<weapon>[:ammo]".  Resolves to the right lump and
// forwards to I_Voice_SayByName.  Unknown tags are no-ops.
void I_Voice_Say (const char* tag);

// Init / shutdown -- called from D_DoomMain around the I_Init*/I_Shutdown*
// pair.  Loads the buddy WAD (cwd-relative "buddy.wad" by default, or from
// the "buddy_wad" entry in aidoom.cfg).  Opens the dedicated audio stream.
// Both are best-effort: any failure is logged but never fatal.
void I_Voice_Init (void);
void I_Voice_Shutdown (void);

#endif