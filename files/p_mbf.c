// MBF / MBF21 action codepointers -- ported from ../winmbf and ../Nugget-Doom, adapted to aiDoom.
// M4a: the misc1/misc2 classics + MBF21 A_SpawnObject are real; the rest are stubs (M4b).
#include "doomdef.h"
#include "doomstat.h"
#include "p_local.h"
#include "s_sound.h"
#include "m_random.h"
#include "tables.h"
#include "r_main.h"   // R_PointToAngle2
#include "info.h"

void A_FaceTarget (mobj_t* actor);   // p_enemy.c

// ---- classic MBF (misc1/misc2) ----
void A_RandomJump (mobj_t *mo)
{
  if (P_Random() < mo->state->misc2)
    P_SetMobjState (mo, mo->state->misc1);
}
void A_PlaySound (mobj_t *mo)
{
  S_StartSound (mo->state->misc2 ? NULL : mo, mo->state->misc1);
}
void A_Spawn (mobj_t *mo)
{
  if (mo->state->misc1)
  {
    mobj_t *n = P_SpawnMobj (mo->x, mo->y, (mo->state->misc2 << FRACBITS) + mo->z,
                             mo->state->misc1 - 1);
    if (n) n->flags = (n->flags & ~MF_FRIEND) | (mo->flags & MF_FRIEND);
  }
}
void A_Turn (mobj_t *mo) { mo->angle += (angle_t)(((uint64_t) mo->state->misc1 << 32) / 360); }
void A_Face (mobj_t *mo) { mo->angle  = (angle_t)(((uint64_t) mo->state->misc1 << 32) / 360); }
void A_Detonate (mobj_t *mo) { P_RadiusAttack (mo, mo->target, mo->info->damage); }
void A_Die (mobj_t *mo) { P_DamageMobj (mo, NULL, NULL, mo->health); }
void A_Scratch (mobj_t *mo)
{
  if (!mo->target) return;
  A_FaceTarget (mo);
  if (P_CheckMeleeRange (mo))
  {
    if (mo->state->misc2) S_StartSound (mo, mo->state->misc2);
    P_DamageMobj (mo->target, mo, mo, mo->state->misc1);
  }
}

// ---- MBF21 monster: A_SpawnObject (args-based) ----
void A_SpawnObject (mobj_t *actor)
{
  int type, angle, ox, oy, oz, vx, vy, vz, fan, dx, dy;
  angle_t an;
  mobj_t *mo;
  if (!actor->state->args[0]) return;
  type  = (int)actor->state->args[0] - 1;
  angle = (int)actor->state->args[1];
  ox = (int)actor->state->args[2]; oy = (int)actor->state->args[3]; oz = (int)actor->state->args[4];
  vx = (int)actor->state->args[5]; vy = (int)actor->state->args[6]; vz = (int)actor->state->args[7];
  if (type < 0 || type >= num_mobjtypes) return;
  an = actor->angle + (angle_t)(((int64_t)angle << 16) / 360);
  fan = an >> ANGLETOFINESHIFT;
  dx = FixedMul (ox, finecosine[fan]) - FixedMul (oy, finesine[fan]);
  dy = FixedMul (ox, finesine[fan])   + FixedMul (oy, finecosine[fan]);
  mo = P_SpawnMobj (actor->x + dx, actor->y + dy, actor->z + oz, type);
  if (!mo) return;
  mo->angle = an;
  mo->momx = FixedMul (vx, finecosine[fan]) - FixedMul (vy, finesine[fan]);
  mo->momy = FixedMul (vx, finesine[fan])   + FixedMul (vy, finecosine[fan]);
  mo->momz = vz;
  if (mo->info->flags & MF_MISSILE)
  {
    if (actor->info->flags & MF_MISSILE) { mo->target = actor->target; mo->tracer = actor->tracer; }
    else                                 { mo->target = actor;         mo->tracer = actor->target; }
  }
  mo->flags = (mo->flags & ~MF_FRIEND) | (actor->flags & MF_FRIEND);
}

