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

#include <string.h>
#include "doomdef.h"
#include "doomstat.h"
#include "m_argv.h"
#include "m_random.h"
#include "tables.h"
#include "p_local.h"
#include "r_state.h"
#include "info.h"
#include "p_ai_director.h"
#include "i_voice.h"			// I_Director_Say -- the spoken game-master persona

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

#define DIR_EMERG_HP		25	// drop an emergency medkit when a survivor is below this
#define DIR_EMERG_NEAR		(768*FRACUNIT)	// ...and no medkit already within this
#define DIR_EMERG_COOLDOWN	(20*TICRATE)	// at most one emergency medkit per this
#define DIR_EMERG_AMMO		30	// drop emergency ammo when total ammo is below this %
#define DIR_EMERG_AMMO_COOLDOWN	(15*TICRATE)	// at most one emergency ammo drop per this
#define DIR_LLM_FALLBACK	(15*TICRATE)	// FSM resumes if the LLM hasn't spawned in this

static int	dir_on;			// -director given (rule FSM active)
static int	dir_llm;		// -aidirector/-aidemo: LLM drives spawns, FSM = fallback
static int	dir_acc;		// intensity * 100 (0..DIR_MAX)
static int	dir_state;
static int	dir_timer;		// SUSTAIN countdown
static int	dir_spawntic;		// tics until next monster spawn
static int	dir_recentdmg;		// damage in the recent window (burst), decaying
static int	dir_relaxstart;		// gametic FADE began
static int	dir_emergtic;		// gametic of the last emergency medkit
static int	dir_emergammotic;	// gametic of the last emergency ammo drop
static int	dir_llmlast;		// gametic of the last LLM spawn/relax command
static int	dir_voicelast;		// gametic of the last spoken director line
static int	dir_vidx;		// rotates spoken-line variants
static int	dir_intro;		// 1 = speak the level-start line on the next tic
static fixed_t	dir_exit_x, dir_exit_y;	// level exit location (for exit-proximity pressure)
static boolean	dir_exit_set;		// found an exit linedef on this map?
static int	dir_exit_found;		// 0 until P_Director_FindExit has run this level

static int     P_Director_ExitProximity (void);	// 0..100, nearer exit = more
static boolean P_Director_Stressed (void);	// player critically low hp/ammo?

// "common" + "special" monster pools (specials lean in at the peak).  DOOM2-only
// monsters (revenant/mancubus/hell knight/chaingunner/...) have NO sprites in a
// DOOM1 IWAD, so spawning one there crashes the renderer -- hence a DOOM1-safe
// special pool and a DOOM2 one, gated by gamemode (see P_Director_SafeType).
static const mobjtype_t dir_common[]   = { MT_POSSESSED, MT_SHOTGUY, MT_TROOP, MT_SERGEANT };
static const mobjtype_t dir_special1[] = { MT_HEAD, MT_BRUISER, MT_SHADOWS, MT_SKULL };           // DOOM1-safe: caco, baron, spectre, lost soul
static const mobjtype_t dir_special2[] = { MT_UNDEAD, MT_FATSO, MT_KNIGHT, MT_BABY, MT_CHAINGUY, MT_HEAD }; // DOOM2: revenant, mancubus, hell knight, arachnotron, chaingunner, caco

#define DIR_TRACK	(dir_on || dir_llm)	// intensity is tracked in either mode

// ---- spoken "game-master" lines (DD* lumps via the dir:* tags / I_Director_Say) ---
// Rate-limited so the announcer stays a voice-of-god, not a chatterbox.  prefix is
// e.g. "dir:horde"; one of n variants is appended ("dir:horde:2").
//   force=1: an important event (phase change / item drop) -- barge in over any
//            line in progress and ignore the cooldown.
//   force=0: ambient flavour -- only when the director is idle and the gap elapsed.
#define DIR_VOICE_GAP	(6*TICRATE)	// min gap between ambient lines
static void P_Director_Voice (const char* prefix, int n, int force)
{
    char tag[40];
    if (!DIR_TRACK) return;			// director not running -> silent
    if (force)
	I_Director_Stop ();			// important line preempts whatever's playing
    else if (I_Director_Busy () || gametic - dir_voicelast < DIR_VOICE_GAP)
	return;
    dir_voicelast = gametic;
    snprintf (tag, sizeof tag, "%s:%d", prefix, (dir_vidx++) % n);
    I_Director_Say (tag, 127, 127);
}

