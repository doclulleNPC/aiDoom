// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	AI-controlled co-op companion (player 2).  Enabled with -aicoop.
//
//	Navigation reuses the engine's monster path logic (P_NewChaseDir / P_Move):
//	it steps around walls and opens doors by itself.  The companion stays a real
//	player (weapons, damage, pickups, reborn all work) -- movement is done on the
//	mobj via movedir, aiming via mo->angle, and firing via the ticcmd, so it can
//	navigate and shoot independently.
//
//	Behaviour:
//	  * auto-heals +1 HP/s up to 100
//	  * yields (steps aside) when the human bumps into it
//	  * below coop_heal_hp it flees monsters and hides until auto-heal brings it
//	    back to coop_defend_hp (hysteresis)
//	  * with monsters in sight: fights (charges when healthy, kites when low)
//	  * no monsters in sight: fetches nearby health / bonus / armor, else follows
//	  * never stands still longer than ~3 s
//
//	Single-machine only (cmd generated locally, never sent over the wire).
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "doomdef.h"
#include "doomstat.h"
#include "d_event.h"
#include "m_argv.h"
#include "p_local.h"
#include "p_mobj.h"
#include "info.h"
#include "r_main.h"
#include "tables.h"
#include "m_fixed.h"

#include "p_ai_coop.h"

// engine internals (p_enemy.c -- no public header)
extern void	P_NewChaseDir (mobj_t* actor);
extern boolean	P_Move (mobj_t* actor);
extern boolean	P_SetMobjState (mobj_t* mobj, statenum_t state);

static int	aicoop;			// -aicoop given
static int	coop_state;		// what we're doing (for the console "where")
static int	summon;			// >0: "come" was ordered -- run to the player
static int	wantmove;		// set when we actually tried to move this tic
static int	hold;			// "wait/stay" -- hold position
static int	forceaggro;		// >0: "attack" ordered -- charge forcetarget
static mobj_t*	forcetarget;		// the forced attack target
// coop_state codes (index into the report text): 0 hold 1 follow 2 fight 3 flee
//                                                4 items 5 yield

#define COOP_KEEP	(192*FRACUNIT)	// fight: hold this far from a monster
#define YIELD_DIST	(48*FRACUNIT)	// step aside when the human is this close
#define IDLE_TICS	105		// ~3 s: don't stay stuck longer than this

// --- Behaviour knobs (loaded from aidoom.cfg via m_misc.c, edited by aicoop_config) ---
int	coop_defend_hp  = 35;		// below this HP: don't charge / kite
int	coop_heal_hp    = 20;		// below this HP: flee & hide until coop_defend_hp
int	coop_sight      = 1280;		// monster acquisition range (map units)
int	coop_follow     = 256;		// follow distance to the human (map units)
int	coop_heal_range = 1024;		// pickup search range (map units)
int	coop_speed      = 13;		// step size per tic for P_Move (player run ~= 16-17)
#define COOP_SIGHT	((fixed_t)coop_sight      * FRACUNIT)
#define COOP_NEAR	((fixed_t)coop_follow     * FRACUNIT)
#define COOP_ITEM_RANGE	((fixed_t)coop_heal_range * FRACUNIT)


void P_AICoop_Init (void)
{
    if (!M_CheckParm ("-aicoop"))
	return;
    aicoop = 1;
    playeringame[1] = true;		// spawn player 2 at the map's co-op start
    if (coop_speed < 4)  coop_speed = 4;
    if (coop_speed > 30) coop_speed = 30;
    mobjinfo[MT_PLAYER].speed = coop_speed;	// step size for P_Move (tunable)
    printf ("P_AICoop: AI co-op companion (player 2) enabled\n");
}


// Nearest live, shootable, visible monster within range (never the human).
static mobj_t* AICoop_FindTarget (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m; fixed_t d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (m == self || m->type == MT_PLAYER)	continue;
	if (!(m->flags & MF_COUNTKILL))		continue;	// monsters only (skip barrels)
	if (!(m->flags & MF_SHOOTABLE))		continue;
	if (m->flags & MF_CORPSE || m->health <= 0) continue;
	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > COOP_SIGHT)			continue;
	if (!P_CheckSight (self, m))		continue;
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}

