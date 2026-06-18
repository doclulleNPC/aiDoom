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

// Build players[1].cmd for this tic.  Call from P_Ticker *before* the
// P_PlayerThink loop.  No-op unless -aicoop is active.
void P_AICoop_BuildCmd (void);

// Console helpers (c_console.c).
const char* P_AICoop_Report (void);		// "where" -- location + state
int	    P_AICoop_Summon (void);		// "come"  -- run to player; 0 if none
const char* P_AICoop_Wait (void);		// "wait/stay" -- toggle hold position
const char* P_AICoop_Attack (void);		// "attack" -- charge nearest threat
const char* P_AICoop_StatusReport (void);	// "report" -- ammo/HP/armor

#endif