// ---- MBF21: remaining monster codepointers (M4b) ----
void A_MonsterProjectile (mobj_t *actor)
{
  int type, angle, an;
  mobj_t *mo;
  if (!actor->target || !actor->state->args[0]) return;
  type = (int)actor->state->args[0] - 1;
  if (type < 0 || type >= num_mobjtypes) return;
  angle = (int)actor->state->args[1];
  A_FaceTarget (actor);
  mo = P_SpawnMissile (actor, actor->target, type);
  if (!mo) return;
  mo->angle += (angle_t)(((int64_t)angle << 16) / 360);
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul (mo->info->speed, finecosine[an]);
  mo->momy = FixedMul (mo->info->speed, finesine[an]);
}
void A_MonsterMeleeAttack (mobj_t *actor)
{
  int dbase, dmod, hitsound, damage;
  if (!actor->target) return;
  dbase    = (int)actor->state->args[0]; if (dbase <= 0) dbase = 3;
  dmod     = (int)actor->state->args[1]; if (dmod  <= 0) dmod  = 8;
  hitsound = (int)actor->state->args[2];
  A_FaceTarget (actor);
  if (!P_CheckMeleeRange (actor)) return;
  if (hitsound) S_StartSound (actor, hitsound);
  damage = (P_Random() % dmod + 1) * dbase;
  P_DamageMobj (actor->target, actor, actor, damage);
}
// MBF21: monster hitscan attack -- args: hspread, vspread, numbullets, damagebase,
// damagemod.  Mirrors A_WeaponBulletAttack's spread so weapon + monster bullets match.
void A_MonsterBulletAttack (mobj_t *actor)
{
  extern fixed_t P_AimLineAttack (mobj_t*, angle_t, fixed_t);
  int hspread, vspread, numbullets, dbase, dmod, i, damage, angle, slope, aimslope;
  if (!actor->target || !actor->state) return;
  hspread    = (int)actor->state->args[0];
  vspread    = (int)actor->state->args[1];
  numbullets = (int)actor->state->args[2];
  dbase      = (int)actor->state->args[3];
  dmod       = (int)actor->state->args[4];
  if (numbullets <= 0) numbullets = 1;
  if (dbase <= 0) dbase = 3;
  if (dmod  <= 0) dmod  = 5;
  A_FaceTarget (actor);
  if (actor->info->attacksound) S_StartSound (actor, actor->info->attacksound);
  aimslope = P_AimLineAttack (actor, actor->angle, MISSILERANGE);
  for (i = 0; i < numbullets; i++)
  {
    damage = (P_Random() % dmod + 1) * dbase;
    angle  = (int)actor->angle + ((P_Random() - P_Random()) * (hspread << 8) / 255);
    slope  = aimslope + (P_Random() - P_Random()) * (vspread << 8) / 255;
    P_LineAttack (actor, (angle_t)angle, MISSILERANGE, slope, damage);
  }
}
// MBF21: halt the actor.
void A_Stop (mobj_t *actor) { actor->momx = actor->momy = actor->momz = 0; }
// MBF: the DOOM-beta lost soul's melee-ish burst (damage = random(1..8) * info->damage).
void A_BetaSkullAttack (mobj_t *actor)
{
  int damage;
  if (!actor->target || actor->target->type == MT_SKULL) return;
  if (actor->info->attacksound) S_StartSound (actor, actor->info->attacksound);
  A_FaceTarget (actor);
  damage = (P_Random() % 8 + 1) * actor->info->damage;
  P_DamageMobj (actor->target, actor, actor, damage);
}
void A_RadiusDamage (mobj_t *actor)
{
  if (actor->state) P_RadiusAttack (actor, actor->target, (int)actor->state->args[0]);
}
void A_NoiseAlert (mobj_t *a) { if (a->target) P_NoiseAlert (a->target, a); }
// A_HealChase (MBF21) lives in p_enemy.c beside A_VileChase -- it needs the vile
// corpse-raise machinery (PIT_VileCheck / corpsehit / viletryx).
void A_SeekTracer (mobj_t *actor)
{
  angle_t exact, maxturn;
  fixed_t dist, slope;
  mobj_t *dest = actor->tracer;
  int fa, deg;
  if (!dest || dest->health <= 0) return;
  deg = (int)(actor->state->args[1] >> 16);          // args[1]: max turn/tic, fixed degrees
  if (deg <= 0) deg = 10;
  maxturn = (angle_t)(deg * (ANG45 / 45));
  exact = R_PointToAngle2 (actor->x, actor->y, dest->x, dest->y);
  if (exact != actor->angle)
  {
    if (exact - actor->angle > 0x80000000)
    { actor->angle -= maxturn; if (exact - actor->angle < 0x80000000) actor->angle = exact; }
    else
    { actor->angle += maxturn; if (exact - actor->angle > 0x80000000) actor->angle = exact; }
  }
  fa = actor->angle >> ANGLETOFINESHIFT;
  actor->momx = FixedMul (actor->info->speed, finecosine[fa]);
  actor->momy = FixedMul (actor->info->speed, finesine[fa]);
  dist = P_AproxDistance (dest->x - actor->x, dest->y - actor->y) / (actor->info->speed ? actor->info->speed : FRACUNIT);
  if (dist < 1) dist = 1;
  slope = (dest->z + 40*FRACUNIT - actor->z) / dist;
  if (slope < actor->momz) actor->momz -= FRACUNIT/8; else actor->momz += FRACUNIT/8;
}
void A_FindTracer (mobj_t *actor)
{ if (!actor->tracer && actor->target) actor->tracer = actor->target; }
void A_ClearTracer (mobj_t *a) { a->tracer = NULL; }
void A_AddFlags (mobj_t *a)
{
  int nf = (int)a->state->args[0];
  boolean relink = (nf & (MF_NOBLOCKMAP|MF_NOSECTOR)) != 0;
  if (relink) P_UnsetThingPosition (a);
  a->flags  |= nf;
  a->flags2 |= (int)a->state->args[1];
  if (relink) P_SetThingPosition (a);
}
void A_RemoveFlags (mobj_t *a)
{
  int rf = (int)a->state->args[0];
  boolean relink = (rf & (MF_NOBLOCKMAP|MF_NOSECTOR)) != 0;
  if (relink) P_UnsetThingPosition (a);
  a->flags  &= ~rf;
  a->flags2 &= ~(int)a->state->args[1];
  if (relink) P_SetThingPosition (a);
}
void A_JumpIfFlagsSet (mobj_t *a)
{
  if ((a->flags  & (int)a->state->args[1]) == (int)a->state->args[1]
   && (a->flags2 & (int)a->state->args[2]) == (int)a->state->args[2])
    P_SetMobjState (a, (int)a->state->args[0]);
}
void A_JumpIfHealthBelow (mobj_t *a)
{ if (a->health < (int)a->state->args[1]) P_SetMobjState (a, (int)a->state->args[0]); }
void A_JumpIfTargetInSight (mobj_t *a)
{ if (a->target && P_CheckSight (a, a->target)) P_SetMobjState (a, (int)a->state->args[0]); }
void A_JumpIfTargetCloser (mobj_t *a)
{ if (a->target && (int)a->state->args[1] > P_AproxDistance (a->x - a->target->x, a->y - a->target->y))
    P_SetMobjState (a, (int)a->state->args[0]); }
