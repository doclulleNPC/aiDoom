// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	From-scratch MUS/MIDI player: a sequencer feeding a small 2-operator FM
//	synth that is configured from the IWAD's GENMIDI instrument patches.
//	This reproduces the Adlib/OPL *style* of the original music (per-channel
//	FM instruments, note envelopes) without emulating the OPL chip cycle for
//	cycle, and without ZMusic or any external library.
//
//	Two front-ends share the one voice engine: the native DOOM MUS lump
//	sequencer (Sequence) and a Standard MIDI File sequencer (MidiSeq) that
//	flattens every SMF track into one tempo-aware event list (ParseMidi).
//	Raw .mid/.smf music thus plays via the same FM voices as MUS, instead of
//	being skipped.
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

#define MUSRATE		11025		// must match i_sound.c SAMPLERATE
#define MUS_TICRATE	140		// MUS delays are measured in 140 Hz ticks
#define SINBITS		12
#define SINLEN		(1 << SINBITS)	// 4096-entry sine table
#define SINMASK		(SINLEN - 1)
#define MAXVOICES	24		// simultaneous notes

// OPL frequency multiplier table (MULT nibble -> x0.5 .. x15).
static const float multtab[16] =
{ 0.5f,1,2,3,4,5,6,7,8,9,10,10,12,12,15,15 };

static float	sintab[SINLEN];

// ---- GENMIDI -------------------------------------------------------------

// One FM operator's patch fields, extracted from a GENMIDI voice.
typedef struct {
    float	mult;		// frequency multiplier
    float	level;		// output attenuation -> linear gain (0..1)
    float	atk, dec, rel;	// envelope increments per sample
    float	sus;		// sustain level (0..1)
} gmop_t;

typedef struct {
    gmop_t	mod, car;	// modulator + carrier
    float	fb;		// modulator self-feedback (0..1)
    int		additive;	// 1 = both operators sound (AM), 0 = FM
    int		fixed;		// fixed-pitch instrument (percussion)
    int		fixednote;	// note to use when fixed
} gminstr_t;

static gminstr_t	instruments[175];
static boolean		have_genmidi;

// Map a 4-bit OPL rate (0=slow .. 15=fast) to a linear envelope increment
// per sample.  Approximate: time ~ 6s / 2^rate.
static float Rate (int r)
{
    float secs;
    if (r <= 0) return 0.0f;		// never changes
    secs = 6.0f / (float)(1 << r);
    if (secs < 0.001f) secs = 0.001f;
    return 1.0f / (secs * MUSRATE);
}

// Total-level / attenuation field (0..63, 0.75 dB steps) -> linear gain.
static float Atten (int tl)
{
    return (float)pow (10.0, -(tl & 0x3f) * 0.0375);	// 0.75 dB per step
}

static void LoadOp (gmop_t* op, const byte* v)
{
    // v[0]=char(mult in low nibble) v[1]=atk/dec v[2]=sus/rel v[5]=level
    op->mult = multtab[v[0] & 0x0f];
    op->level = Atten (v[5]);
    op->atk = Rate ((v[1] >> 4) & 0x0f);
    op->dec = Rate (v[1] & 0x0f);
    op->sus = (float)(15 - ((v[2] >> 4) & 0x0f)) / 15.0f;	// SL: 0=loud
    op->rel = Rate (v[2] & 0x0f);
    if (op->rel <= 0.0f) op->rel = Rate (7);			// avoid stuck notes
}

boolean MUS_Init (void)
{
    const byte*	lump;
    int		i, ln;

    for (i = 0; i < SINLEN; i++)
	sintab[i] = (float)sin (2.0 * M_PI * i / SINLEN);

    ln = W_CheckNumForName ("GENMIDI");
    if (ln < 0)
	return false;
    lump = W_CacheLumpNum (ln, PU_STATIC);
    if (memcmp (lump, "#OPL_II#", 8))
	return false;

    for (i = 0; i < 175; i++)
    {
	const byte*	e = lump + 8 + i*36;
	gminstr_t*	in = &instruments[i];
	unsigned short	flags = e[0] | (e[1] << 8);
	const byte*	v1 = e + 4;		// first voice (16 bytes)

	LoadOp (&in->mod, v1 + 0);
	LoadOp (&in->car, v1 + 7);
	in->fb = (float)((v1[6] >> 1) & 7) / 7.0f;
	in->additive = (v1[6] & 1);
	in->fixed = (flags & 1) != 0;
	in->fixednote = e[3];
    }
    have_genmidi = true;
    return true;
}

// ---- MUS sequencer + voices ----------------------------------------------

typedef struct {
    int		active;
    int		release;	// in release phase
    int		chan, note;
    float	vel;		// 0..1 (note velocity * channel volume snapshot)
    gminstr_t*	in;
    double	mphase, cphase;	// 0..1 phase accumulators
    double	minc, cinc;	// phase increment per sample
    float	menv, cenv;	// current envelope gains
    int		mstate, cstate;	// 0 attack,1 decay,2 sustain,3 release
    float	lastmod;	// for feedback
    int		age;
} voice_t;

static voice_t		voices[MAXVOICES];
static int		voiceage;

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