// Public entry so other modules can make the Director speak a line: the LLM order
// handler (tactics) and G_ExitLevel (level clear).  Thin wrapper over the
// rate-limited internal P_Director_Voice.
void P_Director_Say (const char* prefix, int n, int force)
{
    P_Director_Voice (prefix, n, force);
}

int  P_Director_Active    (void) { return dir_on; }
int  P_Director_Intensity (void) { return DIR_TRACK ? dir_acc / 100 : 0; }
int  P_Director_State     (void) { return dir_state; }
int  P_Director_RecentDmg (void) { return dir_recentdmg; }

void P_Director_Init (void)
{
    dir_on  = (M_CheckParm ("-director") > 0);
    dir_llm = (M_CheckParm ("-aidirector") > 0) || (M_CheckParm ("-aidemo") > 0);
    if (dir_on)
	fprintf (stderr, "P_Director: rule-based L4D director ON (-director)\n");
    else if (dir_llm)
	fprintf (stderr, "P_Director: stress tracking ON for the LLM director (-aidirector)\n");
}

void P_Director_Reset (void)
{
    dir_acc = 0; dir_state = DIR_BUILDUP; dir_timer = 0;
    dir_spawntic = 3*TICRATE; dir_recentdmg = 0; dir_relaxstart = 0;
    dir_emergtic = gametic - DIR_EMERG_COOLDOWN;	// allow an emergency medkit immediately
    dir_emergammotic = gametic - DIR_EMERG_AMMO_COOLDOWN;	// ...and emergency ammo
    dir_llmlast = 0;
    dir_voicelast = gametic - DIR_VOICE_GAP;	// allow a line immediately
    dir_intro = 1;				// the Director greets you on the first tic
    dir_exit_found = 0; dir_exit_set = false;	// re-locate the exit on the new map
}

// ---- stress feeds ----------------------------------------------------------

void P_Director_NoteDamage (mobj_t* victim, int damage)
{
    int	burst;
    if (!DIR_TRACK || damage <= 0 || !victim || !victim->player) return;
    dir_recentdmg += damage;
    burst = dir_recentdmg * 300 / 40;		// 0..300 (% extra) over a ~40-HP window
    if (burst > 300) burst = 300;
    dir_acc += damage * (100 + burst);		// 10 HP alone ~ +10.0; in a burst ~ +40.0
    if (dir_acc > DIR_MAX) dir_acc = DIR_MAX;

    // The Director gloats when the human is freshly driven to critical health.
    if (victim->player == &players[0] && victim->player->health > 0
	&& victim->player->health < 20)
    {
	static int downtic;
	if (gametic - downtic > 8*TICRATE) { P_Director_Voice ("dir:down", 3, 0); downtic = gametic; }
    }
}

void P_Director_NoteKill (mobj_t* victim, mobj_t* killer)
{
    int	r, cq, w, wp;
    if (!DIR_TRACK || !victim || !killer || !killer->player) return;
    if (!(victim->flags & MF_COUNTKILL)) return;
    // The human (not the buddy) on a kill-streak -> the Director acknowledges it.
    if (killer->player == &players[0])
    {
	static int streak, streaktic;
	if (gametic - streaktic < 3*TICRATE) streak++; else streak = 1;
	streaktic = gametic;
	if (streak >= 6) { P_Director_Voice ("dir:spree", 3, 0); streak = 0; }
    }
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
int P_Director_AmmoPct (void)
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
    const mobjtype_t* spec = (gamemode == commercial) ? dir_special2 : dir_special1;
    int nspec = (gamemode == commercial)
	      ? (int)(sizeof(dir_special2)/sizeof(dir_special2[0]))
	      : (int)(sizeof(dir_special1)/sizeof(dir_special1[0]));
    // More high-tier pressure: specials lean in from mid-intensity (was 3/4 peak)
    // and more often (was ~43%).
    // Specials/bosses lean in at high intensity -- but NOT when the player is
    // critically low on health or ammo (don't drop a Cyberdemon on a dying player).
    if ((dir_state == DIR_SUSTAIN || dir_acc > DIR_PEAK/2) && P_Random () < 150
	&& !P_Director_Stressed ())
    {
	P_Director_Voice ("dir:big", 3, 0);	// a special leans in (ambient, rate-limited)
	return spec[P_Random () % nspec];
    }
    return dir_common[P_Random () % (int)(sizeof(dir_common)/sizeof(dir_common[0]))];
}

