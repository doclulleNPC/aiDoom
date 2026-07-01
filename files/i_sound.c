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
//	System interface for sound.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_unix.c,v 1.5 1997/02/03 22:45:10 b1 Exp $";

#include <stdlib.h>
#include <math.h>

#include <SDL3/SDL.h>

#include "z_zone.h"

#include "m_swap.h"
#include "i_system.h"
#include "i_sound.h"
#include "i_mus.h"		// native MUS music playback (OPL-style FM synth)
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"

#include "doomdef.h"

#define NUM_CHANNELS		8

// Mixer configuration
static boolean		use_old_mixer = false;
static SDL_AudioDeviceID	audio_device = 0;
static SDL_AudioStream*	sfx_streams[NUM_CHANNELS] = { NULL };
static SDL_AudioStream*	music_stream = NULL;

// SDL3 audio output: a stream bound to the default playback device, fed by
// I_SDLAudioCallback below.  (SDL3 replaced the SDL1 push/pull-callback API.)
static SDL_AudioStream*	audiostream = NULL;
static Uint8*		mixbuffer = NULL;	// one slice of mixed S16 stereo
static int		mixbufferbytes = 0;


// The number of internal mixing channels,
//  the samples calculated for each mixing step,
//  the size of the 16bit, 2 hardware channel (stereo)
//  mixing buffer, and the samplerate of the raw data.


// Needed for calling the actual sound output.
static int SAMPLECOUNT=		512;

#define SAMPLERATE		11025	// Hz

// The actual lengths of all sound effects.
int		*lengths;		// DSDHacked: grown with S_sfx (dsdhacked.c)
extern int	num_sfx;

// The actual output device.
int	audio_fd;


// The channel step amount...
unsigned int	channelstep[NUM_CHANNELS];
// ... and a 0.16 bit remainder of last step.
unsigned int	channelstepremainder[NUM_CHANNELS];


// The channel data pointers, start and end.
unsigned char*	channels[NUM_CHANNELS];
unsigned char*	channelsend[NUM_CHANNELS];


// Time/gametic that the channel started playing,
//  used to determine oldest, which automatically
//  has lowest priority.
// In case number of active sounds exceeds
//  available channels.
int		channelstart[NUM_CHANNELS];

// The sound in channel handles,
//  determined on registration,
//  might be used to unregister/stop/modify,
//  currently unused.
int 		channelhandles[NUM_CHANNELS];

// SFX id of the playing sound effect.
// Used to catch duplicates (like chainsaw).
int		channelids[NUM_CHANNELS];			

// Pitch to stepping lookup, unused.
int		steptable[256];

// Volume lookups.
int		vol_lookup[128*256];

// Hardware left and right channel volume lookup.
int*		channelleftvol_lookup[NUM_CHANNELS];
int*		channelrightvol_lookup[NUM_CHANNELS];



