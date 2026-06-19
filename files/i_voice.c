// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available under the terms of the DOOM Source Code License
// as published by id Software. All rights reserved.
//
// DESCRIPTION:
//   Offline voice playback for the AI co-op buddy.  See i_voice.h.
//
//   Pipeline:
//     p_ai_coop.c::AICoop_Say(tag)   (replaces the old buddy_say.txt writer)
//        -> I_Voice_Say(tag)
//           -> I_Voice_SayByName(lumpname)
//              -> lump cache lookup
//              -> SDL_AudioStream (separate from i_sound.c's SFX mixer)
//              -> stb_vorbis decodes OGG -> PCM frames -> SDL_PutAudioStreamData
//
//   Why a separate stream: see i_voice.h.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <SDL3/SDL.h>

// Single-header Vorbis decoder.  build.sh compiles stb_vorbis.c on its own (the
// *.c glob), so that object IS the implementation -- here we pull in only the
// API declarations (STB_VORBIS_HEADER_ONLY), otherwise the symbols are defined
// twice and the link fails with "multiple definition of stb_vorbis_*".
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
// stb_vorbis.c #define's `L` to `(PLAYBACK_LEFT | PLAYBACK_MONO)` near the end
// of its implementation section.  That clobbers every identifier named `L`
// in the rest of this translation unit -- e.g. `int L = strlen(p);` becomes
// `int (PLAYBACK_LEFT | PLAYBACK_MONO) = strlen(p);` and cl.exe dies with
// C2059 "Syntaxfehler: Konstante".  Undefine it here.
#ifdef L
#undef L
#endif

#include "i_voice.h"
#include "w_wad.h"
#include "z_zone.h"        // PU_STATIC, Z_Free
#include "m_misc.h"        // ConfigName etc.
#include "doomdef.h"

// W_AddFile lives in w_wad.c but isn't in w_wad.h (it's only called from
// D_DoomMain at startup).  Forward-declare so we can add buddy.wad.
extern void   W_AddFile (char *filename);
// w_wad.c internals: lumpcache is malloc'd ONCE in W_InitMultipleFiles, sized to
// numlumps at that moment.  Adding buddy.wad later grows numlumps but NOT
// lumpcache, so we must grow it ourselves (see I_Voice_Init) -- otherwise
// W_CacheLumpNum on a buddy lump writes past the array and corrupts the heap.
extern void** lumpcache;
extern int    numlumps;


// ----- lump-name -> "tag" mapping (must match tools/bake_buddy_voice.py) ---

// Read a value for "key" from aidoom.cfg (working dir).  Local copy of the
// pattern used in d_main.c::IWAD_CfgGet -- we don't want to expose that as
// public API for a single caller.  Returns 1 if found, copies into `out`.
//
// Note: parameter types are written without `const` because the MSVC C
// compiler (cl.exe, /Ze legacy mode used by the Windows build) sometimes
// sees the keyword via transitive SDK headers as a macro and chokes on
// it.  WAD APIs and SDL3 want plain `char*` anyway.
static int Buddy_CfgGet (char* key, char* out, int n)
{
    FILE* f = fopen ("aidoom.cfg", "r");
    char  line[256], k[64], v[192];
    int   found = 0;
    if (!f) return 0;
    while (fgets (line, sizeof(line), f))
    {
        if (sscanf (line, " %63s %191[^\n]", k, v) == 2 && !strcmp (k, key))
        {
            char* p = v;
            int   L = (int)strlen (p);
            if (L >= 2 && p[0] == '"' && p[L-1] == '"') { p[L-1] = 0; p++; }
            strncpy (out, p, n-1); out[n-1] = 0; found = 1;
        }
    }
    fclose (f);
    return found;
}
// The buddy speaks by *tag* from the playsim, never by lump name directly.
// This table maps each tag to the 8-char lump name baked into buddy.wad.
// Adding a new spoken phrase means (1) add an entry to bake_buddy_voice.py's
// PHRASES list, (2) add a row here.  Keep them in sync.

typedef struct voicemap_s
{
    const char* tag;        // playsim-facing name, e.g. "contact" / "state:fighting"
    const char* lumpname;   // 8-char Doom lump name
} voicemap_t;

