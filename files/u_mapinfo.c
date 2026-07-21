// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	UMAPINFO parser + lookup (https://doomwiki.org/wiki/UMAPINFO).
//	Self-contained: its own tokenizer, no external scanner.  Field set and
//	semantics follow Woof's g_umapinfo.c (Christoph Oelckers / Roman Fomin).
//	See files/u_mapinfo.h for the public API and struct.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "doomdef.h"
#include "doomstat.h"
#include "doomtype.h"
#include "w_wad.h"
#include "z_zone.h"	// PU_CACHE
#include "u_mapinfo.h"

static umap_t*	maps;
static int	nummaps;

// Set by G_DoCompleted when UMAPINFO redirects the exit; consumed by G_DoWorldDone
// so the next map can cross episodes (which wminfo.next alone can't express).
int	u_next_episode = 0;	// 1-based target episode (0 = not overridden)
int	u_next_map     = 0;	// 1-based target map

#ifdef _WIN32
#define strncasecmp _strnicmp
#define strcasecmp  _stricmp
#endif

// The vanilla mobjtype_t order, addressed by name for the bossaction field.  The
// index IS the mobjtype (aiDoom appends its extra actors AFTER this vanilla set,
// so 0..N-1 line up with MT_PLAYER..MT_MSObj).  Only these (all real boss types
// live here) are accepted; DEHEXTRA/Boom names are intentionally omitted.
static const char* const actor_names[] =
{
    "DoomPlayer","ZombieMan","ShotgunGuy","Archvile","ArchvileFire","Revenant",
    "RevenantTracer","RevenantTracerSmoke","Fatso","FatShot","ChaingunGuy",
    "DoomImp","Demon","Spectre","Cacodemon","BaronOfHell","BaronBall",
    "HellKnight","LostSoul","SpiderMastermind","Arachnotron","Cyberdemon",
    "PainElemental","WolfensteinSS","CommanderKeen","BossBrain","BossEye",
    "BossTarget","SpawnShot","SpawnFire","ExplosiveBarrel","DoomImpBall",
    "CacodemonBall","Rocket","PlasmaBall","BFGBall","ArachnotronPlasma",
    "BulletPuff","Blood","TeleportFog","ItemFog","TeleportDest","BFGExtra",
    "GreenArmor","BlueArmor","HealthBonus","ArmorBonus","BlueCard","RedCard",
    "YellowCard","YellowSkull","RedSkull","BlueSkull","Stimpack","Medikit",
    "Soulsphere","InvulnerabilitySphere","Berserk","BlurSphere","RadSuit",
    "Allmap","Infrared","Megasphere","Clip","ClipBox","RocketAmmo","RocketBox",
    "Cell","CellPack","Shell","ShellBox","Backpack","BFG9000","Chaingun",
    "Chainsaw","RocketLauncher","PlasmaRifle","Shotgun","SuperShotgun",
    "TechLamp","TechLamp2","Column","TallGreenColumn","ShortGreenColumn",
    "TallRedColumn","ShortRedColumn","SkullColumn","HeartColumn","EvilEye",
    "FloatingSkull","TorchTree","BlueTorch","GreenTorch","RedTorch",
    "ShortBlueTorch","ShortGreenTorch","ShortRedTorch","Stalagtite","TechPillar",
    "CandleStick","Candelabra","BloodyTwitch","Meat2","Meat3","Meat4","Meat5",
    "NonsolidMeat2","NonsolidMeat4","NonsolidMeat3","NonsolidMeat5",
    "NonsolidTwitch","DeadCacodemon","DeadMarine","DeadZombieMan","DeadDemon",
    "DeadLostSoul","DeadDoomImp","DeadShotgunGuy","GibbedMarine",
    "GibbedMarineExtra","HeadsOnAStick","Gibs","HeadOnAStick","HeadCandles",
    "DeadStick","LiveStick","BigTree","BurningBarrel","HangNoGuts","HangBNoBrain",
    "HangTLookingDown","HangTSkull","HangTLookingUp","HangTNoBrain","ColonGibs",
    "SmallBloodPool","BrainStem"
};
#define NUM_ACTOR_NAMES ((int)(sizeof actor_names / sizeof actor_names[0]))

// ================================================================= tokenizer
typedef enum { TK_EOF, TK_IDENT, TK_STRING, TK_INT, TK_PUNCT } toktype_t;

