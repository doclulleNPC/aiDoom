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
#include "tables.h"                 // ANG45 / ANG180 (attacker-direction face)
#include "r_defs.h"
#include "r_main.h"                 // R_PointToAngle2
#include "v_video.h"
#include "w_wad.h"                  // W_CheckNumForName / W_CacheLumpNum (aidoom.wad faces)
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

// Buddy status line, indexed by P_AICoop_State() (0=follow .. 5=grab).
static const char* buf_status[] = {
    "FOLLOWING", "ATTACKING", "HEALING", "HOLDING", "COMING", "GRABBING",
};


// hu_font is loaded by HU_Init; nothing else to cache here.
void HU_Buddy_Init  (void) {}
void HU_Buddy_SetRes (void) {}


// ---------------------------------------------------------------------------
//  Buddy mugshot faces (BUF*) -- a distinct set from the player's STF*, packed
//  into aidoom.wad (tools/bake_buddy_face.py).  Loaded lazily: the WAD is added at
//  startup (I_Voice_Init), so the lumps exist by the time the HUD ticks/draws; if
//  aidoom.wad is absent the faces stay unloaded and the HUD falls back to a text
//  label.  The face is ANIMATED exactly like the player's (st_stuff.c): a flat
//  42-entry array in the same layout, driven by a ported ST_updateFaceWidget state
//  machine ticked once per game tic (HU_Buddy_Ticker).
//
//  Layout per pain level p (0=healthy .. 4=near death), 8 faces each:
//     +0,1,2  straight (look c/r/l)   +3 turn-right  +4 turn-left
//     +5 ouch  +6 evil grin  +7 rampage     then GOD (40), DEAD (41).
// ---------------------------------------------------------------------------
#define BF_NUMFACES		42
#define BF_STRIDE		8
#define BF_TURNOFFSET		3
#define BF_OUCHOFFSET		5
#define BF_EVILGRINOFFSET	6
#define BF_RAMPAGEOFFSET	7
#define BF_GODFACE		40
#define BF_DEADFACE		41
#define BF_EVILGRINCOUNT	(2*TICRATE)
#define BF_STRAIGHTFACECOUNT	(TICRATE/2)
#define BF_TURNCOUNT		(1*TICRATE)
#define BF_RAMPAGEDELAY		(2*TICRATE)
#define BF_MUCHPAIN		20

static patch_t* buf_faces[BF_NUMFACES];
static int      faces_tried;        // 0 = not yet, 1 = attempted
static int      faces_ok;           // all 42 lumps present?
static int      bf_index;           // current face (index into buf_faces), set by the Ticker
static int      bf_count;           // tics left on the current expression

// Cosmetic RNG for the random "look" direction.  Deliberately NOT M_Random -- the
// HUD must never touch the game RNG (it would desync the deterministic playsim).
static int HU_Buddy_FaceRand (void)
{
    static unsigned s = 1;
    s = s * 1103515245u + 12345u;
    return (int)((s >> 16) & 0x7fff);
}

static patch_t* HU_Buddy_LoadFace (const char* name, int* ok)
{
    char nm[9];
    int  l;
    strncpy (nm, name, 8); nm[8] = 0;
    l = W_CheckNumForName (nm);
    if (l < 0) { *ok = 0; return NULL; }
    return (patch_t*) W_CacheLumpNum (l, PU_STATIC);
}

// Medikit pickup sprite -- shown in the HUD (in place of the mugshot) while the
// buddy is DOWN, so the player knows there's a revivable body to reach.
static patch_t* HU_Buddy_Medkit (void)
{
    static patch_t* med;
    static int      tried;
    if (!tried)
    {
	int l = W_CheckNumForName ("MEDIA0");	// MT_MISC11 medikit sprite, frame A
	if (l >= 0) med = (patch_t*) W_CacheLumpNum (l, PU_STATIC);
	tried = 1;
    }
    return med;
}

static void HU_Buddy_LoadFaces (void)
{
    int  i, j, fn = 0, ok = 1;
    char nm[9];
    if (faces_tried) return;
    faces_tried = 1;
    for (i = 0; i < 5; i++)
    {
	for (j = 0; j < 3; j++)
	    { sprintf (nm, "BUFST%d%d", i, j); buf_faces[fn++] = HU_Buddy_LoadFace (nm, &ok); }
	sprintf (nm, "BUFTR%d0", i);  buf_faces[fn++] = HU_Buddy_LoadFace (nm, &ok);  // turn right
	sprintf (nm, "BUFTL%d0", i);  buf_faces[fn++] = HU_Buddy_LoadFace (nm, &ok);  // turn left
	sprintf (nm, "BUFOUCH%d", i); buf_faces[fn++] = HU_Buddy_LoadFace (nm, &ok);  // ouch
	sprintf (nm, "BUFEVL%d", i);  buf_faces[fn++] = HU_Buddy_LoadFace (nm, &ok);  // evil grin
	sprintf (nm, "BUFKILL%d", i); buf_faces[fn++] = HU_Buddy_LoadFace (nm, &ok);  // rampage
    }
    buf_faces[fn++] = HU_Buddy_LoadFace ("BUFGOD0",  &ok);
    buf_faces[fn++] = HU_Buddy_LoadFace ("BUFDEAD0", &ok);
    faces_ok = ok;
}

