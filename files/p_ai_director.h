// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Left-4-Dead-style "AI Director" (rule-based / offline).  Tracks a per-tic
//	player STRESS/intensity (damage-burst + close-quarters kills + low ammo)
//	and drives a build-up -> peak -> relax spawn cycle: it spawns extra monsters
//	out of sight behind the survivors while building tension, and drops items
//	during the relax.  Enabled with -director.  The LLM variant (-aidirector)
//	reads the same intensity in `observe` and decides spawns itself.
//
//-----------------------------------------------------------------------------

#ifndef __P_AI_DIRECTOR__
#define __P_AI_DIRECTOR__

struct mobj_s;

// Parse -director.  Call once at startup (after P_AICoop_Init).
void P_Director_Init (void);

// Reset intensity + FSM at level load (from P_SetupLevel).
void P_Director_Reset (void);

// Per-tic update: decay intensity, run the spawn/relax FSM.  From P_Ticker.
// No-op unless -director was given.
void P_Director_Ticker (void);

// Stress feeds (from P_DamageMobj): a player took `damage`; a monster died to
// `killer`.  No-op unless -director is active.
void P_Director_NoteDamage (struct mobj_s* victim, int damage);
void P_Director_NoteKill   (struct mobj_s* victim, struct mobj_s* killer);

// Current intensity 0..100 (for HUD / the LLM observe stream).  0 if disabled.
int  P_Director_Intensity (void);

// True if the rule-based director is active (-director).
int  P_Director_Active (void);

// Stress fields for the LLM `observe` stream (p_ai_llm.c).
int  P_Director_State (void);		// 0 buildup, 1 sustain, 2 fade
int  P_Director_RecentDmg (void);	// damage in the recent burst window
int  P_Director_AmmoPct (void);		// carried-ammo fill 0..100

// LLM director act verbs (-aidirector): spawn monsters / drop an item / relax.
// No-ops unless the LLM director is active; each defers the rule-FSM fallback.
void P_Director_LLMSpawn (const char* type, int count);
void P_Director_LLMItem  (const char* kind);
void P_Director_Relax    (void);

#endif
