// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Small top-of-screen HUD overlay for the AI co-op companion ("buddy"),
//	parallel to the player status bar at the bottom.  Shown only when the
//	companion is alive and the user hasn't disabled it via the `show_buddy_hud`
//	config key (Options -> Messages / default config).  Rendered as a strip
//	of small digits (shortnum-style) plus the small heads-up font, sitting at
//	the top of the screen centred under WIDESCREENDELTA (so it lines up with
//	the centred status bar).
//
//	Why a separate module instead of st_stuff: st_stuff's widget code is
//	coupled to STlib_drawNum's BG-buffer wipe that subtracts ST_Y from each
//	widget y, so reusing it for a non-status-bar position would either need
//	a fake ST_Y or a rewrite.  The HUD is also rendered only when the buddy
//	is alive and -aicoop is active, with no intermission/finale/menu flow,
//	which doesn't fit the status bar's lifecycle.
//-----------------------------------------------------------------------------
#ifndef __HU_BUDDY__
#define __HU_BUDDY__

#include "doomtype.h"

// One-time setup: load font/number patches, allocate the BG-save buffer.
// Safe to call before -aicoop is parsed -- when the buddy is inactive the
// Drawer is a no-op, so the buffer is just sitting unused.
void HU_Buddy_Init (void);

// Draw the buddy's stats at the top of the screen.  Caller is responsible for
// only invoking this during GS_LEVEL with gametic set (the same gate as
// ST_Drawer / HU_Drawer).  No-op if -aicoop isn't active, the companion is
// dead/missing, or `show_buddy_hud` is off.
void HU_Buddy_Drawer (void);

// Called from i_video.c when the internal resolution changes.  Currently a
// no-op (the HUD doesn't need a per-resolution scratch buffer), but kept as
// a stable hook so the call site doesn't need to change if we add one later.
void HU_Buddy_SetRes (void);

// Current on/off state, exposed for the console / Options menu.
extern int show_buddy_hud;

#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------