// FACESTRIDE * pain-bucket, exactly like ST_calcPainOffset.
static int HU_Buddy_PainOffset (int health)
{
    if (health > 100) health = 100;
    if (health < 0)   health = 0;
    return BF_STRIDE * (((100 - health) * 5) / 101);
}

// Ported ST_updateFaceWidget, driven by the buddy's player_t.  Runs once per tic
// (HU_Buddy_Ticker).  Precedence: dead > evil grin > attacked/turn > self-hurt >
// rampage > god > idle look.  (The vanilla "ouch" health-delta test is inverted
// here so the ouch face actually triggers on a big hit -- the original `health -
// oldhealth > MUCHPAIN` is the well-known Doom bug that all but disables it.)
static void HU_Buddy_FaceTick (player_t* bot)
{
    static int     lastattackdown = -1;
    static int     priority = 0;
    static int     oldhealth = -1;
    static boolean oldweap[NUMWEAPONS];
    int            i;
    angle_t        badang, diffang;
    boolean        grin;

    if (priority < 10 && !bot->health)
	{ priority = 9; bf_index = BF_DEADFACE; bf_count = 1; }

    if (priority < 9 && bot->bonuscount)
    {
	grin = false;
	for (i = 0; i < NUMWEAPONS; i++)
	    if (oldweap[i] != bot->weaponowned[i]) { grin = true; oldweap[i] = bot->weaponowned[i]; }
	if (grin)
	    { priority = 8; bf_count = BF_EVILGRINCOUNT;
	      bf_index = HU_Buddy_PainOffset (bot->health) + BF_EVILGRINOFFSET; }
    }

    if (priority < 8 && bot->damagecount && bot->attacker && bot->attacker != bot->mo)
    {
	priority = 7;
	if (oldhealth - bot->health > BF_MUCHPAIN)
	    { bf_count = BF_TURNCOUNT; bf_index = HU_Buddy_PainOffset (bot->health) + BF_OUCHOFFSET; }
	else
	{
	    badang = R_PointToAngle2 (bot->mo->x, bot->mo->y, bot->attacker->x, bot->attacker->y);
	    if (badang > bot->mo->angle) { diffang = badang - bot->mo->angle; i = diffang > ANG180; }
	    else                         { diffang = bot->mo->angle - badang; i = diffang <= ANG180; }
	    bf_count = BF_TURNCOUNT;
	    bf_index = HU_Buddy_PainOffset (bot->health);
	    if      (diffang < ANG45) bf_index += BF_RAMPAGEOFFSET;   // head-on
	    else if (i)               bf_index += BF_TURNOFFSET;      // turn right
	    else                      bf_index += BF_TURNOFFSET + 1;  // turn left
	}
    }

    if (priority < 7 && bot->damagecount)
    {
	if (oldhealth - bot->health > BF_MUCHPAIN)
	    { priority = 7; bf_count = BF_TURNCOUNT;
	      bf_index = HU_Buddy_PainOffset (bot->health) + BF_OUCHOFFSET; }
	else
	    { priority = 6; bf_count = BF_TURNCOUNT;
	      bf_index = HU_Buddy_PainOffset (bot->health) + BF_RAMPAGEOFFSET; }
    }

    if (priority < 6)
    {
	if (bot->attackdown)
	{
	    if (lastattackdown == -1) lastattackdown = BF_RAMPAGEDELAY;
	    else if (!--lastattackdown)
		{ priority = 5; bf_index = HU_Buddy_PainOffset (bot->health) + BF_RAMPAGEOFFSET;
		  bf_count = 1; lastattackdown = 1; }
	}
	else lastattackdown = -1;
    }

    if (priority < 5 && ((bot->cheats & CF_GODMODE) || bot->powers[pw_invulnerability]))
	{ priority = 4; bf_index = BF_GODFACE; bf_count = 1; }

    if (!bf_count)
	{ bf_index = HU_Buddy_PainOffset (bot->health) + (HU_Buddy_FaceRand () % 3);
	  bf_count = BF_STRAIGHTFACECOUNT; priority = 0; }

    bf_count--;
    oldhealth = bot->health;
}