// Nearest pickup worth grabbing: health, health/armor bonuses, armor.
static mobj_t* AICoop_FindItem (mobj_t* self)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m; fixed_t d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (!(m->flags & MF_SPECIAL))	continue;	// a pickup still in the world
	switch (m->sprite)
	{
	  case SPR_STIM: case SPR_MEDI: case SPR_SOUL: case SPR_MEGA:	// health
	  case SPR_BON1: case SPR_BON2:					// health/armor bonus
	  case SPR_ARM1: case SPR_ARM2:					// armor
	    break;
	  default:
	    continue;
	}
	d = P_AproxDistance (m->x - self->x, m->y - self->y);
	if (d > COOP_ITEM_RANGE)	continue;
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}

// Nearest live shootable monster to a point (used by the "attack" order).
static mobj_t* AICoop_NearestMonsterTo (fixed_t x, fixed_t y)
{
    thinker_t*	th;
    mobj_t*	best = NULL;
    fixed_t	bestd = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
	mobj_t*	m; fixed_t d;
	if (th->function.acp1 != (actionf_p1)P_MobjThinker) continue;
	m = (mobj_t*)th;
	if (m->type == MT_PLAYER)		continue;
	if (!(m->flags & MF_COUNTKILL))		continue;
	if (!(m->flags & MF_SHOOTABLE))		continue;
	if (m->flags & MF_CORPSE || m->health <= 0) continue;
	d = P_AproxDistance (m->x - x, m->y - y);
	if (!best || d < bestd) { best = m; bestd = d; }
    }
    return best;
}

// Move toward a goal mobj using the monster pathing (walls + doors handled).
static void AICoop_Chase (mobj_t* mo, mobj_t* goal)
{
    wantmove = 1;
    mo->target = goal;
    if (--mo->movecount < 0 || !P_Move (mo))
	P_NewChaseDir (mo);
}

// Step away from (fx,fy): pick the 8-dir leading away, try it then turned dirs.
static void AICoop_MoveAway (mobj_t* mo, fixed_t fx, fixed_t fy)
{
    angle_t	a = R_PointToAngle2 (fx, fy, mo->x, mo->y);	// from threat -> us
    wantmove = 1;
    int		base = (int)((a + (1u<<28)) >> 29) & 7;		// nearest of 8 dirs
    static const int off[5] = { 0, 1, 7, 2, 6 };		// straight, +-45, +-90
    int		i;
    for (i = 0; i < 5; i++)
    {
	mo->movedir = (base + off[i]) & 7;
	if (P_Move (mo)) return;
    }
    mo->movedir = 8;		// DI_NODIR (p_enemy.c) -- fully blocked, no move
}


