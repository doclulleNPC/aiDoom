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

#include "c_console.h"

extern patch_t*		hu_font[HU_FONTSIZE];
extern const char*	shiftxform;
extern lighttable_t*	colormaps;
extern void		I_Quit (void);

#define CON_H		120		// console height in BASE (320x200) rows
#define CON_DARK	22		// colormap level used to dim the view behind it
#define CON_LINES	256		// scrollback ring size
#define CON_LINEW	128
#define CON_INPUTW	128
#define LINE_STEP	9		// BASE pixels per text row

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
    C_Printf ("aiDoom console.  Type 'help'.  Toggle with ` (backquote).");
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
	C_Printf ("commands: help clear echo quit god noclip give map<e m>/warp");
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
    else
	C_Printf ("unknown command: %s", cmd);
}


// ---------------------------------------------------------------- input

// Key that toggles the console.  Default '^' (the key left of 1 on many
// non-US layouts); configurable via "key_console" in the config (m_misc.c).
int	key_console = '^';

boolean C_Responder (event_t* ev)
{
    int c;

    // toggle (consume the key press; ignore its key-up)
    if (ev->data1 == key_console)
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
static void C_DrawString (int x, int y, const char* s)
{
    for (; *s; s++)
    {
	int ch = toupper((unsigned char)*s);
	if (ch == ' ' || ch < HU_FONTSTART || ch > HU_FONTEND) { x += 4; continue; }
	patch_t* p = hu_font[ch - HU_FONTSTART];
	if (x + SHORT(p->width) > BASE_WIDTH) break;
	V_DrawPatch (x, y, 0, p);
	x += SHORT(p->width);
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
