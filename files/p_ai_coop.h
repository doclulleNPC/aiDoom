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

// Parse -aicoop and, if present, enable player 2.  Call once after
// D_CheckNetGame (so it isn't clobbered) and before the first level loads.
void P_AICoop_Init (void);

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

#endif
