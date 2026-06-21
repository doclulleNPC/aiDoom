// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Co-op companion (player 2).  Pick one of:
//	  -coop    : rule-based bot (current default; runs without any LLM)
//	  -aicoop  : AI-driven companion layer (see AI_IMPROVEMENTS.md #1)
//	          -- until that ships, -aicoop is a stub that falls back to
//	          the rule-based bot.  Once the AI layer is implemented, the
//	          same flag will instead route ticcmd generation through an LLM
//	          director (with the rule bot as the timeout/failure fallback).
//
//	Specifying both flags at once is a user error -- the two are mutually
//	exclusive.  P_AICoop_Init prints a warning and disables the buddy.
//
//	Single-machine only (no real netgame): the cmd is generated locally and
//	never carried over the wire.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "doomdef.h"
#include "doomdata.h"
#include "doomstat.h"
#include "d_event.h"
#include "d_items.h"
#include "m_argv.h"
#include "p_local.h"
#include "p_mobj.h"
#include "info.h"
#include "r_main.h"
#include "r_state.h"
#include "tables.h"
#include "m_fixed.h"

#include "p_ai_coop.h"

static int	companion_active;	// buddy enabled (-coop OR -aicoop)
static int	aicoop_layer;		// -aicoop given: AI-driven layer requested
static int	buddy_react;		// reaction delay (tics) before firing a fresh target (-buddyreact)
static int	coop_state;		// 0 follow 1 fight 2 heal 3 hold 4 come 5 items
static int	summon;			// "come": tics left running to the player
static int	hold;			// "wait/stay": hold position
static int	forceaggro;		// "attack": tics left charging forcetarget
static mobj_t*	forcetarget;		// the forced attack target
static int	ai_goto;		// LLM "goto": tics left moving to (ai_gx,ai_gy)
static fixed_t	ai_gx, ai_gy;		// LLM goto destination
static int	react_timer;		// tics left before firing a freshly-sighted target
static mobj_t*	react_last;		// the target the reaction timer is counting for

#define COOP_BLAST_SAFE	(176*FRACUNIT)	// don't fire rocket/BFG at a target closer than this (splash)
#define COOP_DODGE_RANGE (256*FRACUNIT)	// react to incoming missiles within this range

// Breadcrumb trail: the human's recent walkable positions.  When the buddy can't
// make progress toward the player (the portal pathfinder is stuck in some tight
// geometry), it replays this trail -- every crumb is a spot the human actually
// stood on, so it is guaranteed reachable and threads the exact gap the human used.
#define CRUMB_MAX	48
#define CRUMB_GAP	(48*FRACUNIT)	// drop a crumb each ~48u the human moves
static fixed_t	crumbx[CRUMB_MAX], crumby[CRUMB_MAX];
static int	crumb_n;		// crumbs held (crumb[0]=oldest .. crumb[n-1]=newest≈player)
static int	trail_active;		// currently following the breadcrumb trail

static void AICoop_CrumbAdd (fixed_t x, fixed_t y)
{
    if (crumb_n > 0 &&
	P_AproxDistance (x - crumbx[crumb_n-1], y - crumby[crumb_n-1]) < CRUMB_GAP)
	return;				// human hasn't moved a full step yet
    if (crumb_n == CRUMB_MAX)		// full -> drop the oldest
    {
	memmove (crumbx, crumbx+1, (CRUMB_MAX-1)*sizeof(fixed_t));
	memmove (crumby, crumby+1, (CRUMB_MAX-1)*sizeof(fixed_t));
	crumb_n--;
    }
    crumbx[crumb_n] = x; crumby[crumb_n] = y; crumb_n++;
}


#define COOP_SIGHT	(1280*FRACUNIT)	// monster acquisition range
#define COOP_TURN	1300		// max angleturn per tic (~7 deg)
#define COOP_FACING	1500		// |remaining turn| under which we open fire
#define COOP_LOOKMAX	56		// vertical aim clamp (mirrors g_game.c LOOKDIRMAX)
#define COOP_NEAR	(256*FRACUNIT)	// follow distance to the human
#define YIELD_DIST	(48*FRACUNIT)	// human this close -> step out of the way
#define COOP_KEEP	(192*FRACUNIT)	// advance toward a monster until this close
#define COOP_RUN	0x32		// forwardmove "run" magnitude
#define COOP_HEAL_HP	50		// seek a med-pack below this health
#define COOP_HEAL_RANGE	(1024*FRACUNIT)	// how far to look for one
#define COOP_ITEM_RANGE	(128*FRACUNIT)	// idle pickups only when right nearby (not "miles away")
#define COOP_GRAB_NEAR	(512*FRACUNIT)	// only grab items while still near the human (else follow)
#define COOP_SUMMON_TICS (7*TICRATE)	// "come" runs to you for this long
#define COOP_ATTACK_TICS (10*TICRATE)	// "attack" charges the target for this long


static int	coop_slot = 1;		// player index the buddy occupies (single-player: 1)

// Call after D_CheckNetGame.  The buddy is SINGLE-PLAYER ONLY: in a netgame it is
// disabled, so real network games run without it (clean lockstep, no extra slot).
void P_AICoop_Init (void)
{
    // -coop  : autonomous rule-based bot.
    // -aicoop: the rule-based bot PLUS the LLM director layer -- an external
    //          director (run/director) sets the buddy's high-level tactic each
    //          cycle (engage/defend/hold/regroup/retreat/grab) over the same TCP
    //          transport it uses for the monsters; the rule-based primitives
    //          execute it, and the buddy reverts to autonomous when the director
    //          goes quiet.  See AGENT_CONTROL.md / p_ai_llm.c.
    int coop    = M_CheckParm ("-coop")    > 0;
    int aicoop  = M_CheckParm ("-aicoop")  > 0;
    int rp      = M_CheckParm ("-buddyreact");	// reaction-time / skill knob (tics)

    // -buddyreact <tics>: delay between sighting a fresh target and opening fire
    // (0 = frame-perfect, the old behaviour; ~14 = a human-ish ~0.4s; higher = dumber).
    if (rp && rp < myargc-1)
    {
	buddy_react = atoi (myargv[rp+1]);
	if (buddy_react < 0)  buddy_react = 0;
	if (buddy_react > 70) buddy_react = 70;
    }

    if (!coop && !aicoop)
	return;

    if (coop && aicoop)
    {
	printf ("P_AICoop: -coop and -aicoop are mutually exclusive.  Pick one:\n"
		"  -coop    rule-based buddy (no LLM needed)\n"
		"  -aicoop  AI-driven buddy (LLM-backed; today falls back to -coop)\n"
		"P_AICoop: co-op companion disabled.  Relaunch with one of the two.\n");
	return;
    }

    if (netgame)
    {
	printf ("P_AICoop: co-op companion is single-player only -- disabled in netgames.\n");
	return;
    }

    coop_slot = 1;
    companion_active = 1;
    aicoop_layer = aicoop;		// -aicoop: accept director tactics for the buddy
				// (P_AICoop_AIMode / P_AICoop_SetDirective) + expose
				// it in the AI `observe` stream.
    playeringame[1] = true;	// spawn the buddy at the co-op start

    if (aicoop)
	printf ("P_AICoop: AI-directed co-op companion enabled (player 2) -- "
		"connect the director to drive its tactics.\n");
    else
	printf ("P_AICoop: rule-based co-op companion enabled (player 2)\n");
}

// Called from P_SetupLevel after P_LoadThings.  If -coop/-aicoop was given
// but the map has no Player_2_Start, the buddy can't spawn this level --
// disable it (so P_AICoop_Slot() returns -1 and the build-cmd path is a no-op)
// and emit a one-shot warning telling the user what to fix.
// Called from P_SetupLevel just before P_LoadThings.  Drops the buddy slot's
// stale mobj AND its stale playerstarts[] entry so that P_AICoop_VerifySpawn
// can reliably distinguish "this map's THINGS contain a Player_2_Start" from
// "this map has no Player_2_Start".
//
// Why we have to touch both:
//   - players[coop_slot].mo is a dangling pointer across map loads (the mobj
//     is freed by Z_FreeTags but the pointer field in the static players[]
//     struct is not zeroed).  Nulling it here makes the post-load "mo != NULL"
//     check reliable.
//   - playerstarts[coop_slot] is also static across map loads and only updated
//     by P_LoadThings if the map has a matching Player_X_Start thing.  If we
//     don't reset it, an E?M? that doesn't override the IWAD's playerstarts[]
//     (e.g. testmap's E2M1 PWAD overlay on doom2's E2M1, which retains the
//     IWAD's P2_Start) will falsely look like it had a P2_Start.  Resetting it
//     to a sentinel (type=0 = "no thing here") forces the post-load check to
//     see type=2 only when THIS map's THINGS explicitly set it.
//
// We deliberately do NOT touch playeringame[coop_slot]: it stays true (set
// by P_AICoop_Init), so P_SpawnPlayer inside P_LoadThings will spawn Player 2
// normally when the map has a Player_2_Start thing.
void P_AICoop_ResetSlot (void)
{
    if (!companion_active) return;
    if (netgame) return;

    players[coop_slot].mo = NULL;
    playerstarts[coop_slot].type = 0;	// sentinel: "no P2_Start thing on this map"
    playerstarts[coop_slot].x = 0;
    playerstarts[coop_slot].y = 0;
    playerstarts[coop_slot].angle = 0;
    playerstarts[coop_slot].options = 0;

    crumb_n = 0; trail_active = 0;	// drop the previous map's breadcrumb trail
}

// ---------------------------------------------------------------------------
//  Savegame: persist the breadcrumb trail so the buddy keeps following the
//  human's path across a save/load (otherwise the trail is empty on load and the
//  buddy can be stranded behind a door the human already walked through).  Written
//  AFTER the consistency marker (see g_game.c), so older saves without the block
//  still load -- the loader only reads it when there are bytes left in the file.
// ---------------------------------------------------------------------------
extern byte* save_p;

void P_AICoop_ArchiveTrail (void)
{
    int i;
    memcpy (save_p, &crumb_n, sizeof(int)); save_p += sizeof(int);
    for (i = 0; i < crumb_n; i++)
    {
	memcpy (save_p, &crumbx[i], sizeof(fixed_t)); save_p += sizeof(fixed_t);
	memcpy (save_p, &crumby[i], sizeof(fixed_t)); save_p += sizeof(fixed_t);
    }
}

void P_AICoop_UnArchiveTrail (void)
{
    int i, n = 0;
    memcpy (&n, save_p, sizeof(int)); save_p += sizeof(int);
    if (n < 0 || n > CRUMB_MAX)		// corrupt/short block -> ignore, start fresh
	{ crumb_n = 0; trail_active = 0; return; }
    for (i = 0; i < n; i++)
    {
	memcpy (&crumbx[i], save_p, sizeof(fixed_t)); save_p += sizeof(fixed_t);
	memcpy (&crumby[i], save_p, sizeof(fixed_t)); save_p += sizeof(fixed_t);
    }
    crumb_n = n;
    trail_active = 0;			// let the watchdog re-engage if the buddy lags
}

