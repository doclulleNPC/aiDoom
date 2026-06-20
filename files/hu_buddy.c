// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	A compact status panel for the AI co-op companion, pinned to the TOP-RIGHT
//	corner -- a mini version of the player's bottom STBAR (st_stuff.c): a mugshot
//	that tracks the buddy's health (STFST00..40 / STFDEAD0) plus HP% / Armor% /
//	ammo in the half-scale STTNUM number font and labels + weapon name in the
//	small Doom HUD font (hu_font / STCFN*).  Sits on a colormap-darkened, bordered
//	panel so it reads against any 3D background.
//
//	Layout (panel-local BASE coords; PANEL_W x PANEL_H):
//
//	   +----------------------------------+
//	   | [FACE]  HP 100%      AM 050      |
//	   |         AR 099       PISTOL      |
//	   +----------------------------------+
//
//	Authored in BASE pixels and written through HU_Buddy_PutBlock, which scales
//	every BASE pixel to an hires x hires screen block and offsets the panel to the
//	top-right, so it grows proportionally with the internal resolution.
//
//-----------------------------------------------------------------------------

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_player.h"
#include "d_items.h"
#include "info.h"
#include "m_fixed.h"
#include "m_swap.h"
#include "p_local.h"
#include "p_mobj.h"
#include "r_defs.h"
#include "r_main.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

#include "hu_stuff.h"               // HU_FONTSTART / HU_FONTSIZE + the small Doom HUD font

#include "hu_buddy.h"
#include "p_ai_coop.h"

// The small Doom HUD font (STCFN033..STCFN095), loaded by HU_Init in hu_stuff.c.
extern patch_t* hu_font[HU_FONTSIZE];

// On/off (persisted as config key `show_buddy_hud`).  Default ON; the Drawer
// is a no-op when -aicoop isn't active anyway.
int show_buddy_hud = 1;

//
// Layout (BASE coords)
//
//	Bar strip: 160 wide, 16 tall, horizontally centred in BASE_WIDTH (320).
//	Top edge at Y=0 (so it sits just below the ceiling on tall screens, above
//	the centred player STBAR in widescreen).  All X positions are absolute
//	BASE pixels.
//
#define BUDDY_BAR_X       80                // bar left edge
#define BUDDY_BAR_Y       0                 // bar top edge
#define BUDDY_BAR_W       160               // bar width  (= half of 320)
#define BUDDY_BAR_H       16                // bar height (= half of STBAR's 32)

// Two thin separator lines: top (Y=0) + bottom (Y=15).  Drawn with PLAYPAL
// index 96 (light grey) so the bar reads against any 3D background.
#define BUDDY_BAR_COL     96

//
// Patch tables (loaded once in HU_Buddy_Init)
//
//	STTNUM0..9  : tall numbers for HP / Armor / Ammo / Dist
//	STTPRCNT    : tall percent sign (drawn after HP and Armor)
//	STYSNUM0..9 : short yellow numbers (rendered at 1x for the small ammo cells)
//	STKEYS0..5  : keycards / skulls
//	STARMS      : arms background grid (we'll just draw it once behind weapon names)
//
static patch_t* p_sttnum[10];
static patch_t* p_sttprcnt;
static patch_t* p_stbar;
static patch_t* p_starmsbg;
static patch_t* p_stkeys[6];
static patch_t* p_sttminus;
static patch_t* p_face[5];		// STFST00..STFST40 (healthy..near-death mugshot)
static patch_t* p_facedead;		// STFDEAD0

//
// Resolution scaling.  The panel is authored in BASE (320x200) pixels but the 3D
// framebuffer screens[0] is the NATIVE SCREENWIDTHxSCREENHEIGHT (hi-res), so a 1:1
// BASE write would leave it a tiny patch in the corner.  We scale every BASE pixel
// to a bs_scale x bs_scale screen block (bs_scale = hires) and offset it to the
// top-RIGHT of the screen.  Set once per frame in HU_Buddy_DrawStrip.
static int bs_scale = 1;
static int bs_xoff  = 0;
static int bs_yoff  = 0;

// Write one BASE-pixel as a bs_scale x bs_scale block at (bx,by) panel-local coords.
static void HU_Buddy_PutBlock (int bx, int by, byte pix)
{
    int sx0 = bx * bs_scale + bs_xoff;
    int sy0 = by * bs_scale + bs_yoff;
    int i, j;
    for (j = 0; j < bs_scale; j++)
    {
	int sy = sy0 + j;
	if (sy < 0 || sy >= SCREENHEIGHT) continue;
	for (i = 0; i < bs_scale; i++)
	{
	    int sx = sx0 + i;
	    if (sx < 0 || sx >= SCREENWIDTH) continue;
	    screens[0][sy * SCREENWIDTH + sx] = pix;
	}
    }
}

