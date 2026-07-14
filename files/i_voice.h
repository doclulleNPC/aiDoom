// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//   System interface for the AI co-op buddy's spoken voice lines (offline).
//
//   The buddy's lines are pre-rendered into a Doom PWAD (aidoom.wad, ~37 OGG
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
// loaded from the aidoom.wad PWAD added by D_DoomMain.  Returns silently if the
// lump is unknown, the WAD isn't loaded, or voice init failed.  Voice init
// failures are non-fatal -- the buddy stays silent rather than crashing.
// lvol/rvol are 0..127 per-channel gains (Doom positional volume): the caller
// computes them from the buddy's position vs the listener, so the voice is
// spatialised (distance + stereo pan) instead of flat at the player.
void I_Voice_SayByName (const char* lumpname, int lvol, int rvol);

// Resolve the aidoom asset/voice WAD path (cfg override or default "aidoom.wad").
// D_DoomMain uses it to add the WAD EARLY -- before W_Init/R_InitSprites -- so any
// sprites baked into it (e.g. the deployable turret MTUR*) register with the sprite
// system.  I_Voice_Init then detects it's already loaded and skips re-adding it.
void I_Voice_ResolveWad (char* buf, int n);

// Speak the buddy's canonical phrase by tag: "contact", "hurt", "clear",
// "state:<what>", "summon_ok", "wait_hold", "wait_move", "attack_ok",
// "attack_none", "status:<weapon>[:ammo]".  Resolves to the right lump and
// forwards to I_Voice_SayByName.  Unknown tags are no-ops.
void I_Voice_Say (const char* tag, int lvol, int rvol);

// Is the buddy still speaking?  Nonzero while PCM remains queued on the voice
// stream.  The playsim uses this to avoid piling lines on top of each other and
// to let a higher-priority line decide whether to barge in (see I_Voice_Stop).
int  I_Voice_Busy (void);

// Cut off whatever is currently playing/queued on the voice stream, so a more
// important line (e.g. a command ack) can start immediately instead of waiting
// the current line out.  No-op if voice init failed.
void I_Voice_Stop (void);

// ----- AI "Director" persona -----------------------------------------------
// A second voice (different ElevenLabs id, "DD*" lumps, played on its OWN audio
// stream) for the L4D-style AI Director: it narrates spawns, intensity phases
// and item drops.  Separate stream => the Director and the buddy can speak
// simultaneously without cutting each other off.  Tag form is "dir:<event>:<n>"
// (see VOICE_MAP / p_ai_director.c).  Unknown tags / failed init are no-ops.
void I_Director_Say (const char* tag, int lvol, int rvol);
int  I_Director_Busy (void);
void I_Director_Stop (void);

// Init / shutdown -- called from D_DoomMain around the I_Init*/I_Shutdown*
// pair.  Loads the buddy WAD (cwd-relative "aidoom.wad" by default, or from
// the "buddy_wad" entry in aidoom.cfg).  Opens the dedicated audio stream.
// Both are best-effort: any failure is logged but never fatal.
void I_Voice_Init (void);
void I_Voice_Shutdown (void);

#endif