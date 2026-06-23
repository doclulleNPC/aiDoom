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
#include "w_wad.h"			// W_CheckNumForName -- detect overlaid DOOM2 sprites
#include "p_ai_director.h"
#include "p_ai_llm.h"			// P_AI_RuleTactics -- rule-based flank/focus orders
#include "p_ai_coop.h"			// P_AICoop_NextWaypoint -- route the player takes to the exit
#include "i_voice.h"			// I_Director_Say -- the spoken game-master persona

// ---- tunables --------------------------------------------------------------
#define DIR_MAX			10000	// intensity*100 ceiling (= 100.00)
#define DIR_PEAK		6000	// -> SUSTAIN at 60
#define DIR_RELAX		2000	// -> BUILDUP again under 20
#define DIR_DECAY		8	// intensity*100 drained per tic (~2.8/s)
#define DIR_SUSTAIN_TICS	(4*TICRATE)	// hold the peak this long
#define DIR_MINRELAX_TICS	(30*TICRATE)	// minimum calm/loot time
#define DIR_MAXMON		64	// don't spawn past this many live monsters
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
#define DIR_HORDE_GAP		(25*TICRATE)	// min gap between LLM-mode periodic hordes

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
static int	dir_seeded;		// 0 until objectives (exit/keys) seeded this level
static int	dir_guardtic;		// gametic of the last objective-guard spawn
static int	dir_hordetic;		// gametic of the last LLM-mode periodic horde

static int     P_Director_ObjProximity (void);	// 0..100, nearer exit/key = more
static boolean P_Director_Stressed (void);	// player critically low hp/ammo?
static boolean P_Director_SpawnMonsterNear (mobjtype_t mt, fixed_t cx, fixed_t cy);
static mobjtype_t P_Director_PickType (void);

// Is `mo` an uncollected key item?  (objective rooms the director defends)
static boolean P_Director_IsKey (mobj_t* mo)
{
    return (mo->sprite == SPR_BKEY || mo->sprite == SPR_RKEY || mo->sprite == SPR_YKEY
	 || mo->sprite == SPR_BSKU || mo->sprite == SPR_RSKU || mo->sprite == SPR_YSKU);
}

// "common" + "special" monster pools (specials lean in at the peak).  DOOM2-only
// monsters (revenant/mancubus/hell knight/chaingunner/...) have NO sprites in a
// DOOM1 IWAD, so spawning one there crashes the renderer -- hence a DOOM1-safe
// special pool and a DOOM2 one, gated by gamemode (see P_Director_SafeType).
static const mobjtype_t dir_common[]   = { MT_POSSESSED, MT_SHOTGUY, MT_TROOP, MT_SERGEANT };
static const mobjtype_t dir_special1[] = { MT_HEAD, MT_BRUISER, MT_SHADOWS, MT_SKULL };           // DOOM1-safe: caco, baron, spectre, lost soul
static const mobjtype_t dir_special2[] = { MT_UNDEAD, MT_FATSO, MT_KNIGHT, MT_BABY, MT_CHAINGUY, MT_HEAD }; // DOOM2: revenant, mancubus, hell knight, arachnotron, chaingunner, caco
// "Miniboss" pool for the level-exit guard -- a real threat must hold the way out
// (general rule: defend the level end).  DOOM2 minibosses when their art is overlaid,
// else a baron.  SafeType() resolves each to the doom2 / freedoom / DOOM1 actor.
static const mobjtype_t dir_guards[]   = { MT_UNDEAD, MT_VILE, MT_FATSO, MT_KNIGHT, MT_BRUISER }; // revenant, arch-vile, mancubus, hell knight, baron
// Heretic monsters (hereticstuff.wad overlaid) mixed into the spawn pools so the
// director uses them too -- a melee trash tier + the Knight as a ranged miniboss.
static const mobjtype_t dir_heretic[]  = { MT_HMUMMY, MT_HCLINK, MT_HIMP };	// golem, sabreclaw, gargoyle