// DOOM2-only monsters have no sprites in a DOOM1 IWAD (-> renderer I_Error).  In
// non-commercial gamemodes substitute a tough DOOM1 stand-in (baron); otherwise
// pass the type through.  Guards BOTH the rule FSM and the LLM spawn path.
static mobjtype_t P_Director_SafeType (mobjtype_t mt)
{
    if (gamemode == commercial) return mt;
    switch (mt)
    {
      case MT_CHAINGUY: case MT_UNDEAD: case MT_FATSO: case MT_BABY:
      case MT_PAIN:     case MT_KNIGHT: case MT_VILE:  case MT_WOLFSS: case MT_KEEN:
	return MT_BRUISER;	// baron of hell -- present in every DOOM1 IWAD
      default:
	return mt;
    }
}

// Human-readable name for notable monsters (NULL = common trash, no callout).
static const char* P_Director_MonName (mobjtype_t mt)
{
    switch (mt)
    {
      case MT_UNDEAD:   return "Revenant";
      case MT_FATSO:    return "Mancubus";
      case MT_KNIGHT:   return "Hell Knight";
      case MT_BRUISER:  return "Baron of Hell";
      case MT_BABY:     return "Arachnotron";
      case MT_HEAD:     return "Cacodemon";
      case MT_CHAINGUY: return "Chaingunner";
      case MT_VILE:     return "Arch-Vile";
      case MT_SPIDER:   return "Spider Mastermind";
      case MT_CYBORG:   return "Cyberdemon";
      default:          return NULL;
    }
}

// Flash a HUD message to the player (the director "announcing" a spawn).
static void P_Director_Announce (const char* msg)
{
    static char buf[80];
    if (!msg || consoleplayer < 0 || !playeringame[consoleplayer]) return;
    snprintf (buf, sizeof buf, "%s", msg);
    players[consoleplayer].message = buf;
}

// Spawn one monster of type `mt` out of sight, in the distance band around (cx,cy),
// and set it charging the nearest survivor.  cx==cy==0 -> centre on that survivor
// (the default "in the dark behind you").  Pass the exit point to populate the room
// the player is heading for.  Bails quietly if no valid hidden spot is found.
static void P_Director_SpawnMonsterNear (mobjtype_t mt, fixed_t cx, fixed_t cy)
{
    mobj_t*	sv;
    int		t;
    static int	lastname;	// rate-limit the per-monster "you hear a X" callout

    mt = P_Director_SafeType (mt);	// never spawn an IWAD-absent monster (crash guard)
    if (P_Director_LiveMonsters () >= DIR_MAXMON) return;
    sv = P_Director_RandomSurvivor ();
    if (!sv) return;
    if (!cx && !cy) { cx = sv->x; cy = sv->y; }		// default: around the survivor

    for (t = 0; t < DIR_SPAWN_TRIES; t++)
    {
	angle_t		ang = (angle_t)(P_Random () << 24);
	int		fa  = ang >> ANGLETOFINESHIFT;
	fixed_t		dist = DIR_NEAR + (P_Random () * ((DIR_FAR - DIR_NEAR) >> 8));
	fixed_t		x = cx + FixedMul (dist, finecosine[fa]);
	fixed_t		y = cy + FixedMul (dist, finesine[fa]);
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
	// (F) announce a notable monster (rate-limited; common trash stays silent).
	{
	    const char* nm = P_Director_MonName (mt);
	    if (nm && gametic - lastname > 4*TICRATE)
	    {
		char m[80]; snprintf (m, sizeof m, "You hear a %s nearby...", nm);
		P_Director_Announce (m);
		lastname = gametic;
	    }
	}
	return;
    }
}

// Default placement: in the dark behind a survivor (the original behaviour).
static void P_Director_SpawnMonsterOf (mobjtype_t mt) { P_Director_SpawnMonsterNear (mt, 0, 0); }

// ---- exit-aware pressure ---------------------------------------------------
// More heat the closer the player gets to the level exit, and the exit room is
// pre-populated so it's a defended objective rather than a free walk-out.

