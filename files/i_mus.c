// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	MUS/MIDI player: a sequencer that drives the cycle-accurate Nuked-OPL3
//	emulator (files/opl3.c, via files/i_opl.c) from the IWAD's GENMIDI
//	instrument patches -- i.e. the *real* Adlib/OPL2 sound of the original
//	DOOM music, no ZMusic / external library.  (This replaced an earlier
//	from-scratch 2-operator FM approximation that only mimicked the style.)
//
//	Two front-ends share the one OPL backend: the native DOOM MUS lump
//	sequencer (Sequence) and a Standard MIDI File sequencer (MidiSequence)
//	that flattens every SMF track into one tempo-aware event list. Each fires
//	note/program/volume/pitch events at OPL_Music_*; MUS_Render lets the
//	emulator generate the samples between events.
//
//-----------------------------------------------------------------------------

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI			// MSVC's <math.h> hides M_PI unless _USE_MATH_DEFINES
#define M_PI 3.14159265358979323846
#endif

#include "doomtype.h"
#include "w_wad.h"
#include "z_zone.h"

#include "i_mus.h"
#include "i_opl.h"		// Nuked-OPL3 music backend (replaces the FM-approx synth)

#define MUSRATE		11025		// must match i_sound.c SAMPLERATE
#define MUS_TICRATE	140		// MUS delays are measured in 140 Hz ticks

static boolean		have_genmidi;

// Load the IWAD's GENMIDI patches and hand them to the OPL3 music backend (i_opl.c).
boolean MUS_Init (void)
{
    const byte*	lump;
    int		ln;

    ln = W_CheckNumForName ("GENMIDI");
    if (ln < 0)
	return false;
    lump = W_CacheLumpNum (ln, PU_STATIC);
    if (memcmp (lump, "#OPL_II#", 8))
	return false;

    OPL_Music_Init (MUSRATE);
    OPL_Music_SetGenmidi (lump, W_LumpLength (ln));
    have_genmidi = OPL_Music_Ready ();
    return have_genmidi;
}

// ---- MUS sequencer + voices ----------------------------------------------

static const byte*	score;		// MUS event stream
static int		scorelen;
static int		scorepos;
static int		scorestart;
static int		looping;
static int		finished;
static int		instr_ch[16];	// program (GENMIDI index) per channel
static float		vol_ch[16];	// 0..1 volume per channel
static double		delaysamples;	// samples left before next event

static int		seqkind;	// 0 = MUS, 1 = MIDI
static int		perc_chan = 15;	// percussion channel (15 = MUS, 9 = MIDI)

// ---- MIDI (Standard MIDI File) -------------------------------------------
// Every SMF track is flattened into one event list, sorted by absolute tick,
// then replayed against the same voice engine as MUS.

enum {	MEV_NOTEOFF, MEV_NOTEON, MEV_PROGRAM, MEV_VOLUME,
	MEV_CHANOFF, MEV_TEMPO, MEV_END };

typedef struct {
    unsigned int	tick;		// absolute tick
    unsigned int	seq;		// emission order (stable-sort tie break)
    byte		type, ch, d1;
    int			d2;		// velocity, or microseconds/quarter (TEMPO)
} midev_t;

static midev_t*		mevbuf;		// merged, tick-sorted event list (Z_Malloc)
static int		mevcount;
static int		mevpos;
static unsigned int	mevtick;	// sequencer's current absolute tick
static int		mid_division;	// SMF time division (PPQN or SMPTE)
static double		mid_spt;	// samples per tick (from current tempo)

// ---- SMF (.mid) parser: flatten all tracks into the tick-sorted mevbuf -------

static unsigned int be16 (const byte* p) { return (p[0] << 8) | p[1]; }
static unsigned int be32 (const byte* p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }

static unsigned int ReadVarLen (const byte** pp, const byte* end)
{
    unsigned int v = 0; byte b;
    do { if (*pp >= end) break; b = *(*pp)++; v = (v << 7) | (b & 0x7f); } while (b & 0x80);
    return v;
}

static int mev_cmp (const void* a, const void* b)
{
    const midev_t* x = a; const midev_t* y = b;
    if (x->tick != y->tick) return (x->tick < y->tick) ? -1 : 1;
    return (x->seq < y->seq) ? -1 : (x->seq > y->seq) ? 1 : 0;	// stable tie-break
}

static void MidEmit (unsigned int tick, int type, int ch, int d1, int d2)
{
    midev_t* e = &mevbuf[mevcount++];
    e->tick = tick; e->seq = (unsigned)mevcount; e->type = (byte)type;
    e->ch = (byte)ch; e->d1 = (byte)d1; e->d2 = d2;
}

