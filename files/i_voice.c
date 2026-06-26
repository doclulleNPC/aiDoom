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
// D_DoomMain at startup).  Forward-declare so we can add aidoom.wad.
extern void   W_AddFile (char *filename);
// w_wad.c internals: lumpcache is malloc'd ONCE in W_InitMultipleFiles, sized to
// numlumps at that moment.  Adding aidoom.wad later grows numlumps but NOT
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
// This table maps each tag to the 8-char lump name baked into aidoom.wad.
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
    { "state:following", "DSWFOLLO" },
    { "state:fighting",  "DSWFIGHT"  },
    { "state:healing",   "DSWHEAL"   },
    { "state:holding",   "DSWHOLD"   },
    { "state:coming",    "DSWCOME"   },
    { "state:grabbing",  "DSWGRAB"   },
    // Console replies
    { "summon_ok",       "DSSUMONO" },
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

    // ---- event callouts (see tools/bake_buddy_voice.py EVENTS) -------------
    { "kill:0","DSKL01" },{ "kill:1","DSKL02" },{ "kill:2","DSKL03" },{ "kill:3","DSKL04" },
    { "killimp:0","DSKI01" },{ "killimp:1","DSKI02" },{ "killimp:2","DSKI03" },
    { "killzm:0","DSKZM01" },{ "killsg:0","DSKSG01" },{ "killcg:0","DSKCG01" },
    { "killpk:0","DSKPK01" },{ "killsc:0","DSKSC01" },{ "killsl:0","DSKSL01" },
    { "killcd:0","DSKCD01" },{ "killpe:0","DSKPE01" },{ "killhk:0","DSKHK01" },
    { "killbn:0","DSKBN01" },{ "killrv:0","DSKRV01" },{ "killmc:0","DSKMC01" },
    { "killar:0","DSKAR01" },{ "killmm:0","DSKMM01" },{ "killcy:0","DSKCY01" },
    { "killav:0","DSKAV01" },{ "killns:0","DSKNS01" },{ "killkn:0","DSKKN01" },
    { "dodge:0","DSDG01" },{ "dodge:1","DSDG02" },{ "dodge:2","DSDG03" },
    { "dry:0","DSDRY01" },{ "dry:1","DSDRY02" },{ "dry:2","DSDRY03" },
    { "barrel:0","DSBR01" },{ "barrel:1","DSBR02" },{ "barrel:2","DSBR03" },
    { "spree:0","DSSP01" },{ "spree:1","DSSP02" },{ "spree:2","DSSP03" },{ "spree:3","DSSP04" },
    { "gib:0","DSGB01" },{ "gib:1","DSGB02" },{ "gib:2","DSGB03" },
    { "crit:0","DSCR01" },{ "crit:1","DSCR02" },{ "crit:2","DSCR03" },
    { "fists:0","DSFS01" },{ "fists:1","DSFS02" },
    { "taunt:0","DSTN01" },{ "taunt:1","DSTN02" },{ "taunt:2","DSTN03" },{ "taunt:3","DSTN04" },
    { "bigmon:0","DSBIG01" },{ "bigmon:1","DSBIG02" },{ "bigmon:2","DSBIG03" },
    { "flank:0","DSFL01" },{ "flank:1","DSFL02" },{ "flank:2","DSFL03" },
    { "infight:0","DSIF01" },{ "infight:1","DSIF02" },
    { "edge:0","DSED01" },{ "edge:1","DSED02" },{ "edge:2","DSED03" },
    { "jump:0","DSJP01" },{ "jump:1","DSJP02" },
    { "door:0","DSDO01" },{ "door:1","DSDO02" },
    { "stuck:0","DSSK01" },{ "stuck:1","DSSK02" },{ "stuck:2","DSSK03" },
    { "lost:0","DSLS01" },{ "lost:1","DSLS02" },{ "lost:2","DSLS03" },
    { "locked:0","DSLK01" },{ "locked:1","DSLK02" },
    { "crush:0","DSCU01" },{ "crush:1","DSCU02" },
    { "plhurt:0","DSPH01" },{ "plhurt:1","DSPH02" },{ "plhurt:2","DSPH03" },
    { "pldown:0","DSPD01" },{ "pldown:1","DSPD02" },
    { "ff:0","DSFF01" },{ "ff:1","DSFF02" },{ "ff:2","DSFF03" },{ "ff:3","DSFF04" },{ "ff:4","DSFF05" },{ "ff:5","DSFF06" },
    { "nice:0","DSNC01" },{ "nice:1","DSNC02" },
    { "pickup:0","DSPU01" },{ "pickup:1","DSPU02" },{ "pickup:2","DSPU03" },
    { "healed:0","DSHL01" },{ "healed:1","DSHL02" },
    { "berserk:0","DSBK01" },{ "berserk:1","DSBK02" },
    { "god:0","DSGOD01" },{ "god:1","DSGOD02" },
    { "arm:0","DSARM01" },{ "arm:1","DSARM02" },
    { "lvlstart:0","DSLV01" },{ "lvlstart:1","DSLV02" },{ "lvlstart:2","DSLV03" },
    { "lvlclear:0","DSWN01" },{ "lvlclear:1","DSWN02" },{ "lvlclear:2","DSWN03" },
    { "secret:0","DSSE01" },{ "secret:1","DSSE02" },
    { "idle:0","DSID01" },{ "idle:1","DSID02" },{ "idle:2","DSID03" },{ "idle:3","DSID04" },
    { "help:0","DSHELP01" },{ "help:1","DSHELP02" },{ "help:2","DSHELP03" },
    { "help:3","DSHELP04" },{ "help:4","DSHELP05" },
    { "revived:0","DSREV01" },{ "revived:1","DSREV02" },{ "revived:2","DSREV03" },
    { "thanks:0","DSTHX01" },{ "thanks:1","DSTHX02" },{ "thanks:2","DSTHX03" },
    { "home:0","DSHOME01" },{ "home:1","DSHOME02" },{ "home:2","DSHOME03" },

    // ---- AI Director persona (DD* lumps, separate voice; see bake_buddy_voice.py
    //      DIRECTOR + p_ai_director.c).  Played on the director stream, not the buddy's.
    { "dir:start:0","DDSTART0" },{ "dir:start:1","DDSTART1" },{ "dir:start:2","DDSTART2" },
    { "dir:build:0","DDBUILD0" },{ "dir:build:1","DDBUILD1" },{ "dir:build:2","DDBUILD2" },
    { "dir:spawn:0","DDSPAWN0" },{ "dir:spawn:1","DDSPAWN1" },
    { "dir:horde:0","DDHORDE0" },{ "dir:horde:1","DDHORDE1" },{ "dir:horde:2","DDHORDE2" },
    { "dir:peak:0","DDPEAK0" },{ "dir:peak:1","DDPEAK1" },{ "dir:peak:2","DDPEAK2" },
    { "dir:big:0","DDBIG0" },{ "dir:big:1","DDBIG1" },{ "dir:big:2","DDBIG2" },
    { "dir:death:0","DDDEATH0" },{ "dir:death:1","DDDEATH1" },{ "dir:death:2","DDDEATH2" },
    { "dir:relax:0","DDRELAX0" },{ "dir:relax:1","DDRELAX1" },{ "dir:relax:2","DDRELAX2" },
    { "dir:gift:0","DDGIFT0" },{ "dir:gift:1","DDGIFT1" },{ "dir:gift:2","DDGIFT2" },
    { "dir:heal:0","DDHEAL0" },{ "dir:heal:1","DDHEAL1" },{ "dir:heal:2","DDHEAL2" },
    { "dir:ammo:0","DDAMMO0" },{ "dir:ammo:1","DDAMMO1" },{ "dir:ammo:2","DDAMMO2" },
    { "dir:flank:0","DDFLANK0" },{ "dir:flank:1","DDFLANK1" },
    { "dir:ambush:0","DDAMBSH0" },{ "dir:ambush:1","DDAMBSH1" },
    { "dir:focus:0","DDFOCUS0" },{ "dir:focus:1","DDFOCUS1" },
    { "dir:fallback:0","DDFALL0" },{ "dir:fallback:1","DDFALL1" },
    { "dir:spree:0","DDSPREE0" },{ "dir:spree:1","DDSPREE1" },{ "dir:spree:2","DDSPREE2" },
    { "dir:down:0","DDDOWN0" },{ "dir:down:1","DDDOWN1" },{ "dir:down:2","DDDOWN2" },
    { "dir:clear:0","DDCLEAR0" },{ "dir:clear:1","DDCLEAR1" },{ "dir:clear:2","DDCLEAR2" },
    { "dir:idle:0","DDIDLE0" },{ "dir:idle:1","DDIDLE1" },{ "dir:idle:2","DDIDLE2" },
};
#define NUM_VOICE_MAP ((int)(sizeof(VOICE_MAP)/sizeof(VOICE_MAP[0])))


