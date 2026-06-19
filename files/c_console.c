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
// strcasecmp is POSIX-only; MSVC names it _stricmp.  Map once for portability.
#ifdef _WIN32
#define strcasecmp _stricmp
#endif

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
#define CON_DARK	26		// colormap level used to dim the view behind it
#define CON_LINES	256		// scrollback ring size
#define CON_LINEW	128
#define CON_INPUTW	128
#define LINE_STEP	9		// BASE pixels per text row (full-size font)

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
    C_Printf ("aiDoom console.  Type 'help'.  Open with F12 or ` (backquote).");
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
    {
	const char* r = P_AICoop_Report ();
	C_Printf ("%s", r);
	// Voice tag derived from coop_state -- see AICOOP_STATE_TAGS in p_ai_coop.c
	// (kept in sync with the enum there).  Use the public accessor since
	// coop_state is static in p_ai_coop.c.
	extern int P_AICoop_State (void);
	static const char* state_tags[] = {
	    "state:following","state:fighting","state:healing",
	    "state:holding", "state:coming",  "state:grabbing"
	};
	int s = P_AICoop_State ();
	if (s >= 0 && s < (int)(sizeof(state_tags)/sizeof(state_tags[0])))
	    P_AICoop_VoiceTag (state_tags[s]);
    }
    else if (!strcmp(cmd, "come") || !strcmp(cmd, "follow"))
    {
	const char* r = P_AICoop_Summon () ? "[Buddy] On my way!"
					   : "[Buddy] (no companion -- launch with -aicoop)";
	C_Printf ("%s", r);
	if (P_AICoop_Slot () >= 0 && !strncmp (r, "[Buddy] On", 10))
	    P_AICoop_VoiceTag ("summon_ok");
    }
    else if (!strcmp(cmd, "wait") || !strcmp(cmd, "stay"))
    {
	const char* r = P_AICoop_Wait ();
	C_Printf ("%s", r);
	if      (!strcmp (r, "[Buddy] Holding position.")) P_AICoop_VoiceTag ("wait_hold");
	else if (!strcmp (r, "[Buddy] Moving out."))       P_AICoop_VoiceTag ("wait_move");
    }
    else if (!strcmp(cmd, "attack"))
    {
	const char* r = P_AICoop_Attack ();
	C_Printf ("%s", r);
	if      (!strcmp (r, "[Buddy] Attacking!"))         P_AICoop_VoiceTag ("attack_ok");
	else if (!strcmp (r, "[Buddy] No targets around.")) P_AICoop_VoiceTag ("attack_none");
    }
    else if (!strcmp(cmd, "report") || !strcmp(cmd, "status"))
    {
	const char* r = P_AICoop_StatusReport ();
	C_Printf ("%s", r);
	// The status reply's weapon name is what we want the buddy to speak.
	// It follows "[Buddy] <hp> HP, <armor>% armor, <weapon>" or with ", <ammo> rounds."
	// at the end.  Pull the last comma-separated token.
	const char* comma = strrchr (r, ',');
	const char* weapon = comma ? comma + 1 : r + 8;  // skip "[Buddy] "
	char  wbuf[32]; int i = 0;
	while (*weapon && *weapon != '.' && i < 30) wbuf[i++] = *weapon++;
	wbuf[i] = 0;
	while (i > 0 && wbuf[i-1] == ' ') wbuf[--i] = 0;
	// Lowercase + map to tag.  Plain variant by default; the "loaded" variant
	// is only used when the ammo line was present (i.e. comma != NULL after weapon).
	int has_ammo = comma && strstr (comma, "rounds") != NULL;
	char tag[64];
	if      (!strcasecmp (wbuf, "fists"))          strcpy (tag, "status:fists");
	else if (!strcasecmp (wbuf, "pistol"))         strcpy (tag, has_ammo ? "status:pistol:ammo"        : "status:pistol");
	else if (!strcasecmp (wbuf, "shotgun"))        strcpy (tag, has_ammo ? "status:shotgun:ammo"       : "status:shotgun");
	else if (!strcasecmp (wbuf, "chaingun"))       strcpy (tag, has_ammo ? "status:chaingun:ammo"      : "status:chaingun");
	else if (!strcasecmp (wbuf, "rocket launcher"))strcpy (tag, has_ammo ? "status:rocketlauncher:ammo": "status:rocketlauncher");
	else if (!strcasecmp (wbuf, "plasma rifle"))   strcpy (tag, has_ammo ? "status:plasma:ammo"        : "status:plasma");
	else if (!strcasecmp (wbuf, "B. F. G."))       strcpy (tag, has_ammo ? "status:bfg:ammo"           : "status:bfg");
	else if (!strcasecmp (wbuf, "chainsaw"))       strcpy (tag, "status:chainsaw");
	else if (!strcasecmp (wbuf, "super shotgun"))  strcpy (tag, has_ammo ? "status:supershotgun:ammo"  : "status:supershotgun");
	else tag[0] = 0;
	if (tag[0]) P_AICoop_VoiceTag (tag);
    }
    else if (!strcmp(cmd, "director") || !strcmp(cmd, "ai") || !strcmp(cmd, "llm"))
	C_Printf ("%s", P_AI_Console (args));
    else
	C_Printf ("unknown command: %s", cmd);
}


// ---------------------------------------------------------------- input

// Key that opens the console.  Configurable via "key_console" (m_misc.c);
// default F12 (the only otherwise-free function key -- it was netgame spy-mode).
// Backquote (`) is always accepted too.
int	key_console = KEY_F12;

boolean C_Responder (event_t* ev)
{
    int c;

    // Backquote: universal toggle (open and close).
    if (ev->data1 == KEY_BACKQUOTE)
    {
	if (ev->type == ev_keydown) { con_open = !con_open; con_shift = 0; }
	return true;
    }

    // Configured key: opens when closed.  Also closes when open -- except if it
    // is Backspace, which must stay free to delete the input line (fall through).
    if (ev->data1 == key_console)
    {
	if (!con_open)
	{
	    if (ev->type == ev_keydown) { con_open = 1; con_shift = 0; }
	    return true;
	}
	if (key_console != KEY_BACKSPACE)
	{
	    if (ev->type == ev_keydown) con_open = 0;
	    return true;
	}
	/* console open and key is Backspace -> fall through to edit the line */
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
// Console rendering now lives in the platform layer (i_video.c) as an SDL
// overlay drawn with the baked DejaVuSansMono ("TrueType") atlas -- crisp,
// anti-aliased and translucent.  c_console only owns state; i_video pulls the
// display lines via C_GetLine().
void C_Drawer (void) { }		// (legacy hook; SDL overlay does the drawing)

// Text for display row r: row 0 = input line (with blinking cursor); rows 1..
// = scrollback, newest first (honouring the scroll offset).  NULL past the end.
const char* C_GetLine (int r)
{
    static char	inbuf[CON_INPUTW+8];
    int		idx;

    if (r == 0)
    {
	con_blink++;
	snprintf (inbuf, sizeof(inbuf), "]%s%s", con_input, ((con_blink>>4)&1) ? "_" : "");
	return inbuf;
    }
    idx = (r - 1) + con_scroll;
    if (idx >= con_count)
	return NULL;
    idx = (con_head - 1 - idx + CON_LINES*64) % CON_LINES;
    return con_text[idx];
}
