// Copyright(C) 1993-2024 Id Software, Inc.
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//
//
// ID24 data tables and supporting code 0.99.1
//
// These are the definitions required to correctly interpret the ID24
// object tables. The structs used do have some incompatibility with
// the vanilla structs, namely each struct now has an index and a
// minimum feature level field at the front.
//
// These definitions are C++17 compatible.
//
// The use of actionf_t matches the one defined in id24thinker.h, and
// and as such requires to be initialised in an initialiser-list
// manner. It also requires the correct function declarations, otherwise
// each function will be set as a void() function and will immediately
// error your program when you get out of the title screen.

typedef enum gamefeatures_e : int32_t
{
	features_invalid 	= -1,
	features_vanilla,
	features_limit_removing,
	features_limit_removing_fixed,
	features_boom_2_02,
	features_complevel9,
	features_mbf,
	features_mbf_dehextra,
	features_mbf21,
	features_mbf21_extended,
	features_id24,

	features_max,
} gamefeatures_t;

typedef enum id24ammotype_e : int32_t
{
	am_fuel 			= -1879048192,
} id24ammotype_t;

typedef enum id24weapontype_e : int32_t
{
	wp_incinerator		= -1879048192,
	wp_calamityblade	= -1879048191,
} id24weapontype_t;

typedef struct spriteinfo_s
{
	int32_t				spritenum;
	int32_t				minimumfeatures;
	const char*			sprite;
} spriteinfo_t;

typedef union statearg_u
{
	int32_t				_int;
	uint32_t			_uint;
	fixed_t				_fixed;
	angle_t				_angle;
	int32_t				_mobjtype;
	int32_t				_statenum;
	int32_t				_sound;
} statearg_t;

typedef struct state_s
{
	int32_t				statenum;
	int32_t				minimumfeatures;

	// Vanilla features
	int32_t				sprite;
	int32_t				frame;
	int32_t				tics;
	actionf_t			action;
	int32_t 			nextstate;
	statearg_t			misc1;
	statearg_t			misc2;
	
	// MBF21 extensions
	int32_t				mbf21flags;
	statearg_t			arg1;
	statearg_t			arg2;
	statearg_t			arg3;
	statearg_t			arg4;
	statearg_t			arg5;
	statearg_t			arg6;
	statearg_t			arg7;
	statearg_t			arg8;

	// ID24 extensions
	const char*			tranmaplump;
} state_t;

typedef struct mobjinfo_s
{
	int32_t				type;
	int32_t				minimumfeatures;

	// Vanilla features
	int32_t				doomednum;
	int32_t				spawnstate;
	int32_t				spawnhealth;
	int32_t				seestate;
	int32_t				seesound;
	int32_t				reactiontime;
	int32_t				attacksound;
	int32_t				painstate;
	int32_t				painchance;
	int32_t				painsound;
	int32_t				meleestate;
	int32_t				missilestate;
	int32_t				deathstate;
	int32_t				xdeathstate;
	int32_t				deathsound;
	int32_t				speed;
	int32_t				radius;
	int32_t				height;
	int32_t				mass;
	int32_t				damage;
	int32_t				activesound;
	int32_t				flags;
	int32_t				raisestate;

	// MBF21 extensions
	fixed_t				fastspeed;
	fixed_t				meleerange;
	int32_t				infightinggroup;
	int32_t				projectilegroup;
	int32_t				splashgroup;
	int32_t				mbf21flags;
	int32_t				ripsound;

	// ID24 extensions
	int32_t				id24flags;
	int32_t				minrespawntics;
	int32_t				respawndice;
	int32_t				dropthing;
	int32_t				pickupammotype;
	int32_t				pickupammocategory;
	int32_t				pickupweapontype;
	int32_t				pickupitemtype;
	int32_t				pickupbonuscount;
	int32_t				pickupsound;
	const char* 		pickupstringmnemonic;
	const char* 		translationlump;
} mobjinfo_t;

typedef struct sfxinfo_s
{
	int32_t 			soundnum;
	int32_t 			minimumfeatures;

	char*				name;
	int32_t				singularity;
	int32_t				priority;
	sfxinfo_s*			link;
	int32_t				pitch;
	int32_t				volume;
	void*				data;
} sfxinfo_t;

typedef struct ammoinfo_s
{
	int32_t				index;
	int32_t				minimumfeatures;

	// Vanilla features
	int32_t				clipammo;
	int32_t				maxammo;

	// ID24 extensions
	int32_t				initialammo;
	int32_t				maxupgradedammo;
	int32_t				boxammo;
	int32_t				backpackammo;
	int32_t				weaponammo;
	int32_t				droppedclipammo;
	int32_t				droppedboxammo;
	int32_t				droppedbackpackammo;
	int32_t				droppedweaponammo;
	int32_t				deathmatchweaponammo;
	fixed_t				skillmul[ sk_max ];
} ammoinfo_t;

typedef struct weaponinfo_s
{
	int32_t				index;
	int32_t				minimumfeatures;

	// Vanilla features
	int32_t				ammo;
	int32_t				upstate;
	int32_t				downstate;
	int32_t				readystate;
	int32_t				atkstate;
	int32_t				flashstate;

	// MBF21 extensions
	int32_t				mbf21flags;
	int32_t				ammopershot;

	// ID24 extensions
	int32_t				slot;
	int32_t				slotpriority;
	int32_t				switchpriority;
	bool				initialowned;
	bool				initialraised;
	const char*			carouselicon;
	int32_t				allowswitchifownedweapon;
	int32_t				noswitchifownedweapon;
	int32_t				allowswitchifowneditem;
	int32_t				noswitchifowneditem;
} weaponinfo_t;
