// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// DESCRIPTION:
//
//
//-----------------------------------------------------------------------------


#ifndef __D_PLAYER__
#define __D_PLAYER__


// The player data structure depends on a number
// of other structs: items (internal inventory),
// animation states (closely tied to the sprites
// used to represent them, unfortunately).
#include "d_items.h"
#include "p_pspr.h"

// In addition, the player is just a special
// case of the generic moving object/actor.
#include "p_mobj.h"

// Finally, for odd reasons, the player input
// is buffered within the player data struct,
// as commands per game tick.
#include "d_ticcmd.h"

#ifdef __GNUG__
#pragma interface
#endif




//
// Player states.
//
//
// (J) DOOM "overflow" inventory.  arti_none is slot 0 (the empty/no selection
// sentinel); the real artifacts follow.  NUMARTIFACTS terminates.
//
// When you pick up a health/armor/ammo item but you're already AT ITS CAP (so
// DOOM would waste it / refuse the pickup), the surplus is stored here to use
// later.  Two storage flavours:
//   - item artifacts (stimpack..bluearmor): inventory[a] is a COUNT of stored
//     items; each use applies the effect once and decrements by 1.
//   - ammo artifacts (arti_ammo_*): inventory[a] is a stored AMMO AMOUNT (the
//     sum of overflowed rounds); use transfers as much as fits and decrements
//     by the transferred amount.
//
typedef enum
{
    arti_none,
    arti_stimpack,		// +10 HP  (heal cap 100)
    arti_medikit,		// +25 HP  (heal cap 100)
    arti_healthbonus,		// +1 HP   (cap 200)
    arti_armorbonus,		// +1 armor point (cap 200)
    arti_greenarmor,		// green armor (100 pts / 1-3 absorb)
    arti_bluearmor,		// blue armor  (200 pts / 1-2 absorb)
    arti_ammo_bullets,		// overflow bullets (stores an AMOUNT, not a count)
    arti_ammo_shells,		// overflow shells
    arti_ammo_rockets,		// overflow rockets
    arti_ammo_cells,		// overflow cells
    // (H) Heretic artifacts -- effects + pickups live in files/p_inv_heretic.c.
    // Appended AFTER the DOOM overflow slots; do NOT reorder the entries above
    // (savegames index this enum).  Wings of Wrath / Morph Ovum are deferred
    // (they need flight / chicken subsystems we don't have yet).
    h_arti_flask,		// Quartz Flask  (+25 HP)
    h_arti_urn,			// Mystic Urn    (+100 HP)
    h_arti_tome,		// Tome of Power (simplified to a Berserk: pw_strength)
    h_arti_torch,		// Torch         (pw_infrared)
    h_arti_bomb,		// Time Bomb of the Ancients (spawns a fusing bomb)
    h_arti_ring,		// Ring of Invincibility (pw_invulnerability)
    h_arti_shadow,		// Shadowsphere  (pw_invisibility + MF_SHADOW)
    h_arti_chaos,		// Chaos Device  (teleport to player start)
    h_arti_wings,		// Wings of Wrath (generic flight: pw_flight)
    h_arti_egg,			// Morph Ovum     (fires egg missiles -> morph to chicken)
    NUMARTIFACTS

} artitype_t;

#define MAXARTICOUNT	16	// per-artifact carry cap for the item artifacts


typedef enum
{
    // Playing or camping.
    PST_LIVE,
    // Dead on the ground, view follows killer.
    PST_DEAD,
    // Ready to restart/respawn???
    PST_REBORN		

} playerstate_t;


//
// Player internal flags, for cheats and debug.
//
typedef enum
{
    // No clipping, walk through barriers.
    CF_NOCLIP		= 1,
    // No damage, no health loss.
    CF_GODMODE		= 2,
    // Not really a cheat, just a debug aid.
    CF_NOMOMENTUM	= 4,
    // Flight: no gravity, climb/descend by looking + jump (console `fly`).
    CF_FLY		= 8

} cheat_t;


//
// Extended player object info: player_t
//
typedef struct player_s
{
    mobj_t*		mo;
    playerstate_t	playerstate;
    ticcmd_t		cmd;

    // Determine POV,
    //  including viewpoint bobbing during movement.
    // Focal origin above r.z
    fixed_t		viewz;
    // Base height above floor for viewz.
    fixed_t		viewheight;
    // Bob/squat speed.
    fixed_t         	deltaviewheight;
    // bounded/scaled total momentum.
    fixed_t         	bob;	

    // This is only used between levels,
    // mo->health is used during levels.
    int			health;	
    int			armorpoints;
    // Armor type is 0-2.
    int			armortype;	

    // Power ups. invinc and invis are tic counters.
    int			powers[NUMPOWERS];
    boolean		cards[NUMCARDS];
    boolean		backpack;
    
    // Frags, kills of other players.
    int			frags[MAXPLAYERS];
    weapontype_t	readyweapon;
    
    // Is wp_nochange if not changing.
    weapontype_t	pendingweapon;

    boolean		weaponowned[NUMWEAPONS];
    int			ammo[NUMAMMO];
    int			maxammo[NUMAMMO];

    // True if button down last tic.
    int			attackdown;
    int			usedown;

    // Bit flags, for cheats and debug.
    // See cheat_t, above.
    int			cheats;		

    // Refired shots are less accurate.
    int			refire;		

     // For intermission stats.
    int			killcount;
    int			itemcount;
    int			secretcount;

    // Hint messages.
    char*		message;	
    
    // For screen flashing (red or bright).
    int			damagecount;
    int			bonuscount;

    // Who did damage (NULL for floors/ceilings).
    mobj_t*		attacker;
    
    // So gun flashes light up areas.
    int			extralight;

    // Current PLAYPAL, ???
    //  can be set to REDCOLORMAP for pain, etc.
    int			fixedcolormap;

    // Player skin colorshift,
    //  0-3 for which color to draw player.
    int			colormap;	

    // Overlay view sprites (gun, etc).
    pspdef_t		psprites[NUMPSPRITES];

    // True if secret level has been done.
    boolean		didsecret;

    // MOD: free-look pitch in BASE-resolution horizon-shift pixels (0 = level).
    int			lookdir;

    // (J) Heretic-style artifact inventory: a held count per artifact and the
    // currently-selected slot (an artitype_t; arti_none = nothing selected).
    int			inventory[NUMARTIFACTS];
    int			invslot;

} player_t;


//
// INTERMISSION
// Structure passed e.g. to WI_Start(wb)
//
typedef struct
{
    boolean	in;	// whether the player is in game
    
    // Player stats, kills, collected items etc.
    int		skills;
    int		sitems;
    int		ssecret;
    int		stime; 
    int		frags[4];
    int		score;	// current score on entry, modified on return
  
} wbplayerstruct_t;

typedef struct
{
    int		epsd;	// episode # (0-2)

    // if true, splash the secret level
    boolean	didsecret;
    
    // previous and next levels, origin 0
    int		last;
    int		next;	
    
    int		maxkills;
    int		maxitems;
    int		maxsecret;
    int		maxfrags;

    // the par time
    int		partime;
    
    // index of this player in game
    int		pnum;	

    wbplayerstruct_t	plyr[MAXPLAYERS];

} wbstartstruct_t;


#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