static boolean P_AICoop_VerifySpawn_warned = false;	// one-shot per process

void P_AICoop_VerifySpawn (void)
{
    mobj_t*	buddy_mo;

    if (!companion_active) return;			// buddy not requested
    if (netgame) return;					// not single-player, off

    // Has the map's Player_2_Start thing produced a real mobj?  P_LoadThings
    // would have written playerstarts[1] and called P_SpawnPlayer.  The
    // authoritative check is: does the buddy player have a mobj?
    // (P_AICoop_ResetSlot nulled players[coop_slot].mo before P_LoadThings,
    // so any non-NULL mo now is the result of THIS map's THINGS, not a leftover
    // from the previous map.  playeringame[coop_slot] stays true throughout
    // because P_AICoop_Init set it; P_SpawnPlayer in P_LoadThings respected
    // that, so the buddy is in the game if and only if there was a real P2 thing.)
    buddy_mo = players[coop_slot].mo;
    // The mobj-pointer check alone is unreliable because players[coop_slot].mo
    // can hold a dangling heap pointer from the previous map's spawn (Z_FreeTags
    // frees the mobj but doesn't NULL the field in the static players[] struct).
    // We cross-check against playerstarts[coop_slot].type, which P_LoadThings
    // set to 2 only if THIS map's THINGS contained a matching Player_2_Start;
    // we reset it to a sentinel in P_AICoop_ResetSlot, so type==2 here is
    // authoritative for "this map had a P2_Start thing".
    if (buddy_mo != NULL && playerstarts[coop_slot].type == 2)
	return;				// all good, buddy spawned

    // Map has no Player_2_Start.  Disable for this level (and all subsequent
    // levels until the user fixes the WAD or removes -coop), and tell them.
    companion_active = 0;				// local-only; not persisted

    if (!P_AICoop_VerifySpawn_warned)
    {
	printf ("\n"
		"P_AICoop: WARNING -- -coop/-aicoop requested but this map has no\n"
		"  Player_2_Start thing.  The co-op buddy will not spawn this level.\n"
		"  Fix: add a Player_2_Start to the map (any editor), or remove -coop.\n");
	P_AICoop_VerifySpawn_warned = true;
    }
}

// The slot the buddy occupies (-1 if disabled).  Used by g_game.c to skip the
// netgame consistency check for it -- the buddy is local-but-deterministic, never
// networked, so no remote command to validate against.
int P_AICoop_Slot (void)
{
    if (!companion_active) return -1;
    return coop_slot;
}

boolean P_AICoop_IsBuddy (player_t* p)
{
    return companion_active && p == &players[coop_slot];
}

// Public read-only accessor for coop_state (used by c_console.c for the voice
// tag mapping).  Returns -1 if the buddy is inactive.
int P_AICoop_State (void)
{
    if (!companion_active) return -1;
    return coop_state;
}

// The live companion mobj, or NULL if there isn't one right now.
static mobj_t* AICoop_Mo (void)
{
    if (!companion_active || !playeringame[coop_slot])		return NULL;
    if (players[coop_slot].playerstate != PST_LIVE)	return NULL;
    return players[coop_slot].mo;
}

// Nearest live human player to (x,y) -- the buddy follows/defends whoever's near
// (in single-player that's just player 0).  Deterministic (index tie-break).
static mobj_t* AICoop_NearestHuman (fixed_t x, fixed_t y)
{
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;
    int		i;

    for (i = 0 ; i < MAXPLAYERS ; i++)
    {
	mobj_t*	m; fixed_t d;
	if (i == coop_slot || !playeringame[i])		continue;
	if (players[i].playerstate != PST_LIVE || !players[i].mo) continue;
	m = players[i].mo;
	d = P_AproxDistance (m->x - x, m->y - y);
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}


// ----------------------------------------------------------------- voice
// The buddy speaks through i_voice.c, which plays an offline-baked OGG from
// buddy.wad via a dedicated SDL3 audio stream.  We pass a "tag" (e.g.
// "contact:0", "state:fighting") and i_voice maps it to the right lump.
// All best-effort: if buddy.wad isn't present or the lump is missing, the
// call is a silent no-op and the deterministic playsim is unaffected.
#include "i_voice.h"

static const char* AICOOP_STATE_TAGS[] =
{
    "state:following",   // COOP_STATE_FOLLOW
    "state:fighting",    // COOP_STATE_FIGHT
    "state:healing",     // COOP_STATE_HEAL
    "state:holding",     // COOP_STATE_HOLD
    "state:coming",      // COOP_STATE_COME
    "state:grabbing",    // COOP_STATE_GRAB
};

// Doom positional-sound constants (mirror s_sound.c) for spatialising the voice.
#define VOICE_CLIPDIST  (1200*0x10000)
#define VOICE_CLOSEDIST (160*0x10000)
#define VOICE_ATTEN     ((VOICE_CLIPDIST-VOICE_CLOSEDIST)>>FRACBITS)
#define VOICE_SWING     (96*0x10000)

// Per-channel 0..127 gains so the buddy's voice comes from *its* world position
// (distance attenuation + stereo pan) -- exactly like Doom SFX
// (S_AdjustSoundParams + i_sound.c's x^2 separation).  lis = the listening human,
// src = the buddy.
static void AICoop_VoicePan (mobj_t* lis, mobj_t* src, int* lvol, int* rvol)
{
    fixed_t adx = abs (lis->x - src->x);
    fixed_t ady = abs (lis->y - src->y);
    fixed_t dist = adx + ady - ((adx < ady ? adx : ady) >> 1);
    angle_t ang;
    int     vol, sep, s;

    if (lis == src) { *lvol = *rvol = 127; return; }		// shouldn't happen
    if (dist > VOICE_CLIPDIST) { *lvol = *rvol = 0; return; }	// too far -> silent

    ang = R_PointToAngle2 (lis->x, lis->y, src->x, src->y) - lis->angle;
    sep = 128 - (FixedMul (VOICE_SWING, finesine[ang >> ANGLETOFINESHIFT]) >> FRACBITS);

    if (dist < VOICE_CLOSEDIST) vol = 127;
    else vol = 127 * ((VOICE_CLIPDIST - dist) >> FRACBITS) / VOICE_ATTEN;

    s = sep + 1;   *lvol = vol - ((vol*s*s) >> 16);		// Doom's x^2 pan
    s = s - 257;   *rvol = vol - ((vol*s*s) >> 16);
    if (*lvol < 0) *lvol = 0; if (*lvol > 127) *lvol = 127;
    if (*rvol < 0) *rvol = 0; if (*rvol > 127) *rvol = 127;
}

static void AICoop_SayTag (const char* tag)
{
    mobj_t*	src = AICoop_Mo ();				// the buddy = sound source
    mobj_t*	lis = playeringame[displayplayer] ? players[displayplayer].mo : NULL;
    int		lvol = 127, rvol = 127;
    if (src && lis && src != lis) AICoop_VoicePan (lis, src, &lvol, &rvol);
    I_Voice_Say (tag, lvol, rvol);
}

// Rate-limited automatic line (combat/ambient): at most one every few seconds,
// rotating through the tag suffixes "0","1","2","3" so the buddy doesn't
// repeat the same phrase back-to-back.  tagprefix is e.g. "contact:" or
// "hurt:"; the index is appended (e.g. "contact:2").
static void AICoop_Callout (const char* tagprefix, int n)
{
    static int last, idx;
    if (gametic - last < 4*TICRATE) return;
    last = gametic;
    static char buf[32];
    snprintf (buf, sizeof(buf), "%s%d", tagprefix, idx % n);
    AICoop_SayTag (buf);
    idx++;
}

// Speak a tagged phrase through i_voice.c (offline OGG via buddy.wad).
// The "[Buddy] ..." console text is unaffected -- this is just the audio.
// Callers pick the exact tag (e.g. "summon_ok", "state:fighting"); the
// tag -> lump-name mapping lives in i_voice.c.
void P_AICoop_VoiceTag (const char* tag)
{
    if (!companion_active || !tag) return;
    AICoop_SayTag (tag);
}


// ---- "hopeless target" blacklist ------------------------------------------
// If the buddy fires at a monster and its health just won't drop (shots blocked /
// can't reach / can't elevate), it blacklists that monster for a few seconds so
// target acquisition skips it and picks another foe (or falls back to following)
// instead of freezing on one it can't hurt.  Keyed by mobj_t* like the rest of the
// buddy's side state; entries simply expire, so a freed/reused pointer self-heals.
#define COOP_BL_MAX	8
#define COOP_BL_TICS	(5*TICRATE)	// how long a hopeless target stays ignored
static struct { mobj_t* mon; int until; } coop_bl[COOP_BL_MAX];

static void AICoop_Blacklist (mobj_t* m)
{
    int	i, slot = 0, oldest = 0x7fffffff;
    for (i = 0; i < COOP_BL_MAX; i++)
    {
	if (coop_bl[i].mon == m)       { slot = i; break; }		// refresh existing
	if (coop_bl[i].until < oldest) { oldest = coop_bl[i].until; slot = i; }
    }
    coop_bl[slot].mon   = m;
    coop_bl[slot].until = gametic + COOP_BL_TICS;
}

static boolean AICoop_IsBlacklisted (mobj_t* m)
{
    int	i;
    for (i = 0; i < COOP_BL_MAX; i++)
	if (coop_bl[i].mon == m && gametic < coop_bl[i].until)
	    return true;
    return false;
}

//
// AICoop_FindTarget
// Nearest live, shootable, visible monster within range (never the human).
//
static mobj_t* AICoop_FindTarget (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m;
	fixed_t	d;

	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;

	if (m == self)			continue;
	if (m->type == MT_PLAYER)	continue;	// never target a human
	if (!(m->flags & MF_COUNTKILL))	continue;	// monsters only (skip barrels etc.)
	if (!(m->flags & MF_SHOOTABLE))	continue;
	if (m->flags & MF_CORPSE)	continue;
	if (m->health <= 0)		continue;
	if (AICoop_IsBlacklisted (m))	continue;	// shots weren't connecting -> skip it

	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > COOP_SIGHT)		continue;
	if (!P_CheckSight (self, m))	continue;

	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}


// Nearest live shootable monster to a point (used by the "attack" order).
static mobj_t* AICoop_NearestMonsterTo (fixed_t x, fixed_t y)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m;
	fixed_t	d;

	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;

	if (m->type == MT_PLAYER)	continue;
	if (!(m->flags & MF_COUNTKILL))	continue;
	if (!(m->flags & MF_SHOOTABLE))	continue;
	if (m->flags & MF_CORPSE)	continue;
	if (m->health <= 0)		continue;

	d = P_AproxDistance (m->x - x, m->y - y);
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}


