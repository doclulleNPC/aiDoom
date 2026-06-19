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
#define COOP_NEAR	(256*FRACUNIT)	// follow distance to the human
#define YIELD_DIST	(48*FRACUNIT)	// human this close -> step out of the way
#define COOP_KEEP	(192*FRACUNIT)	// advance toward a monster until this close
#define COOP_RUN	0x32		// forwardmove "run" magnitude
#define COOP_HEAL_HP	50		// seek a med-pack below this health
#define COOP_HEAL_RANGE	(1024*FRACUNIT)	// how far to look for one
#define COOP_ITEM_RANGE	(1024*FRACUNIT)	// how far to look for an idle pickup
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

    if (dist < 24*FRACUNIT)
	return true;				// practically there
    steps = dist / (24*FRACUNIT);
    if (steps > 64)
	return false;				// too far -- don't bother (bounds cost)

    for (i = 1; i <= steps; i++)
    {
	fixed_t	frac = (i << 16) / steps;	// i/steps as 16.16
	fixed_t	px   = self->x + FixedMul (dx, frac);
	fixed_t	py   = self->y + FixedMul (dy, frac);

	if (!P_CheckPosition (self, px, py))		return false;	// wall/obstacle
	if (tmceilingz - tmfloorz < 56*FRACUNIT)	return false;	// won't fit
	if (tmfloorz - fz > 24*FRACUNIT)		return false;	// step up too high
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
#define PF_MAXPOP	3000		// cap on Dijkstra node expansions
#define PF_PATHMAX	256		// max sub-sectors in a reconstructed path
#define PF_INF		0x7fffffff

static int	pf_level = -1;		// episode*100+map the graph was built for
static int	pf_n;			// sub-sector count
static fixed_t*	pf_cx;			// centroid x / y per sub-sector
static fixed_t*	pf_cy;
static int*	pf_segss;		// sub-sector each seg belongs to
static int*	pf_segnb;		// neighbour sub-sector across each seg (-1 none)
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

static void PF_Build (void)
{
    int	i, j;

    free (pf_cx); free (pf_cy); free (pf_segss); free (pf_segnb);
    free (pf_dist); free (pf_prev); free (pf_done);

    pf_n     = numsubsectors;
    pf_cx    = malloc (pf_n * sizeof(fixed_t));
    pf_cy    = malloc (pf_n * sizeof(fixed_t));
    pf_dist  = malloc (pf_n * sizeof(int));
    pf_prev  = malloc (pf_n * sizeof(int));
    pf_done  = malloc (pf_n);
    pf_segss = malloc (numsegs * sizeof(int));
    pf_segnb = malloc (numsegs * sizeof(int));

    // centroid of each sub-sector (mean of its segs' endpoints) + seg->ss map
    for (i = 0; i < pf_n; i++)
    {
	subsector_t*	ss = &subsectors[i];
	int64_t		sx = 0, sy = 0;
	int		cnt = 0, s;
	for (s = 0; s < ss->numlines; s++)
	{
	    seg_t* sg = &segs[ss->firstline + s];
	    sx += sg->v1->x; sy += sg->v1->y;
	    sx += sg->v2->x; sy += sg->v2->y;
	    cnt += 2;
	    pf_segss[ss->firstline + s] = i;
	}
	pf_cx[i] = cnt ? (fixed_t)(sx / cnt) : 0;
	pf_cy[i] = cnt ? (fixed_t)(sy / cnt) : 0;
    }

    // neighbour sub-sector across each two-sided seg: probe a point ~4 units off
    // the seg midpoint on each side; the side that isn't our own sub-sector is it
    for (j = 0; j < numsegs; j++)
    {
	seg_t*	sg = &segs[j];
	fixed_t	mx, my, nx, ny, len, ox, oy;
	int	a, b, self;

	pf_segnb[j] = -1;
	if (!sg->backsector) continue;			// one-sided wall

	mx = (sg->v1->x + sg->v2->x) >> 1;
	my = (sg->v1->y + sg->v2->y) >> 1;
	nx = -(sg->v2->y - sg->v1->y);			// perpendicular to the seg
	ny =  (sg->v2->x - sg->v1->x);
	len = P_AproxDistance (nx, ny);
	if (len < FRACUNIT) continue;
	ox = (fixed_t)(((int64_t)nx * (4*FRACUNIT)) / len);	// ~4-unit offset
	oy = (fixed_t)(((int64_t)ny * (4*FRACUNIT)) / len);

	self = pf_segss[j];
	a = PF_SS (mx + ox, my + oy);
	b = PF_SS (mx - ox, my - oy);
	pf_segnb[j] = (a != self) ? a : (b != self ? b : -1);
    }
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
	subsector_t*	ss;

	for (i = 0; i < pf_n; i++)			// pop nearest unvisited
	    if (!pf_done[i] && pf_dist[i] < best) { best = pf_dist[i]; u = i; }
	if (u < 0) break;				// nothing left reachable
	if (u == goal) return true;
	pf_done[u] = 1; pop++;

	ss = &subsectors[u];
	for (s = 0; s < ss->numlines; s++)
	{
	    int	segi = ss->firstline + s;
	    int	v = pf_segnb[segi];
	    int	w;
	    if (v < 0 || pf_done[v]) continue;
	    w = PF_EdgeWeight (&segs[segi], u, v);
	    if (w < 0) continue;
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
    { PF_Build (); pf_level = gameepisode*100 + gamemap; }

    start = PF_SS (mo->x, mo->y);
    goal  = PF_SS (dx, dy);
    if (start == goal) { *wx = dx; *wy = dy; return true; }
    if (!PF_Dijkstra (start, goal)) return false;

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
    boolean	stuck;
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

    // Progress check (movement applied last tic): if we asked to move but barely
    // moved, we're stuck -- maybe against a door.
    stuck = (triedmove && P_AproxDistance (mo->x - lastx, mo->y - lasty) < 2*FRACUNIT);
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
	// idle: collect a nearby item, else follow the human
	else if ((item = AICoop_FindItem (mo)) != NULL)
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
	int gss = PF_SS (tx, ty);
	if (--navtimer <= 0 || gss != navgoal)
	{
	    navtimer = 10; navgoal = gss;
	    navok = PF_NextWaypoint (mo, tx, ty, &navwx, &navwy);
	}
	if (navok) { stx = navwx; sty = navwy; }
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

    if (fire && aimmon && abs(rem) < COOP_FACING && P_CheckSight (mo, aimmon))
	cmd->buttons |= BT_ATTACK;

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
	cmd->forwardmove = COOP_RUN;
	triedmove = 1;
    }
    else
    {
	triedmove = (movethresh >= 0 && dist > movethresh);

	// For low-priority moves (following), don't step onto a damaging floor.
	if (triedmove && avoiddamage)
	{
	    unsigned fa = mo->angle >> ANGLETOFINESHIFT;
	    fixed_t  ax = mo->x + FixedMul (32*FRACUNIT, finecosine[fa]);
	    fixed_t  ay = mo->y + FixedMul (32*FRACUNIT, finesine[fa]);
	    if (AICoop_DamagingFloor (ax, ay))
		triedmove = 0;
	}

	if (triedmove)
	    cmd->forwardmove = COOP_RUN;
    }

    // Doors: if we're trying to move but stuck (e.g. pushing a closed door), tap
    // Use *once*, then leave it alone for a bit -- spamming Use re-triggers a DR
    // door every tic and it just bounces open/shut.  Monsters open doors the same
    // way (in P_Move); here we do it through the player's Use line.
    if (triedmove && stuck && doorwait == 0)
    {
	cmd->buttons |= BT_USE;
	doorwait = 40;				// ~ door open time; no Use until then
    }
}