// Fill a panel-local BASE rectangle (used for the panel background + border).
static void HU_Buddy_FillRect (int bx, int by, int bw, int bh, byte col)
{
    int x, y;
    for (y = by; y < by + bh; y++)
	for (x = bx; x < bx + bw; x++)
	    HU_Buddy_PutBlock (x, y, col);
}

// Short weapon names (readyweapon index -> label) for the panel -- kept <=7 chars
// so they fit the column in the Doom HUD font.
static const char* weapon_short[] = {
    "FIST", "PISTOL", "SHOTGUN", "CHAINGN",
    "ROCKET", "PLASMA", "BFG", "SAW", "SSG",
};


// Forward decls
static void HU_Buddy_DrawPatchHalf (int x, int y, patch_t* p);
static void HU_Buddy_DrawFontStr  (int x, int y, const char* s, byte col);
static void HU_Buddy_DrawNumberTall (int x, int y, int n, byte col, int width);
static void HU_Buddy_DrawStrip (player_t* bot, mobj_t* bot_mo);


// ===========================================================================
//  Init
// ===========================================================================

void HU_Buddy_Init (void)
{
    int   i;
    char  namebuf[9];

    // Tall numbers (STTNUM0..9, 14x16 each) -- 7x8 after half scaling.
    for (i = 0; i < 10; i++)
    {
	sprintf (namebuf, "STTNUM%d", i);
	p_sttnum[i] = (patch_t*) W_CacheLumpName (namebuf, PU_STATIC);
    }
    p_sttprcnt = (patch_t*) W_CacheLumpName ("STTPRCNT", PU_STATIC);
    p_sttminus = (patch_t*) W_CacheLumpName ("STTMINUS", PU_STATIC);
    p_stbar    = (patch_t*) W_CacheLumpName ("STBAR",    PU_STATIC);
    p_starmsbg = (patch_t*) W_CacheLumpName ("STARMS",   PU_STATIC);

    for (i = 0; i < 6; i++)
    {
	sprintf (namebuf, "STKEYS%d", i);
	p_stkeys[i] = (patch_t*) W_CacheLumpName (namebuf, PU_STATIC);
    }

    // Mugshot faces: STFST00 (healthy) .. STFST40 (near death), + STFDEAD0.
    for (i = 0; i < 5; i++)
    {
	sprintf (namebuf, "STFST%d0", i);
	p_face[i] = (patch_t*) W_CacheLumpName (namebuf, PU_STATIC);
    }
    p_facedead = (patch_t*) W_CacheLumpName ("STFDEAD0", PU_STATIC);
}

void HU_Buddy_SetRes (void)
{
}


// ===========================================================================
//  Patch half-scale renderer
//
//	DOOM patches are column-major: each column is a list of posts (topdelta,
//	length, data[length]) terminated by topdelta=0xff.  Pixels are raw
//	PLAYPAL indices -- just like the FG bytebuffer -- so we can copy them
//	straight in.
//
//	We render at 0.5x by reading a 2x2 patch block for every output pixel and
//	taking the top-left pixel (nearest-neighbour).  For patches whose width
//	or height is odd we still emit floor(w/2) x floor(h/2) output pixels so
//	the half-scaled bar stays 160 wide and 16 tall.
// ===========================================================================

static void HU_Buddy_DrawPatchHalf (int x, int y, patch_t* p)
{
    int  pw = SHORT (p->width);
    int  ph = SHORT (p->height);
    int  ox = x - SHORT (p->leftoffset) / 2;
    int  oy = y - SHORT (p->topoffset)  / 2;
    int  col, row;

    for (col = 0; col < pw; col++)
    {
	int col_x = ox + col / 2;
	if (col_x < 0 || col_x >= SCREENWIDTH) continue;
	if ((col & 1) != 0) continue;       // take every 2nd source column
	{
	    int*      cofs   = (int*) ((byte*) p + 8);          // columnofs[]
	    int       coff   = LONG (cofs[col]);
	    column_t* column = (column_t*) ((byte*) p + coff);

	    while (column->topdelta != 0xff)
	    {
		int   topdelta = column->topdelta;
		int   length   = column->length;
		byte* src      = (byte*) column + 3;

		// Walk each pixel in this post.  We emit output rows for every
		// second source row, starting at topdelta/2.
		int   start_row = oy + topdelta / 2;
		int   end_row   = oy + (topdelta + length) / 2;
		int   src_y;
		int   dy;

		if (start_row < 0) start_row = 0;
		if (end_row >= SCREENHEIGHT) end_row = SCREENHEIGHT - 1;

		for (src_y = topdelta, dy = start_row;
		     dy <= end_row && src_y < topdelta + length;
		     src_y += 2, dy++)
		{
		    byte  pix    = src[src_y];
		    HU_Buddy_PutBlock (col_x, dy, pix);
		}

		// Next post in this column
		column = (column_t*) ((byte*) column + column->length + 4);
	    }
	}
    }
}


