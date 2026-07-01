// MBF / MBF21 action codepointers -- ported from ../winmbf and ../Nugget-Doom, adapted to aiDoom.
// M4a: the misc1/misc2 classics + MBF21 A_SpawnObject are real; the rest are stubs (M4b).
#include "doomdef.h"
#include "doomstat.h"
#include "p_local.h"
#include "s_sound.h"
#include "m_random.h"
#include "tables.h"
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
void A_MonsterProjectile (mobj_t *a) { (void)a; }
void A_MonsterMeleeAttack (mobj_t *a){ (void)a; }
void A_RadiusDamage (mobj_t *a)      { (void)a; }
void A_NoiseAlert (mobj_t *a)        { (void)a; }
void A_HealChase (mobj_t *a)         { (void)a; }
void A_SeekTracer (mobj_t *a)        { (void)a; }
void A_FindTracer (mobj_t *a)        { (void)a; }
void A_ClearTracer (mobj_t *a)       { (void)a; }
void A_AddFlags (mobj_t *a)          { (void)a; }
void A_RemoveFlags (mobj_t *a)       { (void)a; }
void A_JumpIfFlagsSet (mobj_t *a)    { (void)a; }
void A_JumpIfHealthBelow (mobj_t *a) { (void)a; }
void A_JumpIfTargetInSight (mobj_t *a){ (void)a; }
void A_JumpIfTargetCloser (mobj_t *a){ (void)a; }
void A_JumpIfTracerInSight (mobj_t *a){ (void)a; }
void A_JumpIfTracerCloser (mobj_t *a){ (void)a; }
void A_Mushroom (mobj_t *a)          { (void)a; }
void A_LineEffect (mobj_t *a)        { (void)a; }