//
// This function loads the sound data from the WAD lump,
//  for single sound.
//
void*
getsfx
( char*         sfxname,
  int*          len )
{
    unsigned char*      sfx;
    unsigned char*      paddedsfx;
    int                 i;
    int                 size;
    int                 paddedsize;
    char                name[20];
    int                 sfxlump;

    
    // Get the sound data from the WAD, allocate lump
    //  in zone memory.
    extern int heretic_mode;
    sprintf(name, "ds%s", sfxname);

    // Now, there is a severe problem with the
    //  sound handling, in it is not (yet/anymore)
    //  gamemode aware. That means, sounds from
    //  DOOM II will be requested even with DOOM
    //  shareware.
    // The sound list is wired into sounds.c,
    //  which sets the external variable.
    // I do not do runtime patches to that
    //  variable. Instead, we will use a
    //  default sound for replacement.
    if ( heretic_mode && W_CheckNumForName(sfxname) != -1 )
      sfxlump = W_GetNumForName(sfxname);		// native Heretic sound (no "ds" prefix)
    else if ( W_CheckNumForName(name) != -1 )
      sfxlump = W_GetNumForName(name);
    else if ( W_CheckNumForName("dspistol") != -1 )
      sfxlump = W_GetNumForName("dspistol");		// DOOM default replacement
    else
      sfxlump = -1;	// no DOOM SFX at all (e.g. heretic.wad) -> emit silence below

    // Heretic-mode / missing-SFX safety: hand back a tiny padded silence buffer
    // instead of W_GetNumForName I_Erroring when neither the named lump nor the
    // dspistol fallback exists.  (Heretic SFX use different lump names; the proper
    // Heretic sound table is a later phase.)
    if (sfxlump < 0)
    {
	int		silbytes = SAMPLECOUNT;
	unsigned char*	sil = (unsigned char*)Z_Malloc (silbytes+8, PU_STATIC, 0);
	for (i=0 ; i<silbytes+8 ; i++)
	    sil[i] = 128;
	*len = silbytes;
	return (void *) (sil + 8);
    }

    size = W_LumpLength( sfxlump );

    // Debug.
    // fprintf( stderr, "." );
    //fprintf( stderr, " -loading  %s (lump %d, %d bytes)\n",
    //	     sfxname, sfxlump, size );
    //fflush( stderr );
    
    sfx = (unsigned char*)W_CacheLumpNum( sfxlump, PU_STATIC );

    // Pads the sound effect out to the mixing buffer size.
    // The original realloc would interfere with zone memory.
    paddedsize = ((size-8 + (SAMPLECOUNT-1)) / SAMPLECOUNT) * SAMPLECOUNT;

    // Allocate from zone memory.
    paddedsfx = (unsigned char*)Z_Malloc( paddedsize+8, PU_STATIC, 0 );
    // ddt: (unsigned char *) realloc(sfx, paddedsize+8);
    // This should interfere with zone memory handling,
    //  which does not kick in in the soundserver.

    // Now copy and pad.
    memcpy(  paddedsfx, sfx, size );
    for (i=size ; i<paddedsize+8 ; i++)
        paddedsfx[i] = 128;

    // Remove the cached lump.
    Z_Free( sfx );
    
    // Preserve padded length.
    *len = paddedsize;

    // Return allocated padded data.
    return (void *) (paddedsfx + 8);
}





