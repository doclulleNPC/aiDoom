//
// i_opl.h -- OPL2/OPL3 music backend for BuddyDoom (public API).
//
// A self-contained passive OPL music player that drives the Nuked-OPL3
// emulator (opl3.c) using the IWAD's GENMIDI patches. The instrument /
// voice / frequency logic is ported from Chocolate/Crispy-Doom's
// i_oplmusic.c. The caller (i_mus.c) feeds it MIDI-style events and pulls
// rendered stereo samples; this module owns no timer and no SDL audio path.
//

#ifndef I_OPL_H
#define I_OPL_H

// Initialise the OPL chip (OPL3_Reset at `samplerate`) and the voice table.
void OPL_Music_Init(int samplerate);

// Parse a GENMIDI lump (8-byte "#OPL_II#" header + 175 instrument records).
// Sets an internal "ready" flag; if the header is wrong or `len` too small,
// the flag stays false and the player produces silence.
void OPL_Music_SetGenmidi(const unsigned char *lump, int len);

// Returns 1 if a valid GENMIDI lump has been parsed, else 0.
int OPL_Music_Ready(void);

// All notes off + reset per-channel state. Call when starting a new song.
void OPL_Music_Reset(void);

// MIDI program change on `chan` (0..15), program `prog` (0..127).
void OPL_Music_Program(int chan, int prog);

// Note on. `vel` 1..127 (vel <= 0 is treated as note off). `chan` 0..15;
// if `chan == perc_channel` (see SetPercChannel) it is the percussion track.
void OPL_Music_NoteOn(int chan, int note, int vel);

// Note off.
void OPL_Music_NoteOff(int chan, int note);

// MIDI channel volume controller (0..127).
void OPL_Music_ChannelVolume(int chan, int vol);

// Pitch bend. Units: 0..127 (MUS-style 7-bit, centred at 64), matching the
// MSB-only behaviour of Crispy/DMX. (i.e. pass the MUS pitch-wheel byte, or
// the MSB of a 14-bit MIDI bend, directly.)
void OPL_Music_PitchBend(int chan, int bend);

// Set which channel is the percussion channel (default 15, the MUS value).
void OPL_Music_SetPercChannel(int ch);

// Silence everything immediately.
void OPL_Music_AllNotesOff(void);

// Render `frames` interleaved stereo samples (writes 2*frames shorts).
void OPL_Music_Render(short *stereo, int frames);

#endif // I_OPL_H