// Locate the level's exit and remember where it is (midpoint of the first exit
// linedef).  Specials: 11 S1-Exit, 51 S1-Secret, 52 W1-Exit, 124 W1-Secret.
static void P_Director_FindExit (void)
{
    int i;
    dir_exit_set = false;
    for (i = 0; i < numlines; i++)
    {
	int sp = lines[i].special;
	if (sp == 11 || sp == 51 || sp == 52 || sp == 124)
	{
	    dir_exit_x = (lines[i].v1->x + lines[i].v2->x) / 2;
	    dir_exit_y = (lines[i].v1->y + lines[i].v2->y) / 2;
	    dir_exit_set = true;
	    return;
	}
    }
}

// 0..100: how close the nearest survivor is to the exit (100 = on top of it).
// 0 if there's no exit on this map.  Ramps over DIR_EXIT_FAR..DIR_EXIT_NEAR.
#define DIR_EXIT_NEAR	(384*FRACUNIT)
#define DIR_EXIT_FAR	(2048*FRACUNIT)
static int P_Director_ExitProximity (void)
{
    int		i;
    fixed_t	best = 0x7fffffff;
    if (!dir_exit_set) return 0;
    for (i = 0; i < MAXPLAYERS; i++)
    {
	if (!playeringame[i] || !players[i].mo || players[i].health <= 0) continue;
	fixed_t d = P_AproxDistance (players[i].mo->x - dir_exit_x,
				     players[i].mo->y - dir_exit_y);
	if (d < best) best = d;
    }
    if (best >= DIR_EXIT_FAR)  return 0;
    if (best <= DIR_EXIT_NEAR) return 100;
    return (int)(100 * (DIR_EXIT_FAR - best) / (DIR_EXIT_FAR - DIR_EXIT_NEAR));
}

// "Don't kick a player who's down": true when a survivor is critically low on
// health OR the squad's ammo is nearly out.  Used to suppress hordes and
// special/boss spawns so the pressure eases instead of becoming a death spiral.
static boolean P_Director_Stressed (void)
{
    int i;
    if (P_Director_AmmoPct () < DIR_EMERG_AMMO) return true;
    for (i = 0; i < MAXPLAYERS; i++)
	if (playeringame[i] && players[i].mo && players[i].health > 0
	    && players[i].health < DIR_EMERG_HP)
	    return true;
    return false;
}

static void P_Director_SpawnMonster (void)
{
    mobjtype_t	mt  = P_Director_PickType ();
    int		exf = P_Director_ExitProximity ();
    // As the player nears the exit, route more spawns INTO the exit room ahead of
    // them; otherwise spawn in the dark behind them as usual.
    if (dir_exit_set && (P_Random () % 100) < exf)
	P_Director_SpawnMonsterNear (mt, dir_exit_x, dir_exit_y);
    else
	P_Director_SpawnMonsterOf (mt);
    P_Director_Voice ("dir:spawn", 2, 0);	// occasional "reinforcements" (rate-limited)
}

// (E) Spawn a horde -- a burst of `count` monsters of one type at once -- and (F)
// announce it.  Used occasionally during build-up/peak for real pressure.
static void P_Director_SpawnWave (int count)
{
    mobjtype_t	mt = P_Director_PickType ();
    int		i;
    for (i = 0; i < count; i++) P_Director_SpawnMonsterOf (mt);
    P_Director_Announce ("A horde is closing in!");	// after, so it wins the HUD line
    P_Director_Voice ("dir:horde", 3, 1);		// the Director relishes it
}

// Is a medkit/stimpack already lying within `range` of (x,y)?  (emergency-medkit guard)
static boolean P_Director_MedkitNear (fixed_t x, fixed_t y, fixed_t range)
{
    thinker_t*	th;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t* m;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if ((m->type == MT_MISC10 || m->type == MT_MISC11)
	    && P_AproxDistance (m->x - x, m->y - y) < range)
	    return true;
    }
    return false;
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

// Relax reward: a medkit + a generous ammo resupply near a survivor.
static void P_Director_SpawnItems (void)
{
    mobj_t* sv = P_Director_RandomSurvivor ();
    if (!sv) return;
    P_Director_DropNear (sv, MT_MISC11);	// medikit
    P_Director_DropNear (sv, MT_MISC17);	// box of bullets
    P_Director_DropNear (sv, MT_MISC23);	// box of shells
    P_Director_DropNear (sv, MT_MISC19);	// box of rockets
    // Ambient (force=0): at the normal FADE transition the "relax" line is spoken
    // just before this, so leave it playing; "gift" only speaks on other drop paths.
    P_Director_Voice ("dir:gift", 3, 0);
}