static const voicemap_t VOICE_MAP[] =
{
    // Auto-callouts (rotated by AICoop_Callout in p_ai_coop.c).  Lump names
    // are exactly 8 chars (Doom WAD spec) and must match tools/bake_buddy_voice.py.
    { "contact:0",       "DSCT001" },
    { "contact:1",       "DSCT002" },
    { "contact:2",       "DSCT003" },
    { "contact:3",       "DSCT004" },
    { "hurt:0",          "DSHR01"  },
    { "hurt:1",          "DSHR02"  },
    { "hurt:2",          "DSHR03"  },
    { "clear:0",         "DSCL01" },
    { "clear:1",         "DSCL02" },
    { "clear:2",         "DSCL03" },
    // State (where-report; HP/distance spoken by HUD, voice just carries state)
    { "state:following", "DSWFOLLOW" },
    { "state:fighting",  "DSWFIGHT"  },
    { "state:healing",   "DSWHEAL"   },
    { "state:holding",   "DSWHOLD"   },
    { "state:coming",    "DSWCOME"   },
    { "state:grabbing",  "DSWGRAB"   },
    // Console replies
    { "summon_ok",       "DSSUMONOK" },
    { "wait_hold",       "DSWAITHD"  },
    { "wait_move",       "DSWAITMV"  },
    { "attack_ok",       "DSATACK"   },
    { "attack_none",     "DSATNONE"  },
    // Status-report weapon names (plain / with-ammo variants).  Lump names use
    // 3-char weapon codes (DSST + 3 + P|A = 8 chars exactly).
    { "status:fists",          "DSSTFISP" },
    { "status:pistol",         "DSSTPISP" },
    { "status:pistol:ammo",    "DSSTPISA" },
    { "status:shotgun",        "DSSTSHTP" },
    { "status:shotgun:ammo",   "DSSTSHTA" },
    { "status:chaingun",       "DSSTCHGP" },
    { "status:chaingun:ammo",  "DSSTCHGA" },
    { "status:rocketlauncher", "DSSTRCKP" },
    { "status:rocketlauncher:ammo", "DSSTRCKA" },
    { "status:plasma",         "DSSTPLSP" },
    { "status:plasma:ammo",    "DSSTPLSA" },
    { "status:bfg",            "DSSTBFGP"  },
    { "status:bfg:ammo",       "DSSTBFGA"  },
    { "status:chainsaw",       "DSSTCSWP"  },
    { "status:supershotgun",   "DSSTSSHP"  },
    { "status:supershotgun:ammo", "DSSTSSHA" },
};
#define NUM_VOICE_MAP ((int)(sizeof(VOICE_MAP)/sizeof(VOICE_MAP[0])))


// ----- lump cache (all ds* lumps from buddy.wad, decoded once on demand) --

typedef struct voicecache_s
{
    char            lumpname[9];   // NUL-terminated 8-char name
    Uint8*          data;          // raw OGG bytes (owned by us, malloc'd)
    int             size;
    int             samplerate;    // Hz; 0 if not yet decoded
    int             channels;      // 1=mono, 2=stereo
    short*          pcm;           // interleaved S16, NULL if not yet decoded
    int             pcmsamples;    // total interleaved samples
    struct voicecache_s* next;
} voicecache_t;

static voicecache_t* cache_head = NULL;


// ----- the dedicated audio stream ------------------------------------------

static SDL_AudioStream* voice_stream = NULL;

static void SDLCALL I_VoiceAudioCallback (void* userdata, SDL_AudioStream* stream,
                                          int additional_amount, int total_amount)
{
    // We pre-decode each lump to PCM at first play and feed SDL in chunks;
    // this callback is just here so SDL knows the stream is alive.  All real
    // playback happens synchronously in I_Voice_SayByName -> SDL_PutAudioStreamData.
    (void)userdata; (void)stream; (void)additional_amount; (void)total_amount;
}


// ----- helpers -------------------------------------------------------------

static voicecache_t* find_cache (const char* lumpname)
{
    for (voicecache_t* v = cache_head; v; v = v->next)
        if (!strncmp(v->lumpname, lumpname, 8)) return v;
    return NULL;
}

static voicecache_t* load_lump (const char* lumpname)
{
    // Try the WAD first (buddy.wad as added by D_DoomMain).
    // W_CheckNumForName takes char* (legacy API), so copy the const name.
    char namebuf[9];
    strncpy (namebuf, lumpname, 8); namebuf[8] = '\0';
    int lump = W_CheckNumForName (namebuf);
    if (lump < 0) return NULL;

    voicecache_t* v = (voicecache_t*)malloc (sizeof(*v));
    if (!v) return NULL;
    memset (v, 0, sizeof(*v));
    strncpy (v->lumpname, lumpname, 8); v->lumpname[8] = '\0';

    // Copy the raw OGG bytes out of the WAD cache so we can release the lump.
    v->size  = W_LumpLength (lump);
    v->data  = (Uint8*)malloc (v->size);
    if (!v->data) { free (v); return NULL; }
    memcpy (v->data, W_CacheLumpNum (lump, PU_STATIC), v->size);
    Z_Free (W_CacheLumpNum (lump, PU_STATIC));  // release the WAD copy

    // Decode now so playback is instant later.  Cheap (lumps are ~5-30 KB each,
    // decoded once at startup).
    int n   = stb_vorbis_decode_memory (v->data, v->size,
                                        &v->channels, &v->samplerate, &v->pcm);
    if (n <= 0 || !v->pcm)
    {
        fprintf (stderr, "I_Voice: failed to decode OGG lump '%s'\n", lumpname);
        Z_Free (v->data);
        free (v);
        return NULL;
    }
    v->pcmsamples = n;

    v->next  = cache_head;
    cache_head = v;
    return v;
}