// ===========================================================================
//  Small Doom HUD font (hu_font, STCFN*) -- full scale, recoloured to `col`.
//  The font glyphs are ~8 px tall (matching the half-scale STTNUM digits), and
//  far crisper/readable than the sub-sampled TTF the panel used before.
// ===========================================================================

// Draw one hu_font glyph at full scale through PutBlock, recoloured to `col`.
// Returns the glyph width (BASE px) to advance.
static int HU_Buddy_DrawFontChar (int x, int y, patch_t* p, byte col)
{
    int  pw = SHORT (p->width);
    int* cofs = (int*) ((byte*) p + 8);
    int  c;
    for (c = 0; c < pw; c++)
    {
	column_t* column = (column_t*) ((byte*) p + LONG (cofs[c]));
	while (column->topdelta != 0xff)
	{
	    int k;
	    for (k = 0; k < column->length; k++)
		HU_Buddy_PutBlock (x + c, y + column->topdelta + k, col);
	    column = (column_t*) ((byte*) column + column->length + 4);
	}
    }
    return pw;
}

static void HU_Buddy_DrawFontStr (int x, int y, const char* s, byte col)
{
    for (; *s; s++)
    {
	char ch = *s;
	if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';        // uppercase
	if (ch == ' ' || ch < HU_FONTSTART || ch > HU_FONTEND) { x += 4; continue; }
	{
	    patch_t* p = hu_font[ch - HU_FONTSTART];
	    if (!p) { x += 4; continue; }
	    x += HU_Buddy_DrawFontChar (x, y, p, col) + 1;
	}
    }
}


// ===========================================================================
//  Tall-number renderer (STTNUM-style, half scale, right-justified)
//
//	Draws a decimal integer at (x,y) right-justified over `width` digits.
//	Each digit is the 7x8 half-scaled STTNUM glyph (originally 14x16).
// ===========================================================================

static void HU_Buddy_DrawNumberTall (int x, int y, int n, byte col, int width)
{
    int digits[8];
    int i;
    int digit_w = SHORT (p_sttnum[0]->width)  / 2;   // 7
    int cx;

    if (n < 0) n = 0;
    if (n > 99999) n = 99999;

    for (i = 0; i < width; i++)
    {
	digits[i] = n % 10;
	n /= 10;
    }

    cx = x;
    for (i = 0; i < width; i++)
    {
	cx -= digit_w;
	HU_Buddy_DrawPatchHalf (cx, y, p_sttnum[digits[i]]);
    }
}


// ===========================================================================
//  Panel layout -- a compact, readable mini status bar pinned to the TOP-RIGHT,
//  styled like the player STBAR (mugshot + big STTNUM numbers) but half size:
//
//	+--------------------------------+
//	| [FACE]  HP 100%   AM 050       |
//	|         AR 099    CHAINGUN     |
//	+--------------------------------+
//
//  Authored in panel-local BASE pixels (origin = panel top-left); PutBlock scales
//  by hires and offsets to the top-right corner (set in HU_Buddy_DrawStrip).
// ===========================================================================

#define PANEL_W        140     // panel size (BASE px) -- wider for the Doom HUD font
#define PANEL_H        26
#define PANEL_RMARGIN  4       // gap from the right screen edge (BASE px)
#define PANEL_TMARGIN  4       // gap from the top screen edge

#define FACE_X         5       // mugshot (24x29 -> 12x14 half)
#define FACE_Y         6
#define C1_LBL         22      // column 1: label x / number right-edge x
#define C1_NUM         66
#define C2_LBL         82      // column 2
#define C2_NUM         126
#define ROW1           5       // two text rows (top of the 8px-tall glyphs)
#define ROW2           15

#define BUDDY_FG       231     // bright yellow (labels + numbers)
#define BUDDY_BORDER   96      // light-grey border
#define BUDDY_BG_DARK  24      // colormap index used to darken the backdrop