#define DIR_TRACK	(dir_on || dir_llm)	// intensity is tracked in either mode

// ---- spoken "game-master" lines (DD* lumps via the dir:* tags / I_Director_Say) ---
// Rate-limited so the announcer stays a voice-of-god, not a chatterbox.  prefix is
// e.g. "dir:horde"; one of n variants is appended ("dir:horde:2").
//   force=1: an important event (phase change / item drop) -- may barge in over a
//            line in progress, but still obeys a hard floor so events don't cascade.
//   force=0: ambient flavour -- only when the director is idle and the long gap elapsed.
#define DIR_VOICE_GAP	(16*TICRATE)	// min gap between ambient lines
#define DIR_VOICE_FLOOR	(6*TICRATE)	// min gap between ANY two lines (even forced ones)
static void P_Director_Voice (const char* prefix, int n, int force)
{
    char tag[40];
    if (!DIR_TRACK) return;			// director not running -> silent
    // A hard floor on EVERY line keeps the Director from machine-gunning: even
    // back-to-back "important" events (peak -> big, relax -> gift) won't both fire.
    if (gametic - dir_voicelast < (force ? DIR_VOICE_FLOOR : DIR_VOICE_GAP))
	return;
    if (force)
	I_Director_Stop ();			// important line preempts whatever's playing
    else if (I_Director_Busy ())
	return;					// ambient: don't talk over a line in progress
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
    dir_seeded = 0; dir_guardtic = 0; dir_hordetic = 0;	// re-seed guards + reset hordes on the new map
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

// DOOM2-tier monsters can be used if we're in DOOM2, OR a DOOM1 game has the DOOM2
// sprites overlaid -- doom2stuff.wad (SKEL...) or freedoom2stuff.wad (FSKE..., the
// renamed Freedoom clones).  SafeType() maps each type to whatever art exists.
//
// Probe the renderer's sprite table (numframes), NOT a lump name: doom2/freedoom store
// the revenant's first frame as a cross-frame MIRROR lump ("SKELA1D1", not "SKELA1"),
// so the old W_CheckNumForName("SKELA1") wrongly failed and every DOOM2 monster got
// downgraded to a baron.  numframes>0 is naming-agnostic.
static boolean P_Director_Doom2Available (void)
{
    if (gamemode == commercial) return true;
    return sprites && (sprites[SPR_SKEL].numframes > 0		// doom2stuff overlay
		    || sprites[SPR_FSKE].numframes > 0);	// freedoom2stuff overlay
}

// Heretic monsters loaded (hereticstuff.wad overlaid)?  Probe the mummy/golem sprite.
static boolean P_Director_HereticAvailable (void)
{
    return sprites && sprites[SPR_HMUM].numframes > 0;
}

static mobjtype_t P_Director_PickType (void)
{
    boolean		doom2 = P_Director_Doom2Available ();
    const mobjtype_t*	spec  = doom2 ? dir_special2 : dir_special1;
    int nspec = doom2
	      ? (int)(sizeof(dir_special2)/sizeof(dir_special2[0]))
	      : (int)(sizeof(dir_special1)/sizeof(dir_special1[0]));
    // More high-tier pressure: specials lean in from mid-intensity (was 3/4 peak)
    // and more often (was ~43%).
    // Specials/bosses lean in at high intensity -- but NOT when the player is
    // critically low on health or ammo (don't drop a Cyberdemon on a dying player).
    if ((dir_state == DIR_SUSTAIN || dir_acc > DIR_PEAK/2) && P_Random () < 150
	&& !P_Director_Stressed ())
    {
	if (P_Director_HereticAvailable () && (P_Random () % 100) < 25)
	    return MT_HKNIGHT;					// Heretic ranged miniboss
	return spec[P_Random () % nspec];
    }
    if (P_Director_HereticAvailable () && (P_Random () % 100) < 35)	// ~35% Heretic trash
	return dir_heretic[P_Random () % (int)(sizeof(dir_heretic)/sizeof(dir_heretic[0]))];
    return dir_common[P_Random () % (int)(sizeof(dir_common)/sizeof(dir_common[0]))];
}

// A tough miniboss to hold an objective room (the level exit).  DOOM2 miniboss when its
// art is overlaid, a Heretic knight if that's loaded, else a baron -- so the way out is
// NEVER guarded by mere trash.
static mobjtype_t P_Director_PickGuard (void)
{
    if (P_Director_HereticAvailable () && (P_Random () % 100) < 30)
	return MT_HKNIGHT;					// Heretic undead warrior holds it
    if (P_Director_Doom2Available ())
	return dir_guards[P_Random () % (int)(sizeof(dir_guards)/sizeof(dir_guards[0]))];
    return MT_BRUISER;		// DOOM1: the baron is the toughest reliable stand-in
}

// Is `mt` one of the "special" (tougher) monster types?  -> the Director's "big"
// line; used only after one ACTUALLY spawns, so the quip matches what entered.
static boolean P_Director_IsSpecial (mobjtype_t mt)
{
    int i;
    for (i = 0; i < (int)(sizeof(dir_special1)/sizeof(dir_special1[0])); i++)
	if (dir_special1[i] == mt) return true;
    for (i = 0; i < (int)(sizeof(dir_special2)/sizeof(dir_special2[0])); i++)
	if (dir_special2[i] == mt) return true;
    if (mt >= MT_FD_UNDEAD && mt <= MT_FD_KEEN) return true;	// Freedoom clones
    if (mt == MT_HKNIGHT) return true;				// Heretic miniboss
    return false;
}

// DOOM2-only monsters have no sprites in a DOOM1 IWAD (-> renderer I_Error).  In
// non-commercial gamemodes substitute a tough DOOM1 stand-in (baron); otherwise
// pass the type through.  Guards BOTH the rule FSM and the LLM spawn path.
static mobjtype_t P_Director_SafeType (mobjtype_t mt)
{
    // numframes>0, not a lump name -- see P_Director_Doom2Available (the mirror-lump trap).
    if (gamemode == commercial) return mt;		// native DOOM2 IWAD
    if (sprites && sprites[SPR_SKEL].numframes > 0) return mt;	// doom2stuff -> real types

    if (sprites && sprites[SPR_FSKE].numframes > 0)
	switch (mt)	// only Freedoom art present -> map to the cloned MT_FD_* actor
	{
	  case MT_UNDEAD:   return MT_FD_UNDEAD;
	  case MT_FATSO:    return MT_FD_FATSO;
	  case MT_VILE:     return MT_FD_VILE;
	  case MT_BABY:     return MT_FD_BABY;
	  case MT_CHAINGUY: return MT_FD_CHAINGUY;
	  case MT_KNIGHT:   return MT_FD_KNIGHT;
	  case MT_PAIN:     return MT_FD_PAIN;
	  case MT_WOLFSS:   return MT_FD_WOLFSS;
	  case MT_KEEN:     return MT_FD_KEEN;
	  default:          return mt;
	}

    switch (mt)		// no DOOM2 art at all -> a tough DOOM1 stand-in
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
      // Freedoom clones read out the same names (SafeType may map to these).
      case MT_FD_UNDEAD:   return "Revenant";
      case MT_FD_FATSO:    return "Mancubus";
      case MT_FD_KNIGHT:   return "Hell Knight";
      case MT_FD_BABY:     return "Arachnotron";
      case MT_FD_CHAINGUY: return "Chaingunner";
      case MT_FD_VILE:     return "Arch-Vile";
      // Heretic monsters (hereticstuff.wad)
      case MT_HKNIGHT:  return "Undead Warrior";
      case MT_HMUMMY:   return "Golem";
      case MT_HIMP:     return "Gargoyle";
      case MT_HCLINK:   return "Sabreclaw";
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
static boolean P_Director_SpawnMonsterNear (mobjtype_t mt, fixed_t cx, fixed_t cy)
{
    mobj_t*	sv;
    int		t;
    static int	lastname;	// rate-limit the per-monster "you hear a X" callout

    mt = P_Director_SafeType (mt);	// never spawn an IWAD-absent monster (crash guard)
    if (P_Director_LiveMonsters () >= DIR_MAXMON) return false;
    sv = P_Director_RandomSurvivor ();
    if (!sv) return false;
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
	return true;			// actually placed one
    }
    return false;			// no valid hidden spot found this time
}

// Spawn an IDLE GUARD close to (cx,cy): it stays at the objective (spawnstate /
// A_Look) and only wakes when the player arrives, instead of charging off like the
// behind-you spawns.  Small band so it's actually IN the room; hidden = no pop-in.
static boolean P_Director_SpawnGuard (mobjtype_t mt, fixed_t cx, fixed_t cy)
{
    int	t;
    mt = P_Director_SafeType (mt);
    if (P_Director_LiveMonsters () >= DIR_MAXMON) return false;
    for (t = 0; t < DIR_SPAWN_TRIES; t++)
    {
	angle_t	ang  = (angle_t)(P_Random () << 24);
	int	fa   = ang >> ANGLETOFINESHIFT;
	fixed_t	dist = 64*FRACUNIT + (P_Random () * ((320*FRACUNIT) >> 8));	// 64..384u
	fixed_t	x    = cx + FixedMul (dist, finecosine[fa]);
	fixed_t	y    = cy + FixedMul (dist, finesine[fa]);
	mobj_t*	mo   = P_SpawnMobj (x, y, ONFLOORZ, mt);
	int	i, seen = 0;
	if (!P_CheckPosition (mo, x, y) || tmceilingz - tmfloorz < mo->height)
	    { P_RemoveMobj (mo); continue; }
	for (i = 0; i < MAXPLAYERS; i++)
	    if (playeringame[i] && players[i].mo && P_CheckSight (players[i].mo, mo))
		{ seen = 1; break; }
	if (seen) { P_RemoveMobj (mo); continue; }
	return true;			// left IDLE at spawnstate -> guards, wakes on sight
    }
    return false;
}

// Default placement: in the dark behind a survivor (the original behaviour).
static boolean P_Director_SpawnMonsterOf (mobjtype_t mt) { return P_Director_SpawnMonsterNear (mt, 0, 0); }

// ---- objective-aware pressure ----------------------------------------------
// More heat the closer the player gets to an OBJECTIVE -- the level exit OR a key
// they still need -- and those rooms get pre-populated so the exit/key is a
// defended objective rather than a free grab.  Spawns (incl. hordes) can land out
// of sight in the room toward the objective, not only in the dark behind you.

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

// An "objective" the player must reach: the level exit, OR a still-uncollected
// key (keys are detected live by sprite, so a picked-up key stops counting).
// Nearest objective to (fx,fy); writes its point to *ox/*oy; 0x7fffffff if none.
static fixed_t P_Director_NearestObjective (fixed_t fx, fixed_t fy, fixed_t* ox, fixed_t* oy)
{
    fixed_t	best = 0x7fffffff;
    thinker_t*	th;
    if (dir_exit_set)
	{ best = P_AproxDistance (fx - dir_exit_x, fy - dir_exit_y); *ox = dir_exit_x; *oy = dir_exit_y; }
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	mo;
	fixed_t	d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	mo = (mobj_t*)th;
	if (!P_Director_IsKey (mo)) continue;		// not a key
	d = P_AproxDistance (fx - mo->x, fy - mo->y);
	if (d < best) { best = d; *ox = mo->x; *oy = mo->y; }
    }
    return best;
}