typedef struct
{
    const char*	p;		// cursor
    const char*	end;
    toktype_t	type;
    char	str[256];	// identifier / string / punct text
    int		num;
    int		line;
} scan_t;

static void SkipWs (scan_t* s)
{
    while (s->p < s->end)
    {
	char c = *s->p;
	if (c == '\n') { s->line++; s->p++; }
	else if (isspace ((unsigned char)c)) s->p++;
	else if (c == '/' && s->p+1 < s->end && s->p[1] == '/')	// line comment
	    { while (s->p < s->end && *s->p != '\n') s->p++; }
	else if (c == '/' && s->p+1 < s->end && s->p[1] == '*')	// block comment
	    { s->p += 2; while (s->p+1 < s->end && !(s->p[0]=='*' && s->p[1]=='/')) { if (*s->p=='\n') s->line++; s->p++; } s->p += 2; }
	else break;
    }
}

// Fetch the next token.  Returns false at end of input.
static boolean Next (scan_t* s)
{
    SkipWs (s);
    if (s->p >= s->end) { s->type = TK_EOF; return false; }

    char c = *s->p;

    if (c == '"')					// quoted string
    {
	int i = 0; s->p++;
	while (s->p < s->end && *s->p != '"')
	{
	    if (*s->p == '\n') s->line++;
	    if (i < (int)sizeof(s->str)-1) s->str[i++] = *s->p;
	    s->p++;
	}
	if (s->p < s->end) s->p++;			// closing quote
	s->str[i] = 0;
	s->type = TK_STRING;
	return true;
    }
    if (isalpha ((unsigned char)c) || c == '_')		// identifier / bool
    {
	int i = 0;
	while (s->p < s->end && (isalnum ((unsigned char)*s->p) || *s->p == '_'))
	{ if (i < (int)sizeof(s->str)-1) s->str[i++] = *s->p; s->p++; }
	s->str[i] = 0;
	s->type = TK_IDENT;
	return true;
    }
    if (isdigit ((unsigned char)c) || (c == '-' && s->p+1 < s->end && isdigit ((unsigned char)s->p[1])))
    {
	s->num = (int)strtol (s->p, (char**)&s->p, 10);
	s->type = TK_INT;
	return true;
    }
    // single punctuation char ( { } = , )
    s->str[0] = c; s->str[1] = 0; s->p++;
    s->type = TK_PUNCT;
    return true;
}

// ================================================================= helpers
static char* Dup (const char* s)
{
    size_t n = strlen (s) + 1;
    char* d = malloc (n);
    if (d) memcpy (d, s, n);
    return d;
}

static void CopyLumpName (char* dst, const char* src)	// 8 chars, upper, NUL-pad
{
    int i;
    for (i = 0; i < 8 && src[i]; i++) dst[i] = (char)toupper ((unsigned char)src[i]);
    for (; i < 9; i++) dst[i] = 0;
}

boolean U_ValidateMapName (const char* name, int* episode, int* map)
{
    char up[16]; int e = -1, m = -1, i;
    if (strlen (name) > 8) return false;
    for (i = 0; name[i] && i < 15; i++) up[i] = (char)toupper ((unsigned char)name[i]);
    up[i] = 0;

    if (gamemode == commercial)
    {
	if (sscanf (up, "MAP%d", &m) != 1) return false;
	e = 1;
    }
    else
    {
	if (sscanf (up, "E%dM%d", &e, &m) != 2) return false;
    }
    if (m < 0 || e < 0) return false;
    if (episode) *episode = e;
    if (map)     *map = m;
    return true;
}

void U_MapName (int episode, int map, char* out)
{
    if (gamemode == commercial) snprintf (out, 9, "MAP%02d", map);
    else                        snprintf (out, 9, "E%dM%d", episode, map);
}

// ================================================================= parse
static char* ParseMultiString (scan_t* s)		// "a","b" -> "a\nb" (malloc)
{
    char* build = NULL;
    do {
	if (s->type != TK_STRING) break;
	if (!build) build = Dup (s->str);
	else {
	    size_t n = strlen (build) + 1 + strlen (s->str) + 1;
	    char* nb = malloc (n);
	    snprintf (nb, n, "%s\n%s", build, s->str);
	    free (build); build = nb;
	}
	// peek for a comma
	const char* save = s->p; int saveln = s->line;
	if (!Next (s) || !(s->type == TK_PUNCT && s->str[0] == ',')) { s->p = save; s->line = saveln; break; }
	Next (s);					// consume the value after ','
    } while (1);
    return build;
}