// Is the floor at (x,y) a damaging sector (nukage / lava / blood / death exit)?
static boolean AICoop_DamagingFloor (fixed_t x, fixed_t y)
{
    sector_t* s = R_PointInSubsector (x, y)->sector;
    switch (s->special)
    {
      case 4:		// lightning + 20% damage
      case 5:		// 10% damage
      case 7:		// 5% damage (nukage)
      case 11:		// 20% damage + end level on death
      case 16:		// 20% damage
	return true;
    }
    return false;
}


//
// AICoop_CanReach
// Can the companion walk in a straight line from its feet to a pickup?  We
// march the segment in ~24-unit steps and at each point require that the marine
// fits (P_CheckPosition: no wall/obstacle, head-room) and that the floor never
// rises more than a 24-unit step.  Rejects items behind a wall or up a ledge so
// the bot doesn't run face-first into geometry trying to fetch them.
//
static boolean AICoop_CanReach (mobj_t* self, fixed_t tx, fixed_t ty, boolean avoiddmg)
{
    fixed_t	dx = tx - self->x;
    fixed_t	dy = ty - self->y;
    fixed_t	dist = P_AproxDistance (dx, dy);
    fixed_t	fz = self->z;			// start at the buddy's feet
    int		steps, i;

    if (dist < 16*FRACUNIT)
	return true;				// practically there
    // step by the buddy radius (16) so consecutive P_CheckPosition boxes (32 wide)
    // overlap -> a wall BETWEEN samples can't slip through (a 24-unit step left a
    // gap, so a waypoint just behind a thin wall looked reachable and the buddy
    // wedged against it).
    steps = dist / (16*FRACUNIT);
    if (steps > 96)
	return false;				// too far -- don't bother (bounds cost)

    for (i = 1; i <= steps; i++)
    {
	fixed_t	frac = (i << 16) / steps;	// i/steps as 16.16
	fixed_t	px   = self->x + FixedMul (dx, frac);
	fixed_t	py   = self->y + FixedMul (dy, frac);

	// Replicate P_TryMove's feasibility so "reachable" means the buddy can
	// actually WALK there (point-sampling P_CheckPosition alone said yes to spots
	// behind a step/ledge the move physics reject, so the buddy wedged there).
	if (!P_CheckPosition (self, px, py))		return false;	// wall/obstacle
	if (tmceilingz - tmfloorz < self->height)	return false;	// doesn't fit
	if (tmceilingz - fz < self->height)		return false;	// no head room
	if (tmfloorz - fz > 24*FRACUNIT)		return false;	// step up too high
	if (tmfloorz - tmdropoffz > 24*FRACUNIT)	return false;	// over a drop-off
	if (avoiddmg && AICoop_DamagingFloor (px, py))	return false;	// nukage/lava
	fz = tmfloorz;
    }
    return true;
}


//
// AICoop_FindHealth
// Nearest health pickup still lying in the world (stimpack/medikit/soul/mega).
//
static mobj_t* AICoop_FindHealth (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m;
	fixed_t	d;

	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;

	if (!(m->flags & MF_SPECIAL))	continue;	// not a pickup (or already taken)
	switch (m->sprite)
	{
	  case SPR_STIM: case SPR_MEDI:
	  case SPR_SOUL: case SPR_MEGA:
	    break;
	  default:
	    continue;
	}

	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > COOP_HEAL_RANGE)	continue;
	if (best && d >= bestd)		continue;	// not closer -> skip the trace
	if (!AICoop_CanReach (self, m->x, m->y, true)) continue;	// can't walk there

	best = m; bestd = d;
    }
    return best;
}


//
// AICoop_FindItem
// Nearest worth-grabbing pickup: health, bonuses, armor, ammo, weapons,
// backpack.  Deliberately skips keys (the human may need them in co-op).
//
static mobj_t* AICoop_FindItem (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;
    mobj_t*	pl = AICoop_NearestHuman (self->x, self->y);	// don't steal the human's items

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m;
	fixed_t	d;

	if (th->function.acp1 != (actionf_p1)P_MobjThinker)
	    continue;
	m = (mobj_t*)th;

	if (!(m->flags & MF_SPECIAL))	continue;	// a pickup still in the world
	switch (m->sprite)
	{
	  case SPR_STIM: case SPR_MEDI: case SPR_SOUL: case SPR_MEGA:	// health
	  case SPR_BON1: case SPR_BON2:					// bonuses
	  case SPR_ARM1: case SPR_ARM2:					// armor
	  case SPR_CLIP: case SPR_AMMO: case SPR_SHEL: case SPR_SBOX:	// ammo
	  case SPR_ROCK: case SPR_BROK: case SPR_CELL: case SPR_CELP:
	  case SPR_BPAK:						// backpack
	  case SPR_SHOT: case SPR_SGN2: case SPR_MGUN: case SPR_LAUN:	// weapons
	  case SPR_PLAS: case SPR_BFUG: case SPR_CSAW:
	    break;
	  default:
	    continue;						// keys & everything else
	}

	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > COOP_ITEM_RANGE)	continue;
	// Leave items the human is closer to -- otherwise the buddy snatches the
	// pickup right as the player walks up to it (looks like it "vanishes").
	if (pl && P_AproxDistance (m->x - pl->x, m->y - pl->y) < d) continue;
	if (best && d >= bestd)		continue;	// not closer -> skip the trace
	if (!AICoop_CanReach (self, m->x, m->y, true)) continue;	// can't walk there

	best = m; bestd = d;
    }
    return best;
}


// ----------------------------------------------------------------- commands
// Replies are short strings the console prints; they start with "[Buddy] ".

const char* P_AICoop_Report (void)
{
    static char		buf[120];
    static const char*	what[]    = { "following you", "fighting", "getting health",
				      "holding position", "coming to you", "grabbing an item" };
    static const char*	compass[8] = { "east","north-east","north","north-west",
				       "west","south-west","south","south-east" };
    mobj_t*	mo = AICoop_Mo ();
    mobj_t*	pl;

    if (!mo)
	return "[Buddy] (no companion -- launch with -coop)";
    pl = AICoop_NearestHuman (mo->x, mo->y);

    if (pl)
    {
	int	units = (int)(P_AproxDistance (mo->x - pl->x, mo->y - pl->y) >> FRACBITS);
	angle_t	a     = R_PointToAngle2 (pl->x, pl->y, mo->x, mo->y);
	int	oct   = (int)((a + (1u<<28)) >> 29) & 7;
	snprintf (buf, sizeof(buf), "[Buddy] %d units to your %s, %d HP -- %s.",
		  units, compass[oct], players[coop_slot].health, what[coop_state]);
    }
    else
	snprintf (buf, sizeof(buf), "[Buddy] %d HP -- %s.", players[coop_slot].health, what[coop_state]);

    return buf;
}

// Order commands are single-machine only: in a netgame they would set state on
// just one node and desync the lockstep, so they are refused there.
int P_AICoop_Summon (void)
{
    if (!AICoop_Mo ())		return 0;
    if (netgame)		return 0;
    summon = COOP_SUMMON_TICS;
    hold   = 0;
    return 1;
}

const char* P_AICoop_Wait (void)
{
    if (!AICoop_Mo ())
	return "[Buddy] (no companion -- launch with -coop)";
    if (netgame)
	return "[Buddy] (orders unavailable in netplay)";
    hold = !hold;
    if (hold) summon = 0;
    return hold ? "[Buddy] Holding position." : "[Buddy] Moving out.";
}

const char* P_AICoop_Attack (void)
{
    mobj_t*	mo = AICoop_Mo ();
    mobj_t*	pl;
    mobj_t*	t;

    if (!mo)
	return "[Buddy] (no companion -- launch with -coop)";
    if (netgame)
	return "[Buddy] (orders unavailable in netplay)";
    pl = AICoop_NearestHuman (mo->x, mo->y);
    t = AICoop_NearestMonsterTo (pl ? pl->x : mo->x, pl ? pl->y : mo->y);
    if (!t)
	return "[Buddy] No targets around.";
    forcetarget = t;
    forceaggro  = COOP_ATTACK_TICS;
    hold = 0;
    return "[Buddy] Attacking!";
}

// ---------------------------------------------------------------------------
//  AI (LLM) director layer
// ---------------------------------------------------------------------------
int P_AICoop_AIMode (void)
{
    return aicoop_layer;
}

// Map a director tactic onto the buddy's existing rule-based overrides (which
// already age + are executed in P_AICoop_BuildCmd).  The director re-sends the
// order every cycle to keep it fresh; when it stops, the timers lapse and the
// buddy reverts to autonomous behaviour.
void P_AICoop_SetDirective (int tactic, struct mobj_s* focus, fixed_t x, fixed_t y, int tics)
{
    static int	ai_last_tactic = -1;
    mobj_t*	mo = AICoop_Mo ();
    if (!mo || netgame)
	return;
    if (tics <= 0)
	tics = 70;				// ~2 s

    // Voice: announce a CHANGED non-combat order once.  Combat orders (engage/
    // defend) stay silent here -- the automatic contact/hurt/clear callouts in
    // BuildCmd already cover the fighting, so we'd only double up.
    if (tactic != ai_last_tactic)
    {
	const char* vtag = NULL;
	switch (tactic)
	{
	  case BUD_HOLD:    vtag = "wait_hold";      break;	// "Holding position"
	  case BUD_REGROUP:
	  case BUD_RETREAT: vtag = "summon_ok";      break;	// "On my way!"
	  case BUD_GOTO:    vtag = "wait_move";       break;	// "Moving out"
	  case BUD_GRAB:    vtag = "state:grabbing";  break;
	  default:          break;				// engage/defend/auto -> silent
	}
	if (vtag) P_AICoop_VoiceTag (vtag);
	ai_last_tactic = tactic;
    }

    // clear all overrides first; the chosen tactic re-arms the ones it needs
    forceaggro = 0; forcetarget = NULL; hold = 0; summon = 0; ai_goto = 0;

    switch (tactic)
    {
      case BUD_ENGAGE:
	forcetarget = (mobj_t*) focus;
	if (!forcetarget) forcetarget = AICoop_FindTarget (mo);
	forceaggro  = tics;
	break;
      case BUD_HOLD:
	hold = 1;
	break;
      case BUD_REGROUP:
      case BUD_RETREAT:
	summon = tics;
	break;
      case BUD_GOTO:
	ai_goto = tics; ai_gx = x; ai_gy = y;
	break;
      case BUD_DEFEND:
      case BUD_GRAB:
      case BUD_AUTO:
      default:
	break;					// overrides cleared -> rule-based
    }
}