// 0..100: how close the nearest survivor is to the nearest objective (exit/key).
// 0 if there's no objective on this map.  Ramps over DIR_OBJ_FAR..DIR_OBJ_NEAR.
#define DIR_OBJ_NEAR	(384*FRACUNIT)
#define DIR_OBJ_FAR	(2048*FRACUNIT)
static int P_Director_ObjProximity (void)
{
    int		i;
    fixed_t	best = 0x7fffffff, ox, oy;
    for (i = 0; i < MAXPLAYERS; i++)
    {
	if (!playeringame[i] || !players[i].mo || players[i].health <= 0) continue;
	fixed_t d = P_Director_NearestObjective (players[i].mo->x, players[i].mo->y, &ox, &oy);
	if (d < best) best = d;
    }
    if (best >= DIR_OBJ_FAR)  return 0;
    if (best <= DIR_OBJ_NEAR) return 100;
    return (int)(100 * (DIR_OBJ_FAR - best) / (DIR_OBJ_FAR - DIR_OBJ_NEAR));
}

// 0..100: how close the nearest survivor is to the level EXIT specifically (vs
// P_Director_ObjProximity, which is the nearest of exit OR keys).  Drives the
// exit-path spawn ramp: the closer to the exit switch, the more it spawns there.
static int P_Director_ExitProximity (void)
{
    int		i;
    fixed_t	best = 0x7fffffff;
    if (!dir_exit_set) return 0;
    for (i = 0; i < MAXPLAYERS; i++)
    {
	if (!playeringame[i] || !players[i].mo || players[i].health <= 0) continue;
	fixed_t d = P_AproxDistance (players[i].mo->x - dir_exit_x, players[i].mo->y - dir_exit_y);
	if (d < best) best = d;
    }
    if (best >= DIR_OBJ_FAR)  return 0;
    if (best <= DIR_OBJ_NEAR) return 100;
    return (int)(100 * (DIR_OBJ_FAR - best) / (DIR_OBJ_FAR - DIR_OBJ_NEAR));
}