//
// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
//
int
addsfx
( int		sfxid,
  int		volume,
  int		step,
  int		seperation )
{
    static unsigned short	handlenums = 0;
 
    int		i;
    int		rc = -1;
    
    int		oldest = gametic;
    int		oldestnum = 0;
    int		slot;

    int		rightvol;
    int		leftvol;

    // Sweep and clear finished streams
    if (!use_old_mixer) {
        for (i = 0; i < NUM_CHANNELS; i++) {
            if (channels[i] && sfx_streams[i] && SDL_GetAudioStreamQueued(sfx_streams[i]) == 0) {
                channels[i] = 0;
                channelhandles[i] = 0;
            }
        }
    }

    // Chainsaw troubles.
    // Play these sound effects only one at a time.
    if ( sfxid == sfx_sawup
	 || sfxid == sfx_sawidl
	 || sfxid == sfx_sawful
	 || sfxid == sfx_sawhit
	 || sfxid == sfx_stnmov
	 || sfxid == sfx_pistol	 )
    {
	// Loop all channels, check.
	for (i=0 ; i<NUM_CHANNELS ; i++)
	{
	    // Active, and using the same SFX?
	    if ( (channels[i])
		 && (channelids[i] == sfxid) )
	    {
		// Reset.
		channels[i] = 0;
		// We are sure that iff,
		//  there will only be one.
		break;
	    }
	}
    }

    // Loop all channels to find oldest SFX.
    for (i=0; (i<NUM_CHANNELS) && (channels[i]); i++)
    {
	if (channelstart[i] < oldest)
	{
	    oldestnum = i;
	    oldest = channelstart[i];
	}
    }

    // Tales from the cryptic.
    // If we found a channel, fine.
    // If not, we simply overwrite the first one, 0.
    // Probably only happens at startup.
    if (i == NUM_CHANNELS)
	slot = oldestnum;
    else
	slot = i;

    // Okay, in the less recent channel,
    //  we will handle the new SFX.
    // Set pointer to raw data.
    channels[slot] = (unsigned char *) S_sfx[sfxid].data;
    // Set pointer to end of raw data.
    channelsend[slot] = channels[slot] + lengths[sfxid];

    // Reset current handle number, limited to 0..100.
    if (!handlenums)
	handlenums = 100;

    // Assign current handle number.
    // Preserved so sounds could be stopped (unused).
    channelhandles[slot] = rc = handlenums++;

    // Set stepping???
    // Kinda getting the impression this is never used.
    channelstep[slot] = step;
    // ???
    channelstepremainder[slot] = 0;
    // Should be gametic, I presume.
    channelstart[slot] = gametic;

    // Separation, that is, orientation/stereo.
    //  range is: 1 - 256
    seperation += 1;

    // Per left/right channel.
    //  x^2 seperation,
    //  adjust volume properly.
    volume *= 8;
    leftvol =
	volume - ((volume*seperation*seperation) >> 16); ///(256*256);
    seperation = seperation - 257;
    rightvol =
	volume - ((volume*seperation*seperation) >> 16);	

    // Sanity check, clamp volume.
    if (rightvol < 0 || rightvol > 127)
	I_Error("rightvol out of bounds");
    
    if (leftvol < 0 || leftvol > 127)
	I_Error("leftvol out of bounds");
    
    // Get the proper lookup table piece
    //  for this volume level???
    channelleftvol_lookup[slot] = &vol_lookup[leftvol*256];
    channelrightvol_lookup[slot] = &vol_lookup[rightvol*256];

    // Preserve sound SFX id,
    //  e.g. for avoiding duplicates of chainsaw.
    channelids[slot] = sfxid;

    // You tell me.
    return rc;
}





//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// See soundserver initdata().
//
void I_SetChannels()
{
  // Init internal lookups (raw data, mixing buffer, channels).
  // This function sets up internal lookups used during
  //  the mixing process. 
  int		i;
  int		j;
    
  int*	steptablemid = steptable + 128;
  
  // Okay, reset internal mixing channels to zero.
  /*for (i=0; i<NUM_CHANNELS; i++)
  {
    channels[i] = 0;
  }*/

  // This table provides step widths for pitch parameters.
  // I fail to see that this is currently used.
  for (i=-128 ; i<128 ; i++)
    steptablemid[i] = (int)(pow(2.0, (i/64.0))*65536.0);
  
  
  // Generates volume lookup tables
  //  which also turn the unsigned samples
  //  into signed samples.
  for (i=0 ; i<128 ; i++)
    for (j=0 ; j<256 ; j++) {
      vol_lookup[i*256+j] = (i*(j-128)*256)/127;
//fprintf(stderr, "vol_lookup[%d*256+%d] = %d\n", i, j, vol_lookup[i*256+j]);
    }
}	

 
void I_SetSfxVolume(int volume)
{
  // Identical to DOS.
  // Basically, this should propagate
  //  the menu/config file setting
  //  to the state variable used in
  //  the mixing.
  snd_SfxVolume = volume;
}

// MUSIC API.  Music gain 0..15 (snd_MusicVolume range); read on the audio thread.
int	i_music_gain = 15;

void I_SetMusicVolume(int volume)
{
  snd_MusicVolume = volume;
  i_music_gain = volume;		// 0..15
}


//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    extern int heretic_mode;
    char namebuf[9];
    int  l;
    // Heretic sounds live in heretic.wad under their NATIVE names (IMPSIT, MUMSIT, ...) with
    // no "ds" prefix.  In heretic_mode try the bare name first.
    if (heretic_mode && (l = W_CheckNumForName ((char*)sfx->name)) >= 0)
	return l;
    sprintf(namebuf, "ds%s", sfx->name);
    l = W_CheckNumForName (namebuf);
    return (l >= 0) ? l : -1;	// missing (e.g. a DOOM sfx under heretic.wad) -> -1, never I_Error
				//   (lumpnum isn't used to index a lump; I_GetSfx emits silence)
}

