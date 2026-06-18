// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Quake-style drop-down developer console.
//
//	Toggle with backquote (`).  While open it captures the keyboard, shows a
//	scrollback of output and an input line, and runs a small set of commands
//	(help, clear, echo, quit, god, noclip, give, map/warp).  The game keeps
//	ticking underneath; the console just overlays the top of the screen.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "v_video.h"
#include "r_defs.h"
#include "m_swap.h"
#include "hu_stuff.h"
#include "g_game.h"
#include "p_mobj.h"

#include "tables.h"		// finecosine, ANGLETOFINESHIFT
#include "info.h"		// mobjtype_t, MT_*
#include "m_fixed.h"		// FixedMul

#include "c_console.h"
#include "p_ai_coop.h"		// companion commands (where/come/wait/attack/report)
#include "p_ai_llm.h"		// director on/off toggle

extern patch_t*		hu_font[HU_FONTSIZE];
extern const char*	shiftxform;
extern lighttable_t*	colormaps;
extern void		I_Quit (void);

// Play-sim hooks, declared by hand to avoid p_local.h (its p_spec.h enums
// 'open'/'close' collide with code elsewhere).
extern thinker_t	thinkercap;
extern mobj_t*		P_SpawnMobj (fixed_t x, fixed_t y, fixed_t z, mobjtype_t type);
extern void		P_DamageMobj (mobj_t* target, mobj_t* inflictor, mobj_t* source, int dmg);
extern void		P_MobjThinker (mobj_t* mobj);

#define CON_H		120		// console height in BASE (320x200) rows
#define CON_DARK	22		// colormap level used to dim the view behind it
#define CON_LINES	256		// scrollback ring size
#define CON_LINEW	128
#define CON_INPUTW	128
#define LINE_STEP	5		// BASE pixels per text row (half-size font)

static char	con_text[CON_LINES][CON_LINEW];
static int	con_head;		// next slot to write
static int	con_count;		// lines stored (<= CON_LINES)
static int	con_scroll;		// scrollback offset (0 = newest)

static char	con_input[CON_INPUTW];
static int	con_inlen;

static int	con_open;
static int	con_shift;
static int	con_blink;		// cursor blink counter (per draw)


int C_Active (void) { return con_open; }


// ---------------------------------------------------------------- output
void C_Printf (const char* fmt, ...)
{
    char	buf[1024];
    char*	p;
    char*	start;
    va_list	ap;

    va_start (ap, fmt);
    vsnprintf (buf, sizeof(buf), fmt, ap);
    va_end (ap);

    start = buf;
    for (p = buf; ; p++)
    {
	if (*p == '\n' || *p == '\0')
	{
	    int	last = *p == '\0';
	    *p = '\0';
	    strncpy (con_text[con_head], start, CON_LINEW-1);
	    con_text[con_head][CON_LINEW-1] = '\0';
	    con_head = (con_head + 1) % CON_LINES;
	    if (con_count < CON_LINES) con_count++;
	    start = p + 1;
	    if (last) break;
	}
    }
    con_scroll = 0;	// jump to the newest line on new output
}


void C_Init (void)
{
    con_head = con_count = con_inlen = con_open = con_shift = con_scroll = 0;
    con_input[0] = '\0';
    C_Printf ("aiDoom console.  Type 'help'.  Toggle with ` (backquote) or the console key.");
}


// ---------------------------------------------------------------- commands
static void C_GiveAll (player_t* p)
{
    int i;
    for (i=0 ; i<NUMWEAPONS ; i++) p->weaponowned[i] = true;
    for (i=0 ; i<NUMAMMO ; i++)    p->ammo[i] = p->maxammo[i];
    for (i=0 ; i<NUMCARDS ; i++)   p->cards[i] = true;
    p->armorpoints = 200;
    p->armortype   = 2;
    p->health = 100;
    if (p->mo) p->mo->health = 100;
}

