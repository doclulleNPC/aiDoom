// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	(G) Revived friendly marines.
//
//	A "Dead Marine" map decoration (DOOM thing type 15 = MT_MISC62) can be
//	revived by the human's USE -- exactly like the downed buddy: it stands up as
//	a friendly marine (MF_FRIEND) that hunts ENEMY monsters through the engine's
//	friendly-AI path (P_LookForPlayers -> P_FriendNearestEnemy, see p_enemy.c).
//	Buddy-mode-only perk: P_ReviveMarineNear no-ops unless P_AICoop_Active() --
//	recruiting allies is squad play, not something a solo marine does.
//
//	PLAY sprite (looks like a marine), zombieman-style hitscan (A_PosAttack),
//	10 HP, and a deliberately slow ~half-the-player pace (a shambling zombie ally).
//	States/mobjinfo are appended to the engine tables at runtime (RevMarine_Init),
//	same additive mechanism as files/heretic.c / files/hexen.c; the enum slots live
//	at the end of statenum_t / mobjtype_t (info.h).  No new sprite -- SPR_PLAY is
//	always present.
//
//-----------------------------------------------------------------------------

#include "doomdef.h"
#include "doomstat.h"			// players[], thinkercap
#include "info.h"
#include "m_fixed.h"
#include "m_random.h"			// P_Random -- pick a random gib decoration on death
#include "tables.h"
#include "sounds.h"
#include "p_local.h"
#include "p_invent.h"			// (J) buddy-mode inventory -- revive is paid from it
#include "p_ai_coop.h"			// P_AICoop_Active -- reviving an ally is a buddy-mode-only perk
#include "revmarine.h"

extern state_t *states;
extern mobjinfo_t *mobjinfo;

// engine pieces we use (declared by hand, like heretic.c/hexen.c)
extern void	A_Look (mobj_t*);
extern void	A_Chase (mobj_t*);
extern void	A_FaceTarget (mobj_t*);
extern void	A_PosAttack (mobj_t*);
extern void	A_Pain (mobj_t*);
extern void	A_Scream (mobj_t*);
extern void	A_Fall (mobj_t*);
extern void	P_MobjThinker (mobj_t*);
extern mobj_t*	P_SpawnMobj (fixed_t x, fixed_t y, fixed_t z, mobjtype_t type);
extern void	P_RemoveMobj (mobj_t*);
extern boolean	P_TryMove (mobj_t* thing, fixed_t x, fixed_t y);
extern boolean	P_CheckPosition (mobj_t* thing, fixed_t x, fixed_t y);
extern void	S_StartSound (void* origin, int sfx_id);

// On death the revived marine GIBS into a permanent bloody mess instead of leaving a
// clean (re-revivable) corpse: spawn a random gib decoration -- DOOM thing 10
// (MT_MISC68, "bloody mess") or 24 (MT_MISC71, "pool of guts") -- and squelch.  The
// marine mobj itself is then removed by its death state chaining to S_NULL, so it can
// never be raised again (it is NOT a thing-15 dead marine).
void A_RevMarineGib (mobj_t* actor)
{
    mobjtype_t t = (P_Random () & 1) ? MT_MISC68 : MT_MISC71;
    P_SpawnMobj (actor->x, actor->y, actor->z, t);
    S_StartSound (actor, sfx_slop);
}

#define BRIGHT		32768			// FF_FULLBRIGHT frame bit
#define REVMAR_RANGE	(64*FRACUNIT)		// human must be this close (and press USE) to revive

static void ST (statenum_t s, int frame, int tics, actionf_p1 act, statenum_t next)
{
    states[s].sprite      = SPR_PLAY;		// marine art (the player sprite)
    states[s].frame       = frame;
    states[s].tics        = tics;
    states[s].action.acp1 = act;
    states[s].nextstate   = next;
    states[s].misc1 = states[s].misc2 = 0;
}

