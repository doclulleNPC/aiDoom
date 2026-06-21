// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	AI-controlled co-op companion (player 2), enabled with -aicoop.
//	A built-in bot fills players[1].cmd each tic: it acquires the nearest
//	visible monster and fires, otherwise follows the human player.
//
//-----------------------------------------------------------------------------

#ifndef __P_AI_COOP__
#define __P_AI_COOP__

#include "m_fixed.h"		// fixed_t
struct mobj_s;

// Parse -aicoop and, if present, enable player 2.  Call once after
// D_CheckNetGame (so it isn't clobbered) and before the first level loads.
void P_AICoop_Init (void);

// Called from P_SetupLevel after P_LoadThings.  If -coop/-aicoop was given
// but the map has no Player_2_Start, prints a one-shot warning and disables
// the buddy for this level.
void P_AICoop_VerifySpawn (void);

// Called from P_SetupLevel just before P_LoadThings.  Clears the buddy slot's
// stale mobj pointer so P_AICoop_VerifySpawn can reliably tell whether THIS
// map's THINGS contain a Player_2_Start (which would have re-spawned the
// buddy mobj).  See p_setup.c for why this is necessary even though it looks
// redundant.
void P_AICoop_ResetSlot (void);

// Build the buddy's ticcmd for this tic.  Call from P_Ticker *before* the
// P_PlayerThink loop.  No-op unless -aicoop is active.  Deterministic, so in a
// netgame every node computes the same command and the buddy stays in lockstep.
void P_AICoop_BuildCmd (void);

// Player slot the buddy occupies, or -1 if disabled.  g_game.c skips the netgame
// consistency check for it (the buddy is local-but-deterministic, never sent).
int  P_AICoop_Slot (void);

// True if player p is the AI co-op buddy.  Used by p_inter.c so the buddy never
// pockets keycards/skulls (the human needs them for locked doors).
boolean P_AICoop_IsBuddy (player_t* p);

// Current buddy state as a small enum (0=follow, 1=fight, 2=heal, 3=hold,
// 4=come, 5=grab).  Exposed for the console / voice system.
int  P_AICoop_State (void);

// Speak a tagged phrase through i_voice.c (offline OGG via buddy.wad).
// Callers pick the exact tag (e.g. "summon_ok", "state:fighting"); the
// tag -> lump-name mapping lives in i_voice.c.
void P_AICoop_VoiceTag (const char* tag);

// Console commands (used by c_console.c).  The const char* ones return a short
// "[Buddy] ..." reply to print; P_AICoop_Summon returns 1 if a companion exists.
const char*	P_AICoop_Report (void);		// "where"  -- distance/dir/HP/doing
int		P_AICoop_Summon (void);		// "come"   -- run to the player
const char*	P_AICoop_Wait (void);		// "wait"/"stay" -- toggle hold
const char*	P_AICoop_Attack (void);		// "attack" -- charge nearest monster
const char*	P_AICoop_StatusReport (void);	// "report" -- HP/armor/weapon/ammo
const char*	P_AICoop_God (void);		// "buddygod" -- toggle buddy god mode
const char*	P_AICoop_GiveAll (void);	// "buddyarm" -- buddy all weapons + ammo + armor

// ---------------------------------------------------------------------------
//  AI (LLM) director layer for the buddy (-aicoop).  The director sets a
//  high-level *tactic* (and an optional focus monster / point); the rule-based
//  BuildCmd executes it per tic.  Directives expire after `tics`, so the buddy
//  reverts to autonomous behaviour if the director stops talking.
// ---------------------------------------------------------------------------
enum {
    BUD_AUTO = 0,	// no override -- pure rule-based
    BUD_ENGAGE,		// fight (focus a specific monster if given, else nearest)
    BUD_DEFEND,		// stay near the player, fight only close threats (rule-based)
    BUD_HOLD,		// hold position
    BUD_REGROUP,	// run to the player
    BUD_RETREAT,	// fall back to the player
    BUD_GOTO,		// move to a point (x,y)
    BUD_GRAB		// collect nearby items (rule-based)
};

// True if -aicoop (AI-driven) mode is active -- the AI transport then exposes the
// buddy in `observe` and accepts `buddy` orders.
int  P_AICoop_AIMode (void);

// Apply a director tactic to the buddy for `tics` (<=0 -> ~2 s default).  `focus`
// is the monster to engage (NULL = nearest); x,y the BUD_GOTO point.
void P_AICoop_SetDirective (int tactic, struct mobj_s* focus, fixed_t x, fixed_t y, int tics);

// Fill (xs,ys) with up to maxpts reachable waypoints along the buddy->player route
// (downsampled portal path), for the AI `observe` stream.  Returns the count.
int  P_AICoop_NavRoute (fixed_t* xs, fixed_t* ys, int maxpts);

// Next reachable waypoint for `mo` toward (dx,dy) via the BSP portal graph (the same
// pathfinder the buddy uses).  Used by director-controlled monsters (p_ai_llm.c) to
// navigate around corners instead of the vanilla straight 8-dir chase.
boolean P_AICoop_NextWaypoint (struct mobj_s* mo, fixed_t dx, fixed_t dy, fixed_t* wx, fixed_t* wy);

// Savegame persistence for the breadcrumb trail.  Written/read AFTER the savegame
// consistency marker (g_game.c), so older saves without the block still load.
void P_AICoop_ArchiveTrail (void);
void P_AICoop_UnArchiveTrail (void);

// Note that a player took `damage` at its current position, feeding the buddy's
// danger heatmap (Safe route mode) + a friendly-fire callout.  From P_DamageMobj.
void P_AICoop_NoteDamage (struct mobj_s* victim, struct mobj_s* source, int damage);

// A monster died (from P_DamageMobj): buddy-kill quip + spree, or "nice" if the human
// scored near the buddy.  And a public rotated-callout wrapper for other modules.
void P_AICoop_NoteKill (struct mobj_s* victim, struct mobj_s* killer);
void P_AICoop_Callout (const char* prefix, int n);

#endif
