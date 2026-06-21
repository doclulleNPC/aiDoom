// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Left-4-Dead-style AI Director (rule-based / offline, -director).
//
//	STRESS MODEL (intensity, 0..100, stored x100 in dir_acc):
//	  (1) damage taken  -- burst-weighted: damage in a short window steepens the
//	      curve, so a flurry of hits/grabs/acid spikes intensity hard.
//	  (2) close-quarters kills -- a kill within melee/shotgun range adds stress;
//	      sniping from afar adds ~none.
//	  (3) ammo -- low carried ammo (player + buddy) raises an intensity floor.
//	  decays every tic -> the "peak fade" that creates the tension rollercoaster.
//
//	FSM:  BUILDUP (spawn behind you, faster when calm) -> SUSTAIN (brief peak)
//	      -> FADE/RELAX (no spawns, drop items, wait for calm) -> BUILDUP.
//
//	Deterministic: runs in the tic flow via P_Random, so demos/netplay stay sync.
//
//-----------------------------------------------------------------------------

#include "doomdef.h"
#include "doomstat.h"
#include "m_argv.h"
#include "m_random.h"
#include "tables.h"
#include "p_local.h"
#include "r_state.h"
#include "info.h"
#include "p_ai_director.h"

// ---- tunables --------------------------------------------------------------
#define DIR_MAX			10000	// intensity*100 ceiling (= 100.00)
#define DIR_PEAK		6000	// -> SUSTAIN at 60
#define DIR_RELAX		2000	// -> BUILDUP again under 20
#define DIR_DECAY		8	// intensity*100 drained per tic (~2.8/s)
#define DIR_SUSTAIN_TICS	(4*TICRATE)	// hold the peak this long
#define DIR_MINRELAX_TICS	(30*TICRATE)	// minimum calm/loot time
#define DIR_MAXMON		40	// don't spawn past this many live monsters
#define DIR_SPAWN_TRIES		16	// candidate spots per spawn attempt
#define DIR_NEAR		(512*FRACUNIT)	// spawn distance band, min
#define DIR_FAR			(1280*FRACUNIT)	//  ... max

enum { DIR_BUILDUP, DIR_SUSTAIN, DIR_FADE };

static int	dir_on;			// -director given
static int	dir_acc;		// intensity * 100 (0..DIR_MAX)
static int	dir_state;
static int	dir_timer;		// SUSTAIN countdown
static int	dir_spawntic;		// tics until next monster spawn
static int	dir_recentdmg;		// damage in the recent window (burst), decaying
static int	dir_relaxstart;		// gametic FADE began

// "common" + "special" monster pools (specials lean in at the peak).
static const mobjtype_t dir_common[]  = { MT_POSSESSED, MT_SHOTGUY, MT_TROOP, MT_SERGEANT };
static const mobjtype_t dir_special[] = { MT_UNDEAD, MT_KNIGHT, MT_HEAD, MT_CHAINGUY };

int  P_Director_Active   (void) { return dir_on; }
int  P_Director_Intensity (void) { return dir_on ? dir_acc / 100 : 0; }

void P_Director_Init (void)
{
    dir_on = (M_CheckParm ("-director") > 0);
    if (dir_on)
	fprintf (stderr, "P_Director: rule-based L4D director ON (-director)\n");
}

void P_Director_Reset (void)
{
    dir_acc = 0; dir_state = DIR_BUILDUP; dir_timer = 0;
    dir_spawntic = 3*TICRATE; dir_recentdmg = 0; dir_relaxstart = 0;
}

// ---- stress feeds ----------------------------------------------------------

void P_Director_NoteDamage (mobj_t* victim, int damage)
{
    int	burst;
    if (!dir_on || damage <= 0 || !victim || !victim->player) return;
    dir_recentdmg += damage;
    burst = dir_recentdmg * 300 / 40;		// 0..300 (% extra) over a ~40-HP window
    if (burst > 300) burst = 300;
    dir_acc += damage * (100 + burst);		// 10 HP alone ~ +10.0; in a burst ~ +40.0
    if (dir_acc > DIR_MAX) dir_acc = DIR_MAX;
}