void RevMarine_Init (void)
{
    mobjinfo_t*	m;

    ST (S_REVMAR_STND, 0,        10, (actionf_p1)A_Look,       S_REVMAR_STND);
    ST (S_REVMAR_RUN1, 0,         4, (actionf_p1)A_Chase,      S_REVMAR_RUN2);
    ST (S_REVMAR_RUN2, 1,         4, (actionf_p1)A_Chase,      S_REVMAR_RUN3);
    ST (S_REVMAR_RUN3, 2,         4, (actionf_p1)A_Chase,      S_REVMAR_RUN4);
    ST (S_REVMAR_RUN4, 3,         4, (actionf_p1)A_Chase,      S_REVMAR_RUN1);
    ST (S_REVMAR_ATK1, 4,         8, (actionf_p1)A_FaceTarget, S_REVMAR_ATK2);
    ST (S_REVMAR_ATK2, BRIGHT|5,  8, (actionf_p1)A_PosAttack,  S_REVMAR_RUN1);	// pistol shot (hitscan)
    ST (S_REVMAR_PAIN, 6,         6, (actionf_p1)A_Pain,       S_REVMAR_RUN1);
    // Death: collapse, then GIB into a permanent bloody mess (A_RevMarineGib at DIE6)
    // and remove the marine (DIE7 -> S_NULL) so it leaves a thing-10/24 gib, NOT a
    // re-revivable thing-15 corpse.
    ST (S_REVMAR_DIE1, 7,         5, (actionf_p1)A_Scream,        S_REVMAR_DIE2);
    ST (S_REVMAR_DIE2, 8,         5, (actionf_p1)A_Fall,          S_REVMAR_DIE3);
    ST (S_REVMAR_DIE3, 9,         5, NULL,                        S_REVMAR_DIE4);
    ST (S_REVMAR_DIE4, 10,        5, NULL,                        S_REVMAR_DIE5);
    ST (S_REVMAR_DIE5, 11,        5, NULL,                        S_REVMAR_DIE6);
    ST (S_REVMAR_DIE6, 12,        4, (actionf_p1)A_RevMarineGib,  S_REVMAR_DIE7);
    ST (S_REVMAR_DIE7, 13,        4, NULL,                        S_NULL);
    // "Get up": the death frames in REVERSE (13->7), then stand.  No action funcs --
    // just the un-dying animation; when it ends, S_REVMAR_STND's A_Look picks a target.
    ST (S_REVMAR_RISE1, 13,       4, NULL,                        S_REVMAR_RISE2);
    ST (S_REVMAR_RISE2, 12,       4, NULL,                        S_REVMAR_RISE3);
    ST (S_REVMAR_RISE3, 11,       4, NULL,                        S_REVMAR_RISE4);
    ST (S_REVMAR_RISE4, 10,       4, NULL,                        S_REVMAR_RISE5);
    ST (S_REVMAR_RISE5, 9,        4, NULL,                        S_REVMAR_RISE6);
    ST (S_REVMAR_RISE6, 8,        4, NULL,                        S_REVMAR_RISE7);
    ST (S_REVMAR_RISE7, 7,        4, NULL,                        S_REVMAR_STND);

    m = &mobjinfo[MT_REVMARINE];
    m->doomednum   = -1;			// never map-placed; only spawned by a revive
    m->spawnstate  = S_REVMAR_STND; m->spawnhealth = 10;	// (G) gets 10 HP
    m->seestate    = S_REVMAR_RUN1; m->seesound    = sfx_posit1; m->reactiontime = 8;
    m->attacksound = 0;             m->painstate   = S_REVMAR_PAIN; m->painchance = 200;
    m->painsound   = sfx_plpain;    m->meleestate  = S_NULL;    m->missilestate = S_REVMAR_ATK1;
    m->deathstate  = S_REVMAR_DIE1; m->xdeathstate = S_NULL;    m->deathsound = sfx_pldeth;
    // Speed 8 == zombieman pace: deliberately slow, well under the player's run (a zombie ally).
    m->speed = 8; m->radius = 16*FRACUNIT; m->height = 56*FRACUNIT; m->mass = 100;
    m->damage = 0; m->activesound = 0;
    // MF_FRIEND is added at spawn so A_Look/A_Chase hunt enemy monsters (P_FriendNearestEnemy);
    // no MF_COUNTKILL -- a friendly ally must not count as a level "monster"/kill.
    m->flags = MF_SOLID | MF_SHOOTABLE;
    // raisestate = the reverse-death "get up" chain, so the revive plays it the same way
    // an Arch-Vile / the director raises a corpse (P_SetMobjState(.., info->raisestate)).
    m->raisestate = S_REVMAR_RISE1;
}

