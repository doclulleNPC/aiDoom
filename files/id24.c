// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	ID24 Legacy-of-Rust content installer.  Fills appended sprite/sound/state/
//	mobjinfo slots (grown via the DSDHacked dynamic tables) from the generated
//	files/id24_gen.h -- which is produced by tools/gen_id24.py from the official
//	ID24 0.99.1 data tables (id24data.cpp).  Art/sounds come from id24res.wad
//	(auto-loaded in d_main.c).  Codepointers are aiDoom's existing MBF21 set.
//
//	Cross-references in the generated tables are self-describingly ENCODED
//	(<= -1e9, type+rel packed) so they can be resolved here against the runtime
//	base indices; vanilla indices and literal args pass through unchanged.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "doomtype.h"
#include "doomdef.h"
#include "info.h"
#include "sounds.h"
#include "d_items.h"
#include "d_player.h"
#include "id24_gen.h"

extern weaponinfo_t	weaponinfo[];

// DSDHacked dynamic-table growth (files/dsdhacked.c).
extern void dsdh_EnsureSpritesCapacity  (int limit);
extern void dsdh_EnsureSFXCapacity      (int limit);
extern void dsdh_EnsureStatesCapacity   (int limit);
extern void dsdh_EnsureMobjInfoCapacity (int limit);

// Codepointers used by the ID24 states (all present in aiDoom's MBF21 layer).
extern void A_Look(),A_Chase(),A_FaceTarget(),A_Pain(),A_Fall(),A_Scream(),
	A_XScream(),A_BossDeath(),A_PlaySound(),A_SPosAttack(),A_CyberAttack(),
	A_Hoof(),A_SpidRefire(),A_RandomJump(),A_MonsterProjectile(),A_SpawnObject(),
	A_RadiusDamage(),A_RemoveFlags(),A_CheckAmmo(),A_ConsumeAmmo(),A_WeaponSound(),
	A_WeaponProjectile(),A_WeaponReady(),A_ReFire(),A_GunFlash(),A_GunFlashTo(),
	A_RefireTo(),A_WeaponJump(),A_Light1(),A_Light2(),A_Lower(),A_Raise();

static const struct { const char* name; actionf_p1 fn; } id24acts[] =
{
	{"A_Look",(actionf_p1)A_Look},{"A_Chase",(actionf_p1)A_Chase},
	{"A_FaceTarget",(actionf_p1)A_FaceTarget},{"A_Pain",(actionf_p1)A_Pain},
	{"A_Fall",(actionf_p1)A_Fall},{"A_Scream",(actionf_p1)A_Scream},
	{"A_XScream",(actionf_p1)A_XScream},{"A_BossDeath",(actionf_p1)A_BossDeath},
	{"A_PlaySound",(actionf_p1)A_PlaySound},{"A_SPosAttack",(actionf_p1)A_SPosAttack},
	{"A_CyberAttack",(actionf_p1)A_CyberAttack},{"A_Hoof",(actionf_p1)A_Hoof},
	{"A_SpidRefire",(actionf_p1)A_SpidRefire},{"A_RandomJump",(actionf_p1)A_RandomJump},
	{"A_MonsterProjectile",(actionf_p1)A_MonsterProjectile},
	{"A_SpawnObject",(actionf_p1)A_SpawnObject},{"A_RadiusDamage",(actionf_p1)A_RadiusDamage},
	{"A_RemoveFlags",(actionf_p1)A_RemoveFlags},{"A_CheckAmmo",(actionf_p1)A_CheckAmmo},
	{"A_ConsumeAmmo",(actionf_p1)A_ConsumeAmmo},{"A_WeaponSound",(actionf_p1)A_WeaponSound},
	{"A_WeaponProjectile",(actionf_p1)A_WeaponProjectile},{"A_WeaponReady",(actionf_p1)A_WeaponReady},
	{"A_ReFire",(actionf_p1)A_ReFire},{"A_GunFlash",(actionf_p1)A_GunFlash},
	{"A_GunFlashTo",(actionf_p1)A_GunFlashTo},{"A_RefireTo",(actionf_p1)A_RefireTo},
	{"A_WeaponJump",(actionf_p1)A_WeaponJump},{"A_Light1",(actionf_p1)A_Light1},
	{"A_Light2",(actionf_p1)A_Light2},{"A_Lower",(actionf_p1)A_Lower},{"A_Raise",(actionf_p1)A_Raise},
	{NULL,NULL}
};