const char* P_AICoop_StatusReport (void)
{
    static char		buf[120];
    static const char*	wn[NUMWEAPONS] = { "fists","pistol","shotgun","chaingun",
				"rocket launcher","plasma rifle","BFG9000","chainsaw","super shotgun" };
    player_t*	bot = &players[coop_slot];
    int		w, am;

    if (!AICoop_Mo ())
	return "[Buddy] (no companion -- launch with -coop)";
    w  = bot->readyweapon;
    am = (weaponinfo[w].ammo < NUMAMMO) ? bot->ammo[weaponinfo[w].ammo] : -1;
    if (am >= 0)
	snprintf (buf, sizeof(buf), "[Buddy] %d HP, %d%% armor, %s, %d rounds.",
		  bot->health, bot->armorpoints, wn[w], am);
    else
	snprintf (buf, sizeof(buf), "[Buddy] %d HP, %d%% armor, %s.",
		  bot->health, bot->armorpoints, wn[w]);
    return buf;
}


// ================ BSP sub-sector Dijkstra pathfinder (Pathfinding.md) =======
// Nodes  = walkable sub-sectors, represented by their centroid.
// Edges  = two-sided segs to the neighbouring sub-sector.
// Weight = centroid distance + penalties (closed door, damaging floor).
// A straight-line "string pull" then picks the furthest reachable waypoint.

#define PF_DOOR_PEN	200		// extra cost (units) to route through a door
#define PF_HAZARD_PEN	1000		// extra cost to route over a damaging floor
#define PF_MAXPOP	8000		// cap on Dijkstra node expansions
#define PF_PATHMAX	512		// max sub-sectors in a reconstructed path
#define PF_INF		0x7fffffff
#define PF_MAXADJ	32		// max graph edges per sub-sector

static int	pf_level = -1;		// episode*100+map the graph was built for
static int	pf_lastbuild;		// gametic of the last graph (re)build
static int	pf_n;			// sub-sector count
static fixed_t*	pf_cx;			// centroid x / y per sub-sector
static fixed_t*	pf_cy;
static int*	pf_nadj;		// edge count per sub-sector
static int*	pf_adj;			// flat [pf_n*PF_MAXADJ] neighbour sub-sectors
static int*	pf_adjw;		// flat [pf_n*PF_MAXADJ] edge weights
static fixed_t*	pf_adjpx;		// flat [pf_n*PF_MAXADJ] PORTAL x: a walkable point
static fixed_t*	pf_adjpy;		//   just inside neighbour v on the u|v boundary
static int*	pf_dist;		// Dijkstra cost-so-far
static int*	pf_prev;		// Dijkstra predecessor
static byte*	pf_done;		// Dijkstra closed flag
static int	pf_path[PF_PATHMAX];

static int PF_SS (fixed_t x, fixed_t y)
{
    return (int)(R_PointInSubsector (x, y) - subsectors);
}

static boolean PF_IsDoorSpecial (int sp)	// push (DR) doors with no key
{
    return (sp == 1 || sp == 31 || sp == 117 || sp == 118);
}

// If a CLOSED push-door (unlocked DR) lies ahead toward the goal, hand back the
// midpoint of its line in (*ox,*oy).  The BSP waypoints are sub-sector centroids,
// which can sit behind the corridor wall beside a doorway -- so the buddy never
// heads INTO the doorway and just grinds the wall next to it.  Steering at the
// door's own midpoint walks it down the corridor into the opening, where the Use
// tap in the stuck handler opens the (unlocked) door.
static boolean AICoop_FindDoorAhead (mobj_t* mo, fixed_t gx, fixed_t gy,
				     fixed_t* ox, fixed_t* oy)
{
    int		i;
    fixed_t	bestd = 256*FRACUNIT;		// only doors we're already near
    boolean	found = false;
    angle_t	togoal = R_PointToAngle2 (mo->x, mo->y, gx, gy);

    for (i = 0 ; i < numlines ; i++)
    {
	line_t*		ld = &lines[i];
	sector_t*	fs, *bs;
	fixed_t		mx, my, d, opening;
	angle_t		todoor;

	if (!ld->backsector || !ld->frontsector)	continue;	// not two-sided
	if (!PF_IsDoorSpecial (ld->special))		continue;	// not an openable door
	fs = ld->frontsector; bs = ld->backsector;
	opening = (fs->ceilingheight < bs->ceilingheight ? fs->ceilingheight : bs->ceilingheight)
		- (fs->floorheight   > bs->floorheight   ? fs->floorheight   : bs->floorheight);
	if (opening >= 56*FRACUNIT)			continue;	// already passable
	mx = (ld->v1->x + ld->v2->x) >> 1;
	my = (ld->v1->y + ld->v2->y) >> 1;
	d  = P_AproxDistance (mx - mo->x, my - mo->y);
	if (d >= bestd)					continue;
	todoor = R_PointToAngle2 (mo->x, mo->y, mx, my);
	if ((angle_t)(todoor - togoal + ANG90) > ANG180) continue;	// not ahead (>90 off)
	bestd = d; *ox = mx; *oy = my; found = true;
    }
    return found;
}

// --- Monster-style chase (Doom P_NewChaseDir) -------------------------------
// Steering straight at a waypoint can't round a tight corner: the buddy grinds the
// wall next to the opening.  Doom monsters solve this by trial-walking the 8 compass
// directions (diagonal toward the target first, then the two axes, then a scan),
// keeping a direction for a while so they escape concave nooks.  We do the same, but
// the trial is a no-move reachability probe (AICoop_CanReach mirrors P_TryMove) and
// the chosen heading drives the ticcmd instead of moving the mobj directly.

// Can the buddy walk ~24u along compass heading `d8` (0=E,1=NE,2=N,..,7=SE)?
static boolean AICoop_ChaseTry (mobj_t* mo, int d8)
{
    angle_t	a    = ((angle_t)d8 * ANG45) >> ANGLETOFINESHIFT;
    fixed_t	step = mo->radius + 24*FRACUNIT;
    return AICoop_CanReach (mo, mo->x + FixedMul (step, finecosine[a]),
				mo->y + FixedMul (step, finesine[a]), true);
}

// Pick (and commit to) a compass heading toward (gx,gy), Doom-monster style.
// Returns the heading as a BAM angle.  Keeps the last heading while it stays
// walkable so the buddy commits to a leg instead of dithering at a corner.
static angle_t AICoop_ChaseDir (mobj_t* mo, fixed_t gx, fixed_t gy)
{
    static int		dir = -1, count;
    static int		flip;
    fixed_t		dx = gx - mo->x, dy = gy - mo->y;
    int			wx = (dx > 10*FRACUNIT) ? 0 : (dx < -10*FRACUNIT) ? 4 : -1;	// E / W
    int			wy = (dy > 10*FRACUNIT) ? 2 : (dy < -10*FRACUNIT) ? 6 : -1;	// N / S
    int			turn = (dir >= 0) ? ((dir + 4) & 7) : -1;			// reverse of current
    int			cand[4], nc = 0, i, s;

    // keep the committed heading while it's still walkable
    if (dir >= 0 && count > 0 && AICoop_ChaseTry (mo, dir)) { count--; return (angle_t)dir * ANG45; }

    if (wx >= 0 && wy >= 0)					// diagonal toward goal
	cand[nc++] = (wy == 2) ? (wx == 0 ? 1 : 3) : (wx == 0 ? 7 : 5);
    if (abs (dx) >= abs (dy)) { if (wx >= 0) cand[nc++] = wx; if (wy >= 0) cand[nc++] = wy; }
    else                      { if (wy >= 0) cand[nc++] = wy; if (wx >= 0) cand[nc++] = wx; }
    if (dir >= 0) cand[nc++] = dir;				// then continue current

    for (i = 0; i < nc; i++)
	if (cand[i] != turn && AICoop_ChaseTry (mo, cand[i]))
	    { dir = cand[i]; count = 8; return (angle_t)dir * ANG45; }

    flip ^= 1;							// scan all 8, alternating sweep side
    for (s = 0; s < 8; s++)
    {
	int d = flip ? s : (7 - s);
	if (d != turn && AICoop_ChaseTry (mo, d))
	    { dir = d; count = 8; return (angle_t)dir * ANG45; }
    }
    if (turn >= 0 && AICoop_ChaseTry (mo, turn))			// last resort: turn around
	{ dir = turn; count = 4; return (angle_t)dir * ANG45; }

    dir = -1;							// boxed in -- head straight at goal
    return R_PointToAngle2 (mo->x, mo->y, gx, gy);
}

static int PF_EdgeWeight (seg_t* sg, int u, int v);	// defined below

// Straight feet-trace between two world points (the "item reachability" trick used
// for graph building): every ~24 units require a player-sized box (ref's radius/
// height) to fit (P_CheckPosition) and the floor to step <=24.  `fz` seeds the
// starting floor height.  Independent of ref's own position (P_CheckPosition tests
// the passed x,y), so it's safe to call while building the graph.
static boolean PF_LineWalkable (fixed_t ax, fixed_t ay, fixed_t bx, fixed_t by,
				mobj_t* ref, fixed_t fz)
{
    fixed_t	dx = bx - ax, dy = by - ay;
    fixed_t	dist = P_AproxDistance (dx, dy);
    int		steps, i;

    if (dist < 24*FRACUNIT) return true;
    steps = dist / (24*FRACUNIT);
    if (steps > 48) return false;
    for (i = 1; i <= steps; i++)
    {
	fixed_t	frac = (i << 16) / steps;
	fixed_t	px = ax + FixedMul (dx, frac);
	fixed_t	py = ay + FixedMul (dy, frac);
	if (!P_CheckPosition (ref, px, py))		return false;
	if (tmceilingz - tmfloorz < 56*FRACUNIT)	return false;
	if (tmfloorz - fz > 24*FRACUNIT)		return false;
	fz = tmfloorz;
    }
    return true;
}

static void PF_AddEdge (int u, int v, int w, fixed_t px, fixed_t py)
{
    int	k;
    if (u < 0 || v < 0 || u == v || w < 0) return;
    for (k = 0; k < pf_nadj[u]; k++)			// dedup, keep cheapest
	if (pf_adj[u*PF_MAXADJ + k] == v)
	{ if (w < pf_adjw[u*PF_MAXADJ + k])
	    { pf_adjw[u*PF_MAXADJ + k] = w; pf_adjpx[u*PF_MAXADJ + k] = px; pf_adjpy[u*PF_MAXADJ + k] = py; }
	  return; }
    if (pf_nadj[u] >= PF_MAXADJ) return;
    pf_adj [u*PF_MAXADJ + pf_nadj[u]] = v;
    pf_adjw[u*PF_MAXADJ + pf_nadj[u]] = w;
    pf_adjpx[u*PF_MAXADJ + pf_nadj[u]] = px;
    pf_adjpy[u*PF_MAXADJ + pf_nadj[u]] = py;
    pf_nadj[u]++;
}

// Portal point on the u->v edge (a walkable spot just inside v), or v's centroid.
static boolean PF_Portal (int u, int v, fixed_t* px, fixed_t* py)
{
    int	k;
    for (k = 0; k < pf_nadj[u]; k++)
	if (pf_adj[u*PF_MAXADJ + k] == v)
	{ *px = pf_adjpx[u*PF_MAXADJ + k]; *py = pf_adjpy[u*PF_MAXADJ + k]; return true; }
    return false;
}

