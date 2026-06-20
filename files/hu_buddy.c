// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	"Second STBAR" -- a small status-bar-style HUD at the top of the screen for
//	the AI co-op companion, mirroring the player's bottom status bar in
//	st_stuff.c but at half size (16 BASE-pixels tall instead of 32) and using
//	a mix of patches (the same lumps STBAR/STTNUM/STYSNUM/STKEYS/STARMS use)
//	and TTF text (for the weapon name + state label, since the IWAD has no
//	"FIGHT"/"FOLLOW" patches).
//
//	Layout (BASE coords, 320x200):
//
//	   X=80..240 (160 wide, centred)  Y=0..15 (16 tall)
//	   +-------+------+------+ +-----+----+
//	   | HP100 | PIST | 042  | | 88U | FOLLOW |
//	   +-------+------+------+ +-----+----+
//
//	Everything is rendered at half scale (each patch pixel becomes a single
//	screen pixel in a sub-sampled 2x2 block read).  Labels use the baked
//	DejaVuSansMono TTF atlas (tools/font_atlas.h, shared with the console),
//	also rendered sub-sampled so they match the patch height (~7 px).
//
//	Authored in BASE_WIDTH (320) coordinates; the sub-sampled render writes
//	1 byte per output pixel into screens[0], so at hires=1 each BASE pixel is
//	one screen pixel.  At higher hires the whole buffer is then uniformly
//	upscaled by SDL when I_FinishUpdate copies it to the texture.
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

#include "../tools/font_atlas.h"   // baked DejaVuSansMono atlas (also used by the console)

#include "hu_buddy.h"
#include "p_ai_coop.h"

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

//
// TTF (DejaVuSansMono baked atlas, sub-sampled to match patch height)
//
#define BUDDY_TTF_SCALE_X   2   // take every 2nd atlas x pixel (10 -> 5)
#define BUDDY_TTF_SCALE_Y   2   // take every 2nd atlas y pixel (19 -> ~9)

//
// Per-state / weapon strings (used both for TTF rendering and skip-detection)
//
static const char* state_str[] = {
    "FOLLOW", "FIGHT", "HEAL", "HOLD", "COME", "GRAB",
};

static const char* weapon_short[] = {
    "FIST", "PISTOL", "SHOTGUN", "CHAINGUN",
    "ROCKET", "PLASMA", "BFG", "CHAINSAW", "SSG",
};


// Forward decls
static void HU_Buddy_DrawPatchHalf (int x, int y, patch_t* p);
static void HU_Buddy_DrawTtfString (int x, int y, const char* s, byte col);
static int  HU_Buddy_TtfStringWidth (const char* s);
static void HU_Buddy_DrawTtfChar (int x, int y, char ch, byte col);
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
		    byte* dest   = screens[0] + dy * SCREENWIDTH + col_x;
		    *dest = pix;
		}

		// Next post in this column
		column = (column_t*) ((byte*) column + column->length + 4);
	    }
	}
    }
}


// ===========================================================================
//  TTF rendering (sub-sampled, so each atlas pixel becomes one screen pixel
//  but we only sample every Nth atlas pixel for the half-scale appearance).
// ===========================================================================

static void HU_Buddy_DrawTtfChar (int x, int y, char ch, byte col)
{
    int c = (unsigned char) ch;
    int idx, gy, gx;

    if (c < FONT_FIRST || c >= FONT_FIRST + FONT_COUNT) return;
    idx = c - FONT_FIRST;

    for (gy = 0; gy < FONT_CH; gy += BUDDY_TTF_SCALE_Y)
    {
	int dy = y + gy / BUDDY_TTF_SCALE_Y;
	if (dy < 0 || dy >= SCREENHEIGHT) continue;
	for (gx = 0; gx < FONT_CW; gx += BUDDY_TTF_SCALE_X)
	{
	    int dx = x + gx / BUDDY_TTF_SCALE_X;
	    if (dx < 0 || dx >= SCREENWIDTH) continue;
	    byte a = font_alpha[gy * FONT_AW + idx * FONT_CW + gx];
	    if (a < 64) continue;            // threshold for visible pixel
	    screens[0][dy * SCREENWIDTH + dx] = col;
	}
    }
}

static int HU_Buddy_TtfStringWidth (const char* s)
{
    int n = (int) strlen (s);
    return n * (FONT_CW / BUDDY_TTF_SCALE_X);
}

