// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	AI-controlled co-op companion (player 2).  Enabled with -aicoop, which
//	marks playeringame[1] so the engine spawns a second marine at the map's
//	co-op start.  Each tic this builds that player's ticcmd with a small
//	built-in brain: acquire the nearest visible monster, turn to it and fire;
//	when hurt grab health; when idle collect nearby items; otherwise follow
//	the human.  It drives the player through the normal ticcmd path, so
//	weapons, damage, pickups and reborn all work like a real co-op peer.
//
//	Console commands (c_console.c) let the human direct it: where / come /
//	wait / attack / report.
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

static int	aicoop;			// -aicoop given
static int	coop_state;		// 0 follow 1 fight 2 heal 3 hold 4 come 5 items
static int	summon;			// "come": tics left running to the player
static int	hold;			// "wait/stay": hold position
static int	forceaggro;		// "attack": tics left charging forcetarget
static mobj_t*	forcetarget;		// the forced attack target

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
    if (!M_CheckParm ("-aicoop"))
	return;

    if (netgame)
    {
	printf ("P_AICoop: AI co-op buddy is single-player only -- disabled in netgames.\n");
	return;
    }

    coop_slot = 1;
    aicoop = 1;
    playeringame[1] = true;		// spawn the buddy at the co-op start
        printf ("P_AICoop: AI co-op companion enabled (player 2)\n");
}

// The slot the buddy occupies (-1 if disabled).  Used by g_game.c to skip the
// netgame consistency check for it -- the buddy is local-but-deterministic, never
// networked, so there is no remote command to validate against.
int P_AICoop_Slot (void)
{
    if (!aicoop) return -1;
    return coop_slot;
}

boolean P_AICoop_IsBuddy (player_t* p)
{
    return aicoop && p == &players[coop_slot];
}

// Public read-only accessor for coop_state (used by c_console.c for the voice
// tag mapping).  Returns -1 if the buddy is inactive.
int P_AICoop_State (void)
{
    if (!aicoop) return -1;
    return coop_state;
}