//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
int
I_StartSound
( int		id,
  int		vol,
  int		sep,
  int		pitch,
  int		priority )
{
    priority = 0;

    if (use_old_mixer)
    {
        if (audiostream) SDL_LockAudioStream(audiostream);
        id = addsfx( id, vol, steptable[pitch], sep );
        if (audiostream) SDL_UnlockAudioStream(audiostream);
        return id;
    }
    else
    {
        int handle = addsfx( id, vol, steptable[pitch], sep );
        if (handle < 0)
            return -1;

        int slot = -1;
        int i;
        for (i = 0; i < NUM_CHANNELS; i++) {
            if (channelhandles[i] == handle) {
                slot = i;
                break;
            }
        }

        if (slot >= 0 && sfx_streams[slot])
        {
            SDL_ClearAudioStream(sfx_streams[slot]);
            SDL_SetAudioStreamFrequencyRatio(sfx_streams[slot], (float)steptable[pitch] / 65536.0f);

            int len = lengths[id];
            short* stereo_buf = malloc(len * 2 * sizeof(short));
            if (stereo_buf)
            {
                unsigned char* raw = (unsigned char*) S_sfx[id].data;
                for (i = 0; i < len; i++) {
                    int sample = raw[i];
                    stereo_buf[i*2] = (short)channelleftvol_lookup[slot][sample];
                    stereo_buf[i*2+1] = (short)channelrightvol_lookup[slot][sample];
                }
                SDL_PutAudioStreamData(sfx_streams[slot], stereo_buf, len * 2 * sizeof(short));
                SDL_FlushAudioStream(sfx_streams[slot]);
                free(stereo_buf);
            }
        }
        return handle;
    }
}


void I_StopSound (int handle)
{
    if (use_old_mixer)
    {
        handle = 0;
        return;
    }

    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (sfx_streams[i] && channelhandles[i] == handle) {
            SDL_ClearAudioStream(sfx_streams[i]);
            channelhandles[i] = 0;
            channels[i] = 0;
            break;
        }
    }
}


int I_SoundIsPlaying(int handle)
{
    if (use_old_mixer)
    {
        return gametic < handle;
    }

    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (sfx_streams[i] && channelhandles[i] == handle) {
            return (SDL_GetAudioStreamQueued(sfx_streams[i]) > 0);
        }
    }
    return false;
}


//
// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the given
//  mixing buffer, and clamping it to the allowed
//  range.
//
// This function currently supports only 16bit.
//
void I_UpdateSound(void *unused, Uint8 *stream, int len)
{
  // Mix current sound data.
  // Data, from raw sound, for right and left.
  register unsigned int	sample;
  register int		dl;
  register int		dr;
  
  // Pointers in audio stream, left, right, end.
  signed short*		leftout;
  signed short*		rightout;
  signed short*		leftend;
  // Step in stream, left and right, thus two.
  int				step;

  // Mixing channel index.
  int				chan;
    
    // Left and right channel
    //  are in audio stream, alternating.
    leftout = (signed short *)stream;
    rightout = ((signed short *)stream)+1;
    step = 2;

    // Determine end, for left channel only
    //  (right channel is implicit).
    leftend = leftout + SAMPLECOUNT*step;

    // Mix sounds into the mixing buffer.
    // Loop over step*SAMPLECOUNT,
    //  that is 512 values for two channels.
    while (leftout != leftend)
    {
	// Reset left/right value. 
	dl = 0;
	dr = 0;

	// Love thy L2 chache - made this a loop.
	// Now more channels could be set at compile time
	//  as well. Thus loop those  channels.
	for ( chan = 0; chan < NUM_CHANNELS; chan++ )
	{
	    // Check channel, if active.
	    if (channels[ chan ])
	    {
		// Get the raw data from the channel. 
		sample = *channels[ chan ];
		// Add left and right part
		//  for this channel (sound)
		//  to the current data.
		// Adjust volume accordingly.
		dl += channelleftvol_lookup[ chan ][sample];
		dr += channelrightvol_lookup[ chan ][sample];
		// Increment index ???
		channelstepremainder[ chan ] += channelstep[ chan ];
		// MSB is next sample???
		channels[ chan ] += channelstepremainder[ chan ] >> 16;
		// Limit to LSB???
		channelstepremainder[ chan ] &= 65536-1;

		// Check whether we are done.
		if (channels[ chan ] >= channelsend[ chan ])
		    channels[ chan ] = 0;
	    }
	}
	
	// Clamp to range. Left hardware channel.
	// Has been char instead of short.
	// if (dl > 127) *leftout = 127;
	// else if (dl < -128) *leftout = -128;
	// else *leftout = dl;

	if (dl > 0x7fff)
	    *leftout = 0x7fff;
	else if (dl < -0x8000)
	    *leftout = -0x8000;
	else
	    *leftout = dl;

	// Same for right hardware channel.
	if (dr > 0x7fff)
	    *rightout = 0x7fff;
	else if (dr < -0x8000)
	    *rightout = -0x8000;
	else
	    *rightout = dr;

	// Increment current pointers in stream
	leftout += step;
	rightout += step;
    }
}