// Map a short name to a thing type for the "spawn" command.
static int C_MobjByName (const char* s)
{
    if (!strcmp(s,"imp"))				 return MT_TROOP;
    if (!strcmp(s,"demon") || !strcmp(s,"pinky"))	 return MT_SERGEANT;
    if (!strcmp(s,"spectre"))				 return MT_SHADOWS;
    if (!strcmp(s,"baron"))				 return MT_BRUISER;
    if (!strcmp(s,"zombie") || !strcmp(s,"zombieman"))	 return MT_POSSESSED;
    if (!strcmp(s,"shotgunner") || !strcmp(s,"sergeant")) return MT_SHOTGUY;
    if (!strcmp(s,"lostsoul") || !strcmp(s,"soul"))	 return MT_SKULL;
    if (!strcmp(s,"barrel"))				 return MT_BARREL;
    return -1;
}

static void C_Execute (char* line)
{
    char	cmd[64];
    char*	args;
    int		n;
    player_t*	pl = &players[consoleplayer];
    boolean	inlevel = (gamestate == GS_LEVEL);

    while (*line == ' ') line++;
    if (!*line) return;

    C_Printf ("] %s", line);			// echo the entered line

    // split command word / rest
    n = 0;
    while (line[n] && line[n] != ' ' && n < (int)sizeof(cmd)-1) { cmd[n] = tolower(line[n]); n++; }
    cmd[n] = '\0';
    args = line + n;
    while (*args == ' ') args++;

    if (!strcmp(cmd, "help"))
    {
	C_Printf ("cheats: god  noclip  give  kill  health <n>  armor <n>  ammo");
	C_Printf ("world:  spawn <thing>  skill <1-5>  map <e> <m> / warp <m>");
	C_Printf ("buddy:  where  come  wait/stay  attack  report");
	C_Printf ("monsterAI: director on|off|demo  (LLM<->Doom)");
	C_Printf ("misc:   clear  echo <text>  quit");
    }
    else if (!strcmp(cmd, "clear"))
	{ con_head = con_count = 0; con_scroll = 0; }
    else if (!strcmp(cmd, "echo"))
	C_Printf ("%s", args);
    else if (!strcmp(cmd, "quit") || !strcmp(cmd, "exit"))
	I_Quit ();
    else if (!inlevel)
	C_Printf ("not in a level.");
    else if (!strcmp(cmd, "god"))
    {
	pl->cheats ^= CF_GODMODE;
	if (pl->cheats & CF_GODMODE) { pl->health = 100; if (pl->mo) pl->mo->health = 100; }
	C_Printf ("god mode %s", (pl->cheats & CF_GODMODE) ? "ON" : "off");
    }
    else if (!strcmp(cmd, "noclip"))
    {
	pl->cheats ^= CF_NOCLIP;
	if (pl->mo)
	{
	    if (pl->cheats & CF_NOCLIP) pl->mo->flags |=  MF_NOCLIP;
	    else                        pl->mo->flags &= ~MF_NOCLIP;
	}
	C_Printf ("noclip %s", (pl->cheats & CF_NOCLIP) ? "ON" : "off");
    }
    else if (!strcmp(cmd, "give"))
	{ C_GiveAll (pl); C_Printf ("gave all weapons, ammo, keys, armor."); }
    else if (!strcmp(cmd, "map") || !strcmp(cmd, "warp"))
    {
	int e = 1, m = 0;
	int got = sscanf (args, "%d %d", &e, &m);
	if (got == 2)        G_DeferedInitNew (gameskill, e, m);
	else if (got == 1)   G_DeferedInitNew (gameskill, 1, e);	// single arg = map number
	else { C_Printf ("usage: map <episode> <map>   (or  map <map>)"); return; }
	con_open = 0;		// close so you can see the new level load
    }
    else if (!strcmp(cmd, "kill"))
    {
	thinker_t*	th;
	int		killed = 0;
	for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
	{
	    mobj_t* mo;
	    if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	    mo = (mobj_t*)th;
	    if ((mo->flags & MF_COUNTKILL) && mo->health > 0)
	    {
		P_DamageMobj (mo, pl->mo, pl->mo, mo->health + 1000);	// gib it
		killed++;
	    }
	}
	C_Printf ("killed %d monsters", killed);
    }
    else if (!strcmp(cmd, "health") || !strcmp(cmd, "hp"))
    {
	int h = 100; sscanf (args, "%d", &h);
	if (h < 1) h = 1; if (h > 999) h = 999;
	pl->health = h; if (pl->mo) pl->mo->health = h;
	C_Printf ("health = %d", h);
    }
    else if (!strcmp(cmd, "armor") || !strcmp(cmd, "armour"))
    {
	int a = 200; sscanf (args, "%d", &a);
	if (a < 0) a = 0; if (a > 999) a = 999;
	pl->armorpoints = a;
	pl->armortype = (a > 100) ? 2 : (a ? 1 : 0);
	C_Printf ("armor = %d", a);
    }
    else if (!strcmp(cmd, "ammo"))
    {
	int i; for (i = 0 ; i < NUMAMMO ; i++) pl->ammo[i] = pl->maxammo[i];
	C_Printf ("ammo refilled.");
    }
    else if (!strcmp(cmd, "skill"))
    {
	int sk;
	if (sscanf (args, "%d", &sk) == 1 && sk >= 1 && sk <= 5)
	    { gameskill = sk-1; C_Printf ("skill = %d (applies to new maps)", sk); }
	else C_Printf ("usage: skill <1-5>");
    }
    else if (!strcmp(cmd, "spawn"))
    {
	int t = C_MobjByName (args);
	if (t < 0)
	    C_Printf ("usage: spawn <imp|demon|spectre|baron|zombie|shotgunner|lostsoul|barrel>");
	else if (pl->mo)
	{
	    unsigned	an = pl->mo->angle >> ANGLETOFINESHIFT;
	    fixed_t	x  = pl->mo->x + FixedMul (96*FRACUNIT, finecosine[an]);
	    fixed_t	y  = pl->mo->y + FixedMul (96*FRACUNIT, finesine[an]);
	    P_SpawnMobj (x, y, pl->mo->z, (mobjtype_t)t);
	    C_Printf ("spawned %s", args);
	}
    }
    else if (!strcmp(cmd, "where") || !strcmp(cmd, "buddy") || !strcmp(cmd, "comp"))
	C_Printf ("%s", P_AICoop_Report ());
    else if (!strcmp(cmd, "come") || !strcmp(cmd, "follow"))
	C_Printf ("%s", P_AICoop_Summon () ? "[Buddy] On my way!"
					   : "[Buddy] (no companion -- launch with -aicoop)");
    else if (!strcmp(cmd, "wait") || !strcmp(cmd, "stay"))
	C_Printf ("%s", P_AICoop_Wait ());
    else if (!strcmp(cmd, "attack"))
	C_Printf ("%s", P_AICoop_Attack ());
    else if (!strcmp(cmd, "report") || !strcmp(cmd, "status"))
	C_Printf ("%s", P_AICoop_StatusReport ());
    else if (!strcmp(cmd, "director") || !strcmp(cmd, "ai") || !strcmp(cmd, "llm"))
	C_Printf ("%s", P_AI_Console (args));
    else
	C_Printf ("unknown command: %s", cmd);
}