static actionf_p1 id24_action (const char* name)
{
	int i;
	if (!name || !name[0] || !strcmp(name,"0")) return NULL;
	for (i = 0; id24acts[i].name; i++)
		if (!strcmp(name, id24acts[i].name)) return id24acts[i].fn;
	return NULL;	// unknown -> no action (safe)
}

// Bases captured before growing (set in ID24_Init).
static int bspr, bstate, bmobj, bsfx;
static boolean id24_installed = false;

// True once the Legacy-of-Rust content is installed (id24res.wad was present).
int ID24_Available (void) { return id24_installed; }

// Console "spawn <name>" -> the appended mobjtype for an ID24 monster, or -1.
int ID24_TypeByName (const char* s)
{
	static const struct { const char* n; int rel; } tbl[] = {
		{"ghoul",0},{"banshee",1},{"mindweaver",2},{"shocktrooper",3},
		{"vassago",4},{"tyrant",5},{"tyrant2",7},{NULL,0} };
	int i;
	if (!id24_installed || !s) return -1;
	for (i = 0; tbl[i].n; i++) if (!strcmp(s, tbl[i].n)) return bmobj + tbl[i].rel;
	return -1;
}

// Resolve a generated cross-reference: encoded id24 ref (<= -1e9) -> base+rel;
// anything else (vanilla index, 0/-1 none, literal arg) passes through.
static int id24_res (int v)
{
	int x, t, rel;
	if (v > -1000000000) return v;
	x = -v - 1000000000; t = x / 100000000; rel = x % 100000000;
	switch (t) { case 0: return bspr+rel; case 1: return bstate+rel;
		     case 2: return bmobj+rel; case 3: return bsfx+rel; }
	return v;
}