static void PF_Build (mobj_t* ref)
{
    int		i, j, s;
    int*	segss;

    free (pf_cx); free (pf_cy); free (pf_nadj); free (pf_adj); free (pf_adjw);
    free (pf_adjpx); free (pf_adjpy);
    free (pf_dist); free (pf_prev); free (pf_done);

    pf_n    = numsubsectors;
    pf_cx   = malloc (pf_n * sizeof(fixed_t));
    pf_cy   = malloc (pf_n * sizeof(fixed_t));
    pf_dist = malloc (pf_n * sizeof(int));
    pf_prev = malloc (pf_n * sizeof(int));
    pf_done = malloc (pf_n);
    pf_nadj = calloc (pf_n, sizeof(int));
    pf_adj  = malloc (pf_n * PF_MAXADJ * sizeof(int));
    pf_adjw = malloc (pf_n * PF_MAXADJ * sizeof(int));
    pf_adjpx= malloc (pf_n * PF_MAXADJ * sizeof(fixed_t));
    pf_adjpy= malloc (pf_n * PF_MAXADJ * sizeof(fixed_t));
    segss   = malloc (numsegs * sizeof(int));

    // centroid of each sub-sector (mean of its segs' endpoints) + seg->ss map
    for (i = 0; i < pf_n; i++)
    {
	subsector_t*	ss = &subsectors[i];
	int64_t		sx = 0, sy = 0;
	int		cnt = 0;
	for (s = 0; s < ss->numlines; s++)
	{
	    seg_t* sg = &segs[ss->firstline + s];
	    sx += sg->v1->x; sy += sg->v1->y;
	    sx += sg->v2->x; sy += sg->v2->y;
	    cnt += 2;
	    segss[ss->firstline + s] = i;
	}
	pf_cx[i] = cnt ? (fixed_t)(sx / cnt) : 0;
	pf_cy[i] = cnt ? (fixed_t)(sy / cnt) : 0;
    }

    // (1) Cross-sector edges: each two-sided seg connects to the sub-sector on its
    // far side (probe ~4u off the midpoint).  PF_EdgeWeight handles doors/steps.
    for (j = 0; j < numsegs; j++)
    {
	seg_t*	sg = &segs[j];
	fixed_t	mx, my, nx, ny, len, ox, oy;
	int	a, b, self, v, w;

	if (!sg->backsector) continue;			// one-sided wall
	mx = (sg->v1->x + sg->v2->x) >> 1;
	my = (sg->v1->y + sg->v2->y) >> 1;
	nx = -(sg->v2->y - sg->v1->y);
	ny =  (sg->v2->x - sg->v1->x);
	len = P_AproxDistance (nx, ny);
	if (len < FRACUNIT) continue;
	ox = (fixed_t)(((int64_t)nx * (4*FRACUNIT)) / len);
	oy = (fixed_t)(((int64_t)ny * (4*FRACUNIT)) / len);

	self = segss[j];
	a = PF_SS (mx + ox, my + oy);
	b = PF_SS (mx - ox, my - oy);
	v = (a != self) ? a : (b != self ? b : -1);
	if (v < 0) continue;
	w = PF_EdgeWeight (sg, self, v);
	if (w >= 0)
	{
	    // Portal = the seg midpoint nudged ~16u INTO v (the destination side), so
	    // steering at it crosses the shared edge instead of stopping on the wall
	    // line.  ox,oy is the 4u normal; v sits on the +normal side iff v==a.
	    int s = (v == a) ? 4 : -4;
	    PF_AddEdge (self, v, w, mx + s*ox, my + s*oy);
	}
    }

    // (2) Grid adjacency -- THE fix for isolated sub-sectors.  In vanilla Doom a
    // sector is split into sub-sectors with NO connecting seg, and an L/concave
    // sector defeats a centroid-to-centroid trace, so some walkable sub-sectors
    // ended up with zero edges (bot standing in one => stuck).  Sample a grid over
    // the map; wherever two adjacent sample points fall in different sub-sectors
    // and a feet-trace between them is clear, connect those sub-sectors.  Catches
    // every walkable adjacency regardless of segs / sector shape.
    {
	const fixed_t	step = 32*FRACUNIT;
	fixed_t		gx, gy;
	fixed_t		xmax = bmaporgx + (fixed_t)bmapwidth *128*FRACUNIT;
	fixed_t		ymax = bmaporgy + (fixed_t)bmapheight*128*FRACUNIT;

	for (gy = bmaporgy; gy < ymax; gy += step)
	for (gx = bmaporgx; gx < xmax; gx += step)
	{
	    int		a = PF_SS (gx, gy);
	    fixed_t	af = subsectors[a].sector->floorheight;
	    int		nb[2], k;

	    nb[0] = PF_SS (gx+step, gy);		// right + down neighbours
	    nb[1] = PF_SS (gx, gy+step);
	    for (k = 0; k < 2; k++)
	    {
		fixed_t	tx = k ? gx : gx+step;
		fixed_t	ty = k ? gy+step : gy;
		int	b = nb[k], w;
		if (b == a) continue;
		if (!PF_LineWalkable (gx, gy, tx, ty, ref, af)) continue;
		w = (int)(P_AproxDistance (pf_cx[a]-pf_cx[b], pf_cy[a]-pf_cy[b]) >> FRACBITS);
		if (w < 1) w = 1;
		// Portal = a grid point INSIDE the destination sub-sector (the walkable
		// sample we just trace-verified), so steering at it crosses the boundary.
		PF_AddEdge (a, b, w + (AICoop_DamagingFloor (pf_cx[b], pf_cy[b]) ? PF_HAZARD_PEN : 0), tx, ty);
		PF_AddEdge (b, a, w + (AICoop_DamagingFloor (pf_cx[a], pf_cy[a]) ? PF_HAZARD_PEN : 0), gx, gy);
	    }
	}
    }

    free (segss);
}

// Cost of crossing seg `sg` from sub-sector u to its neighbour v; -1 = blocked.
static int PF_EdgeWeight (seg_t* sg, int u, int v)
{
    int	w = (int)(P_AproxDistance (pf_cx[u]-pf_cx[v], pf_cy[u]-pf_cy[v]) >> FRACBITS);
    line_t* ld = sg->linedef;

    if (w < 1) w = 1;
    if (ld)						// real wall line (not a BSP miniseg)
    {
	sector_t*	fs = sg->frontsector;
	sector_t*	bs = sg->backsector;
	fixed_t		opening, step;

	if (ld->flags & ML_BLOCKING) return -1;		// impassable rail
	opening = (fs->ceilingheight < bs->ceilingheight ? fs->ceilingheight : bs->ceilingheight)
		- (fs->floorheight   > bs->floorheight   ? fs->floorheight   : bs->floorheight);
	step    = bs->floorheight - fs->floorheight;

	if (opening < 56*FRACUNIT)			// won't fit right now
	{
	    if (PF_IsDoorSpecial (ld->special)) w += PF_DOOR_PEN;	// can open it
	    else return -1;
	}
	else if (step > 24*FRACUNIT)
	    return -1;					// step up too high (no lifts)
    }
    if (AICoop_DamagingFloor (pf_cx[v], pf_cy[v]))
	w += PF_HAZARD_PEN;
    return w;
}

// A* over the sub-sector graph: same edges + relaxation as Dijkstra, but the node we
// pop is the one with the lowest f = g + h, where g = pf_dist (cost so far) and h =
// the straight-line centroid distance to the goal.  h is admissible (edge weights are
// distances plus only positive door/hazard penalties, so they never undershoot the
// straight line), so the path stays optimal while far fewer nodes get expanded toward
// a distant goal than plain Dijkstra (which fans out to everything closer first).
static boolean PF_AStar (int start, int goal)
{
    int	i, pop = 0;
    fixed_t	gx = pf_cx[goal], gy = pf_cy[goal];

    for (i = 0; i < pf_n; i++) { pf_dist[i] = PF_INF; pf_prev[i] = -1; pf_done[i] = 0; }
    pf_dist[start] = 0;

    while (pop < PF_MAXPOP)
    {
	int	u = -1, bestf = PF_INF, s;

	for (i = 0; i < pf_n; i++)			// pop lowest f = g + h
	    if (!pf_done[i] && pf_dist[i] < PF_INF)
	    {
		int h = (int)(P_AproxDistance (pf_cx[i]-gx, pf_cy[i]-gy) >> FRACBITS);
		int f = pf_dist[i] + h;
		if (f < bestf) { bestf = f; u = i; }
	    }
	if (u < 0) break;				// nothing left reachable
	if (u == goal) return true;			// goal popped -> optimal path found
	pf_done[u] = 1; pop++;

	for (s = 0; s < pf_nadj[u]; s++)
	{
	    int	v = pf_adj[u*PF_MAXADJ + s];
	    int	w = pf_adjw[u*PF_MAXADJ + s];
	    if (pf_done[v]) continue;
	    if (pf_dist[u] + w < pf_dist[v]) { pf_dist[v] = pf_dist[u] + w; pf_prev[v] = u; }
	}
    }
    return pf_dist[goal] < PF_INF;
}

// Next point to steer toward to reach (dx,dy).  Returns false if unreachable.
static boolean PF_NextWaypoint (mobj_t* mo, fixed_t dx, fixed_t dy, fixed_t* wx, fixed_t* wy)
{
    int		start, goal, len, i, c, np, prev;
    fixed_t	portx[PF_PATHMAX], porty[PF_PATHMAX];

    if (pf_level != gameepisode*100 + gamemap || !pf_cx)
    { PF_Build (mo); pf_level = gameepisode*100 + gamemap; pf_lastbuild = gametic; }

    start = PF_SS (mo->x, mo->y);
    goal  = PF_SS (dx, dy);
    if (start == goal) { *wx = dx; *wy = dy; return true; }
    if (!PF_AStar (start, goal))
    {
	// No route -- the graph is built once at level start, so a door/lift/secret
	// wall that has since OPENED isn't in it yet (it had no passable edge when
	// built).  Rebuild from the current map state and retry (rate-limited to
	// ~1.5s so a genuinely unreachable target doesn't thrash).
	if (gametic - pf_lastbuild > 50)
	{
	    PF_Build (mo); pf_lastbuild = gametic;
	    if (!PF_AStar (start, goal)) return false;
	}
	else
	    return false;
    }

    // reconstruct goal -> ... -> first step (pf_path[len-1] is adjacent to start)
    len = 0; c = goal;
    while (c != -1 && c != start && len < PF_PATHMAX) { pf_path[len++] = c; c = pf_prev[c]; }
    if (len == 0) { *wx = dx; *wy = dy; return true; }

    // Collect PORTAL points along the route (start -> n1 -> ... -> goal).  A portal
    // is a walkable point on a sub-sector boundary, so -- unlike a sub-sector
    // CENTROID, which can sit behind a wall and made the buddy grind/chase it -- it
    // is always reachable, and the buddy can steer straight to it.
    np = 0; prev = start;
    for (i = len - 1; i >= 0; i--)			// path order: start -> goal
    {
	fixed_t qx, qy;
	if (PF_Portal (prev, pf_path[i], &qx, &qy)) { portx[np] = qx; porty[np] = qy; np++; }
	prev = pf_path[i];
    }

    // Funnel-lite string pull: steer to the FURTHEST point we can straight-walk to --
    // the goal itself if it's in sight, else the furthest reachable portal, else the
    // first portal (always on our own sub-sector's boundary, hence reachable).
    if (AICoop_CanReach (mo, dx, dy, true)) { *wx = dx; *wy = dy; return true; }
    for (i = np - 1; i >= 0; i--)
	if (AICoop_CanReach (mo, portx[i], porty[i], true)) { *wx = portx[i]; *wy = porty[i]; return true; }
    if (np > 0) { *wx = portx[0]; *wy = porty[0]; return true; }
    *wx = dx; *wy = dy;
    return true;
}

