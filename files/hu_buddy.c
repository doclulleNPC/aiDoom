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
#include "w_wad.h"                  // W_CheckNumForName / W_CacheLumpNum (buddy.wad faces)
#include "z_zone.h"                 // PU_STATIC

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


// ---------------------------------------------------------------------------
//  Buddy mugshot faces (BUF*) -- a distinct set from the player's STF*, packed
//  into buddy.wad (tools/bake_buddy_face.py).  Loaded lazily on first draw: the
//  WAD is added at startup (I_Voice_Init), so the lumps exist by the time the HUD
//  draws; if buddy.wad is absent the faces stay NULL and the HUD shows text only.
// ---------------------------------------------------------------------------
static patch_t* buf_face[5];        // BUFST00/10/20/30/40 (healthy .. near-death)
static patch_t* buf_dead;           // BUFDEAD0
static int      faces_tried;        // 0 = not yet, 1 = attempted (loaded or absent)

static void HU_Buddy_LoadFaces (void)
{
    int i, l;
    char nm[9];
    if (faces_tried) return;
    faces_tried = 1;
    for (i = 0; i < 5; i++)
    {
	sprintf (nm, "BUFST%d0", i);
	l = W_CheckNumForName (nm);
	buf_face[i] = (l >= 0) ? (patch_t*) W_CacheLumpNum (l, PU_STATIC) : NULL;
    }
    strcpy (nm, "BUFDEAD0");
    l = W_CheckNumForName (nm);
    buf_dead = (l >= 0) ? (patch_t*) W_CacheLumpNum (l, PU_STATIC) : NULL;
}

// Mugshot for the buddy's current health (mirrors the player's pain-offset buckets).
static patch_t* HU_Buddy_Face (int hp)
{
    int pain;
    if (hp <= 0) return buf_dead;
    pain = (100 - (hp > 100 ? 100 : hp)) / 20;
    if (pain < 0) pain = 0;
    if (pain > 4) pain = 4;
    return buf_face[pain];
}


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

// Top-right readout: the buddy's mugshot (by health) followed by two right-aligned
// message-font lines (HP/armor, weapon/ammo).  The mugshot replaces the old "BUDDY"
// label; if the BUF* faces are missing it falls back to that text label.
static void HU_Buddy_DrawStrip (player_t* bot)
{
    int      hp   = bot->health;
    int      arm  = bot->armorpoints;
    int      w    = bot->readyweapon;
    int      ammo = -1;
    int      wb   = SCREENWIDTH / hires;   // wide base width = the V_ coordinate space
    int      textw, tx;
    patch_t* face;
    char     l1[40], l2[40];

    HU_Buddy_LoadFaces ();
    face = HU_Buddy_Face (hp);

    if (w >= 0 && w < NUMWEAPONS && weaponinfo[w].ammo < NUMAMMO)
	ammo = bot->ammo[weaponinfo[w].ammo];

    // With a mugshot the lines are just stats; without one, prefix the "BUDDY" label.
    snprintf (l1, sizeof l1, face ? "HP %d  AR %d" : "BUDDY  HP %d  AR %d", hp, arm);
    if (ammo >= 0) snprintf (l2, sizeof l2, "%s %d",
			     (w >= 0 && w < NUMWEAPONS) ? weapon_short[w] : "", ammo);
    else           snprintf (l2, sizeof l2, "%s",
			     (w >= 0 && w < NUMWEAPONS) ? weapon_short[w] : "");

    textw = HU_Buddy_TextW (l1);
    { int w2 = HU_Buddy_TextW (l2); if (w2 > textw) textw = w2; }
    tx = wb - 4 - textw;

    HU_Buddy_Text (tx, 2,  l1);
    HU_Buddy_Text (tx, 12, l2);

    // Mugshot just left of the text block (BUF* patches carry a -5/-2 offset, so
    // V_DrawPatch shifts them right/down a touch -- accounted for in the x below).
    if (face) V_DrawPatch (tx - SHORT (face->width) - 6, 1, 0, face);
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