// Choose where the next spawn group centres.  Bias toward the nearest objective
// (exit/key) so monsters appear out of sight in the room the player is heading to
// -- more so the nearer the player already is to it -- instead of always "in the
// dark behind you".  cx==cy==0 means the classic behind-the-player spawn.  The
// spawner's own P_CheckSight keeps whatever we pick hidden.
static void P_Director_SpawnCenter (fixed_t* cx, fixed_t* cy)
{
    mobj_t*	pl = P_Director_RandomSurvivor ();
    fixed_t	ox, oy;
    *cx = *cy = 0;					// default: behind the survivor
    if (!pl) return;

    // EXIT-PATH RAMP (sub-rule): increasingly often as the player nears the exit
    // switch, drop the spawn group ON THE ROUTE to the exit -- the pathfinder's next
    // waypoint from the player toward the exit -- so the way out gets steadily more
    // contested the closer they get.  ~20% far away, up to ~90% right by the exit.
    if (dir_exit_set
	&& (P_Random () % 100) < 20 + P_Director_ExitProximity () * 7 / 10)
    {
	fixed_t wx, wy;
	if (P_AICoop_NextWaypoint (pl, dir_exit_x, dir_exit_y, &wx, &wy))
	    { *cx = wx; *cy = wy; return; }		// on the path ahead of the player
	*cx = dir_exit_x; *cy = dir_exit_y; return;	// fallback: the exit room itself
    }

    if (P_Director_NearestObjective (pl->x, pl->y, &ox, &oy) == 0x7fffffff)
	return;						// no exit/key objective on this map
    // ~40% baseline toward the objective + a proximity ramp (up to +60%): far away
    // it's a sometimes-thing, right on top of it nearly every spawn defends the room.
    if ((P_Random () % 100) < 40 + P_Director_ObjProximity () * 6 / 10)
	{ *cx = ox; *cy = oy; }
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
    mobjtype_t	mt = P_Director_SafeType (P_Director_PickType ());
    fixed_t	cx, cy;
    boolean	ok;
    // Center the spawn behind the player OR out of sight in the room toward the
    // nearest objective (exit/key) -- P_Director_SpawnCenter decides.
    P_Director_SpawnCenter (&cx, &cy);
    ok = P_Director_SpawnMonsterNear (mt, cx, cy);
    // Only speak when something ACTUALLY entered (else the line wouldn't match the
    // game), and pick the line by what spawned: a tougher type -> "big", else trash.
    if (!ok) return;
    if (P_Director_IsSpecial (mt)) P_Director_Voice ("dir:big", 3, 0);
    else                           P_Director_Voice ("dir:spawn", 2, 0);
}

