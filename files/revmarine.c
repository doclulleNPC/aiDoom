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
#define REVMAR_SIGHT	(1400*FRACUNIT)		// aggro/fire range -- acquires + shoots enemies within this
#define REVMAR_BUDDY_RANGE (96*FRACUNIT)	// buddy auto-revives a corpse within this
#define REVMAR_MAXHP	100			// revived marines regenerate up to this (1 HP/sec)
#define REVMAR_STARTHP	10			// ...starting from this on revive

// ---------------------------------------------------------------------------
// Aggressive marine AI.  The stock friendly A_Look/A_Chase only glance 180 deg
// ahead and gate their fire behind P_CheckMissileRange, so revived marines tended
// to wander instead of engaging.  These acquire the nearest enemy ALL AROUND and
// shoot the instant one is in sight + range, closing in otherwise.
// ---------------------------------------------------------------------------
static mobj_t* RevMarine_NearestEnemy (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next ; th != &thinkercap ; th = th->next)
    {
	mobj_t*	m;
	fixed_t	d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (!(m->flags & MF_COUNTKILL))	continue;	// real monsters only
	if (m->flags & MF_FRIEND)	continue;	// not our own allies
	if (!(m->flags & MF_SHOOTABLE))	continue;
	if (m->flags & MF_CORPSE)	continue;
	if (m->health <= 0)		continue;
	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > REVMAR_SIGHT)		continue;
	if (!P_CheckSight (self, m))	continue;
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}

void A_RevMarineLook (mobj_t* self)
{
    mobj_t* e = RevMarine_NearestEnemy (self);
    if (e)
    {
	self->target = e;
	if (self->info->seesound) S_StartSound (self, self->info->seesound);
	P_SetMobjState (self, self->info->seestate);
    }
}

void A_RevMarineChase (mobj_t* self)
{
    mobj_t* t = self->target;

    if (!t || t->health <= 0 || !(t->flags & MF_SHOOTABLE) || (t->flags & MF_CORPSE))
    {
	t = RevMarine_NearestEnemy (self);
	self->target = t;
	if (!t) { A_Chase (self); return; }		// nothing in reach -> stock wander/re-look
    }

    A_FaceTarget (self);
    if (P_CheckSight (self, t)
	&& P_AproxDistance (t->x - self->x, t->y - self->y) <= REVMAR_SIGHT)
    {
	P_SetMobjState (self, self->info->missilestate);	// shoot now -- no random gate
	return;
    }
    A_Chase (self);						// else close the distance
}

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

    ST (S_REVMAR_STND, 0,        10, (actionf_p1)A_RevMarineLook,  S_REVMAR_STND);
    ST (S_REVMAR_RUN1, 0,         4, (actionf_p1)A_RevMarineChase, S_REVMAR_RUN2);
    ST (S_REVMAR_RUN2, 1,         4, (actionf_p1)A_RevMarineChase, S_REVMAR_RUN3);
    ST (S_REVMAR_RUN3, 2,         4, (actionf_p1)A_RevMarineChase, S_REVMAR_RUN4);
    ST (S_REVMAR_RUN4, 3,         4, (actionf_p1)A_RevMarineChase, S_REVMAR_RUN1);
    ST (S_REVMAR_ATK1, 4,         5, (actionf_p1)A_FaceTarget, S_REVMAR_ATK2);
    ST (S_REVMAR_ATK2, BRIGHT|5,  6, (actionf_p1)A_PosAttack,  S_REVMAR_RUN1);	// pistol shot (hitscan)
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

// Nearest Dead Marine (thing type 15 = MT_MISC62) within `range` of `who`, in sight.
static mobj_t* RevMarine_FindCorpse (mobj_t* who, fixed_t range)
{
    thinker_t*	th;
    mobj_t*	corpse = NULL;
    fixed_t	bestd  = range;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	c;
	fixed_t	d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	c = (mobj_t*)th;
	if (c->type != MT_MISC62) continue;		// dead marine (thing type 15)
	if (!P_CheckSight (who, c)) continue;		// not through closed doors / walls
	d = P_AproxDistance (c->x - who->x, c->y - who->y);
	if (d < bestd) { bestd = d; corpse = c; }
    }
    return corpse;
}