// Darken the (scaled) panel rect through a colormap so the panel reads against any
// 3D background without fully hiding it.
static void HU_Buddy_DarkenRect (int bx, int by, int bw, int bh, int mapidx)
{
    int x0 = bx * bs_scale + bs_xoff, y0 = by * bs_scale + bs_yoff;
    int x1 = x0 + bw * bs_scale,      y1 = y0 + bh * bs_scale;
    int x, y;
    for (y = y0; y < y1; y++)
    {
	if (y < 0 || y >= SCREENHEIGHT) continue;
	for (x = x0; x < x1; x++)
	{
	    if (x < 0 || x >= SCREENWIDTH) continue;
	    byte* p = &screens[0][y * SCREENWIDTH + x];
	    *p = colormaps[mapidx * 256 + *p];
	}
    }
}


static void HU_Buddy_DrawStrip (player_t* bot, mobj_t* bot_mo)
{
    int      hp   = bot->health;
    int      arm  = bot->armorpoints;
    int      w    = bot->readyweapon;
    int      ammo = -1;
    int      pain;
    patch_t* face;

    (void) bot_mo;

    // Pin to the TOP-RIGHT corner, scaled by hires.  Everything below is authored in
    // panel-local BASE pixels and routed through PutBlock, which applies these.
    bs_scale = hires;
    bs_xoff  = SCREENWIDTH - (PANEL_W + PANEL_RMARGIN) * bs_scale;
    bs_yoff  = PANEL_TMARGIN * bs_scale;

    if (w >= 0 && w < NUMWEAPONS && weaponinfo[w].ammo < NUMAMMO)
	ammo = bot->ammo[weaponinfo[w].ammo];

    // Background: darken the backdrop, then a light border.
    HU_Buddy_DarkenRect (0, 0, PANEL_W, PANEL_H, BUDDY_BG_DARK);
    {
	int x, y;
	for (x = 0; x < PANEL_W; x++) { HU_Buddy_PutBlock (x, 0, BUDDY_BORDER); HU_Buddy_PutBlock (x, PANEL_H - 1, BUDDY_BORDER); }
	for (y = 0; y < PANEL_H; y++) { HU_Buddy_PutBlock (0, y, BUDDY_BORDER); HU_Buddy_PutBlock (PANEL_W - 1, y, BUDDY_BORDER); }
    }

    // Mugshot, picked by health (healthy -> near-death; dead face at 0 HP).
    pain = (100 - (hp < 0 ? 0 : hp > 100 ? 100 : hp)) / 20;
    if (pain < 0) pain = 0;
    if (pain > 4) pain = 4;
    face = (hp <= 0) ? p_facedead : p_face[pain];
    if (face) HU_Buddy_DrawPatchHalf (FACE_X, FACE_Y, face);

    // Column 1: HP% (row1), Armor% (row2)
    HU_Buddy_DrawFontStr  (C1_LBL, ROW1, "HP", BUDDY_FG);
    HU_Buddy_DrawNumberTall (C1_NUM, ROW1, hp, BUDDY_FG, 3);
    HU_Buddy_DrawPatchHalf  (C1_NUM, ROW1, p_sttprcnt);
    HU_Buddy_DrawFontStr  (C1_LBL, ROW2, "AR", BUDDY_FG);
    HU_Buddy_DrawNumberTall (C1_NUM, ROW2, arm, BUDDY_FG, 3);
    HU_Buddy_DrawPatchHalf  (C1_NUM, ROW2, p_sttprcnt);

    // Column 2: ammo (row1, if the weapon uses it), weapon name (row2)
    if (ammo >= 0)
    {
	HU_Buddy_DrawFontStr  (C2_LBL, ROW1, "AM", BUDDY_FG);
	HU_Buddy_DrawNumberTall (C2_NUM, ROW1, ammo, BUDDY_FG, 3);
    }
    if (w >= 0 && w < NUMWEAPONS)
	HU_Buddy_DrawFontStr (C2_LBL, ROW2, weapon_short[w], BUDDY_FG);
}


// ===========================================================================
//  Drawer (called from HU_Drawer in hu_stuff.c)
// ===========================================================================

void HU_Buddy_Drawer (void)
{
    player_t* bot;
    mobj_t*   bot_mo;
    int       slot;

    if (!show_buddy_hud) return;
    slot = P_AICoop_Slot ();
    if (slot < 0) return;
    if (!playeringame[slot]) return;
    bot = &players[slot];
    if (bot->playerstate != PST_LIVE || !bot->mo) return;
    bot_mo = bot->mo;

    if (menuactive || paused) return;
    {
	extern gamestate_t wipegamestate;
	if (wipegamestate != GS_LEVEL) return;
    }

    HU_Buddy_DrawStrip (bot, bot_mo);
}