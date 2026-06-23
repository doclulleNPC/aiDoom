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
#include "heretic.h"		// Heretic_SpawnMummy (console spawn)
#include "info.h"		// mobjtype_t, MT_*
#include "d_items.h"		// weaponinfo (give <weapon> -> its ammo)
#include "m_fixed.h"		// FixedMul

#include "c_console.h"
#include "p_ai_coop.h"		// companion commands (where/come/wait/attack/report)
#include "p_ai_llm.h"		// director on/off toggle

extern patch_t*		hu_font[HU_FONTSIZE];
extern const char*	shiftxform;
extern lighttable_t*	colormaps;
extern int		crosshair;	// r_draw.c -- gameplay crosshair style
extern boolean		menuactive;	// m_menu.c -- don't fire key binds in a menu
extern void		I_Quit (void);

// Play-sim hooks, declared by hand to avoid p_local.h (its p_spec.h enums
// 'open'/'close' collide with code elsewhere).
extern thinker_t	thinkercap;
extern mobj_t*		P_SpawnMobj (fixed_t x, fixed_t y, fixed_t z, mobjtype_t type);
extern mobj_t*		P_SpawnMonsterChecked (fixed_t x, fixed_t y, mobjtype_t type);
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

// Key bindings: bindings[doomkeycode] = a console command run when that key is
// pressed in-game (console closed, not in a menu).  In-memory for the session.
static char	bindings[256][CON_INPUTW];


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

// weapon index by name for "give <weapon>"; -1 if unknown.
static int C_WeaponByName (const char* s)
{
    if (!strcmp(s,"fist"))			return wp_fist;
    if (!strcmp(s,"chainsaw") || !strcmp(s,"saw")) return wp_chainsaw;
    if (!strcmp(s,"pistol"))			return wp_pistol;
    if (!strcmp(s,"shotgun"))			return wp_shotgun;
    if (!strcmp(s,"ssg") || !strcmp(s,"supershotgun")) return wp_supershotgun;
    if (!strcmp(s,"chaingun"))			return wp_chaingun;
    if (!strcmp(s,"rocket") || !strcmp(s,"rocketlauncher") || !strcmp(s,"launcher")) return wp_missile;
    if (!strcmp(s,"plasma"))			return wp_plasma;
    if (!strcmp(s,"bfg") || !strcmp(s,"bfg9000"))	return wp_bfg;
    return -1;
}

// keycard/skull index by name for "give <key>"; -1 if unknown.
static int C_CardByName (const char* s)
{
    if (!strcmp(s,"bluecard")   || !strcmp(s,"blue"))	return it_bluecard;
    if (!strcmp(s,"yellowcard") || !strcmp(s,"yellow"))	return it_yellowcard;
    if (!strcmp(s,"redcard")    || !strcmp(s,"red"))	return it_redcard;
    if (!strcmp(s,"blueskull"))				return it_blueskull;
    if (!strcmp(s,"yellowskull"))			return it_yellowskull;
    if (!strcmp(s,"redskull"))				return it_redskull;
    return -1;
}

// Doom keycode for a "bind <key>" name: a single printable char, or a named key.
static int C_KeyByName (const char* s)
{
    if (!s || !s[0]) return -1;
    if (!s[1]) return (unsigned char)s[0];		// single char -> its code
    if (!strcmp(s,"space"))  return ' ';
    if (!strcmp(s,"tab"))    return KEY_TAB;
    if (!strcmp(s,"enter") || !strcmp(s,"return")) return KEY_ENTER;
    if (!strcmp(s,"esc") || !strcmp(s,"escape"))   return KEY_ESCAPE;
    if (!strcmp(s,"up"))     return KEY_UPARROW;
    if (!strcmp(s,"down"))   return KEY_DOWNARROW;
    if (!strcmp(s,"left"))   return KEY_LEFTARROW;
    if (!strcmp(s,"right"))  return KEY_RIGHTARROW;
    if (!strcmp(s,"mouse1") || !strcmp(s,"mousel")) return KEY_MOUSE1;
    if (!strcmp(s,"mouse2") || !strcmp(s,"mouser")) return KEY_MOUSE2;
    if (!strcmp(s,"mouse3") || !strcmp(s,"mousem")) return KEY_MOUSE3;
    if (!strcmp(s,"mwheelup"))   return KEY_MWHEELUP;
    if (!strcmp(s,"mwheeldown")) return KEY_MWHEELDOWN;
    if (s[0]=='f' && (s[1]>='1' && s[1]<='9'))
    {
	int n = atoi (s+1);
	if (n >= 1 && n <= 10) return KEY_F1 + (n-1);
	if (n == 11) return KEY_F11;
	if (n == 12) return KEY_F12;
    }
    return -1;
}