// The live companion mobj, or NULL if there isn't one right now.
static mobj_t* AICoop_Mo (void)
{
    if (!aicoop || !playeringame[coop_slot])		return NULL;
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
    if (!aicoop || !tag) return;
    AICoop_SayTag (tag);
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
	return "[Buddy] (no companion -- launch with -aicoop)";
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
	return "[Buddy] (no companion -- launch with -aicoop)";
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
	return "[Buddy] (no companion -- launch with -aicoop)";
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

const char* P_AICoop_StatusReport (void)
{
    static char		buf[120];
    static const char*	wn[NUMWEAPONS] = { "fists","pistol","shotgun","chaingun",
				"rocket launcher","plasma rifle","BFG9000","chainsaw","super shotgun" };
    player_t*	bot = &players[coop_slot];
    int		w, am;

    if (!AICoop_Mo ())
	return "[Buddy] (no companion -- launch with -aicoop)";
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

static void PF_AddEdge (int u, int v, int w)
{
    int	k;
    if (u < 0 || v < 0 || u == v || w < 0) return;
    for (k = 0; k < pf_nadj[u]; k++)			// dedup, keep cheapest
	if (pf_adj[u*PF_MAXADJ + k] == v)
	{ if (w < pf_adjw[u*PF_MAXADJ + k]) pf_adjw[u*PF_MAXADJ + k] = w; return; }
    if (pf_nadj[u] >= PF_MAXADJ) return;
    pf_adj [u*PF_MAXADJ + pf_nadj[u]] = v;
    pf_adjw[u*PF_MAXADJ + pf_nadj[u]] = w;
    pf_nadj[u]++;
}

static void PF_Build (mobj_t* ref)
{
    int		i, j, s;
    int*	segss;

    free (pf_cx); free (pf_cy); free (pf_nadj); free (pf_adj); free (pf_adjw);
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
	if (w >= 0) PF_AddEdge (self, v, w);
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
		PF_AddEdge (a, b, w + (AICoop_DamagingFloor (pf_cx[b], pf_cy[b]) ? PF_HAZARD_PEN : 0));
		PF_AddEdge (b, a, w + (AICoop_DamagingFloor (pf_cx[a], pf_cy[a]) ? PF_HAZARD_PEN : 0));
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

static boolean PF_Dijkstra (int start, int goal)
{
    int	i, pop = 0;

    for (i = 0; i < pf_n; i++) { pf_dist[i] = PF_INF; pf_prev[i] = -1; pf_done[i] = 0; }
    pf_dist[start] = 0;

    while (pop < PF_MAXPOP)
    {
	int		u = -1, best = PF_INF, s;

	for (i = 0; i < pf_n; i++)			// pop nearest unvisited
	    if (!pf_done[i] && pf_dist[i] < best) { best = pf_dist[i]; u = i; }
	if (u < 0) break;				// nothing left reachable
	if (u == goal) return true;
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
    int	start, goal, len, i, pick, c;

    if (pf_level != gameepisode*100 + gamemap || !pf_cx)
    { PF_Build (mo); pf_level = gameepisode*100 + gamemap; pf_lastbuild = gametic; }

    start = PF_SS (mo->x, mo->y);
    goal  = PF_SS (dx, dy);
    if (start == goal) { *wx = dx; *wy = dy; return true; }
    if (!PF_Dijkstra (start, goal))
    {
	// No route -- the graph is built once at level start, so a door/lift/secret
	// wall that has since OPENED isn't in it yet (it had no passable edge when
	// built).  Rebuild from the current map state and retry (rate-limited to
	// ~1.5s so a genuinely unreachable target doesn't thrash).
	if (gametic - pf_lastbuild > 50)
	{
	    PF_Build (mo); pf_lastbuild = gametic;
	    if (!PF_Dijkstra (start, goal)) return false;
	}
	else
	    return false;
    }

    // reconstruct goal -> ... -> first step (pf_path[len-1] is adjacent to start)
    len = 0; c = goal;
    while (c != -1 && c != start && len < PF_PATHMAX) { pf_path[len++] = c; c = pf_prev[c]; }
    if (len == 0) { *wx = dx; *wy = dy; return true; }

    // string pull: steer to the furthest path node reachable in a straight walk
    pick = len - 1;					// fallback: the first step
    for (i = 0; i < len; i++)				// i=0 is the goal end
    {
	fixed_t px = (i == 0) ? dx : pf_cx[pf_path[i]];
	fixed_t py = (i == 0) ? dy : pf_cy[pf_path[i]];
	if (AICoop_CanReach (mo, px, py, true)) { pick = i; break; }
    }
    if (pick == 0) { *wx = dx; *wy = dy; }
    else { *wx = pf_cx[pf_path[pick]]; *wy = pf_cy[pf_path[pick]]; }
    return true;
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

    if (!aicoop || !playeringame[coop_slot])
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
	// (wait/stay) ordered: hold position; still face & fire at a monster
	if (hold)
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

    // Navigate: if asked to walk somewhere, route there via the BSP pathfinder and
    // steer toward the next waypoint (re-pathed every ~10 tics / on goal change).
    // Combat aims directly (monsters are in sight), so it leaves stx/sty == tx/ty.
    stx = tx; sty = ty;
    if (navigate)
    {
	// Coarse route: BSP waypoint toward the player (cached, re-pathed ~3x/s).
	fixed_t goalx = tx, goaly = ty;
	int gss = PF_SS (tx, ty);
	if (--navtimer <= 0 || gss != navgoal)
	{
	    navtimer = 10; navgoal = gss;
	    navok = PF_NextWaypoint (mo, tx, ty, &navwx, &navwy);
	}
	if (navok) { goalx = navwx; goaly = navwy; }
	// A closed door on the way?  Head STRAIGHT at the doorway (no sweep) so we
	// enter the corridor toward it -- TraceSteer would sweep away from the shut
	// door (it reads as a wall) and the buddy would oscillate beside it forever.
	{
	    fixed_t	ddx, ddy;
	    if (AICoop_FindDoorAhead (mo, tx, ty, &ddx, &ddy)
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
	    if (linetarget && abs(rem) < COOP_FACING)
		cmd->buttons |= BT_ATTACK;
	    else if (!linetarget && dist < 768*FRACUNIT)
		backoff = true;
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
}