// (E) Spawn a horde -- a burst of `count` monsters of one type at once -- and (F)
// announce it.  Used occasionally during build-up/peak for real pressure.
static void P_Director_SpawnWave (int count)
{
    mobjtype_t	mt = P_Director_PickType ();
    fixed_t	cx, cy;
    int		i, spawned = 0;
    // The whole horde shares one centre -- behind the player, or out of sight in
    // the room toward the exit/key (so hordes don't always come from behind you).
    P_Director_SpawnCenter (&cx, &cy);
    for (i = 0; i < count; i++) if (P_Director_SpawnMonsterNear (mt, cx, cy)) spawned++;
    // Only call it a horde (HUD + voice) if one actually materialised -- spawns can
    // all fail (no hidden spot / monster cap), and "Here comes the horde" with no
    // horde is exactly the mismatch we want to avoid.
    if (spawned < 2)
	return;
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
    // DOOM1 + doom2stuff overlay: hand out the super shotgun (drop it near a survivor)
    // until someone owns it, so the DOOM2 weapon actually reaches the player.
    if (doom2_overlay)
    {
	int i, owned = 0;
	for (i = 0; i < MAXPLAYERS; i++)
	    if (playeringame[i] && players[i].weaponowned[wp_supershotgun]) { owned = 1; break; }
	if (!owned) P_Director_DropNear (sv, MT_SUPERSHOTGUN);
    }
    // Ambient (force=0): at the normal FADE transition the "relax" line is spoken
    // just before this, so leave it playing; "gift" only speaks on other drop paths.
    P_Director_Voice ("dir:gift", 3, 0);
}