// Public pathfinder: next reachable waypoint for `mo` toward (dx,dy).  The sub-sector
// graph is map-global, so director-controlled MONSTERS use it too (p_ai_llm.c) to
// round corners toward their target instead of the vanilla straight-line 8-dir walk.
boolean P_AICoop_NextWaypoint (mobj_t* mo, fixed_t dx, fixed_t dy, fixed_t* wx, fixed_t* wy)
{
    return PF_NextWaypoint (mo, dx, dy, wx, wy);
}

// Topological route for the LLM director: fill (xs,ys) with up to `maxpts` reachable
// waypoints along the buddy->player path (the portal route, downsampled), so the
// director has real spatial context + valid coordinates it can steer the buddy to
// with a `goto`.  Returns the number of points (0 if same room / no route).
int P_AICoop_NavRoute (fixed_t* xs, fixed_t* ys, int maxpts)
{
    mobj_t*	mo = AICoop_Mo ();
    mobj_t*	pl;
    int		start, goal, len, i, c, n, prev, np, step;
    fixed_t	px[PF_PATHMAX], py[PF_PATHMAX];

    if (!mo || maxpts <= 0) return 0;
    pl = AICoop_NearestHuman (mo->x, mo->y);
    if (!pl) return 0;

    if (pf_level != gameepisode*100 + gamemap || !pf_cx)
	{ PF_Build (mo); pf_level = gameepisode*100 + gamemap; pf_lastbuild = gametic; }

    start = PF_SS (mo->x, mo->y);
    goal  = PF_SS (pl->x, pl->y);
    if (start == goal || !PF_AStar (start, goal)) return 0;

    len = 0; c = goal;
    while (c != -1 && c != start && len < PF_PATHMAX) { pf_path[len++] = c; c = pf_prev[c]; }
    if (len == 0) return 0;

    n = 0; prev = start;
    for (i = len - 1; i >= 0; i--)			// start -> goal portal points
    {
	fixed_t qx, qy;
	if (PF_Portal (prev, pf_path[i], &qx, &qy)) { px[n] = qx; py[n] = qy; n++; }
	prev = pf_path[i];
    }
    if (n == 0) return 0;

    step = (n + maxpts - 1) / maxpts; if (step < 1) step = 1;	// downsample to fit
    np = 0;
    for (i = 0; i < n && np < maxpts; i += step) { xs[np] = px[i]; ys[np] = py[i]; np++; }
    if (np && np < maxpts && (xs[np-1] != px[n-1] || ys[np-1] != py[n-1]))
	{ xs[np] = px[n-1]; ys[np] = py[n-1]; np++; }	// always keep the last (nearest player)
    return np;
}


// Move toward a world point using forward+side thrust, so the buddy heads
// straight there *regardless of which way it's facing*.  Pure forwardmove only
// goes where the marine looks, and the slow turn (COOP_TURN) lags -- so while
// turning it walks the wrong way, drifts into walls and snags on corners.
// angleturn still faces the target separately (for aiming/firing).
static void AICoop_ThrustToward (ticcmd_t* cmd, mobj_t* mo, fixed_t tx, fixed_t ty)
{
    angle_t rel = (R_PointToAngle2 (mo->x, mo->y, tx, ty) - mo->angle) >> ANGLETOFINESHIFT;
    cmd->forwardmove =  (signed char)(FixedMul (COOP_RUN*FRACUNIT, finecosine[rel]) >> FRACBITS);
    cmd->sidemove    = -(signed char)(FixedMul (COOP_RUN*FRACUNIT, finesine[rel])   >> FRACBITS);
}

// Cajun-bot-style missile dodge: if a live projectile is closing on us roughly on a
// collision heading, sidestep perpendicular to it (toward whichever side is walkable).
// Sets the move to the dodge and returns 1; the caller still aims/fires this tic.
static int AICoop_DodgeMissile (ticcmd_t* cmd, mobj_t* mo)
{
    thinker_t*	th;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m;
	angle_t	mv;
	int	side;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (!(m->flags & MF_MISSILE)) continue;
	if (m->target == mo) continue;				// our own shot -- ignore
	if (!m->momx && !m->momy) continue;			// not travelling
	if (P_AproxDistance (m->x - mo->x, m->y - mo->y) > COOP_DODGE_RANGE) continue;
	mv = R_PointToAngle2 (0, 0, m->momx, m->momy);		// the missile's heading
	// is it heading at us? compare its heading to the bearing from it to us
	if (abs ((int)(mv - R_PointToAngle2 (m->x, m->y, mo->x, mo->y))) > (int)(ANG45/2))
	    continue;						// >~22 deg off -> it'll miss
	for (side = 0; side < 2; side++)			// step perpendicular, walkable side
	{
	    angle_t perp = mv + (side ? (angle_t)-ANG90 : ANG90);
	    int     a    = perp >> ANGLETOFINESHIFT;
	    fixed_t dx   = mo->x + FixedMul (96*FRACUNIT, finecosine[a]);
	    fixed_t dy   = mo->y + FixedMul (96*FRACUNIT, finesine[a]);
	    if (AICoop_CanReach (mo, dx, dy, false))
	    {
		AICoop_ThrustToward (cmd, mo, dx, dy);
		return 1;
	    }
	}
    }
    return 0;
}

// Best ranged weapon the buddy owns with ammo (melee weapons excluded; rockets/BFG
// skipped -- splash would hurt the buddy/human at the ranges it fights).  Used to
// switch off the chainsaw/fist when the target is out of melee reach.
static int AICoop_BestRanged (player_t* p)
{
    static const int	pri[] = { wp_chaingun, wp_supershotgun, wp_shotgun, wp_plasma, wp_pistol };
    int			i;
    for (i = 0; i < 5; i++)
    {
	int		w = pri[i];
	ammotype_t	a;
	if (!p->weaponowned[w]) continue;
	a = weaponinfo[w].ammo;
	if (a == am_noammo || p->ammo[a] > 0) return w;
    }
    return -1;
}

// Feet-trace steering -- the "item reachability" trick (AICoop_CanReach) applied
// to movement.  Aim at the goal; if the straight floor-trace is blocked, sweep
// the heading outward in 22.5° steps -- the *full* circle (±180°), so it can also
// turn toward a door beside or behind it -- and steer toward the first clear
// direction.  If nothing toward the goal is clear, head to the centre of the
// current room (subsector centroid) to get off the wall, then re-evaluate next
// tic (this is the "go to the middle of the room first, then path" behaviour).
static void AICoop_TraceSteer (mobj_t* mo, fixed_t gx, fixed_t gy, fixed_t* sx, fixed_t* sy)
{
    static const int seq[16] = { 0,1,-1,2,-2,3,-3,4,-4,5,-5,6,-6,7,-7,8 };  // *22.5°
    angle_t	base  = R_PointToAngle2 (mo->x, mo->y, gx, gy);
    fixed_t	gdist = P_AproxDistance (gx - mo->x, gy - mo->y);
    fixed_t	probe = (gdist < 192*FRACUNIT) ? gdist : 192*FRACUNIT;
    int		i, ss;

    if (AICoop_CanReach (mo, gx, gy, true)) { *sx = gx; *sy = gy; return; }
    for (i = 0 ; i < 16 ; i++)
    {
	angle_t a  = (base + (angle_t)seq[i] * (ANG45/2)) >> ANGLETOFINESHIFT;
	fixed_t px = mo->x + FixedMul (probe, finecosine[a]);
	fixed_t py = mo->y + FixedMul (probe, finesine[a]);
	if (AICoop_CanReach (mo, px, py, true)) { *sx = px; *sy = py; return; }
    }
    // Nothing clear toward the goal -> aim for the centre of our own room.
    ss = PF_SS (mo->x, mo->y);
    if (pf_cx && ss >= 0 && ss < pf_n) { *sx = pf_cx[ss]; *sy = pf_cy[ss]; return; }
    *sx = gx; *sy = gy;
}

// Guard against fixating on a pickup the buddy can reach horizontally but never
// actually collect (e.g. it sits a little above on a ledge/pedestal -> the buddy
// oscillates on it forever, as seen stuck in an E1M2 secret).  If we've targeted
// the same item for >2s without grabbing it, blacklist it for a few seconds so the
// buddy gives up and resumes following.
static boolean AICoop_GrabStuck (mobj_t* item)
{
    static mobj_t*	cur;
    static int		started;
    static mobj_t*	skip;
    static int		skipuntil;

    if (item == skip && gametic < skipuntil)	return true;
    if (item != cur) { cur = item; started = gametic; }
    if (gametic - started > 2*TICRATE)
    { skip = item; skipuntil = gametic + 5*TICRATE; return true; }
    return false;
}