void ID24_Init (void)
{
	int i, k;

	// The resource WAD must actually be present -- probe a Ghoul sprite lump.
	extern int W_CheckNumForName (char* name);
	if (W_CheckNumForName ("GHULA1") < 0)
		return;			// no id24res.wad -> skip (content stays absent)

	bspr = num_sprites; bsfx = num_sfx; bstate = num_states; bmobj = num_mobjtypes;

	// --- sprites ---
	dsdh_EnsureSpritesCapacity (bspr + ID24_NSPR - 1);
	for (i = 0; i < ID24_NSPR; i++)
	{
		char* nm = malloc (5);
		memcpy (nm, id24_sprnames[i], 4); nm[4] = 0;
		sprnames[bspr + i] = nm;
	}

	// --- sounds --- (S_sfx name is the part after "ds", lower-cased)
	dsdh_EnsureSFXCapacity (bsfx + ID24_NSFX - 1);
	for (i = 0; i < ID24_NSFX; i++)
	{
		char* nm = malloc (9); int j;
		for (j = 0; j < 8 && id24_sfx[i].name[j]; j++) nm[j] = (char)tolower((unsigned char)id24_sfx[i].name[j]);
		nm[j] = 0;
		memset (&S_sfx[bsfx + i], 0, sizeof (sfxinfo_t));
		S_sfx[bsfx + i].name       = nm;
		S_sfx[bsfx + i].priority   = id24_sfx[i].prio;
		S_sfx[bsfx + i].pitch      = -1;
		S_sfx[bsfx + i].volume     = -1;
		S_sfx[bsfx + i].lumpnum    = -1;	// resolved lazily on first play
	}

	// --- states ---
	dsdh_EnsureStatesCapacity (bstate + ID24_NSTATE - 1);
	for (i = 0; i < ID24_NSTATE; i++)
	{
		state_t* st = &states[bstate + i];
		st->sprite     = id24_res (id24_st[i].sprite);
		st->frame      = id24_st[i].frame;
		st->tics       = id24_st[i].tics;
		st->action.acp1= id24_action (id24_st[i].action);
		st->nextstate  = id24_res (id24_st[i].next);
		st->misc1      = id24_res (id24_st[i].misc1);
		st->misc2      = id24_res (id24_st[i].misc2);
		st->flags      = id24_st[i].flags;
		for (k = 0; k < 8 && k < MAXSTATEARGS; k++)
			st->args[k] = id24_res (id24_st[i].args[k]);
	}

	// --- mobjinfo ---
	dsdh_EnsureMobjInfoCapacity (bmobj + ID24_NMOBJ - 1);
	for (i = 0; i < ID24_NMOBJ; i++)
	{
		mobjinfo_t* m = &mobjinfo[bmobj + i];
		memset (m, 0, sizeof (mobjinfo_t));
		m->doomednum    = id24_mobj[i].doomednum;
		m->spawnstate   = id24_res (id24_mobj[i].spawnstate);
		m->spawnhealth  = id24_mobj[i].spawnhealth;
		m->seestate     = id24_res (id24_mobj[i].seestate);
		m->seesound     = id24_res (id24_mobj[i].seesound);
		m->reactiontime = id24_mobj[i].reactiontime;
		m->attacksound  = id24_res (id24_mobj[i].attacksound);
		m->painstate    = id24_res (id24_mobj[i].painstate);
		m->painchance   = id24_mobj[i].painchance;
		m->painsound    = id24_res (id24_mobj[i].painsound);
		m->meleestate   = id24_res (id24_mobj[i].meleestate);
		m->missilestate = id24_res (id24_mobj[i].missilestate);
		m->deathstate   = id24_res (id24_mobj[i].deathstate);
		m->xdeathstate  = id24_res (id24_mobj[i].xdeathstate);
		m->deathsound   = id24_res (id24_mobj[i].deathsound);
		m->speed        = id24_mobj[i].speed;
		m->radius       = id24_mobj[i].radius;
		m->height       = id24_mobj[i].height;
		m->mass         = id24_mobj[i].mass;
		m->damage       = id24_mobj[i].damage;
		m->activesound  = id24_res (id24_mobj[i].activesound);
		m->flags        = id24_mobj[i].flags;
		m->raisestate   = id24_res (id24_mobj[i].raisestate);
		m->flags2           = id24_mobj[i].mbf21flags;
		m->infighting_group = id24_mobj[i].infight;
		m->projectile_group = id24_mobj[i].projgrp;
		m->splash_group     = id24_mobj[i].splashgrp;
		m->altspeed         = id24_mobj[i].fastspeed;
		m->meleerange       = id24_mobj[i].meleerange;
		m->droppeditem      = id24_res (id24_mobj[i].dropthing);
	}

	// --- weapons: fill the appended wp_incinerator / wp_calamityblade slots.
	// (weaponinfo[] auto-zero-fills the 2 new NUMWEAPONS entries; we set them here
	// since they reference the runtime-installed ID24 states.)
	for (i = 0; i < ID24_NWPN && (wp_incinerator + i) < NUMWEAPONS; i++)
	{
		weaponinfo_t* wi = &weaponinfo[wp_incinerator + i];
		wi->ammo       = (ammotype_t) id24_wpn[i][0];	// am_fuel
		wi->upstate    = id24_res (id24_wpn[i][1]);
		wi->downstate  = id24_res (id24_wpn[i][2]);
		wi->readystate = id24_res (id24_wpn[i][3]);
		wi->atkstate   = id24_res (id24_wpn[i][4]);
		wi->flashstate = id24_res (id24_wpn[i][5]);
	}

	id24_installed = true;
	printf ("ID24: installed %d sprites, %d sounds, %d states, %d things, %d weapons"
		" (Legacy of Rust content).\n", ID24_NSPR, ID24_NSFX, ID24_NSTATE, ID24_NMOBJ, ID24_NWPN);
}

// Console "give <name>": grant an ID24 weapon (+ full fuel) or fuel ammo.  Returns 1
// if handled.  `pl` is a player_t*.
int ID24_Give (void* pl_v, const char* s)
{
	player_t* pl = (player_t*) pl_v;
	if (!id24_installed || !pl || !s) return 0;
	if (!strcmp (s, "incinerator"))
	{ pl->weaponowned[wp_incinerator] = true; pl->pendingweapon = wp_incinerator;
	  pl->ammo[am_fuel] = pl->maxammo[am_fuel]; return 1; }
	if (!strcmp (s, "heatwave") || !strcmp (s, "calamityblade") || !strcmp (s, "blade"))
	{ pl->weaponowned[wp_calamityblade] = true; pl->pendingweapon = wp_calamityblade;
	  pl->ammo[am_fuel] = pl->maxammo[am_fuel]; return 1; }
	if (!strcmp (s, "fuel"))
	{ pl->ammo[am_fuel] = pl->maxammo[am_fuel]; return 1; }
	return 0;
}