// Read one "key = ..." line into mape.  Assumes the identifier is current token.
static void ParseProperty (scan_t* s, umap_t* mape)
{
    char key[64];
    strncpy (key, s->str, sizeof key - 1); key[sizeof key - 1] = 0;

    if (!Next (s) || !(s->type == TK_PUNCT && s->str[0] == '=')) return;	// need '='
    Next (s);								// first value

    if (!strcasecmp (key, "levelname") && s->type == TK_STRING)
	{ free (mape->levelname); mape->levelname = Dup (s->str); }
    else if (!strcasecmp (key, "author") && s->type == TK_STRING)
	{ free (mape->author); mape->author = Dup (s->str); }
    else if (!strcasecmp (key, "label"))
    {
	if (s->type == TK_IDENT && !strcasecmp (s->str, "clear")) mape->label_clear = true;
	else if (s->type == TK_STRING) { mape->label_clear = false; free (mape->label); mape->label = Dup (s->str); }
    }
    else if (!strcasecmp (key, "next")       && s->type == TK_STRING) CopyLumpName (mape->nextmap,   s->str);
    else if (!strcasecmp (key, "nextsecret") && s->type == TK_STRING) CopyLumpName (mape->nextsecret, s->str);
    else if (!strcasecmp (key, "levelpic")   && s->type == TK_STRING) CopyLumpName (mape->levelpic,   s->str);
    else if (!strcasecmp (key, "skytexture") && s->type == TK_STRING) CopyLumpName (mape->skytexture, s->str);
    else if (!strcasecmp (key, "music")      && s->type == TK_STRING) CopyLumpName (mape->music,      s->str);
    else if (!strcasecmp (key, "exitpic")    && s->type == TK_STRING) CopyLumpName (mape->exitpic,    s->str);
    else if (!strcasecmp (key, "enterpic")   && s->type == TK_STRING) CopyLumpName (mape->enterpic,   s->str);
    else if (!strcasecmp (key, "exitanim")   && s->type == TK_STRING) CopyLumpName (mape->exitanim,   s->str);
    else if (!strcasecmp (key, "enteranim")  && s->type == TK_STRING) CopyLumpName (mape->enteranim,  s->str);
    else if (!strcasecmp (key, "endfinale")  && s->type == TK_STRING) { mape->endflags |= U_END_ART; CopyLumpName (mape->endfinale, s->str); CopyLumpName (mape->endpic, s->str); }
    else if (!strcasecmp (key, "interbackdrop") && s->type == TK_STRING) CopyLumpName (mape->interbackdrop, s->str);
    else if (!strcasecmp (key, "intermusic") && s->type == TK_STRING) CopyLumpName (mape->intermusic, s->str);
    else if (!strcasecmp (key, "partime")    && s->type == TK_INT)    mape->partime = s->num;
    else if (!strcasecmp (key, "endpic")     && s->type == TK_STRING) { mape->endflags |= U_END_ART; CopyLumpName (mape->endpic, s->str); }
    else if (!strcasecmp (key, "endgame"))
	{ if (s->type == TK_IDENT && !strcasecmp (s->str, "true"))  mape->endflags |= U_END_STANDARD;
	  else                                                       mape->endflags &= ~U_END_ANY; }
    else if (!strcasecmp (key, "endcast"))
	{ if (s->type == TK_IDENT && !strcasecmp (s->str, "true"))  mape->endflags |= U_END_CAST; }
    else if (!strcasecmp (key, "endbunny"))
	{ if (s->type == TK_IDENT && !strcasecmp (s->str, "true"))  mape->endflags |= U_END_BUNNY; }
    else if (!strcasecmp (key, "nointermission"))
	mape->nointermission = (s->type == TK_IDENT && !strcasecmp (s->str, "true"));
    else if (!strcasecmp (key, "intertext"))
    {
	if (s->type == TK_IDENT && !strcasecmp (s->str, "clear")) mape->intertext_clear = true;
	else { mape->intertext_clear = false; free (mape->intertext); mape->intertext = ParseMultiString (s); }
    }
    else if (!strcasecmp (key, "intertextsecret"))
    {
	if (s->type == TK_IDENT && !strcasecmp (s->str, "clear")) mape->intertextsecret_clear = true;
	else { mape->intertextsecret_clear = false; free (mape->intertextsecret); mape->intertextsecret = ParseMultiString (s); }
    }
    else if (!strcasecmp (key, "bossaction"))
    {
	if (s->type == TK_IDENT && !strcasecmp (s->str, "clear"))
	{
	    mape->bossaction_clear = true;
	    free (mape->bossactions); mape->bossactions = NULL; mape->numbossactions = 0;
	}
	else if (s->type == TK_IDENT)
	{
	    int type;
	    for (type = 0; type < NUM_ACTOR_NAMES; type++)
		if (!strcasecmp (s->str, actor_names[type])) break;
	    // thingtype , special , tag
	    if (Next (s) && s->type == TK_PUNCT && s->str[0] == ',' && Next (s) && s->type == TK_INT)
	    {
		int special = s->num, tag = 0;
		if (Next (s) && s->type == TK_PUNCT && s->str[0] == ',' && Next (s) && s->type == TK_INT)
		    tag = s->num;
		// 0-tag only allowed for level-exit specials
		boolean exitline = (special==11 || special==51 || special==52 || special==124
				 || (special>=2069 && special<=2074));
		if (type < NUM_ACTOR_NAMES && (tag != 0 || exitline))
		{
		    u_bossaction_t ba = { type, special, tag };
		    mape->bossactions = realloc (mape->bossactions,
						 (mape->numbossactions + 1) * sizeof *mape->bossactions);
		    mape->bossactions[mape->numbossactions++] = ba;
		}
	    }
	}
    }
    // Unknown key or type mismatch: swallow any remaining comma-separated values.
    {
	const char* save = s->p; int saveln = s->line;
	while (Next (s) && s->type == TK_PUNCT && s->str[0] == ',')
	    { Next (s); save = s->p; saveln = s->line; }
	s->p = save; s->line = saveln;		// leave the lookahead token unconsumed
    }
}