// ---- per-tic FSM -----------------------------------------------------------

void P_Director_Ticker (void)
{
    int	floor, i, runfsm;

    if (!DIR_TRACK || gamestate != GS_LEVEL || paused) return;

    // Locate the level exit once (lines[] is loaded by now) for exit-proximity pressure.
    if (!dir_exit_found) { dir_exit_found = 1; P_Director_FindExit (); }

    // Director voice: greet the player once on the first in-level tic, then drop
    // occasional menace during long lulls (low intensity, nothing else spoken).
    if (dir_intro)
	{ dir_intro = 0; P_Director_Voice ("dir:start", 3, 1); }
    else if (dir_acc < DIR_RELAX && gametic - dir_voicelast > 25*TICRATE)
	P_Director_Voice ("dir:idle", 3, 0);

    // burst window + intensity decay
    if (dir_recentdmg > 0) dir_recentdmg -= (dir_recentdmg >> 4) + 1;	// fades over ~1-2 s
    dir_acc -= DIR_DECAY;
    if (dir_acc < 0) dir_acc = 0;

    // low ammo keeps a gentle floor up (you feel exposed).  Capped BELOW DIR_RELAX so
    // FADE can always reach calm and cycle back to BUILDUP (else low ammo deadlocks it).
    floor = (100 - P_Director_AmmoPct ()) * 15 / 100;	// 0..15 intensity (x100 below)
    floor *= 100;
    if (dir_acc < floor) dir_acc = floor;

    // (A) Emergency medkit: any survivor critically low and none nearby -> drop one
    // by them (rate-limited).  Runs in both modes so you're never stranded at 1 HP.
    if (gametic - dir_emergtic > DIR_EMERG_COOLDOWN)
	for (i = 0; i < MAXPLAYERS; i++)
	{
	    player_t* p = &players[i];
	    if (!playeringame[i] || !p->mo || p->health <= 0 || p->health >= DIR_EMERG_HP)
		continue;
	    if (P_Director_MedkitNear (p->mo->x, p->mo->y, DIR_EMERG_NEAR)) continue;
	    P_Director_DropNear (p->mo, MT_MISC11);	// medikit within reach
	    dir_emergtic = gametic;
	    P_Director_Voice ("dir:heal", 3, 1);	// "you're dying. how dull."
	    break;
	}

    // (A2) Emergency ammo: when the squad's total ammo runs low, drop a bullets +
    // shells resupply by a survivor (rate-limited).  Runs in both modes.
    if (gametic - dir_emergammotic > DIR_EMERG_AMMO_COOLDOWN
	&& P_Director_AmmoPct () < DIR_EMERG_AMMO)
    {
	mobj_t*	sv = P_Director_RandomSurvivor ();
	if (sv)
	{
	    P_Director_DropNear (sv, MT_MISC17);	// box of bullets
	    P_Director_DropNear (sv, MT_MISC23);	// box of shells
	    dir_emergammotic = gametic;
	    P_Director_Announce ("Ammo resupply dropped nearby.");
	    P_Director_Voice ("dir:ammo", 3, 1);	// "reload. I insist."
	}
    }

    // (B) Under the LLM director the model drives spawns; the rule FSM only runs as a
    // fallback when the LLM has gone quiet.  Pure -director always runs the FSM.
    runfsm = !dir_llm || (gametic - dir_llmlast > DIR_LLM_FALLBACK);
    if (!runfsm) return;

    int exf      = P_Director_ExitProximity ();	// 0..100, ramps up near the exit
    int stressed = P_Director_Stressed ();	// player almost dead / out of ammo

    switch (dir_state)
    {
      case DIR_BUILDUP:
	if (dir_acc >= DIR_PEAK)
	    { dir_state = DIR_SUSTAIN; dir_timer = DIR_SUSTAIN_TICS;
	      P_Director_Voice ("dir:peak", 3, 1); }		// "now it gets interesting"
	else if (--dir_spawntic <= 0)
	{
	    // ~1-in-4 spawns is a horde (4-6) -- but never a horde on a stressed player.
	    if (P_Random () < 64 && !stressed) P_Director_SpawnWave (4 + (P_Random () % 3));
	    else                               P_Director_SpawnMonster ();
	    // Faster trickle when calm (build tension), slower as intensity climbs...
	    dir_spawntic = 35 + (dir_acc / 100) * 2;	// ~1 s (calm) .. ~3 s (near peak)
	    // ...but ramp the pressure up hard the closer the player is to the exit.
	    dir_spawntic = dir_spawntic * (100 - exf*3/4) / 100;	// up to ~75% faster at the exit
	    if (dir_spawntic < 8) dir_spawntic = 8;
	}
	break;

      case DIR_SUSTAIN:
	if (--dir_timer <= 0)
	    { dir_state = DIR_FADE; dir_relaxstart = gametic;
	      P_Director_Voice ("dir:relax", 3, 1);		// "catch your breath..." (before the gift line)
	      P_Director_SpawnItems (); }
	else if (--dir_spawntic <= 0)
	{
	    // At the peak, hordes are the norm (bigger, 5-7) -- unless the player is stressed.
	    if (P_Random () < 140 && !stressed) P_Director_SpawnWave (5 + (P_Random () % 3));
	    else                                P_Director_SpawnMonster ();
	    dir_spawntic = 2*TICRATE;
	    dir_spawntic = dir_spawntic * (100 - exf*3/4) / 100;
	    if (dir_spawntic < 8) dir_spawntic = 8;
	}
	break;

      case DIR_FADE:
	// No spawns -- let them breathe and loot until calm + the min relax elapses.
	// Near the exit the lull is much shorter, so the way out stays contested.
	{
	    int relaxneed = DIR_MINRELAX_TICS * (100 - exf*3/4) / 100;
	    if (dir_acc < DIR_RELAX && gametic - dir_relaxstart > relaxneed)
		{ dir_state = DIR_BUILDUP; dir_spawntic = TICRATE; }
	}
	break;
    }
}

