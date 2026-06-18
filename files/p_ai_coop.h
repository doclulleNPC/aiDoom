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

#endif