static double NoteInc (int note)
{
    // MIDI note -> Hz -> phase increment (cycles per sample).
    double hz = 440.0 * pow (2.0, (note - 69) / 12.0);
    return hz / MUSRATE;
}

static void StartNote (int ch, int note, int vol)
{
    voice_t*	v = NULL;
    int		i, oldest = 0x7fffffff, oi = 0;
    gminstr_t*	in;
    int		prog;

    if (ch == perc_chan)		// percussion: instrument by note
	prog = 128 + note - 35;
    else
	prog = instr_ch[ch];
    if (prog < 0 || prog > 174)
	prog = 0;
    in = &instruments[prog];

    for (i = 0; i < MAXVOICES; i++)
    {
	if (!voices[i].active) { v = &voices[i]; break; }
	if (voices[i].age < oldest) { oldest = voices[i].age; oi = i; }
    }
    if (!v) v = &voices[oi];		// steal oldest

    v->active = 1; v->release = 0;
    v->chan = ch; v->note = note;
    v->in = in;
    v->vel = (vol / 127.0f) * vol_ch[ch];
    v->mphase = v->cphase = 0.0;
    {
	int n = in->fixed ? in->fixednote : note;
	v->cinc = NoteInc (n) * in->car.mult;
	v->minc = NoteInc (n) * in->mod.mult;
    }
    v->menv = v->cenv = 0.0f;
    v->mstate = v->cstate = 0;		// attack
    v->lastmod = 0.0f;
    v->age = ++voiceage;
}

static void StopNote (int ch, int note)
{
    int	i;
    for (i = 0; i < MAXVOICES; i++)
	if (voices[i].active && !voices[i].release
	    && voices[i].chan == ch && voices[i].note == note)
	{
	    voices[i].release = 1;
	    voices[i].mstate = voices[i].cstate = 3;	// release
	}
}

static void AllNotesOff (void)
{
    int i;
    for (i = 0; i < MAXVOICES; i++)
	voices[i].active = 0;
}

// Advance one operator's envelope one sample; returns its gain.
static float EnvStep (int* state, float* env, const gmop_t* op)
{
    switch (*state)
    {
      case 0:	// attack
	*env += op->atk > 0 ? op->atk : 1.0f;
	if (*env >= 1.0f) { *env = 1.0f; *state = 1; }
	break;
      case 1:	// decay
	*env -= op->dec;
	if (*env <= op->sus) { *env = op->sus; *state = 2; }
	break;
      case 2:	// sustain
	break;
      case 3:	// release
	*env -= op->rel;
	if (*env < 0.0f) *env = 0.0f;
	break;
    }
    return *env;
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
	  case 2:	// pitch wheel (ignored)
	    scorepos++;
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
		    instr_ch[ch] = val;
		else if (ctrl == 3)		// channel volume
		    vol_ch[ch] = val / 127.0f;
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
	  case MEV_PROGRAM: instr_ch[e->ch] = e->d1; break;
	  case MEV_VOLUME:  vol_ch[e->ch]  = e->d1 / 127.0f; break;
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
    AllNotesOff ();
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

void MUS_Render (short* out, int frames)
{
    int	f, i;

    for (f = 0; f < frames; f++)
    {
	float	mix = 0.0f;

	// advance the sequencer for this sample (MUS or MIDI front-end)
	while (delaysamples <= 0.0 && !finished)
	    seqkind ? MidiSequence () : Sequence ();
	if (finished)
	{
	    if (looping && (score || mevbuf)) { MUS_Start (1); }
	    else { out[f*2] = out[f*2+1] = 0; continue; }
	}
	delaysamples -= 1.0;

	// synthesize all active voices
	for (i = 0; i < MAXVOICES; i++)
	{
	    voice_t*	v = &voices[i];
	    float	m, c, cg, mg;

	    if (!v->active) continue;

	    mg = EnvStep (&v->mstate, &v->menv, &v->in->mod);
	    cg = EnvStep (&v->cstate, &v->cenv, &v->in->car);

	    // modulator (with a little self-feedback)
	    v->mphase += v->minc;
	    if (v->mphase >= 1.0) v->mphase -= 1.0;
	    m = sintab[(int)(v->mphase*SINLEN + v->lastmod*v->in->fb*SINLEN) & SINMASK]
		* mg * v->in->mod.level;
	    v->lastmod = m;

	    // carrier: FM (phase-modulated by m) or additive
	    v->cphase += v->cinc;
	    if (v->cphase >= 1.0) v->cphase -= 1.0;
	    if (v->in->additive)
		c = sintab[(int)(v->cphase*SINLEN) & SINMASK] * cg * v->in->car.level + m;
	    else
		c = sintab[(int)(v->cphase*SINLEN + m*2.0f*SINLEN) & SINMASK]
		    * cg * v->in->car.level;

	    mix += c * v->vel;

	    // retire fully-released voices
	    if (v->release && v->cenv <= 0.0f)
		v->active = 0;
	}

	mix *= 0.35f;				// headroom for many voices
	if (mix >  1.0f) mix =  1.0f;
	if (mix < -1.0f) mix = -1.0f;
	out[f*2] = out[f*2+1] = (short)(mix * 30000.0f);
    }
}