// ---- LLM-driven spawns (the -aidirector model calls these via act verbs) ----

static mobjtype_t P_Director_TypeByName (const char* s)
{
    if (!strcmp(s,"zombie")   || !strcmp(s,"zombieman"))   return MT_POSSESSED;
    if (!strcmp(s,"shotgun")  || !strcmp(s,"shotguy"))     return MT_SHOTGUY;
    if (!strcmp(s,"chaingun") || !strcmp(s,"chaingunner")) return MT_CHAINGUY;
    if (!strcmp(s,"imp"))                                  return MT_TROOP;
    if (!strcmp(s,"pinky")    || !strcmp(s,"demon"))       return MT_SERGEANT;
    if (!strcmp(s,"spectre"))                              return MT_SHADOWS;
    if (!strcmp(s,"lost")     || !strcmp(s,"lostsoul"))    return MT_SKULL;
    if (!strcmp(s,"caco")     || !strcmp(s,"cacodemon"))   return MT_HEAD;
    if (!strcmp(s,"pain"))                                 return MT_PAIN;
    if (!strcmp(s,"knight")   || !strcmp(s,"hellknight"))  return MT_KNIGHT;
    if (!strcmp(s,"baron"))                                return MT_BRUISER;
    if (!strcmp(s,"revenant"))                             return MT_UNDEAD;
    if (!strcmp(s,"mancubus"))                             return MT_FATSO;
    if (!strcmp(s,"arachnotron"))                          return MT_BABY;
    return MT_TROOP;					// default
}

// LLM act: spawn `count` (1..8) monsters of `type` out of sight behind the survivors.
void P_Director_LLMSpawn (const char* type, int count)
{
    mobjtype_t	mt = P_Director_TypeByName (type);
    int		i;
    if (!dir_llm) return;
    if (count < 1) count = 1;
    if (count > 8) count = 8;
    for (i = 0; i < count; i++) P_Director_SpawnMonsterOf (mt);
    dir_llmlast = gametic;				// defer the FSM fallback
}

// LLM act: drop an item near a survivor ("medkit"/"health" -> medikit, else ammo).
void P_Director_LLMItem (const char* kind)
{
    mobj_t* sv = P_Director_RandomSurvivor ();
    if (!dir_llm || !sv) return;
    if (!strcmp(kind,"medkit") || !strcmp(kind,"health") || !strcmp(kind,"medikit"))
	P_Director_DropNear (sv, MT_MISC11);
    else
	P_Director_DropNear (sv, MT_MISC23);		// box of bullets
    dir_llmlast = gametic;
}

// LLM act: enter the relax phase (stop spawning for a while).
void P_Director_Relax (void)
{
    if (!dir_llm) return;
    dir_state = DIR_FADE; dir_relaxstart = gametic; dir_llmlast = gametic;
}