void P_AICoop_BuildCmd (void)
{
    player_t*	bot;
    mobj_t*	mo;
    mobj_t*	tgt;
    mobj_t*	heal;
    mobj_t*	item;
    mobj_t*	pl;
    mobj_t*	aimmon = NULL;		// the monster we're firing at (for the sight test)
    ticcmd_t*	cmd;
    angle_t	want, delta;
    fixed_t	tx = 0, ty = 0, stx, sty, dist;
    fixed_t	movethresh = -1;	// move when dist > this; -1 = stand still
    int		rem, turn, haveaim = 0, fire = 0, avoiddamage = 0, navigate = 0;
    boolean	stuck, backoff = false;	// backoff: can't hit the target, retreat to open the angle
    static fixed_t lastx, lasty;	// where we were last tic (progress check)
    static int	doorwait, triedmove;	// door pulse cooldown / did we try to move
    static int	navtimer, navgoal = -1;	// pathfinder re-path cooldown / cached goal ss
    static fixed_t navwx, navwy;	// cached waypoint
    static boolean navok;
    static mobj_t* dmg_mon;		// monster the damage-watchdog is tracking
    static int	dmg_hp0, dmg_firetics;	// its health at baseline / tics fired with no drop

    if (!companion_active || !playeringame[coop_slot])
	return;

    bot = &players[coop_slot];
    cmd = &bot->cmd;

    // Dead: tap "use" so co-op reborns the companion at its start.
    if (bot->playerstate == PST_DEAD)
    {
	memset (cmd, 0, sizeof(*cmd));
	cmd->buttons |= BT_USE;
	return;
    }
    if (bot->playerstate != PST_LIVE || !bot->mo)
	return;

    mo = bot->mo;
    memset (cmd, 0, sizeof(*cmd));

    // Progress check.  "stuck" if we tried to move but either barely moved this tic
    // (wedged solid) OR made no NET progress over a ~0.4s window -- the latter
    // catches oscillating in place in front of a closed door (per-tic it's moving
    // side to side, so the old check never flagged it and the door never got Used).
    {
	static fixed_t	winx, winy;
	static int	wintic;
	static boolean	oscillating;
	if (gametic - wintic >= 14)
	{
	    if (wintic) oscillating = P_AproxDistance (mo->x-winx, mo->y-winy) < 40*FRACUNIT;
	    winx = mo->x; winy = mo->y; wintic = gametic;
	}
	stuck = triedmove
	     && (P_AproxDistance (mo->x - lastx, mo->y - lasty) < 2*FRACUNIT || oscillating);
    }
    lastx = mo->x; lasty = mo->y;
    if (doorwait > 0) doorwait--;

    pl = AICoop_NearestHuman (mo->x, mo->y);

    // Breadcrumb trail upkeep + long-horizon "stuck reaching the player" watchdog.
    if (pl)
    {
	static int     prog_tic;
	static fixed_t best_pld = 0x7fffffff;	// closest we've gotten to the player
	static int     noprog;
	fixed_t        pld = P_AproxDistance (mo->x - pl->x, mo->y - pl->y);

	AICoop_CrumbAdd (pl->x, pl->y);

	if (gametic - prog_tic >= 35)			// re-check ~1x/s
	{
	    prog_tic = gametic;
	    // Progress = reaching a NEW minimum distance.  Tracking the best (not the
	    // last) defeats jitter: oscillating in place bounces pld ~+/-100u, which the
	    // old "closer than last second" test mistook for progress, so the watchdog
	    // never tripped and the trail/fallback never kicked in.
	    if (pld < best_pld - 32*FRACUNIT) { best_pld = pld; noprog = 0; }
	    else                                noprog++;
	    if (pld <= COOP_NEAR) { best_pld = pld; noprog = 0; trail_active = 0; }
	    else if (noprog >= 3)  trail_active = 1;		// ~3 s no closer -> trail/fallback
	    else if (noprog == 0)  trail_active = 0;		// gaining -> normal nav
	}
	else if (pld <= COOP_NEAR) trail_active = 0;		// reached the player
	if (pld < best_pld) best_pld = pld;			// keep the running minimum fresh
    }
    else
	trail_active = 0;

    // Yield (top priority): the human is bumping into us -> get out of the way by
    // stepping straight away from them.  Use forward+side move so we slide aside
    // immediately instead of slowly turning around (which keeps blocking).
    if (pl && P_AproxDistance (pl->x - mo->x, pl->y - mo->y) < YIELD_DIST)
    {
	unsigned fa = (R_PointToAngle2 (pl->x, pl->y, mo->x, mo->y) - mo->angle)
		      >> ANGLETOFINESHIFT;
	cmd->forwardmove =  (signed char)(FixedMul (COOP_RUN*FRACUNIT, finecosine[fa]) >> FRACBITS);
	cmd->sidemove    = -(signed char)(FixedMul (COOP_RUN*FRACUNIT, finesine[fa])   >> FRACBITS);
	triedmove  = 1;
	coop_state = 0;
	return;
    }

    tgt  = AICoop_FindTarget (mo);
    heal = (bot->health < COOP_HEAL_HP) ? AICoop_FindHealth (mo) : NULL;

    // Voice: automatic combat / hurt / all-clear callouts (rate-limited).
        // The phrases themselves live as OGG lumps in buddy.wad; we just hand
        // the tag prefix to AICoop_Callout, which appends the rotated index.
        {
    	static mobj_t*	lasttgt;
    	static int	lasthp = 100;
    	if (tgt && !lasttgt)				AICoop_Callout ("contact:", 4);
    	else if (!tgt && lasttgt)			AICoop_Callout ("clear:",   3);
    	if (bot->health < COOP_HEAL_HP && lasthp >= COOP_HEAL_HP) AICoop_Callout ("hurt:", 3);
    	lasttgt = tgt; lasthp = bot->health;
        }

    // Announce a newly picked-up weapon ("Buddy: got the shotgun!") -- text to the
    // human + a voice line (reusing the status:<weapon> phrase that names it).
    {
	static const char* wname[NUMWEAPONS] =
	    { "fists","pistol","shotgun","chaingun","rocket launcher",
	      "plasma rifle","BFG9000","chainsaw","super shotgun" };
	static const char* wtag[NUMWEAPONS] =
	    { "status:fists","status:pistol","status:shotgun","status:chaingun",
	      "status:rocketlauncher","status:plasma","status:bfg","status:chainsaw",
	      "status:supershotgun" };
	static int  ownedmask;
	static char gotmsg[64];
	int newmask = 0, wi;
	for (wi = 0 ; wi < NUMWEAPONS ; wi++)
	    if (bot->weaponowned[wi]) newmask |= (1 << wi);
	if (ownedmask && (newmask & ~ownedmask))		// skip the spawn loadout
	    for (wi = 0 ; wi < NUMWEAPONS ; wi++)
		if ((newmask & ~ownedmask) & (1 << wi))
		{
		    AICoop_SayTag (wtag[wi]);
		    snprintf (gotmsg, sizeof(gotmsg), "Buddy: got the %s!", wname[wi]);
		    if (playeringame[displayplayer]) players[displayplayer].message = gotmsg;
		    break;
		}
	ownedmask = newmask;
    }

    if (summon > 0)     summon--;
    if (forceaggro > 0) forceaggro--;
    if (ai_goto > 0)    ai_goto--;

    // "come" ends as soon as the buddy has reached the player (don't run the
    // whole timer once it's already next to you).
    if (summon > 0 && pl && P_AproxDistance (pl->x - mo->x, pl->y - mo->y) <= COOP_NEAR/2)
	summon = 0;

    coop_state = 0;

    // (attack) ordered: charge the forced target until it (or the timer) dies
    if (forceaggro > 0)
    {
	if (!forcetarget || forcetarget->health <= 0
	    || (forcetarget->flags & MF_CORPSE) || !(forcetarget->flags & MF_SHOOTABLE))
	    forcetarget = AICoop_FindTarget (mo);		// reacquire
	if (forcetarget)
	{
	    coop_state = 1; haveaim = 1; fire = 1; aimmon = forcetarget;
	    movethresh = COOP_KEEP;
	    tx = forcetarget->x; ty = forcetarget->y;
	}
	else
	    forceaggro = 0;
    }

    if (!haveaim)
    {
	// (LLM goto) ordered: move to a point, ignoring fights/items
	if (ai_goto > 0)
	{
	    coop_state = 4; haveaim = 1; movethresh = 24*FRACUNIT; navigate = 1;
	    tx = ai_gx; ty = ai_gy;
	}
	// (wait/stay) ordered: hold position; still face & fire at a monster
	else if (hold)
	{
	    coop_state = 3;
	    if (tgt) { coop_state = 1; haveaim = 1; fire = 1; aimmon = tgt; tx = tgt->x; ty = tgt->y; }
	    // else: no aim -> stand still
	}
	// (come) ordered: run to the player, ignoring fights/items
	else if (summon > 0 && pl)
	{
	    coop_state = 4; haveaim = 1; movethresh = COOP_NEAR/2; navigate = 1;
	    tx = pl->x; ty = pl->y;
	}
	// hurt: break off and grab the nearest med-pack
	else if (heal)
	{
	    coop_state = 2; haveaim = 1; movethresh = 16*FRACUNIT;
	    tx = heal->x; ty = heal->y;
	}
	// fight the nearest monster
	else if (tgt)
	{
	    coop_state = 1; haveaim = 1; fire = 1; aimmon = tgt;
	    movethresh = COOP_KEEP;
	    tx = tgt->x; ty = tgt->y;
	}
	// idle: collect a nearby item, but ONLY while still near the human (don't
	// wander off / linger for an item while the player walks away), and not one
	// we've been failing to grab (AICoop_GrabStuck) -- else follow the human.
	else if (pl
		 && P_AproxDistance (pl->x - mo->x, pl->y - mo->y) < COOP_GRAB_NEAR
		 && (item = AICoop_FindItem (mo)) != NULL
		 && !AICoop_GrabStuck (item))
	{
	    coop_state = 5; haveaim = 1; movethresh = 16*FRACUNIT;
	    tx = item->x; ty = item->y;
	}
	else if (pl)
	{
	    coop_state = 0; haveaim = 1; movethresh = COOP_NEAR; avoiddamage = 1; navigate = 1;
	    tx = pl->x; ty = pl->y;
	}
    }

    if (!haveaim)
	{ triedmove = 0; return; }		// nothing to do -> stand still

    // Breadcrumb override: when stuck reaching the player, replay the human's trail.
    // Steer STRAIGHT at the NEWEST crumb we can directly reach (closest to the player
    // on the human's actual path) -- not via the pathfinder, so we never detour the
    // wrong way around a wall, and each step is a verified-walkable hop toward them.
    int chase_player = 0;
    if (trail_active && pl && (coop_state == 0 || coop_state == 4))
    {
	int i, used = 0;
	for (i = crumb_n-1; i >= 0; i--)
	    if (AICoop_CanReach (mo, crumbx[i], crumby[i], false))
	    {
		tx = crumbx[i]; ty = crumby[i];
		navigate = 0; movethresh = 24*FRACUNIT;	// steer straight, no PF detour
		used = 1; break;
	    }
	// No reachable crumb (e.g. loaded a save where the human never laid a trail,
	// or a closed door cut the trail off): chase the human DIRECTLY -- keep
	// navigate on but aim its door-aware corner-rounding (FindDoorAhead + ChaseDir)
	// at the human instead of the oscillating BSP waypoint, so it rounds corners and
	// Uses shut doors on the way instead of grinding one wall.
	if (!used)
	{
	    tx = pl->x; ty = pl->y;
	    navigate = 1; chase_player = 1; movethresh = COOP_NEAR/2;
	}
    }

    // Navigate: if asked to walk somewhere, route there via the BSP pathfinder and
    // steer toward the next waypoint (re-pathed every ~10 tics / on goal change).
    // Combat aims directly (monsters are in sight), so it leaves stx/sty == tx/ty.
    stx = tx; sty = ty;
    if (navigate)
    {
	// Coarse route: BSP portal waypoint toward the player (cached, re-pathed ~3x/s).
	fixed_t goalx = tx, goaly = ty;
	int gss = PF_SS (tx, ty);
	if (--navtimer <= 0 || gss != navgoal)
	{
	    navtimer = 10; navgoal = gss;
	    navok = PF_NextWaypoint (mo, tx, ty, &navwx, &navwy);
	}
	if (navok && !chase_player) { goalx = navwx; goaly = navwy; }
	// A closed door on the way?  Head STRAIGHT at the doorway (no sweep) so we
	// enter the corridor toward it -- TraceSteer would sweep away from the shut
	// door (it reads as a wall) and the buddy would oscillate beside it forever.
	{
	    fixed_t	ddx, ddy;
	    if (AICoop_CanReach (mo, tx, ty, false))
	    {
		// The human is directly reachable -- go straight to them and ignore the
		// BSP waypoint.  Without this, a stale far waypoint (e.g. after the human
		// takes a teleporter/secret the graph doesn't model) makes the buddy leave
		// the human it's standing next to and loop back and forth.  avoiddmg=false:
		// follow the human even across nukage (e.g. MAP01's teleporter lands in it).
		stx = tx; sty = ty;
	    }
	    else if (AICoop_FindDoorAhead (mo, tx, ty, &ddx, &ddy)
		&& AICoop_CanReach (mo, ddx, ddy, true))
	    {
		stx = ddx; sty = ddy;		// doorway in reach -> head right at it + Use
	    }
	    else if (AICoop_CanReach (mo, goalx, goaly, true))
	    {
		stx = goalx; sty = goaly;	// clear straight shot -> head right at it
	    }
	    else
	    {
		// Waypoint (or an out-of-reach doorway) is behind a wall/corner: steering
		// straight at it just grinds the wall, so navigate the corner Doom-monster
		// style -- trial-walk the 8 compass headings toward the waypoint and commit
		// to the best one.  This walks us up to the doorway, where the branch above
		// then takes over and Use opens it.
		angle_t	cd = AICoop_ChaseDir (mo, goalx, goaly);
		angle_t	a  = cd >> ANGLETOFINESHIFT;
		stx = mo->x + FixedMul (96*FRACUNIT, finecosine[a]);
		sty = mo->y + FixedMul (96*FRACUNIT, finesine[a]);
	    }
	}
    }

    // turn toward the steer point (waypoint when navigating), clamped
    want  = R_PointToAngle2 (mo->x, mo->y, stx, sty);
    delta = want - mo->angle;
    rem   = (short)(delta >> 16);		// shortest signed turn (BAM>>16)
    turn  = rem;
    if (turn >  COOP_TURN) turn =  COOP_TURN;
    if (turn < -COOP_TURN) turn = -COOP_TURN;
    cmd->angleturn = (short)turn;

    dist = P_AproxDistance (tx - mo->x, ty - mo->y);

    if (fire && aimmon)
    {
	// Wrong weapon?  If holding the chainsaw/fist but the target is out of melee
	// reach, switch to a ranged weapon -- otherwise the buddy revs the saw at a foe
	// it can never touch (the bug: after picking up a chainsaw it got stuck on it,
	// revving at distant monsters).  The engine performs the switch via pendingweapon.
	if ((bot->readyweapon == wp_fist || bot->readyweapon == wp_chainsaw)
	    && bot->pendingweapon == wp_nochange
	    && P_AproxDistance (aimmon->x - mo->x, aimmon->y - mo->y) > 80*FRACUNIT)
	{
	    int w = AICoop_BestRanged (bot);
	    if (w >= 0) bot->pendingweapon = w;
	}

	// Splash-weapon suicide guard: a rocket/BFG fired at a target inside blast range
	// gibs the buddy too.  Switch to a non-splash weapon (BestRanged skips both) and
	// hold the shot until the swap lands.
	int splash_close = (bot->readyweapon == wp_missile || bot->readyweapon == wp_bfg)
		&& P_AproxDistance (aimmon->x - mo->x, aimmon->y - mo->y) < COOP_BLAST_SAFE;
	if (splash_close && bot->pendingweapon == wp_nochange)
	{
	    int w = AICoop_BestRanged (bot);
	    if (w >= 0) bot->pendingweapon = w;
	}

	// Reaction time (-buddyreact): wait a beat after sighting a *fresh* target before
	// opening fire, so the buddy isn't frame-perfect (0 = instant, the old behaviour).
	if (aimmon != react_last) { react_timer = buddy_react; react_last = aimmon; }
	if (react_timer > 0) react_timer--;

	// Aim vertically at the target's centre: if autoaim misses (target above or
	// below) the weapon falls back to lookdir ("shoot where you look"), so the
	// shot elevates instead of plugging the wall/crate in front.
	fixed_t	dz = (aimmon->z + (aimmon->height>>1)) - (mo->z + (mo->height>>1));
	fixed_t	hd = P_AproxDistance (aimmon->x - mo->x, aimmon->y - mo->y);
	int	ld = hd ? (int)((FixedDiv (dz, hd) * 160) >> FRACBITS) : 0;
	bot->lookdir = ld > COOP_LOOKMAX ? COOP_LOOKMAX : (ld < -COOP_LOOKMAX ? -COOP_LOOKMAX : ld);

	// Clear shot?  Autoaim-probe along the bearing: linetarget==NULL means the line
	// is blocked (e.g. the crate the monster stands on) or too steep to reach.  Then
	// don't waste ammo -- retreat to open the angle (it stays facing, so it fires the
	// moment the shot clears).  Capped so it doesn't back off into the next county.
	{
	    angle_t	aang = R_PointToAngle2 (mo->x, mo->y, aimmon->x, aimmon->y);
	    P_AimLineAttack (mo, aang, COOP_SIGHT);
	    if (linetarget && linetarget->player)
	    {
		// Friendly fire guard: the autoaim trace hits a PLAYER (the human is
		// between us and the monster) -- DON'T shoot.  Strafe a little to clear
		// the angle so the next tic has a safe shot.
		cmd->sidemove += ((gametic / 16) & 1) ? COOP_RUN : -COOP_RUN;
	    }
	    else if (linetarget && abs(rem) < COOP_FACING && react_timer == 0 && !splash_close)
		cmd->buttons |= BT_ATTACK;
	    else if (!linetarget && dist < 768*FRACUNIT)
		backoff = true;
	}

	// Damage-progress watchdog: remember the target's health, and while we're
	// actually firing at it count the tics; if its health hasn't dropped after a
	// few attacks' worth (~2s of fire) the shots aren't connecting -- blacklist it
	// so we switch to another target (or fall back to following) instead of
	// freezing here.  Any damage at all resets the window.
	if (aimmon != dmg_mon)
	{
	    dmg_mon = aimmon; dmg_hp0 = aimmon->health; dmg_firetics = 0;
	}
	else if (cmd->buttons & BT_ATTACK)
	{
	    if (aimmon->health < dmg_hp0)		// hurting it -> keep at it
	    {
		dmg_hp0 = aimmon->health; dmg_firetics = 0;
	    }
	    else if (++dmg_firetics >= 2*TICRATE)	// fired ~2s, no damage -> give up on it
	    {
		AICoop_Blacklist (aimmon);
		dmg_mon = NULL;
	    }
	}
    }
    else
	bot->lookdir = 0;

    if (AICoop_DamagingFloor (mo->x, mo->y) && pl)
    {
	// Standing in nukage/lava -- get OUT.  Bolt to the nearest human (on safe
	// ground) and never freeze here (the avoidance below would set triedmove=0
	// and the buddy would just stand in the hazard and die).
	want = R_PointToAngle2 (mo->x, mo->y, pl->x, pl->y);
	rem  = (short)((want - mo->angle) >> 16);
	turn = rem;
	if (turn >  COOP_TURN) turn =  COOP_TURN;
	if (turn < -COOP_TURN) turn = -COOP_TURN;
	cmd->angleturn   = (short)turn;
	AICoop_ThrustToward (cmd, mo, pl->x, pl->y);
	triedmove = 1;
    }
    else
    {
	triedmove = (movethresh >= 0 && dist > movethresh);

	// "Close enough, hold" uses straight-line distance -- but if the human is right
	// the other side of a wall / (secret) door, the gap is small yet not walkable.
	// So if we'd idle but can't actually walk straight to them, keep following the
	// route instead of parking on the wrong side of the door.
	if (!triedmove && navigate && navok && !AICoop_CanReach (mo, tx, ty, false))
	    triedmove = 1;

	// For low-priority moves (following), don't step onto a damaging floor --
	// check the actual move direction (toward the waypoint), not just facing.
	if (triedmove && avoiddamage)
	{
	    unsigned fa = R_PointToAngle2 (mo->x, mo->y, stx, sty) >> ANGLETOFINESHIFT;
	    fixed_t  ax = mo->x + FixedMul (32*FRACUNIT, finecosine[fa]);
	    fixed_t  ay = mo->y + FixedMul (32*FRACUNIT, finesine[fa]);
	    if (AICoop_DamagingFloor (ax, ay))
		triedmove = 0;
	}

	if (triedmove)
	    AICoop_ThrustToward (cmd, mo, stx, sty);	// straight to the waypoint
    }

    // Can't hit the target (it's above/behind a crate): back straight away from it
    // -- facing stays on it, so the buddy keeps aiming up and fires the instant the
    // angle opens.  Only onto safe, reachable ground (don't reverse into nukage/a
    // wall).  Overrides the combat advance above.
    if (backoff && aimmon)
    {
	angle_t	ra = R_PointToAngle2 (aimmon->x, aimmon->y, mo->x, mo->y) >> ANGLETOFINESHIFT;
	fixed_t	rx = mo->x + FixedMul (64*FRACUNIT, finecosine[ra]);
	fixed_t	ry = mo->y + FixedMul (64*FRACUNIT, finesine[ra]);
	if (!AICoop_DamagingFloor (rx, ry) && AICoop_CanReach (mo, rx, ry, true))
	{
	    AICoop_ThrustToward (cmd, mo, rx, ry);
	    triedmove = 1;
	}
    }

    // Wedged while trying to move -- most often a closed door on the path, else a
    // corner / a blocking thing (e.g. a barrel; we don't shoot those).
    if (triedmove && stuck)
    {
	static int wig;
	// Tap Use for a door in front (we already face the steer point, which is the
	// doorway when one is ahead).  Gated so we don't reverse a DR door mid-rise:
	// 45 tics > the ~32-tic open, so by the next tap the door is passable and we
	// are walking through (no longer stuck), so no second tap fires.
	if (doorwait == 0) { cmd->buttons |= BT_USE; doorwait = 45; }
	// Sideways wiggle to slip past a barrel / convex corner (non-door wedge).
	cmd->sidemove += ((wig++ / 24) & 1) ? COOP_RUN : -COOP_RUN;
    }

    // Missile dodge has the final say on movement: if a projectile is closing on us,
    // sidestep it (overriding the approach/backoff move for this tic).  Aim and fire
    // are separate fields, so the buddy keeps shooting while it strafes clear.
    AICoop_DodgeMissile (cmd, mo);
}
