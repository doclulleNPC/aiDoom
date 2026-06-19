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

// Speak a line via the voice helper (writes buddy_say.txt -> tools/buddy_voice.py).
// Pass a "[Buddy] ..." reply; the tag is stripped.  No-op unless the buddy is active.
void P_AICoop_Voice (const char* line);

// Console commands (used by c_console.c).  The const char* ones return a short
// "[Buddy] ..." reply to print; P_AICoop_Summon returns 1 if a companion exists.
const char*	P_AICoop_Report (void);		// "where"  -- distance/dir/HP/doing
int		P_AICoop_Summon (void);		// "come"   -- run to the player
const char*	P_AICoop_Wait (void);		// "wait"/"stay" -- toggle hold
const char*	P_AICoop_Attack (void);		// "attack" -- charge nearest monster
const char*	P_AICoop_StatusReport (void);	// "report" -- HP/armor/weapon/ammo

#endif