void P_Director_NoteKill (mobj_t* victim, mobj_t* killer)
{
    int	r, cq, w, wp;
    if (!dir_on || !victim || !killer || !killer->player) return;
    if (!(victim->flags & MF_COUNTKILL)) return;
    r  = (int)(P_AproxDistance (victim->x - killer->x, victim->y - killer->y) >> FRACBITS);
    cq = (r < 200) ? 100 : (r < 450) ? 40 : 0;	// close-quarters factor (sniping ~ 0)
    if (!cq) return;
    wp = killer->player->readyweapon;
    w  = (wp == wp_fist || wp == wp_chainsaw || wp == wp_shotgun || wp == wp_supershotgun) ? 150 : 100;
    dir_acc += cq * w * 5 / 100;		// close + shotgun ~ +7.5 per kill
    if (dir_acc > DIR_MAX) dir_acc = DIR_MAX;
}

// ---- helpers ---------------------------------------------------------------

// Average carried-ammo fill across survivors, 0 (empty) .. 100 (full).
static int P_Director_AmmoPct (void)
{
    int	i, a, total = 0, max = 0;
    for (i = 0; i < MAXPLAYERS; i++)
    {
	if (!playeringame[i]) continue;
	for (a = 0; a < NUMAMMO; a++) { total += players[i].ammo[a]; max += players[i].maxammo[a]; }
    }
    return max ? total * 100 / max : 100;
}

static mobj_t* P_Director_RandomSurvivor (void)
{
    mobj_t*	list[MAXPLAYERS];
    int		i, n = 0;
    for (i = 0; i < MAXPLAYERS; i++)
	if (playeringame[i] && players[i].mo && players[i].health > 0)
	    list[n++] = players[i].mo;
    return n ? list[P_Random () % n] : NULL;
}

static int P_Director_LiveMonsters (void)
{
    thinker_t*	th;
    int		n = 0;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t* m;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if ((m->flags & MF_COUNTKILL) && m->health > 0) n++;
    }
    return n;
}

static mobjtype_t P_Director_PickType (void)
{
    // Lean on specials at/near the peak, commons while building up.
    if ((dir_state == DIR_SUSTAIN || dir_acc > DIR_PEAK*3/4) && P_Random () < 110)
	return dir_special[P_Random () % (int)(sizeof(dir_special)/sizeof(dir_special[0]))];
    return dir_common[P_Random () % (int)(sizeof(dir_common)/sizeof(dir_common[0]))];
}

// Spawn one monster out of sight, in the distance band behind a survivor, and set
// it charging.  Bails quietly if no valid hidden spot is found.
static void P_Director_SpawnMonster (void)
{
    mobj_t*	sv;
    int		t;

    if (P_Director_LiveMonsters () >= DIR_MAXMON) return;
    sv = P_Director_RandomSurvivor ();
    if (!sv) return;

    for (t = 0; t < DIR_SPAWN_TRIES; t++)
    {
	angle_t		ang = (angle_t)(P_Random () << 24);
	int		fa  = ang >> ANGLETOFINESHIFT;
	fixed_t		dist = DIR_NEAR + (P_Random () * ((DIR_FAR - DIR_NEAR) >> 8));
	fixed_t		x = sv->x + FixedMul (dist, finecosine[fa]);
	fixed_t		y = sv->y + FixedMul (dist, finesine[fa]);
	mobjtype_t	mt = P_Director_PickType ();
	mobj_t*		mo;
	int		i, seen = 0;

	mo = P_SpawnMobj (x, y, ONFLOORZ, mt);
	// Fits where it landed (no wall/thing overlap, enough head room)?
	if (!P_CheckPosition (mo, x, y) || tmceilingz - tmfloorz < mo->height)
	    { P_RemoveMobj (mo); continue; }
	// Must be hidden from every survivor (L4D: spawn in the dark behind you).
	for (i = 0; i < MAXPLAYERS; i++)
	    if (playeringame[i] && players[i].mo && P_CheckSight (players[i].mo, mo))
		{ seen = 1; break; }
	if (seen) { P_RemoveMobj (mo); continue; }

	// Good -- send it after the survivor immediately.
	mo->target = sv;
	P_SetMobjState (mo, mo->info->seestate);
	return;
    }
}