void P_AICoop_BuildCmd (void)
{
    player_t*	bot;
    mobj_t*	mo;
    mobj_t*	mon;
    mobj_t*	pl;
    mobj_t*	item;
    ticcmd_t*	cmd;
    fixed_t	dist;
    fixed_t	ml;			// distance moved during the previous tic
    static int	healtic, fleeing, idletic, curspeed;
    static fixed_t lastx, lasty;

    if (!aicoop || !playeringame[1])
	return;

    bot = &players[1];
    cmd = &bot->cmd;

    // Dead: tap Use so co-op reborns the companion at its start.
    if (bot->playerstate == PST_DEAD)
    {
	memset (cmd, 0, sizeof(*cmd));
	cmd->buttons |= BT_USE;
	return;
    }
    if (bot->playerstate != PST_LIVE || !bot->mo)
	return;

    mo = bot->mo;
    memset (cmd, 0, sizeof(*cmd));

    // (6) auto-heal: +1 HP per second up to 100
    if (++healtic >= 35)
    {
	healtic = 0;
	if (bot->health < 100) { bot->health++; mo->health = bot->health; }
    }

    pl  = playeringame[0] ? players[0].mo : NULL;
    mon = AICoop_FindTarget (mo);

    // (7) flee hysteresis: enter below heal_hp, leave once back to defend_hp
    if (bot->health < coop_heal_hp)            fleeing = 1;
    else if (bot->health >= coop_defend_hp)    fleeing = 0;

    coop_state = 0;
    wantmove   = 0;

    // Acceleration ramp: measure how far we moved last tic; if we kept moving,
    // wind P_Move's step size up toward coop_speed, otherwise start slow.  This
    // gives a gradual run-up instead of snapping to full speed in one tic.
    ml = P_AproxDistance (mo->x - lastx, mo->y - lasty);
    lastx = mo->x; lasty = mo->y;
    if (ml > FRACUNIT) { curspeed += 3; if (curspeed > coop_speed) curspeed = coop_speed; }
    else                 curspeed = coop_speed / 3;
    if (curspeed < 4) curspeed = 4;
    mobjinfo[MT_PLAYER].speed = curspeed;

    // (2,5) yield: the human is right on top of us -> step aside (top priority)
    if (pl && P_AproxDistance (pl->x - mo->x, pl->y - mo->y) < YIELD_DIST)
    {
	AICoop_MoveAway (mo, pl->x, pl->y);
	coop_state = 5;
    }
    // "attack" ordered: charge the forced target until it (or the timer) dies
    else if (forceaggro > 0)
    {
	mobj_t* t = forcetarget;
	forceaggro--;
	if (!t || t->health <= 0 || (t->flags & MF_CORPSE) || !(t->flags & MF_SHOOTABLE))
	    t = forcetarget = AICoop_FindTarget (mo);	// target down -> reacquire
	if (t)
	{
	    coop_state = 2;
	    dist = P_AproxDistance (t->x - mo->x, t->y - mo->y);
	    mo->angle = R_PointToAngle2 (mo->x, mo->y, t->x, t->y);
	    if (P_CheckSight (mo, t))
		cmd->buttons |= BT_ATTACK;
	    if (dist > COOP_KEEP)
		AICoop_Chase (mo, t);			// always close in (forced)
	}
	else
	    forceaggro = 0;				// nothing left to attack
    }
    // "wait/stay" ordered: hold position; still face & fire at anything in sight
    else if (hold)
    {
	coop_state = 0;
	if (mon)
	{
	    coop_state = 2;
	    mo->angle = R_PointToAngle2 (mo->x, mo->y, mon->x, mon->y);
	    if (P_CheckSight (mo, mon))
		cmd->buttons |= BT_ATTACK;
	}
    }
    // (7) flee & hide: run from monsters, regroup on the player when clear
    else if (fleeing)
    {
	coop_state = 3;
	if (mon)
	{
	    AICoop_MoveAway (mo, mon->x, mon->y);
	    mo->angle = R_PointToAngle2 (mon->x, mon->y, mo->x, mo->y);
	}
	else if (pl)
	{
	    AICoop_Chase (mo, pl);
	    mo->angle = R_PointToAngle2 (mo->x, mo->y, pl->x, pl->y);
	}
    }
    // "come" ordered from the console: run to the player, ignoring fights/items
    else if (summon > 0 && pl)
    {
	summon--;
	AICoop_Chase (mo, pl);
	mo->angle = R_PointToAngle2 (mo->x, mo->y, pl->x, pl->y);
	coop_state = 1;
    }
    // fight: aim and fire; charge when healthy, kite when low
    else if (mon)
    {
	coop_state = 2;
	int defensive = (bot->health < coop_defend_hp);
	dist = P_AproxDistance (mon->x - mo->x, mon->y - mo->y);
	mo->angle = R_PointToAngle2 (mo->x, mo->y, mon->x, mon->y);
	if (P_CheckSight (mo, mon))
	    cmd->buttons |= BT_ATTACK;
	if (defensive)
	{
	    if (dist < COOP_KEEP) AICoop_MoveAway (mo, mon->x, mon->y);	// back off
	}
	else if (dist > COOP_KEEP)
	    AICoop_Chase (mo, mon);					// close in
    }
    // (4) no monster in sight: fetch nearby pickups, else follow the human
    else if ((item = AICoop_FindItem (mo)) != NULL)
    {
	AICoop_Chase (mo, item);
	mo->angle = R_PointToAngle2 (mo->x, mo->y, item->x, item->y);
	coop_state = 4;
    }
    else if (pl)
    {
	coop_state = 1;
	if (P_AproxDistance (pl->x - mo->x, pl->y - mo->y) > COOP_NEAR)
	{
	    AICoop_Chase (mo, pl);
	    mo->angle = R_PointToAngle2 (mo->x, mo->y, pl->x, pl->y);
	}
    }

    // Walk animation: P_Move drives the mobj directly (forwardmove stays 0), so
    // P_MovePlayer never starts the run frames -- do it here.  Only touch the
    // locomotion states so we don't stomp on attack/pain/death animations.
    {
	statenum_t st = (statenum_t)(mo->state - states);
	if (st == S_PLAY || (st >= S_PLAY_RUN1 && st <= S_PLAY_RUN4))
	{
	    if (wantmove)
	    {
		if (st == S_PLAY) P_SetMobjState (mo, S_PLAY_RUN1);
	    }
	    else if (st != S_PLAY)
		P_SetMobjState (mo, S_PLAY);
	}
    }

    // (3) Anti-stuck only: trying to move but no progress for ~3 s -> shuffle.
    // When there's nothing to do (no goal) we just stand calmly.
    if (wantmove && ml < 2*FRACUNIT) idletic++; else idletic = 0;
    if (idletic >= IDLE_TICS)
    {
	mo->movedir = (mo->movedir + 2) & 7;	// perpendicular step
	P_Move (mo);
	idletic = 0;
    }
}