static const char* tag_to_lumpname (const char* tag)
{
    if (!tag) return NULL;
    for (int i = 0; i < NUM_VOICE_MAP; i++)
        if (!strcmp (VOICE_MAP[i].tag, tag)) return VOICE_MAP[i].lumpname;
    return NULL;
}


static SDL_AudioSpec make_spec (int rate, int channels)
{
    SDL_AudioSpec s;
    SDL_zero (s);
    s.format   = SDL_AUDIO_S16;
    s.freq     = rate;
    s.channels = (Uint8)channels;
    return s;
}


// ----- public API ----------------------------------------------------------

static short clamp16 (int v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (short)v;
}

// lvol/rvol are 0..127 per-channel gains (Doom's positional volume) so the
// buddy's voice comes from *its* spot in the world -- distance attenuation +
// stereo pan computed by the caller (p_ai_coop) relative to the listener.
void I_Voice_SayByName (const char* lumpname, int lvol, int rvol)
{
    if (!voice_stream || !lumpname) return; // init failed/skipped or no name
    if (lvol <= 0 && rvol <= 0)    return;  // inaudible (buddy too far) -> skip

    voicecache_t* v = find_cache (lumpname);
    if (!v) v = load_lump (lumpname);
    if (!v || !v->pcm) return;              // lump missing or decode failed

    // Bind the stream as STEREO once (so we can pan); SDL resamples to the device.
    static int bound = 0;
    if (!bound)
    {
        SDL_AudioSpec spec = make_spec (v->samplerate, 2);
        if (!SDL_SetAudioStreamFormat (voice_stream, &spec, NULL))
        {
            fprintf (stderr, "I_Voice: SDL_SetAudioStreamFormat: %s\n", SDL_GetError());
            return;
        }
        bound = 1;
    }

    // Render to a temporary interleaved-stereo buffer with the per-channel gains
    // (the cached PCM is shared across plays, so we must not scale it in place).
    int     frames = v->pcmsamples;         // samples per channel from stb decode
    short*  out    = (short*)malloc ((size_t)frames * 2 * sizeof(short));
    if (!out) return;
    for (int i = 0; i < frames; i++)
    {
        int sl = (v->channels == 1) ? v->pcm[i] : v->pcm[2*i];
        int sr = (v->channels == 1) ? v->pcm[i] : v->pcm[2*i+1];
        out[2*i]   = clamp16 (sl * lvol / 127);
        out[2*i+1] = clamp16 (sr * rvol / 127);
    }
    SDL_PutAudioStreamData (voice_stream, out, frames * 2 * (int)sizeof(short));
    free (out);
}

void I_Voice_Say (const char* tag, int lvol, int rvol)
{
    const char* lumpname = tag_to_lumpname (tag);
    if (!lumpname) return;                 // unknown tag -> silent
    I_Voice_SayByName (lumpname, lvol, rvol);
}


void I_Voice_Init (void)
{
    // Resolve the buddy WAD path: command-line -file takes precedence (via the
    // existing W_AddFile mechanism in D_DoomMain), else aidoom.cfg `buddy_wad`
    // entry, else "buddy.wad" in CWD.
    char wadpath[256];
    if (!Buddy_CfgGet ("buddy_wad", wadpath, sizeof(wadpath)) || !*wadpath)
        strncpy (wadpath, "buddy.wad", sizeof(wadpath));

    // Try to add it; detect success via "is any DS* lump now known?".
    int oldnumlumps = numlumps;
    W_AddFile (wadpath);
    // Grow lumpcache to cover the lumps buddy.wad just added (W_AddFile doesn't),
    // and zero the new slots so W_CacheLumpNum sees them as not-yet-cached.
    if (numlumps > oldnumlumps && lumpcache)
    {
        lumpcache = (void**)realloc (lumpcache, numlumps * sizeof(*lumpcache));
        memset (lumpcache + oldnumlumps, 0, (numlumps - oldnumlumps) * sizeof(*lumpcache));
    }
    int probe = W_CheckNumForName ("DSCT001");
    if (probe < 0)
    {
        fprintf (stderr, "I_Voice: no buddy voice WAD at '%s' (silent)\n", wadpath);
    }
    else
    {
        fprintf (stderr, "I_Voice: loaded buddy voice WAD '%s'\n", wadpath);
    }

    // Open the dedicated audio stream.  SDL will resample to whatever the
    // device wants; we bind the format lazily in I_Voice_SayByName when the
    // first OGG tells us its rate.
    voice_stream = SDL_OpenAudioDeviceStream (
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL,
        I_VoiceAudioCallback, NULL);
    if (!voice_stream)
    {
        fprintf (stderr, "I_Voice: SDL_OpenAudioDeviceStream: %s -- voice disabled\n",
                 SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice (voice_stream);
}


void I_Voice_Shutdown (void)
{
    if (voice_stream) { SDL_DestroyAudioStream (voice_stream); voice_stream = NULL; }
    while (cache_head)
    {
        voicecache_t* v = cache_head;
        cache_head = v->next;
        if (v->pcm)  free (v->pcm);    // allocated by stb_vorbis with malloc
        free (v->data);
        free (v);
    }
}