// Stand a Dead Marine corpse up as a friendly marine (MF_FRIEND) and consume it.
// Shared by the human USE and the buddy auto-revive.
static void RevMarine_Raise (mobj_t* corpse)
{
    mobj_t* mar = P_SpawnMobj (corpse->x, corpse->y, corpse->z, MT_REVMARINE);
    mar->flags |= MF_FRIEND;				// hunts enemy monsters, not the player
    mar->health = REVMAR_STARTHP;
    mar->angle  = corpse->angle;
    P_SetMobjState (mar, mar->info->raisestate);	// Arch-Vile-style reverse-death "get up"
    // Restore the full LIVING hitbox from mobjinfo so hitscans/projectiles connect.
    mar->height = mar->info->height;
    mar->radius = mar->info->radius;
    P_RemoveMobj (corpse);

    // Stood up inside something?  Shove to the nearest free spot.
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
}

// ---------------------------------------------------------------------------
// USE near a Dead Marine (thing type 15 = MT_MISC62): stand it up as a friendly
// marine.  The human donates one stimpack (or ten health bonuses) from the
// buddy-mode inventory.  Returns a HUD message, or NULL if no corpse is in reach
// (so the USE falls through to doors / the buddy revive).
// ---------------------------------------------------------------------------
const char* P_ReviveMarineNear (player_t* presser)
{
    mobj_t*	corpse;

    if (!presser || !presser->mo) return NULL;
    // Buddy-mode-only perk: recruiting an ally is squad play.  Silent no-op otherwise.
    if (!P_AICoop_Active ()) return NULL;

    corpse = RevMarine_FindCorpse (presser->mo, REVMAR_RANGE);
    if (!corpse) return NULL;				// nothing in reach -> fall through to doors

    // Cost: one stimpack, else ten health bonuses.  The reviver's own health is untouched.
    if      (presser->inventory[arti_stimpack]    >= 1)  presser->inventory[arti_stimpack]    -= 1;
    else if (presser->inventory[arti_healthbonus] >= 10) presser->inventory[arti_healthbonus] -= 10;
    else
    {
	presser->message = "[Marine] (need a stimpack or 10 health bonuses to revive)";
	return NULL;
    }

    RevMarine_Raise (corpse);

    // If that emptied the selected inventory slot, hop the selection to a held one.
    if (presser->invslot != arti_none && presser->inventory[presser->invslot] <= 0)
	P_InvScroll (presser, +1);

    return "[Marine] Revived a marine -- he's with you now!";
}

// ---------------------------------------------------------------------------
// The AI buddy can also revive a Dead Marine on its own -- but only when it can
// afford the field surgery: at least 1 Medikit AND 2 Stimpacks in its own pack
// (all consumed).  Called each tic from the buddy AI (throttled there).
// ---------------------------------------------------------------------------
void RevMarine_BuddyTryRevive (player_t* bot)
{
    mobj_t* corpse;

    if (!bot || !bot->mo || bot->playerstate != PST_LIVE) return;
    if (bot->inventory[arti_medikit] < 1 || bot->inventory[arti_stimpack] < 2) return;

    corpse = RevMarine_FindCorpse (bot->mo, REVMAR_BUDDY_RANGE);
    if (!corpse) return;

    bot->inventory[arti_medikit]  -= 1;			// the medic kit
    bot->inventory[arti_stimpack] -= 2;			// + two stims
    RevMarine_Raise (corpse);
    players[consoleplayer].message = "[Buddy] Patched up a downed marine -- he's with us!";
}

// ---------------------------------------------------------------------------
// Per-second regeneration: every revived marine heals 1 HP up to REVMAR_MAXHP.
// Called from P_Ticker.
// ---------------------------------------------------------------------------
void RevMarine_Ticker (void)
{
    thinker_t*	th;

    if (gametic % TICRATE != 0)				// once per second
	return;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (m->type == MT_REVMARINE && m->health > 0 && m->health < REVMAR_MAXHP)
	    m->health++;
    }
}