void
I_UpdateSoundParams
( int	handle,
  int	vol,
  int	sep,
  int	pitch)
{
    if (use_old_mixer)
    {
        handle = vol = sep = pitch = 0;
        return;
    }

    int i;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (sfx_streams[i] && channelhandles[i] == handle) {
            float gain = vol / 15.0f;
            SDL_SetAudioStreamGain(sfx_streams[i], gain);
            SDL_SetAudioStreamFrequencyRatio(sfx_streams[i], (float)steptable[pitch] / 65536.0f);
            break;
        }
    }
}


//
// SDL3 audio stream callback: SDL asks for more bytes; we mix one slice at a
// time (I_UpdateSound always produces exactly mixbufferbytes) and push it.
//
static void I_MixMusic (Uint8* buf, int bytes);	// mix the synth into the SFX slice (below)

static void SDLCALL
I_SDLAudioCallback (void *userdata, SDL_AudioStream *stream,
		    int additional_amount, int total_amount)
{
    (void)userdata; (void)total_amount;
    while (additional_amount > 0)
    {
	I_UpdateSound(NULL, mixbuffer, mixbufferbytes);
	I_MixMusic(mixbuffer, mixbufferbytes);	// add music on top of the SFX mix
	SDL_PutAudioStreamData(stream, mixbuffer, mixbufferbytes);
	additional_amount -= mixbufferbytes;
    }
}


static void SDLCALL I_SDLMusicCallback(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount);

void I_ShutdownSound(void)
{
    if (use_old_mixer)
    {
        if (audiostream) { SDL_DestroyAudioStream(audiostream); audiostream = NULL; }
        if (mixbuffer)   { free(mixbuffer); mixbuffer = NULL; }
        return;
    }

    int i;
    for (i = 0; i < NUM_CHANNELS; i++)
    {
        if (sfx_streams[i]) {
            SDL_DestroyAudioStream(sfx_streams[i]);
            sfx_streams[i] = NULL;
        }
    }

    if (music_stream) {
        SDL_DestroyAudioStream(music_stream);
        music_stream = NULL;
    }

    if (audio_device) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
}