// Parse a Standard MIDI File into mevbuf (all tracks merged, tick-sorted).
// Returns true on success; sets seqkind=1 + percussion channel.
static boolean MidiRegister (const byte* data, int length)
{
    unsigned int hlen, ntracks, division, maxend = 0;
    const byte* p; int t, cap;

    if (length < 14 || memcmp (data, "MThd", 4)) return false;
    hlen     = be32 (data + 4);
    ntracks  = be16 (data + 10);
    division = be16 (data + 12);
    if ((division & 0x8000) || division == 0) division = 480;	// SMPTE unsupported -> assume PPQN
    mid_division = (int)division;

    cap = length / 2 + 16;					// <= 1 event per 2 bytes
    if (mevbuf) Z_Free (mevbuf);
    mevbuf = Z_Malloc (cap * sizeof(midev_t), PU_STATIC, 0);
    mevcount = 0;
    p = data + 8 + hlen;

    for (t = 0; t < (int)ntracks && p + 8 <= data + length; t++)
    {
	unsigned int tlen, abst = 0; const byte* tp; const byte* tend; byte run = 0;
	if (memcmp (p, "MTrk", 4)) break;
	tlen = be32 (p + 4);
	tp = p + 8; tend = tp + tlen;
	if (tend > data + length) tend = data + length;
	p = tend;

	while (tp < tend && mevcount < cap - 2)
	{
	    byte st; int ch;
	    abst += ReadVarLen (&tp, tend);
	    if (tp >= tend) break;
	    st = *tp;
	    if (st & 0x80) { tp++; run = (st < 0xF0) ? st : 0; } else st = run;	// running status
	    ch = st & 0x0f;
	    switch (st & 0xf0)
	    {
	      case 0x80: { int n = *tp++ & 0x7f; tp++;			// note off
			   MidEmit (abst, MEV_NOTEOFF, ch, n, 0); break; }
	      case 0x90: { int n = *tp++ & 0x7f, vel = *tp++ & 0x7f;	// note on (vel 0 = off)
			   MidEmit (abst, vel ? MEV_NOTEON : MEV_NOTEOFF, ch, n, vel); break; }
	      case 0xA0: tp += 2; break;				// poly aftertouch
	      case 0xB0: { int cc = *tp++ & 0x7f, val = *tp++ & 0x7f;	// controller
			   if (cc == 7)  MidEmit (abst, MEV_VOLUME, ch, val, 0);	// channel volume
			   else if (cc == 120 || cc == 123) MidEmit (abst, MEV_CHANOFF, ch, 0, 0);
			   break; }
	      case 0xC0: MidEmit (abst, MEV_PROGRAM, ch, *tp++ & 0x7f, 0); break;	// program
	      case 0xD0: tp += 1; break;				// channel pressure
	      case 0xE0: tp += 2; break;				// pitch bend
	      case 0xF0:
		if (st == 0xFF) { int mt = *tp++; unsigned int ml = ReadVarLen (&tp, tend);
		    if (mt == 0x51 && ml == 3)
			MidEmit (abst, MEV_TEMPO, 0, 0, (tp[0]<<16)|(tp[1]<<8)|tp[2]);
		    tp += ml; }
		else { unsigned int sl = ReadVarLen (&tp, tend); tp += sl; }	// sysex
		break;
	      default: tp = (byte*)tend; break;				// malformed -> end track
	    }
	}
	if (abst > maxend) maxend = abst;
    }

    MidEmit (maxend, MEV_END, 0, 0, 0);			// true song length (for looping)
    qsort (mevbuf, mevcount, sizeof(midev_t), mev_cmp);
    seqkind = 1; perc_chan = 9;				// MIDI: percussion = channel 10
    return mevcount > 1;
}

static void StartNote (int ch, int note, int vol)
{
    OPL_Music_NoteOn (ch, note, vol);	// OPL3 backend (program/volume tracked per channel)
}

static void StopNote (int ch, int note)
{
    OPL_Music_NoteOff (ch, note);
}

static void AllNotesOff (void)
{
    OPL_Music_AllNotesOff ();
}

// Read MUS events up to (and including) the next delay; sets delaysamples.
static void Sequence (void)
{
    for (;;)
    {
	int	ev, type, ch, last, data;

	if (scorepos >= scorelen) { finished = 1; return; }
	ev = score[scorepos++];
	type = (ev >> 4) & 7;
	ch = ev & 15;
	last = ev & 0x80;

	switch (type)
	{
	  case 0:	// release note
	    data = score[scorepos++] & 0x7f;
	    StopNote (ch, data);
	    break;
	  case 1:	// play note
	    data = score[scorepos++];
	    {
		int note = data & 0x7f;
		int vol = 127;
		if (data & 0x80) vol = score[scorepos++] & 0x7f;
		StartNote (ch, note, vol);
	    }
	    break;
	  case 2:	// pitch wheel (MUS byte 0..255, centre 128 -> OPL 0..127, centre 64)
	    OPL_Music_PitchBend (ch, (score[scorepos++] & 0xff) >> 1);
	    break;
	  case 3:	// system event
	    data = score[scorepos++] & 0x7f;
	    if (data == 10 || data == 11) AllNotesOff ();	// all sounds/notes off
	    break;
	  case 4:	// change controller
	    {
		int ctrl = score[scorepos++] & 0x7f;
		int val  = score[scorepos++] & 0x7f;
		if (ctrl == 0)			// program change
		    { instr_ch[ch] = val; OPL_Music_Program (ch, val); }
		else if (ctrl == 3)		// channel volume
		    { vol_ch[ch] = val / 127.0f; OPL_Music_ChannelVolume (ch, val); }
	    }
	    break;
	  case 6:	// score end
	    finished = 1;
	    return;
	  default:	// 5,7 unused -- skip a byte defensively
	    scorepos++;
	    break;
	}

	if (last)	// followed by a variable-length delay (140 Hz ticks)
	{
	    int delay = 0, b;
	    do {
		b = score[scorepos++];
		delay = (delay << 7) | (b & 0x7f);
	    } while ((b & 0x80) && scorepos < scorelen);
	    delaysamples += (double)delay * MUSRATE / MUS_TICRATE;
	    return;
	}
    }
}

