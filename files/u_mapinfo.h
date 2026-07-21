// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	UMAPINFO support (https://doomwiki.org/wiki/UMAPINFO).  A self-contained
//	parser + lookup for the UMAPINFO lump: per-map level name, next/secret map,
//	music, sky, par time, boss actions, end-game triggers and intermission text.
//	Modelled on Woof's g_umapinfo.c but with no external scanner dependency.
//
//	UMAPINFO is also the one ID24-baseline component this fork was missing
//	(MBF21/Boom/DSDHacked/DEHEXTRA/MUSINFO are already present).
//
//-----------------------------------------------------------------------------
#ifndef __U_MAPINFO__
#define __U_MAPINFO__

#include "doomtype.h"

// One boss-death action (replicates the hardcoded "tag 666" behaviour).
typedef struct
{
    int	type;		// mobjtype_t of the boss
    int	special;	// linedef special to run when the last one dies
    int	tag;		// sector tag (0 only for level-exit specials)
} u_bossaction_t;

// end-game flags (mape.endflags)
enum
{
    U_END_STANDARD = 0x01,	// endgame=true -> the IWAD's default post-game finale
    U_END_CAST     = 0x02,	// endcast=true -> cast call
    U_END_BUNNY    = 0x04,	// endbunny=true -> bunny scroller
    U_END_ART      = 0x08,	// endpic=LUMP  -> single end graphic
    U_END_ANY      = 0x0F
};

typedef struct umap_s
{
    char	mapname[9];		// "MAPxx" / "ExMy" (upper-case)
    char*	levelname;		// readable name (malloc'd) or NULL
    char*	label;			// automap label override (malloc'd) or NULL
    boolean	label_clear;		// label = clear -> no prefix
    char*	author;			// malloc'd or NULL
    char*	intertext;		// regular-exit story text (malloc'd) or NULL
    char*	intertextsecret;	// secret-exit story text (malloc'd) or NULL
    boolean	intertext_clear;
    boolean	intertextsecret_clear;
    char	levelpic[9];		// intermission "entering/finished" patch
    char	nextmap[9];		// regular exit target ("" = default)
    char	nextsecret[9];		// secret exit target ("" = default)
    char	music[9];		// map music lump ("" = default)
    char	skytexture[9];		// sky texture ("" = default)
    char	endpic[9];		// U_END_ART graphic
    char	exitpic[9];		// intermission "finished" background
    char	enterpic[9];		// intermission "entering" background
    char	interbackdrop[9];	// intertext backdrop flat/patch
    char	intermusic[9];		// intertext music
    int		partime;		// par time in SECONDS (0 = unset)
    int		endflags;		// U_END_* bits (0 = normal progression)
    boolean	nointermission;		// skip the level-finished screen
    boolean	bossaction_clear;	// bossaction = clear (suppress hardcoded 666)
    u_bossaction_t* bossactions;	// malloc'd array (may be NULL)
    int		numbossactions;
} umap_t;

// Cross-episode next-map carry (see u_mapinfo.c).  G_DoCompleted sets them from a
// UMAPINFO next/nextsecret; G_DoWorldDone applies + clears them.
extern int	u_next_episode;
extern int	u_next_map;

// Parse every UMAPINFO lump in load order.  Call once after WAD init + gamemode.
void	U_LoadMapInfo (void);

// The UMAPINFO record for a level, or NULL if none.  episode is 1-based (1 for
// commercial), map is 1-based.
umap_t*	U_LookupMap (int episode, int map);

// "ExMy" / "MAPxx" name for an episode/map pair -> out[9].
void	U_MapName (int episode, int map, char* out);

// Parse an "ExMy" / "MAPxx" name into episode/map (per current gamemode).  Either
// out pointer may be NULL.  Returns false if it isn't a valid name for this game.
boolean	U_ValidateMapName (const char* name, int* episode, int* map);

#endif	// __U_MAPINFO__