// Seed the objective rooms (the exit + every uncollected key) with a small
// out-of-sight guard, so those rooms are defended from the very start of the level.
static void P_Director_SeedObjectives (void)
{
    thinker_t*	th;
    int		n;
    if (dir_exit_set)
    {
	P_Director_SpawnGuard (P_Director_PickGuard (), dir_exit_x, dir_exit_y);	// a real miniboss holds the exit
	for (n = 0; n < 2; n++) P_Director_SpawnGuard (P_Director_PickType (), dir_exit_x, dir_exit_y);	// + escorts
    }
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	mo;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	mo = (mobj_t*)th;
	if (!P_Director_IsKey (mo)) continue;
	for (n = 0; n < 2; n++) P_Director_SpawnGuard (P_Director_PickType (), mo->x, mo->y);
    }
}

// Within `r` of the exit or any uncollected key?  Those rooms are the "guard zones":
// monsters there are left idle to ambush the player; everything else is woken to hunt.
#define DIR_GUARD_RADIUS	(512*FRACUNIT)
static boolean P_Director_NearAnyObjective (fixed_t x, fixed_t y, fixed_t r)
{
    thinker_t*	th;
    if (dir_exit_set && P_AproxDistance (x - dir_exit_x, y - dir_exit_y) < r)
	return true;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	mo;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	mo = (mobj_t*)th;
	if (!P_Director_IsKey (mo)) continue;
	if (P_AproxDistance (x - mo->x, y - mo->y) < r) return true;
    }
    return false;
}