static void HU_Buddy_DrawTtfString (int x, int y, const char* s, byte col)
{
    while (*s)
    {
	HU_Buddy_DrawTtfChar (x, y, *s, col);
	x += FONT_CW / BUDDY_TTF_SCALE_X;
	s++;
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
//  Strip layout (centred, half-scale, mimics the player STBAR field order)
//
//	  HP 100%   WEAPON   AMMO 042   88U   STATE:FOLLOW
//	  ^TTF  ^patch  ^TTF  ^patch  ^patch ^TTF
// ===========================================================================

// All X positions are absolute BASE pixels inside the centred 160-wide bar.
// Y positions are the top of the relevant glyph (baseline-style -- patches and
// TTF both drawn from their top edge here).
#define X_HP_LABEL   (BUDDY_BAR_X + 4)            // "HP" label
#define X_HP_VALUE   (X_HP_LABEL + 12)            // 3-digit value (right-justified here)
#define X_HP_PCT     (X_HP_VALUE + 0)             // % glyph after value
#define X_WEAPON     (X_HP_PCT + 9)               // weapon name
#define X_AMMO_LBL   (X_WEAPON + 32)              // "AM" label
#define X_AMMO_VALUE (X_AMMO_LBL + 12)            // 3-digit ammo
#define X_DIST_VALUE (X_AMMO_VALUE + 18)          // 3-digit dist + 'U'
#define X_STATE_LBL  (X_DIST_VALUE + 22)          // "S:" label
#define X_STATE_VAL  (X_STATE_LBL + 8)            // FOLLOW/FIGHT/etc.

// Y values: all elements aligned to the patch baseline (Y=2..15 inside the
// 16-tall bar).
#define Y_LABEL      (BUDDY_BAR_Y + 8)            // TTF label baseline
#define Y_PATCH      (BUDDY_BAR_Y + 8)            // patch top (matches label baseline)
#define Y_DIGITS     (BUDDY_BAR_Y + 4)            // digits sit higher (taller glyphs)


static void HU_Buddy_DrawStrip (player_t* bot, mobj_t* bot_mo)
{
    int     hp   = bot->health;
    int     arm  = bot->armorpoints;
    int     w    = bot->readyweapon;
    int     ammo = -1;
    int     dist = -1;
    int     st   = P_AICoop_State ();
    mobj_t* lis;
    int     i;
    fixed_t dx, dy;
    char    cell[16];

    if (w >= 0 && w < NUMWEAPONS && weaponinfo[w].ammo < NUMAMMO)
	ammo = bot->ammo[weaponinfo[w].ammo];

    // Distance to nearest live human (consoleplayer preferred).
    lis = players[consoleplayer].mo;
    if (!lis || players[consoleplayer].playerstate != PST_LIVE)
    {
	lis = NULL;
	for (i = 0; i < MAXPLAYERS; i++)
	{
	    if (i == P_AICoop_Slot () || !playeringame[i]) continue;
	    if (players[i].playerstate != PST_LIVE || !players[i].mo) continue;
	    lis = players[i].mo;
	    break;
	}
    }
    if (lis)
    {
	dx = bot_mo->x - lis->x;
	dy = bot_mo->y - lis->y;
	dist = (int)(P_AproxDistance (dx, dy) >> FRACBITS);
    }

    // Separator lines (top + bottom of the bar).  PLAYPAL index 96 is a
    // light grey that reads against both the dark sky and bright walls.
    {
	int x;
	for (x = BUDDY_BAR_X; x < BUDDY_BAR_X + BUDDY_BAR_W; x++)
	{
	    screens[0][BUDDY_BAR_Y * SCREENWIDTH + x] = BUDDY_BAR_COL;
	    screens[0][(BUDDY_BAR_Y + BUDDY_BAR_H - 1) * SCREENWIDTH + x]
		= BUDDY_BAR_COL;
	}
    }

    // HP: TTF "HP" + STTNUM-style 3 digits + STTPRCNT percent sign
    HU_Buddy_DrawTtfString   (X_HP_LABEL, Y_LABEL, "HP", 231);    // bright yellow
    HU_Buddy_DrawNumberTall  (X_HP_VALUE, Y_DIGITS, hp, 231, 3);
    HU_Buddy_DrawPatchHalf   (X_HP_PCT + 6, Y_DIGITS, p_sttprcnt);

    // Weapon name -- always present (melee weapons show their name too).
    if (w >= 0 && w < NUMWEAPONS)
	HU_Buddy_DrawTtfString (X_WEAPON, Y_LABEL, weapon_short[w], 231);

    // Ammo: only for weapons that actually take ammo.  TTF "A:" label +
    // STTNUM digits.
    if (ammo >= 0)
    {
	HU_Buddy_DrawTtfString  (X_AMMO_LBL, Y_LABEL, "A:", 231);
	HU_Buddy_DrawNumberTall (X_AMMO_VALUE, Y_DIGITS, ammo, 231, 3);
    }

    // Distance: TTF "D:" + digits + TTF "U" suffix.
    if (dist >= 0)
    {
	HU_Buddy_DrawTtfString  (X_DIST_VALUE - 12, Y_LABEL, "D:", 231);
	HU_Buddy_DrawNumberTall (X_DIST_VALUE,      Y_DIGITS, dist, 231, 3);
	HU_Buddy_DrawTtfChar    (X_DIST_VALUE + 4,  Y_LABEL, 'U', 231);
    }

    // State: TTF "S:" + state string.
    if (st >= 0 && st < (int)(sizeof(state_str)/sizeof(state_str[0])))
    {
	HU_Buddy_DrawTtfString (X_STATE_LBL, Y_LABEL, "S:", 231);
	HU_Buddy_DrawTtfString (X_STATE_VAL, Y_LABEL, state_str[st], 231);
    }

    (void) cell;
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