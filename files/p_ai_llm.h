// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	LLM "AI Director" hook for monster control.  An external director
//	(an LLM, a script, or the built-in demo) issues high-level squad
//	*orders* to monsters; the engine's existing AI primitives execute them
//	per tic.  See AGENT_CONTROL.md sections 12-13.
//
//-----------------------------------------------------------------------------

#ifndef __P_AI_LLM__
#define __P_AI_LLM__

#include "p_mobj.h"

// Called once (lazily) to parse -aidirector / -aidemo and open the socket.
void	P_AI_Init (void);

// Clear all directives + the monster id registry (call on level setup).
void	P_AI_Reset (void);

// Per-gameplay-tic: poll the director socket, age directive timers, run the
// optional built-in demo director.
void	P_AI_Ticker (void);

// True if `actor` currently has an active, non-default directive -> A_Chase
// should defer to A_LLMChase instead of the vanilla logic.
int	P_AI_Active (mobj_t* actor);

// Execute the current directive for `actor` (movement order + standard
// attack/upkeep), using the engine's own P_Move / A_FaceTarget / attack states.
void	A_LLMChase (mobj_t* actor);

// Console: toggle the LLM<->Doom monster director.  arg "on"/"off"/"demo"/""(toggle).
// Returns a one-line status string.
const char* P_AI_Console (const char* arg);

#endif