void
I_InitSound()
{ 
  SDL_AudioSpec wanted;
  int i;
  
  // Secure and configure sound device first.
  fprintf( stderr, "I_InitSound: ");

  use_old_mixer = (M_CheckParm("-oldmixer") != 0);
  
  if (use_old_mixer)
  {
      // Open the audio device (SDL3: signed 16-bit native-endian stereo).
      SDL_zero(wanted);
      wanted.freq = SAMPLERATE;
      wanted.format = SDL_AUDIO_S16;
      wanted.channels = 2;

      // One mixed slice = SAMPLECOUNT stereo S16 frames.
      mixbufferbytes = SAMPLECOUNT * wanted.channels * (int)sizeof(Sint16);
      mixbuffer = (Uint8 *) malloc(mixbufferbytes);
      if ( mixbuffer == NULL ) {
        fprintf(stderr, "couldn't allocate audio mixing buffer\n");
        return;
      }

      audiostream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                              &wanted, I_SDLAudioCallback, NULL);
      if ( audiostream == NULL ) {
        fprintf(stderr, "couldn't open audio: %s\n", SDL_GetError());
        free(mixbuffer); mixbuffer = NULL;
        return;
      }
      fprintf(stderr, " configured audio device with %d samples/slice (old mixer)\n", SAMPLECOUNT);
  }
  else
  {
      audio_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
      if (audio_device == 0) {
          fprintf(stderr, "couldn't open audio device: %s\n", SDL_GetError());
          return;
      }

      SDL_AudioSpec src_spec;
      src_spec.freq = SAMPLERATE; // 11025
      src_spec.format = SDL_AUDIO_S16;
      src_spec.channels = 2; // stereo

      for (i = 0; i < NUM_CHANNELS; i++)
      {
          sfx_streams[i] = SDL_CreateAudioStream(&src_spec, NULL);
          if (!sfx_streams[i]) {
              fprintf(stderr, "couldn't create sfx stream %d: %s\n", i, SDL_GetError());
          } else {
              SDL_BindAudioStream(audio_device, sfx_streams[i]);
          }
      }

      SDL_AudioSpec mus_src_spec;
      mus_src_spec.freq = SAMPLERATE; // 11025
      mus_src_spec.format = SDL_AUDIO_S16;
      mus_src_spec.channels = 2; // stereo

      music_stream = SDL_CreateAudioStream(&mus_src_spec, NULL);
      if (music_stream)
      {
          SDL_BindAudioStream(audio_device, music_stream);
          SDL_SetAudioStreamGetCallback(music_stream, I_SDLMusicCallback, NULL);
      }

      SDL_ResumeAudioDevice(audio_device);
      fprintf(stderr, " configured audio device (new multi-stream mixer)\n");
  }

  // Initialize external data (all sounds) at start, keep static.
  fprintf( stderr, "I_InitSound: ");
  
  if (!lengths) lengths = calloc(num_sfx, sizeof(int));
  for (i=1 ; i<num_sfx ; i++)
  { 
    // DSDHacked gap slots (grown but unnamed) have no name -> nothing to load.
    if (!S_sfx[i].name || !S_sfx[i].name[0])
    { S_sfx[i].data = NULL; lengths[i] = 0; continue; }
    // Alias? Example is the chaingun sound linked to pistol.  DeHackEd stores a link as a raw
    // small-int "pointer" (deh_procSounds), so only follow it when it actually points into the
    // sound table; otherwise treat it as a normal (unlinked) sound and load by name.
    if (S_sfx[i].link >= S_sfx && S_sfx[i].link < S_sfx + num_sfx)
    {
      S_sfx[i].data = S_sfx[i].link->data;
      lengths[i] = lengths[S_sfx[i].link - S_sfx];
    }
    else
    {
      // Load data from WAD file.
      S_sfx[i].data = getsfx( S_sfx[i].name, &lengths[i] );
    }
  }

  fprintf( stderr, " pre-cached all sound data\n");
  
  // Finished initialization.
  fprintf(stderr, "I_InitSound: sound module ready\n");
  if (use_old_mixer)
      SDL_ResumeAudioStreamDevice(audiostream);
}




//
// MUSIC API -- native MUS playback via the OPL-style FM synth in i_mus.c.
//
// DOOM's music lumps are MUS (DMX), rendered by the GENMIDI-configured FM synth
// in i_mus.c at SAMPLERATE (11025 Hz, S16 stereo).  We pull that PCM through a
// second SDL audio stream bound to the same device as the SFX, so SDL mixes it.
// A non-MUS lump (e.g. raw MIDI) registers as "no music" and is skipped.
//
static int		mus_kind;	// 0 none, 2 MUS (synth)
static int		mus_paused;
static int		mus_geninit;	// MUS_Init() done?