static void P_Director_DropNear (mobj_t* sv, mobjtype_t mt)
{
    int		t;
    for (t = 0; t < DIR_SPAWN_TRIES; t++)
    {
	angle_t	ang = (angle_t)(P_Random () << 24);
	int	fa  = ang >> ANGLETOFINESHIFT;
	fixed_t	dist = (64 + (P_Random () & 127)) * FRACUNIT;	// 64..191u, within reach
	fixed_t	x = sv->x + FixedMul (dist, finecosine[fa]);
	fixed_t	y = sv->y + FixedMul (dist, finesine[fa]);
	mobj_t*	mo = P_SpawnMobj (x, y, ONFLOORZ, mt);
	if (P_CheckPosition (mo, x, y)) return;			// landed somewhere reachable
	P_RemoveMobj (mo);
    }
}

// Relax reward: a medkit + a box of bullets near a survivor.
static void P_Director_SpawnItems (void)
{
    mobj_t* sv = P_Director_RandomSurvivor ();
    if (!sv) return;
    P_Director_DropNear (sv, MT_MISC11);	// medikit
    P_Director_DropNear (sv, MT_MISC23);	// box of bullets
}

// ---- per-tic FSM -----------------------------------------------------------

void P_Director_Ticker (void)
{
    int	floor;

    if (!dir_on || gamestate != GS_LEVEL || paused) return;

    // burst window + intensity decay
    if (dir_recentdmg > 0) dir_recentdmg -= (dir_recentdmg >> 4) + 1;	// fades over ~1-2 s
    dir_acc -= DIR_DECAY;
    if (dir_acc < 0) dir_acc = 0;

    // low ammo keeps a gentle floor up (you feel exposed).  Capped BELOW DIR_RELAX so
    // FADE can always reach calm and cycle back to BUILDUP (else low ammo deadlocks it).
    floor = (100 - P_Director_AmmoPct ()) * 15 / 100;	// 0..15 intensity (x100 below)
    floor *= 100;
    if (dir_acc < floor) dir_acc = floor;

    switch (dir_state)
    {
      case DIR_BUILDUP:
	if (dir_acc >= DIR_PEAK)
	    { dir_state = DIR_SUSTAIN; dir_timer = DIR_SUSTAIN_TICS; }
	else if (--dir_spawntic <= 0)
	{
	    P_Director_SpawnMonster ();
	    // Faster trickle when calm (build tension), slower as intensity climbs.
	    dir_spawntic = 35 + (dir_acc / 100) * 2;	// ~1 s (calm) .. ~3 s (near peak)
	}
	break;

      case DIR_SUSTAIN:
	if (--dir_timer <= 0)
	    { dir_state = DIR_FADE; dir_relaxstart = gametic; P_Director_SpawnItems (); }
	else if (--dir_spawntic <= 0)
	    { P_Director_SpawnMonster (); dir_spawntic = 2*TICRATE; }	// light peak pressure
	break;

      case DIR_FADE:
	// No spawns -- let them breathe and loot until calm + the min relax elapses.
	if (dir_acc < DIR_RELAX && gametic - dir_relaxstart > DIR_MINRELAX_TICS)
	    { dir_state = DIR_BUILDUP; dir_spawntic = TICRATE; }
	break;
    }
}