// Console "where": a one-line spoken-style status answer.
const char* P_AICoop_Report (void)
{
    static char		buf[160];
    static const char*	dirs[8]  = { "east","north-east","north","north-west",
					"west","south-west","south","south-east" };
    static const char*	doing[6] = { "holding position","right behind you","fighting",
					"falling back (low HP)","grabbing supplies",
					"stepping out of your way" };
    player_t*	bot = &players[1];
    mobj_t*	me;
    mobj_t*	pl;
    int		dist = 0, di = 0, st;

    if (!aicoop || !playeringame[1] || bot->playerstate == PST_DEAD || !bot->mo)
    {
	snprintf (buf, sizeof(buf), "[Buddy] (no companion -- launch with -aicoop)");
	return buf;
    }
    me = bot->mo;
    pl = playeringame[0] ? players[0].mo : NULL;
    if (pl)
    {
	dist = P_AproxDistance (me->x - pl->x, me->y - pl->y) >> FRACBITS;
	di   = (int)((R_PointToAngle2 (pl->x, pl->y, me->x, me->y) + (1u<<28)) >> 29) & 7;
    }
    st = (coop_state >= 0 && coop_state <= 5) ? coop_state : 0;
    snprintf (buf, sizeof(buf), "[Buddy] %d units to your %s, %d HP -- %s.",
	      dist, dirs[di], bot->health, doing[st]);
    return buf;
}


// Console "come": order the companion to run to the player.  0 if none.
int P_AICoop_Summon (void)
{
    if (!aicoop || !playeringame[1] || players[1].playerstate == PST_DEAD || !players[1].mo)
	return 0;
    summon = 245;		// ~7 s of coming to the player
    hold = 0;
    return 1;
}

#define COOP_NONE	"[Buddy] (no companion -- launch with -aicoop)"
static int AICoop_Here (void)
{
    return aicoop && playeringame[1]
	&& players[1].playerstate != PST_DEAD && players[1].mo;
}

// Console "wait" / "stay": toggle holding position.
const char* P_AICoop_Wait (void)
{
    if (!AICoop_Here ()) return COOP_NONE;
    hold = !hold;
    forceaggro = 0; summon = 0;		// holding cancels other orders
    return hold ? "[Buddy] Holding position." : "[Buddy] Moving out.";
}

// Console "attack": charge the monster nearest the player.
const char* P_AICoop_Attack (void)
{
    mobj_t* pl;
    if (!AICoop_Here ()) return COOP_NONE;
    pl = playeringame[0] ? players[0].mo : players[1].mo;
    forcetarget = AICoop_NearestMonsterTo (pl->x, pl->y);
    if (!forcetarget) return "[Buddy] No targets around.";
    forceaggro = 350;			// ~10 s
    hold = 0;
    return "[Buddy] Attacking!";
}

// Console "report": ammo / status line.
const char* P_AICoop_StatusReport (void)
{
    static char		buf[160];
    static const char*	wn[NUMWEAPONS] = { "fists","pistol","shotgun","chaingun",
	"rocket launcher","plasma rifle","BFG9000","chainsaw","super shotgun" };
    player_t*	bot = &players[1];
    int		w, at;
    char	ammo[40];

    if (!AICoop_Here ()) return COOP_NONE;

    w = bot->readyweapon;
    switch (w)
    {
      case wp_pistol: case wp_chaingun:      at = am_clip;   break;
      case wp_shotgun: case wp_supershotgun: at = am_shell;  break;
      case wp_missile:                       at = am_misl;   break;
      case wp_plasma: case wp_bfg:           at = am_cell;   break;
      default:                               at = am_noammo; break;
    }
    if (at == am_noammo) snprintf (ammo, sizeof(ammo), "no ammo");
    else                 snprintf (ammo, sizeof(ammo), "%d rounds", bot->ammo[at]);

    if (w < 0 || w >= NUMWEAPONS) w = 0;
    snprintf (buf, sizeof(buf), "[Buddy] %d HP, %d%% armor, %s, %s.",
	      bot->health, bot->armorpoints, wn[w], ammo);
    return buf;
}