void I_InitMusic(void)		{ }
void I_ShutdownMusic(void)	{ }

// Render the FM synth and ADD it on top of the already-SFX-mixed slice `buf`.  Called
// from I_SDLAudioCallback -- the device callback that actually fires.  (The previous
// design bound a separate music stream with its own get-callback, but SDL never pulled
// it, so no music ever played; mixing into the SFX slice here is reliable.)
static void I_MixMusic (Uint8* buf, int bytes)
{
    short	mus[2048];		// >= one SFX slice (SAMPLECOUNT*2 = 1024 shorts at 512)
    short*	mb = (short*)buf;
    int		n  = bytes / (int)sizeof(short);	// total interleaved samples
    float	gain;
    int		i;

    if (mus_kind != 2 || mus_paused) return;
    if (n > (int)(sizeof(mus)/sizeof(mus[0]))) n = (int)(sizeof(mus)/sizeof(mus[0]));
    MUS_Render (mus, n/2);				// n/2 stereo frames
    gain = i_music_gain / 15.0f;
    for (i = 0; i < n; i++)
    {
	int v = mb[i] + (int)(mus[i] * gain);
	mb[i] = (short)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
    }
}

int I_RegisterSong(void* data, int length)
{
    mus_kind = 0;
    if (!mus_geninit) { MUS_Init (); mus_geninit = 1; }	// load GENMIDI once
    if (MUS_Register (data, length)) { mus_kind = 2; return 2; }	// MUS or raw MIDI
    return 0;	// unrecognised lump (or GENMIDI missing) -> silently no music
}

void I_PlaySong(int handle, int looping)
{
    if (!handle || mus_kind != 2)
	return;
    MUS_Start (looping);		// I_MixMusic now renders it into the SFX slice
    mus_paused = 0;
}

void I_PauseSong (int handle)	{ (void)handle; mus_paused = 1; }
void I_ResumeSong (int handle)	{ (void)handle; mus_paused = 0; }

void I_StopSong(int handle)
{
    (void)handle;
    if (mus_kind == 2)
	MUS_Stop ();
}

void I_UnRegisterSong(int handle)
{
    (void)handle;
    if (mus_kind == 2)
	MUS_Stop ();
    mus_kind = 0;
}

// Is the song playing?
int I_QrySongPlaying(int handle)
{
    (void)handle;
    return mus_kind != 0;
}


static void SDLCALL
I_SDLMusicCallback (void *userdata, SDL_AudioStream *stream,
		    int additional_amount, int total_amount)
{
    (void)userdata; (void)total_amount;
    short temp_buf[2048];
    while (additional_amount > 0)
    {
	int chunk = additional_amount;
	if (chunk > (int)sizeof(temp_buf)) chunk = sizeof(temp_buf);
	int frames = chunk / (2 * (int)sizeof(short)); // S16 stereo -> 4 bytes per frame
	
	if (mus_kind == 2 && !mus_paused)
	{
	    MUS_Render(temp_buf, frames);
	    // Apply volume gain (0..15)
	    float gain = i_music_gain / 15.0f;
	    if (gain != 1.0f)
	    {
		int i;
		for (i = 0; i < frames * 2; i++)
		    temp_buf[i] = (short)(temp_buf[i] * gain);
	    }
	    SDL_PutAudioStreamData(stream, temp_buf, frames * 2 * (int)sizeof(short));
	}
	else
	{
	    // If no music, put silence
	    memset(temp_buf, 0, frames * 2 * (int)sizeof(short));
	    SDL_PutAudioStreamData(stream, temp_buf, frames * 2 * (int)sizeof(short));
	}
	additional_amount -= frames * 2 * (int)sizeof(short);
    }
}

