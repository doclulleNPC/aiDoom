// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	A compact readout for the AI co-op companion, pinned to the TOP-RIGHT corner.
//	It is drawn in the small Doom HUD message font (hu_font / STCFN*) at the SAME
//	size and native colour as the in-game pickup messages -- via V_DrawPatch, which
//	handles the hi-res scaling -- so it stays crisp and readable at any internal
//	resolution.  Two right-aligned lines:
//
//	   BUDDY  HP 100  AR 99
//	   SHOTGUN 50
//
//	Authored in BASE (320x200 / wide-base) coordinates; V_DrawPatch does the rest.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "d_items.h"
#include "m_swap.h"
#include "r_defs.h"
#include "v_video.h"

#include "hu_stuff.h"               // HU_FONTSTART / HU_FONTSIZE + the small Doom HUD font

#include "hu_buddy.h"
#include "p_ai_coop.h"

// The small Doom HUD font (STCFN033..STCFN095), loaded by HU_Init in hu_stuff.c.
extern patch_t* hu_font[HU_FONTSIZE];

// On/off (persisted as config key `show_buddy_hud`).  Default ON; the Drawer is a
// no-op when no co-op buddy is active anyway.
int show_buddy_hud = 1;

// Weapon names (readyweapon index -> label) for the readout.
static const char* weapon_short[] = {
    "FIST", "PISTOL", "SHOTGUN", "CHAINGUN",
    "ROCKET", "PLASMA", "BFG", "CHAINSAW", "SSG",
};


// hu_font is loaded by HU_Init; nothing else to cache here.
void HU_Buddy_Init  (void) {}
void HU_Buddy_SetRes (void) {}


// ===========================================================================
//  Small Doom HUD font (hu_font) -- drawn / measured exactly like a message.
//  V_DrawPatch copies the patch's own (native-colour) pixels and applies the
//  hi-res scaling, so the readout matches the in-game messages 1:1.
// ===========================================================================

static int HU_Buddy_TextW (const char* s)
{
    int w = 0;
    for (; *s; s++)
    {
	char c = *s;
	if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
	if (c == ' ' || c < HU_FONTSTART || c > HU_FONTEND) { w += 4; continue; }
	{ patch_t* p = hu_font[c - HU_FONTSTART]; w += p ? SHORT (p->width) : 4; }
    }
    return w;
}

static void HU_Buddy_Text (int x, int y, const char* s)
{
    for (; *s; s++)
    {
	char c = *s;
	if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
	if (c == ' ' || c < HU_FONTSTART || c > HU_FONTEND) { x += 4; continue; }
	{
	    patch_t* p = hu_font[c - HU_FONTSTART];
	    if (!p) { x += 4; continue; }
	    V_DrawPatch (x, y, 0, p);
	    x += SHORT (p->width);
	}
    }
}

// Two right-aligned message-font lines at the top-right (HP/armor, weapon/ammo).
static void HU_Buddy_DrawStrip (player_t* bot)
{
    int  hp   = bot->health;
    int  arm  = bot->armorpoints;
    int  w    = bot->readyweapon;
    int  ammo = -1;
    int  wb   = SCREENWIDTH / hires;   // wide base width = the V_ coordinate space
    char l1[40], l2[40];

    if (w >= 0 && w < NUMWEAPONS && weaponinfo[w].ammo < NUMAMMO)
	ammo = bot->ammo[weaponinfo[w].ammo];

    snprintf (l1, sizeof l1, "BUDDY  HP %d  AR %d", hp, arm);
    if (ammo >= 0) snprintf (l2, sizeof l2, "%s %d",
			     (w >= 0 && w < NUMWEAPONS) ? weapon_short[w] : "", ammo);
    else           snprintf (l2, sizeof l2, "%s",
			     (w >= 0 && w < NUMWEAPONS) ? weapon_short[w] : "");

    HU_Buddy_Text (wb - 4 - HU_Buddy_TextW (l1), 1,  l1);
    HU_Buddy_Text (wb - 4 - HU_Buddy_TextW (l2), 11, l2);
}


// ===========================================================================
//  Drawer (called from HU_Drawer in hu_stuff.c)
// ===========================================================================

void HU_Buddy_Drawer (void)
{
    player_t* bot;
    int       slot;

    if (!show_buddy_hud) return;
    slot = P_AICoop_Slot ();
    if (slot < 0 || !playeringame[slot]) return;
    bot = &players[slot];
    if (bot->playerstate != PST_LIVE || !bot->mo) return;
    if (menuactive || paused) return;
    {
	extern gamestate_t wipegamestate;
	if (wipegamestate != GS_LEVEL) return;
    }

    HU_Buddy_DrawStrip (bot);
}