// ---------------------------------------------------------------- input

// Key that toggles the console.  Configurable via "key_console" (m_misc.c).
// Backquote/tilde (`) is always accepted as well: on Linux/SDL the top-left
// key (labelled ^ on many layouts) reports as KEY_BACKQUOTE, not '^'.
int	key_console = '^';

boolean C_Responder (event_t* ev)
{
    int c;

    // toggle (consume the key press; ignore its key-up)
    if (ev->data1 == key_console || ev->data1 == KEY_BACKQUOTE)
    {
	if (ev->type == ev_keydown) { con_open = !con_open; con_shift = 0; }
	return true;
    }

    if (!con_open)
	return false;

    // track shift
    if (ev->data1 == KEY_RSHIFT)
    {
	con_shift = (ev->type == ev_keydown);
	return true;
    }

    if (ev->type != ev_keydown)
	return true;			// swallow everything else while open

    switch (ev->data1)
    {
      case KEY_ESCAPE:
	con_open = 0;
	return true;
      case KEY_ENTER:
	if (con_inlen) { C_Execute (con_input); con_input[0] = '\0'; con_inlen = 0; }
	return true;
      case KEY_BACKSPACE:
	if (con_inlen) con_input[--con_inlen] = '\0';
	return true;
      case KEY_UPARROW:
	if (con_scroll < con_count-1) con_scroll++;
	return true;
      case KEY_DOWNARROW:
	if (con_scroll > 0) con_scroll--;
	return true;
    }

    // printable character
    c = ev->data1;
    if (c >= 32 && c < 127)
    {
	if (con_shift) c = shiftxform[c];
	if (con_inlen < CON_INPUTW-1)
	{
	    con_input[con_inlen++] = (char)c;
	    con_input[con_inlen] = '\0';
	}
    }
    return true;
}