// Per-tic face update (called from HU_Ticker).
void HU_Buddy_Ticker (void)
{
    int       slot;
    player_t* bot;

    if (!show_buddy_hud) return;
    slot = P_AICoop_Slot ();
    if (slot < 0 || !playeringame[slot]) return;
    HU_Buddy_LoadFaces ();
    if (!faces_ok) return;
    bot = &players[slot];
    HU_Buddy_FaceTick (bot);
}

// The current animated mugshot (or NULL if the faces aren't available).
static patch_t* HU_Buddy_Face (void)
{
    HU_Buddy_LoadFaces ();
    if (!faces_ok || bf_index < 0 || bf_index >= BF_NUMFACES) return NULL;
    return buf_faces[bf_index];
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
    int      textw, tx, st;
    patch_t* face;
    char     l1[40], l2[40], l3[40];

    // Downed (incapacitated, not dead): replace the mugshot + stats with a
    // medikit and a REVIVE prompt so the player knows to reach and revive him.
    if (bot->playerstate == PST_DEAD)
    {
	patch_t*    med = HU_Buddy_Medkit ();
	const char* d1  = "BUDDY DOWN";
	const char* d2  = "REVIVE: USE";
	int         dw  = HU_Buddy_TextW (d1);
	int         w2  = HU_Buddy_TextW (d2);
	int         dtx;
	if (w2 > dw) dw = w2;
	dtx = wb - 4 - dw;
	HU_Buddy_Text (dtx, 2,  d1);
	HU_Buddy_Text (dtx, 12, d2);
	if (med) V_DrawPatch (dtx - SHORT (med->width) - 6, 1, 0, med);

	// Compass: a top-centre arrow pointing the human toward the downed buddy so
	// they can find and revive him.  Screen-relative bearing -> one of 4 cardinal
	// arrows (RARR* PNGs in aidoom.wad, decoded via V_CachePNG).
	{
	    mobj_t* pl = players[consoleplayer].mo;	// the human (not the buddy)
	    mobj_t* bd = bot->mo;
	    if (pl && bd)
	    {
		// rel angle: 0 ahead, ANG90 to our left, ANG180 behind, ANG270 to our right.
		angle_t		rel = R_PointToAngle2 (pl->x, pl->y, bd->x, bd->y) - pl->angle;
		unsigned	q   = (unsigned)(rel + ANG45) >> 30;	// 0..3
		static const char* arr[4] = { "RARRC0", "RARRA0", "RARRD0", "RARRB0" }; // up,left,down,right
		patch_t*	a = V_CachePNG (arr[q]);
		if (a)
		    V_DrawPatch (wb/2 - SHORT (a->width)/2, 18, 0, a);	// top-centre
	    }
	}
	return;
    }

    face = HU_Buddy_Face ();
    st   = P_AICoop_State ();

    if (w >= 0 && w < NUMWEAPONS && weaponinfo[w].ammo < NUMAMMO)
	ammo = bot->ammo[weaponinfo[w].ammo];

    // With a mugshot the lines are just stats; without one, prefix the "BUDDY" label.
    snprintf (l1, sizeof l1, face ? "HP %d  AR %d" : "BUDDY  HP %d  AR %d", hp, arm);
    if (ammo >= 0) snprintf (l2, sizeof l2, "%s %d",
			     (w >= 0 && w < NUMWEAPONS) ? weapon_short[w] : "", ammo);
    else           snprintf (l2, sizeof l2, "%s",
			     (w >= 0 && w < NUMWEAPONS) ? weapon_short[w] : "");
    snprintf (l3, sizeof l3, "%s",
	      (st >= 0 && st < (int)(sizeof(buf_status)/sizeof(buf_status[0]))) ? buf_status[st] : "");

    textw = HU_Buddy_TextW (l1);
    { int w2 = HU_Buddy_TextW (l2); if (w2 > textw) textw = w2; }
    { int w3 = HU_Buddy_TextW (l3); if (w3 > textw) textw = w3; }
    tx = wb - 4 - textw;

    HU_Buddy_Text (tx, 2,  l1);
    HU_Buddy_Text (tx, 12, l2);
    HU_Buddy_Text (tx, 22, l3);

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
    if (!bot->mo) return;
    // Draw while alive OR while DOWN (PST_DEAD = incapacitated, revivable) -- the
    // down view shows a medkit so the player can find him.
    if (bot->playerstate != PST_LIVE && bot->playerstate != PST_DEAD) return;
    if (menuactive || paused) return;
    {
	extern gamestate_t wipegamestate;
	if (wipegamestate != GS_LEVEL) return;
    }

    HU_Buddy_DrawStrip (bot);
}