// ---------------------------------------------------------------------------
// USE near a Dead Marine (thing type 15 = MT_MISC62): stand it up as a friendly
// marine.  Same deal as the buddy revive -- the human donates 10 HP, and if the
// ally stands up overlapping something it is shoved to the nearest free spot so
// the two never wedge.  Returns a HUD message, or NULL if no corpse is in reach
// (so the USE falls through to doors / the buddy revive).
// ---------------------------------------------------------------------------
const char* P_ReviveMarineNear (player_t* presser)
{
    thinker_t*	th;
    mobj_t*	corpse = NULL;
    fixed_t	bestd  = REVMAR_RANGE;
    mobj_t*	mar;

    if (!presser || !presser->mo) return NULL;

    // Buddy-mode-only perk: recruiting an ally is squad play.  Silent no-op outside
    // buddy mode (like P_AICoop_RevivePress with no downed buddy) so USE falls
    // through to door-opening as normal instead of a confusing message every press.
    if (!P_AICoop_Active ()) return NULL;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	c;
	fixed_t	d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	c = (mobj_t*)th;
	if (c->type != MT_MISC62) continue;		// dead marine (thing type 15)
	
	// Check line of sight: do not revive through closed doors or walls
	if (!P_CheckSight (presser->mo, c)) continue;
	
	d = P_AproxDistance (c->x - presser->mo->x, c->y - presser->mo->y);
	if (d < bestd) { bestd = d; corpse = c; }
    }
    if (!corpse) return NULL;				// nothing to revive in reach

    // Cost: spend from the buddy-mode inventory -- one stimpack (worth 10 HP) OR ten
    // health bonuses (10x1 HP).  The reviver's own health is NOT touched.
    if      (presser->inventory[arti_stimpack]    >= 1)  presser->inventory[arti_stimpack]    -= 1;
    else if (presser->inventory[arti_healthbonus] >= 10) presser->inventory[arti_healthbonus] -= 10;
    else
    {
	presser->message = "[Marine] (need a stimpack or 10 health bonuses to revive)";
	return NULL;
    }

    mar = P_SpawnMobj (corpse->x, corpse->y, corpse->z, MT_REVMARINE);
    mar->flags |= MF_FRIEND;				// hunts enemy monsters, not the player
    mar->health = 10;
    mar->angle  = corpse->angle;
    P_SetMobjState (mar, mar->info->raisestate);	// play the death animation in REVERSE (Arch-Vile-style get up)
    // Restore the full LIVING hitbox from mobjinfo (cf. P_Director_ReviveCorpse's ghost-monster
    // fix) so hitscans/projectiles connect -- a zero/short height would make the marine
    // unshootable.  Flags are left untouched so MF_FRIEND (set above) survives.
    mar->height = mar->info->height;
    mar->radius = mar->info->radius;
    P_RemoveMobj (corpse);

    // If that emptied the selected inventory slot, hop the selection to a held one.
    if (presser->invslot != arti_none && presser->inventory[presser->invslot] <= 0)
	P_InvScroll (presser, +1);

    // Stood up inside the reviver or a wall?  Shove to the nearest free spot.
    if (!P_CheckPosition (mar, mar->x, mar->y))
    {
	static const int ox[8] = { 1, 1, 0, -1, -1, -1,  0,  1 };
	static const int oy[8] = { 0, 1, 1,  1,  0, -1, -1, -1 };
	fixed_t step = mar->radius * 2 + 8*FRACUNIT;
	int k;
	for (k = 0; k < 8; k++)
	    if (P_TryMove (mar, mar->x + ox[k]*step, mar->y + oy[k]*step))
		break;
    }
    return "[Marine] Revived a marine -- he's with you now!";
}