// Wake every monster that ISN'T an objective guard: give it the player as target and
// drop it into its chase state, so the level hunts you instead of standing around like
// vanilla.  Monsters near the exit/keys (and the seeded guards) are left idle so they
// stay and ambush -- A_Look wakes them when the player walks in.
static void P_Director_WakeMonsters (void)
{
    thinker_t*	th;
    mobj_t*	pl = (consoleplayer >= 0 && playeringame[consoleplayer])
		     ? players[consoleplayer].mo : NULL;
    if (!pl || pl->health <= 0) return;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (!(m->flags & MF_COUNTKILL) || m->health <= 0) continue;
	if (m->target) continue;					// already awake
	if (P_Director_NearAnyObjective (m->x, m->y, DIR_GUARD_RADIUS)) continue;	// guard -> stay idle
	m->target = pl;
	if (m->info->seestate) P_SetMobjState (m, m->info->seestate);
    }
}

// ---- per-tic FSM -----------------------------------------------------------

void P_Director_Ticker (void)
{
    int	floor, i, runfsm;

    if (!DIR_TRACK || gamestate != GS_LEVEL || paused) return;

    // Locate the level exit once (lines[] is loaded by now) for exit-proximity pressure.
    if (!dir_exit_found) { dir_exit_found = 1; P_Director_FindExit (); }
    // ...then seed the exit/key rooms with a starting guard (once per level).
    if (!dir_seeded)     { dir_seeded = 1; P_Director_SeedObjectives (); }

    // Always keep the objectives defended: every ~10 s drop one extra monster at the
    // nearest objective (exit or a key), out of sight -- so the exit/key rooms are
    // never a free walk-in.  Skipped when the player is critically low (no piling on).
    if (gametic - dir_guardtic > 10*TICRATE && !P_Director_Stressed ())
    {
	fixed_t ox, oy;
	mobj_t*	sv = P_Director_RandomSurvivor ();
	if (sv && P_Director_NearestObjective (sv->x, sv->y, &ox, &oy) != 0x7fffffff
	    && P_Director_SpawnGuard (P_Director_PickType (), ox, oy))
	    dir_guardtic = gametic;
    }

    // Periodic HORDE -- guarantee real waves even when the LLM director has the FSM
    // suppressed (the LLM tends to issue single spawns).  Pure -director runs hordes
    // through the FSM, so only fire this when the FSM is currently suppressed.  Out
    // of sight, behind/ahead of the player; skipped when the player is stressed.
    {
	boolean fsm_suppressed = dir_llm && (gametic - dir_llmlast <= DIR_LLM_FALLBACK);
	if (fsm_suppressed && gametic - dir_hordetic > DIR_HORDE_GAP && !P_Director_Stressed ())
	{
	    P_Director_SpawnWave (5 + (P_Random () % 4) + P_Director_ExitProximity () / 25);
	    dir_hordetic = gametic;
	}
    }

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

    // Rule layer in charge (pure -director, OR an LLM director gone quiet): wake the
    // level so monsters hunt the player -- except the objective guards near exit/keys,
    // which stay idle and ambush -- then run coordinated tactics (flank/focus/fallback)
    // on the awake hunters.  These also serve as the LLM's fallback; when the LLM is
    // actively issuing orders runfsm is false and we returned above, so no conflict.
    {
	static int wake_tic;
	if (gametic - wake_tic > 35) { wake_tic = gametic; P_Director_WakeMonsters (); }
	P_AI_RuleTactics ();
    }

    int exf      = P_Director_ObjProximity ();	// 0..100, ramps up near an exit/key
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
	    if (P_Random () < 96 && !stressed) P_Director_SpawnWave (5 + (P_Random () % 4) + P_Director_ExitProximity () / 25);
	    else                               P_Director_SpawnMonster ();
	    // Faster trickle when calm (build tension), slower as intensity climbs...
	    dir_spawntic = 24 + (dir_acc / 100) * 2;	// faster trickle (~0.7 s calm .. ~2.4 s)
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
	    if (P_Random () < 170 && !stressed) P_Director_SpawnWave (6 + (P_Random () % 4) + P_Director_ExitProximity () / 25);
	    else                                P_Director_SpawnMonster ();
	    dir_spawntic = 49;		// ~1.4 s between peak hordes
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