static void ParseMapEntry (scan_t* s)
{
    // current token is the "map" identifier; next is the map name
    if (!Next (s) || s->type != TK_IDENT) return;
    char name[16]; strncpy (name, s->str, sizeof name - 1); name[sizeof name - 1] = 0;
    if (!U_ValidateMapName (name, NULL, NULL)) { fprintf (stderr, "UMAPINFO: invalid map name %s\n", name); }

    if (!Next (s) || !(s->type == TK_PUNCT && s->str[0] == '{')) return;

    umap_t entry; memset (&entry, 0, sizeof entry);
    CopyLumpName (entry.mapname, name);

    while (Next (s))
    {
	if (s->type == TK_PUNCT && s->str[0] == '}') break;
	if (s->type == TK_IDENT) ParseProperty (s, &entry);
    }

    // Merge into the table (replace an existing record for the same map).
    int i;
    for (i = 0; i < nummaps; i++)
	if (!strcmp (maps[i].mapname, entry.mapname)) break;
    if (i == nummaps)
    {
	maps = realloc (maps, (nummaps + 1) * sizeof *maps);
	nummaps++;
    }
    maps[i] = entry;
}

static void ParseLump (const char* data, int len)
{
    scan_t s; memset (&s, 0, sizeof s);
    s.p = data; s.end = data + len; s.line = 1;
    while (Next (&s))
	if (s.type == TK_IDENT && !strcasecmp (s.str, "map")) ParseMapEntry (&s);
}

void U_LoadMapInfo (void)
{
    int i, found = 0;
    // Parse EVERY UMAPINFO lump in load order (later PWADs override earlier maps).
    for (i = 0; i < numlumps; i++)
    {
	if (strncasecmp (lumpinfo[i].name, "UMAPINFO", 8)) continue;
	ParseLump ((const char*) W_CacheLumpNum (i, PU_CACHE), W_LumpLength (i));
	found++;
    }
    if (found)
	printf ("UMAPINFO: parsed %d lump(s) -> %d map record(s).\n", found, nummaps);
}

umap_t* U_LookupMap (int episode, int map)
{
    char name[9]; int i;
    if (!maps) return NULL;
    U_MapName (episode, map, name);
    for (i = 0; i < nummaps; i++)
	if (!strcasecmp (maps[i].mapname, name)) return &maps[i];
    return NULL;
}