// ----- lump cache (all ds* lumps from aidoom.wad, decoded once on demand) --

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
// Separate stream for the AI Director persona, so the Director (voice-of-god) and
// the buddy can talk at the same time without clearing each other's queue.
static SDL_AudioStream* director_stream = NULL;

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
    // Try the WAD first (aidoom.wad as added by D_DoomMain).
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
// Decode `lumpname` and queue it (panned by lvol/rvol, 0..127) on `stream`.
// `bound` tracks whether this stream's format has been bound yet (each stream
// binds once on its first line).  Shared by the buddy and Director paths.
static void play_on_stream (SDL_AudioStream* stream, int* bound,
                            const char* lumpname, int lvol, int rvol)
{
    if (!stream || !lumpname) return;       // init failed/skipped or no name
    if (lvol <= 0 && rvol <= 0)    return;  // inaudible (too far) -> skip

    voicecache_t* v = find_cache (lumpname);
    if (!v) v = load_lump (lumpname);
    if (!v || !v->pcm) return;              // lump missing or decode failed

    // Bind the stream as STEREO once (so we can pan); SDL resamples to the device.
    if (!*bound)
    {
        SDL_AudioSpec spec = make_spec (v->samplerate, 2);
        if (!SDL_SetAudioStreamFormat (stream, &spec, NULL))
        {
            fprintf (stderr, "I_Voice: SDL_SetAudioStreamFormat: %s\n", SDL_GetError());
            return;
        }
        *bound = 1;
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
    SDL_PutAudioStreamData (stream, out, frames * 2 * (int)sizeof(short));
    free (out);
}

void I_Voice_SayByName (const char* lumpname, int lvol, int rvol)
{
    static int bound = 0;
    play_on_stream (voice_stream, &bound, lumpname, lvol, rvol);
}

// ----- AI Director persona (own stream, own voice) --------------------------

void I_Director_Say (const char* tag, int lvol, int rvol)
{
    static int bound = 0;
    const char* lumpname = tag_to_lumpname (tag);
    if (!lumpname) return;                  // unknown tag -> silent
    play_on_stream (director_stream, &bound, lumpname, lvol, rvol);
}

int I_Director_Busy (void)
{
    if (!director_stream) return 0;
    return SDL_GetAudioStreamQueued (director_stream) > 0;
}

void I_Director_Stop (void)
{
    if (director_stream) SDL_ClearAudioStream (director_stream);
}

void I_Voice_Say (const char* tag, int lvol, int rvol)
{
    const char* lumpname = tag_to_lumpname (tag);
    if (!lumpname) return;                 // unknown tag -> silent
    I_Voice_SayByName (lumpname, lvol, rvol);
}

int I_Voice_Busy (void)
{
    if (!voice_stream) return 0;
    // Bytes still queued for the device == the buddy is still talking.
    return SDL_GetAudioStreamQueued (voice_stream) > 0;
}

void I_Voice_Stop (void)
{
    if (voice_stream) SDL_ClearAudioStream (voice_stream);
}


void I_Voice_Init (void)
{
    // Resolve the voice WAD path: command-line -file takes precedence (via the
    // existing W_AddFile mechanism in D_DoomMain), else aidoom.cfg `aidoom_wad`
    // (legacy `buddy_wad` still honoured), else "aidoom.wad" in CWD.
    char wadpath[256];
    if ((!Buddy_CfgGet ("aidoom_wad", wadpath, sizeof(wadpath)) || !*wadpath) &&
        (!Buddy_CfgGet ("buddy_wad",  wadpath, sizeof(wadpath)) || !*wadpath))
        strncpy (wadpath, "aidoom.wad", sizeof(wadpath));

    // Try to add it; detect success via "is any DS* lump now known?".
    int oldnumlumps = numlumps;
    W_AddFile (wadpath);
    // Grow lumpcache to cover the lumps aidoom.wad just added (W_AddFile doesn't),
    // and zero the new slots so W_CacheLumpNum sees them as not-yet-cached.
    if (numlumps > oldnumlumps && lumpcache)
    {
        void** oldcache = lumpcache;
        lumpcache = (void**)realloc (lumpcache, numlumps * sizeof(*lumpcache));
        memset (lumpcache + oldnumlumps, 0, (numlumps - oldnumlumps) * sizeof(*lumpcache));
        // CRITICAL: realloc may have MOVED lumpcache.  Every lump cached so far
        // (R_Init etc. ran before us) has a zone block whose owner back-pointer
        // still points into the OLD array; on the next purge Z_Free would write
        // NULL through that freed address and corrupt the heap (later surfacing
        // as e.g. a NULL drawseg->curline crash in the sprite renderer).  When
        // the array moved, re-point each live block's owner to the new slot.
        if (lumpcache != oldcache)
        {
            int i;
            for (i = 0; i < oldnumlumps; i++)
                if (lumpcache[i])
                    Z_ChangeUser (lumpcache[i], &lumpcache[i]);
        }
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

    // Second stream for the AI Director persona (DD* lumps).  Best-effort: if it
    // fails, only the Director goes silent; the buddy is unaffected.
    director_stream = SDL_OpenAudioDeviceStream (
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL,
        I_VoiceAudioCallback, NULL);
    if (director_stream)
        SDL_ResumeAudioStreamDevice (director_stream);
    else
        fprintf (stderr, "I_Voice: director stream open failed: %s -- director silent\n",
                 SDL_GetError());
}


void I_Voice_Shutdown (void)
{
    if (voice_stream) { SDL_DestroyAudioStream (voice_stream); voice_stream = NULL; }
    if (director_stream) { SDL_DestroyAudioStream (director_stream); director_stream = NULL; }
    while (cache_head)
    {
        voicecache_t* v = cache_head;
        cache_head = v->next;
        if (v->pcm)  free (v->pcm);    // allocated by stb_vorbis with malloc
        free (v->data);
        free (v);
    }
}