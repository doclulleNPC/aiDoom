// MBF / MBF21 action codepointers.  M2: stubs so DeHackEd patches that reference them link and
// load; the real implementations (ported from ../winmbf/Source/p_enemy.c) land in M4.
#include "doomdef.h"
#include "p_local.h"

void A_Detonate   (mobj_t* mo) { (void)mo; }   // mbf: radius damage from Damage field
void A_Die        (mobj_t* mo) { (void)mo; }   // mbf: unconditional death
void A_Face       (mobj_t* mo) { (void)mo; }   // mbf: turn by Args1 BAM
void A_LineEffect (mobj_t* mo) { (void)mo; }   // mbf: trigger a linedef special
void A_Mushroom   (mobj_t* mo) { (void)mo; }   // mbf: mushroom cloud of fireballs
void A_PlaySound  (mobj_t* mo) { (void)mo; }   // mbf: play sound Args1
void A_RandomJump (mobj_t* mo) { (void)mo; }   // mbf: jump to state Args1 with prob Args2
void A_Scratch    (mobj_t* mo) { (void)mo; }   // mbf: melee Args1 damage
void A_Spawn      (mobj_t* mo) { (void)mo; }   // mbf: spawn thing Args1
void A_Turn       (mobj_t* mo) { (void)mo; }   // mbf: rotate angle by Args1 BAM