void A_JumpIfTracerInSight (mobj_t *a)
{ if (a->tracer && P_CheckSight (a, a->tracer)) P_SetMobjState (a, (int)a->state->args[0]); }
void A_JumpIfTracerCloser (mobj_t *a)
{ if (a->tracer && (int)a->state->args[1] > P_AproxDistance (a->x - a->tracer->x, a->y - a->tracer->y))
    P_SetMobjState (a, (int)a->state->args[0]); }
void A_Mushroom (mobj_t *actor)   // classic MBF: normal explosion + a cloud of falling fireballs
{
  extern void A_Explode (mobj_t*);
  int i, j, n = actor->info->damage;
  fixed_t m1 = actor->state->misc1 ? actor->state->misc1 : FRACUNIT*4;
  fixed_t m2 = actor->state->misc2 ? actor->state->misc2 : FRACUNIT/2;
  A_Explode (actor);
  for (i = -n; i <= n; i += 8)
    for (j = -n; j <= n; j += 8)
    {
      mobj_t target = *actor, *mo;
      target.x += i << FRACBITS;
      target.y += j << FRACBITS;
      target.z += P_AproxDistance (i, j) * m1;
      mo = P_SpawnMissile (actor, &target, MT_FATSHOT);
      if (!mo) continue;
      mo->momx = FixedMul (mo->momx, m2);
      mo->momy = FixedMul (mo->momy, m2);
      mo->momz = FixedMul (mo->momz, m2);
      mo->flags &= ~MF_NOGRAVITY;
    }
}
// A_LineEffect (MBF, killough 11/98): the thing activates a linedef special on itself.
// misc1 = linedef type, misc2 = sector tag.  Tries a USE (switch) trigger first, then a
// CROSS (walk) trigger, exactly like a fake player stepping on/pressing a tagged line.
// Fires at most once per thing (MIF_LINEDONE), so a looping state can't spam it.
void A_LineEffect (mobj_t *mo)
{
    extern boolean P_UseSpecialLine (mobj_t* thing, line_t* line, int side);
    extern void    P_CrossSpecialLine (int linenum, int side, mobj_t* thing);
    static player_t junkplayer;			// fake, "alive" player for the trigger

    if (!mo || !mo->state || numlines <= 0) return;
    if (mo->intflags & MIF_LINEDONE)	return;	// already used up for this thing

    line_t junk = lines[0];			// a throwaway linedef based off the first
    junk.special = (short) mo->state->misc1;	// the linedef type to run
    if (junk.special)
    {
	player_t* oldplayer = mo->player;
	mo->player = &junkplayer;
	junkplayer.health = 100;
	junk.tag = (short) mo->state->misc2;	// target sector tag

	if (!P_UseSpecialLine (mo, &junk, 0))	// try USE (switch-type)...
	{					// ...else CROSS (walk-type): run it via lines[0]
	    line_t save = lines[0];
	    lines[0] = junk;
	    P_CrossSpecialLine (0, 0, mo);
	    junk.special = lines[0].special;	// pick up a one-shot special getting cleared
	    lines[0] = save;
	}
	if (!junk.special)			// non-repeatable special consumed
	    mo->intflags |= MIF_LINEDONE;
	mo->player = oldplayer;
    }
}