// Reverse: a printable name for a keycode (static buffer), for "bind" listing.
static const char* C_KeyName (int k)
{
    static char b[8];
    switch (k) {
      case ' ':            return "space";
      case KEY_TAB:        return "tab";
      case KEY_ENTER:      return "enter";
      case KEY_ESCAPE:     return "esc";
      case KEY_UPARROW:    return "up";
      case KEY_DOWNARROW:  return "down";
      case KEY_LEFTARROW:  return "left";
      case KEY_RIGHTARROW: return "right";
      case KEY_F11:        return "f11";
      case KEY_F12:        return "f12";
      case KEY_MOUSE1:     return "mouse1";
      case KEY_MOUSE2:     return "mouse2";
      case KEY_MOUSE3:     return "mouse3";
      case KEY_MWHEELUP:   return "mwheelup";
      case KEY_MWHEELDOWN: return "mwheeldown";
    }
    if (k >= KEY_F1 && k <= KEY_F10) { snprintf(b,sizeof(b),"f%d", k-KEY_F1+1); return b; }
    if (k > 32 && k < 127) { b[0]=(char)k; b[1]=0; return b; }
    snprintf (b, sizeof(b), "0x%02x", k & 0xff);
    return b;
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
    // Heretic monsters (mummy/clink/gargoyle) -- only when hereticstuff.wad is loaded.
    // ("imp" stays the DOOM imp above; the Heretic one is "gargoyle".)
    if (s[0] && Heretic_Available ())
    {
	int h = Heretic_TypeByName (s);
	if (h >= 0) return h;
    }
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
	C_Printf ("cheats: god  noclip  allmap  kill  health <n>  armor <n>");
	C_Printf ("give:   all|weapons|ammo|keys|armor|health|<weapon>|<key>");
	C_Printf ("world:  spawn <thing>  skill <1-5>  map <e> <m> / warp <m>");
	C_Printf ("view:   crosshair 0..3");
	C_Printf ("keys:   bind <key> <command> | unbind <key> | bind (list)");
	C_Printf ("buddy:  where  come  wait/stay  attack  report  buddygod  buddyarm  buddyhome");
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
    else if (!strcmp(cmd, "allmap") || !strcmp(cmd, "fullmap") || !strcmp(cmd, "iddt"))
    {
	// Reveal the whole level on the automap (the Computer Area Map effect).
	pl->powers[pw_allmap] = pl->powers[pw_allmap] ? 0 : 1;
	C_Printf ("allmap %s", pl->powers[pw_allmap] ? "ON" : "off");
    }
    else if (!strcmp(cmd, "give"))
    {
	int w, k, i;
	for (i = 0 ; args[i] ; i++) args[i] = tolower(args[i]);	// case-insensitive
	if (!*args)
	    C_Printf ("usage: give all|weapons|ammo|keys|armor|health|<weapon>|<key>");
	else if (!strcmp(args,"all"))     { C_GiveAll (pl); C_Printf ("gave everything."); }
	else if (!strcmp(args,"weapons")) { for(i=0;i<NUMWEAPONS;i++) pl->weaponowned[i]=true; C_Printf("gave all weapons."); }
	else if (!strcmp(args,"ammo"))    { for(i=0;i<NUMAMMO;i++) pl->ammo[i]=pl->maxammo[i]; C_Printf("gave max ammo."); }
	else if (!strcmp(args,"keys") || !strcmp(args,"cards")) { for(i=0;i<NUMCARDS;i++) pl->cards[i]=true; C_Printf("gave all keys."); }
	else if (!strcmp(args,"armor") || !strcmp(args,"armour")) { pl->armorpoints=200; pl->armortype=2; C_Printf("gave armor."); }
	else if (!strcmp(args,"health") || !strcmp(args,"hp")) { pl->health=100; if(pl->mo)pl->mo->health=100; C_Printf("gave health."); }
	else if ((w = C_WeaponByName(args)) >= 0)
	    { pl->weaponowned[w]=true;
	      if (weaponinfo[w].ammo < NUMAMMO) pl->ammo[weaponinfo[w].ammo]=pl->maxammo[weaponinfo[w].ammo];
	      C_Printf ("gave %s.", args); }
	else if ((k = C_CardByName(args)) >= 0) { pl->cards[k]=true; C_Printf("gave %s.", args); }
	else C_Printf ("give: unknown '%s'", args);
    }
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
    else if (!strcmp(cmd, "heretic"))
    {
	// spawn a Heretic monster in front of the player: heretic [mummy|clink|imp]
	int type = Heretic_TypeByName (args);
	if (!Heretic_Available ())
	    C_Printf ("heretic: hereticstuff.wad not loaded (no H* sprites)");
	else if (type < 0)
	    C_Printf ("heretic: unknown monster '%s' (mummy|clink|imp)", args);
	else if (pl->mo)
	{
	    int		fa = pl->mo->angle >> ANGLETOFINESHIFT;
	    fixed_t	x  = pl->mo->x + FixedMul (160*FRACUNIT, finecosine[fa]);
	    fixed_t	y  = pl->mo->y + FixedMul (160*FRACUNIT, finesine[fa]);
	    mobj_t*	hm = Heretic_Spawn (type, x, y);
	    if (hm)
	    {
		hm->target = pl->mo;
		P_SetMobjState (hm, hm->info->seestate);	// wake it
		C_Printf ("spawned Heretic monster");
	    }
	    else C_Printf ("heretic: couldn't place it");
	}
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
    else if (!strcmp(cmd, "spawn") || !strcmp(cmd, "summon"))
    {
	int t = C_MobjByName (args);
	if (t < 0)
	    C_Printf ("usage: summon <imp|demon|spectre|baron|zombie|shotgunner|lostsoul|barrel|mummy|clink|gargoyle>");
	else if (pl->mo)
	{
	    unsigned	an = pl->mo->angle >> ANGLETOFINESHIFT;
	    fixed_t	x  = pl->mo->x + FixedMul (96*FRACUNIT, finecosine[an]);
	    fixed_t	y  = pl->mo->y + FixedMul (96*FRACUNIT, finesine[an]);
	    // (C) only place it if it actually fits (no wall) + the sector is tall enough
	    mobj_t*	m  = P_SpawnMonsterChecked (x, y, (mobjtype_t)t);
	    if (!m)
		C_Printf ("summon: no room in front (wall / low ceiling) -- face open space");
	    else
	    {
		m->target = pl->mo;
		if (m->info->seestate) P_SetMobjState (m, m->info->seestate);	// wake it
		C_Printf ("summoned %s", args);
	    }
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
    else if (!strcmp(cmd, "buddygod"))
	C_Printf ("%s", P_AICoop_God ());
    else if (!strcmp(cmd, "buddyarm") || !strcmp(cmd, "buddygive"))
	C_Printf ("%s", P_AICoop_GiveAll ());
    else if (!strcmp(cmd, "buddyhome") || !strcmp(cmd, "buddytp"))
	C_Printf ("%s", P_AICoop_Home ());
    else if (!strcmp(cmd, "director") || !strcmp(cmd, "ai") || !strcmp(cmd, "llm"))
	C_Printf ("%s", P_AI_Console (args));
    else if (!strcmp(cmd, "crosshair") || !strcmp(cmd, "xhair"))
    {
	if (*args) { crosshair = atoi(args); if (crosshair<0) crosshair=0; if (crosshair>3) crosshair=3; }
	C_Printf ("crosshair %d  (0 off, 1 cross, 2 dot, 3 big)", crosshair);
    }
    else if (!strcmp(cmd, "bind"))
    {
	if (!*args)					// list current bindings
	{
	    int j, any = 0;
	    for (j = 0 ; j < 256 ; j++)
		if (bindings[j][0]) { C_Printf ("  %s = %s", C_KeyName(j), bindings[j]); any = 1; }
	    if (!any) C_Printf ("no binds.  usage: bind <key> <command>   e.g. bind k \"give all\"");
	}
	else
	{
	    char keyw[32]; int kn = 0, key; char* rest;
	    while (args[kn] && args[kn] != ' ' && kn < 31) { keyw[kn] = tolower(args[kn]); kn++; }
	    keyw[kn] = '\0';
	    rest = args + kn; while (*rest == ' ') rest++;
	    key = C_KeyByName (keyw);
	    if (key < 0)        C_Printf ("bind: unknown key '%s'", keyw);
	    else if (!*rest)    C_Printf ("%s = %s", keyw, bindings[key][0] ? bindings[key] : "(unbound)");
	    else { strncpy (bindings[key], rest, CON_INPUTW-1); bindings[key][CON_INPUTW-1] = '\0';
		   C_Printf ("bound %s -> %s", keyw, rest); }
	}
    }
    else if (!strcmp(cmd, "unbind"))
    {
	int j, key;
	for (j = 0 ; args[j] ; j++) args[j] = tolower(args[j]);
	key = C_KeyByName (args);
	if (key < 0) C_Printf ("unbind: unknown key '%s'", args);
	else { bindings[key][0] = '\0'; C_Printf ("unbound %s", args); }
    }
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
    {
	// Key bindings: console closed and not in a menu -> run the bound command
	// and swallow the key (the bind overrides its normal game function).
	if (ev->type == ev_keydown && !menuactive && bindings[ev->data1 & 0xff][0])
	{
	    char tmp[CON_INPUTW];
	    strncpy (tmp, bindings[ev->data1 & 0xff], sizeof(tmp)-1);
	    tmp[sizeof(tmp)-1] = '\0';
	    C_Execute (tmp);
	    return true;
	}
	return false;
    }

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