// MIDI counterpart of Sequence(): emit every event at the current tick, then set
// the delay (samples) to the next event.  Same voice engine as MUS.
static void MidiSequence (void)
{
    for (;;)
    {
	midev_t* e;
	if (mevpos >= mevcount) { finished = 1; return; }
	e = &mevbuf[mevpos];
	if (e->tick > mevtick)			// wait until the next event
	{
	    delaysamples += (double)(e->tick - mevtick) * mid_spt;
	    mevtick = e->tick;
	    return;
	}
	mevpos++;
	switch (e->type)
	{
	  case MEV_NOTEON:  StartNote (e->ch, e->d1, e->d2); break;
	  case MEV_NOTEOFF: StopNote  (e->ch, e->d1); break;
	  case MEV_PROGRAM: instr_ch[e->ch] = e->d1; OPL_Music_Program (e->ch, e->d1); break;
	  case MEV_VOLUME:  vol_ch[e->ch]  = e->d1 / 127.0f; OPL_Music_ChannelVolume (e->ch, e->d1); break;
	  case MEV_CHANOFF: AllNotesOff (); break;
	  case MEV_TEMPO:   if (mid_division > 0)
				mid_spt = (double)e->d2 / 1e6 * MUSRATE / mid_division;
			    break;
	  case MEV_END:     finished = 1; return;
	}
    }
}

boolean MUS_Register (const void* data, int length)
{
    const byte*	p = data;

    if (!have_genmidi || length < 16) return false;

    if (!memcmp (p, "MUS\x1a", 4))		// native DOOM MUS lump
    {
	// header: id[4] scoreLen[2] scoreStart[2] channels[2] ...
	scorestart = p[6] | (p[7] << 8);
	score = p;
	scorelen = length;
	seqkind = 0; perc_chan = 15;
	return true;
    }
    if (!memcmp (p, "MThd", 4))			// raw Standard MIDI File
	return MidiRegister (p, length);

    return false;				// unknown -> "no music"
}

void MUS_Start (int loop)
{
    int i;
    OPL_Music_Reset ();				// silence + reset OPL channel state
    OPL_Music_SetPercChannel (perc_chan);	// MUS = 15, MIDI = 9
    for (i = 0; i < 16; i++) { instr_ch[i] = 0; vol_ch[i] = 1.0f; }
    scorepos = scorestart;
    // MIDI: rewind the event list + reset tempo (default 120 BPM = 500000 us/quarter).
    mevpos = 0; mevtick = 0;
    if (mid_division > 0)
	mid_spt = 500000.0 / 1e6 * MUSRATE / mid_division;
    delaysamples = 0;
    finished = 0;
    looping = loop;
}

void MUS_Stop (void)
{
    AllNotesOff ();
    finished = 1;
}

// Advance the sequencer event-by-event and let the OPL3 emulator (i_opl.c) render the
// samples between events.  Chunked render: each pass emits up to the next timed event,
// so register writes land on the right sample, then OPL_Music_Render fills that span.
void MUS_Render (short* out, int frames)
{
    int	done = 0;
    int	restarts = 0;

    if (!have_genmidi) { memset (out, 0, frames * 2 * sizeof(short)); return; }

    while (done < frames)
    {
	int chunk;

	// fire every event due now; sets delaysamples to the gap before the next one
	while (delaysamples <= 0.0 && !finished)
	    seqkind ? MidiSequence () : Sequence ();

	if (finished)
	{
	    if (looping && (score || mevbuf) && ++restarts <= 2)
		MUS_Start (1);				// loop (guard against empty-song spin)
	    else
	    {
		memset (out + done*2, 0, (frames-done) * 2 * sizeof(short));
		return;
	    }
	    continue;
	}

	chunk = frames - done;
	if (delaysamples > 0.0 && delaysamples < chunk) chunk = (int)delaysamples;
	if (chunk < 1) chunk = 1;			// always make progress
	OPL_Music_Render (out + done*2, chunk);
	done	     += chunk;
	delaysamples -= chunk;
    }
}