// ---------------------------------------------------------------- drawing

// Draw an hu_font patch at HALF its base size: take every other source
// pixel (column/row) so the glyph occupies (w/2 x h/2) BASE pixels, then the
// usual hires scaling applies.  Keeps the console text small but in BASE coords.
static void C_DrawPatchHalf (int x, int y, patch_t* patch)
{
    int		s = hires;
    int		w = SHORT(patch->width);
    int		col;

    x -= SHORT(patch->leftoffset) / 2;
    y -= SHORT(patch->topoffset)  / 2;

    for (col = 0 ; col < w ; col += 2)
    {
	int		ocol = col >> 1;
	column_t*	column = (column_t *)((byte *)patch + LONG(patch->columnofs[col]));

	while (column->topdelta != 0xff)
	{
	    byte*	src = (byte *)column + 3;
	    int		len = column->length;
	    int		k;
	    for (k = 0 ; k < len ; k++)
	    {
		int	srow = column->topdelta + k;
		int	orow, sx, sy, i, j;
		byte	px;
		if (srow & 1) continue;			// keep even rows only
		orow = srow >> 1;
		sx = (x + ocol) * s;
		sy = (y + orow) * s;
		if (sx < 0 || sy < 0 || sx + s > SCREENWIDTH || sy + s > SCREENHEIGHT)
		    continue;
		px = src[k];
		for (i = 0 ; i < s ; i++)
		{
		    byte* d = screens[0] + (sy + i)*SCREENWIDTH + sx;
		    for (j = 0 ; j < s ; j++) d[j] = px;
		}
	    }
	    column = (column_t *)((byte *)column + len + 4);
	}
    }
}

static void C_DrawString (int x, int y, const char* s)
{
    for (; *s; s++)
    {
	int ch = toupper((unsigned char)*s);
	int cw;
	if (ch == ' ' || ch < HU_FONTSTART || ch > HU_FONTEND) { x += 2; continue; }
	patch_t* p = hu_font[ch - HU_FONTSTART];
	cw = SHORT(p->width) / 2; if (cw < 1) cw = 1;
	if (x + cw > BASE_WIDTH) break;
	C_DrawPatchHalf (x, y, p);
	x += cw;
    }
}

void C_Drawer (void)
{
    int		rows, npix, i, y, line, vis;
    byte*	dst;
    char	inbuf[CON_INPUTW+4];

    if (!con_open)
	return;

    // dim the view behind the console
    rows = CON_H * hires;
    if (rows > SCREENHEIGHT) rows = SCREENHEIGHT;
    npix = rows * SCREENWIDTH;
    dst = screens[0];
    if (colormaps)
	for (i=0 ; i<npix ; i++)
	    dst[i] = colormaps[CON_DARK*256 + dst[i]];
    // a separator line at the bottom edge
    if (rows >= 2)
	memset (dst + (rows-2)*SCREENWIDTH, 176, 2*SCREENWIDTH);

    // input line at the bottom of the console
    con_blink++;
    snprintf (inbuf, sizeof(inbuf), "]%s%s", con_input,
	      ((con_blink>>4)&1) ? "_" : "");
    C_DrawString (2, CON_H - 10, inbuf);

    // scrollback above it, newest at the bottom
    vis  = (CON_H - 14) / LINE_STEP;
    y    = CON_H - 10 - LINE_STEP;
    for (line = 0 ; line < vis ; line++)
    {
	int idx = line + con_scroll;
	if (idx >= con_count) break;
	idx = (con_head - 1 - idx + CON_LINES*64) % CON_LINES;
	C_DrawString (2, y, con_text[idx]);
	y -= LINE_STEP;
    }
}
