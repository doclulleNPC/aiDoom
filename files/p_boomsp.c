// p_boomsp.c -- Boom scroller thinkers (wall/flat texture scroll + object carry).
// Ported from ../winmbf/Source/p_spec.c (killough).  Flat-texture visual scroll (sc_floor/
// sc_ceiling) tracks the sector offset here; the renderer hook to draw the scrolled flat is a
// remaining B4 item -- wall scroll (sc_side) and conveyor carry (sc_carry) are fully live.
#include "doomdef.h"
#include "doomstat.h"
#include "p_local.h"
#include "p_spec.h"
#include "r_state.h"
#include "z_zone.h"
#include "m_fixed.h"
#include "tables.h"
#define SCROLL_SHIFT 5
#define CARRYFACTOR ((3*FRACUNIT)>>5)
void Add_WallScroller(int64_t dx, int64_t dy, const line_t *l, int control, int accel);	// int64_t (NOT long: 32-bit on MSVC x86) must match the definition below
int P_FindLineFromLineTag(const line_t *line, int start);
#ifndef D_MININT
#define D_MININT MININT
#endif


void T_Scroll(scroll_t *s)
{
  fixed_t dx = s->dx, dy = s->dy;

  if (s->control != -1)
    {   // compute scroll amounts based on a sector's height changes
      fixed_t height = sectors[s->control].floorheight +
        sectors[s->control].ceilingheight;
      fixed_t delta = height - s->last_height;
      s->last_height = height;
      dx = FixedMul(dx, delta);
      dy = FixedMul(dy, delta);
    }

  // killough 3/14/98: Add acceleration
  if (s->accel)
    {
      s->vdx = dx += s->vdx;
      s->vdy = dy += s->vdy;
    }

  if (!(dx | dy))                   // no-op if both (x,y) offsets 0
    return;

  switch (s->type)
    {
      side_t *side;
      sector_t *sec;
      fixed_t height, waterheight;  // killough 4/4/98: add waterheight
          mobj_t *thing;

    case sc_side:                   // killough 3/7/98: Scroll wall texture
        side = sides + s->affectee;
        side->textureoffset += dx;
        side->rowoffset += dy;
        break;

    case sc_floor:                  // killough 3/7/98: Scroll floor texture
        sec = sectors + s->affectee;
        sec->floor_xoffs += dx;
        sec->floor_yoffs += dy;
        break;

    case sc_ceiling:               // killough 3/7/98: Scroll ceiling texture
        sec = sectors + s->affectee;
        sec->ceiling_xoffs += dx;
        sec->ceiling_yoffs += dy;
        break;

    case sc_carry:

      // killough 3/7/98: Carry things on floor
      // killough 3/20/98: use new sector list which reflects true members
      // killough 3/27/98: fix carrier bug
      // killough 4/4/98: Underwater, carry things even w/o gravity

      sec = sectors + s->affectee;
      height = sec->floorheight;
      waterheight = D_MININT;   // (aiDoom: no transfer-height water yet)

      // Move objects only if on floor or underwater,
      // non-floating, and clipped.

      for (thing = sec->thinglist; thing; thing = thing->snext)
        if (!(thing->flags & MF_NOCLIP) &&
            (!(thing->flags & MF_NOGRAVITY || thing->z > height) ||
             thing->z < waterheight))
	  thing->momx += dx, thing->momy += dy;
      break;

    case sc_carry_ceiling:       // to be added later
      break;
    }
}


static void Add_Scroller(int type, fixed_t dx, fixed_t dy,
                         int control, int affectee, int accel)
{
  scroll_t *s = Z_Malloc(sizeof *s, PU_LEVSPEC, 0);
  s->thinker.function.acp1 = (actionf_p1)T_Scroll;
  s->type = type;
  s->dx = dx;
  s->dy = dy;
  s->accel = accel;
  s->vdx = s->vdy = 0;
  if ((s->control = control) != -1)
    s->last_height =
      sectors[control].floorheight + sectors[control].ceilingheight;
  s->affectee = affectee;
  P_AddThinker(&s->thinker);
}

// Initialize the scrollers
void P_SpawnScrollers(void)
{
  int i;
  line_t *l = lines;

  for (i=0;i<numlines;i++,l++)
    {
      fixed_t dx = l->dx >> SCROLL_SHIFT;  // direction and speed of scrolling
      fixed_t dy = l->dy >> SCROLL_SHIFT;
      int control = -1, accel = 0;         // no control sector or acceleration
      int special = l->special;

      // killough 3/7/98: Types 245-249 are same as 250-254 except that the
      // first side's sector's heights cause scrolling when they change, and
      // this linedef controls the direction and speed of the scrolling. The
      // most complicated linedef since donuts, but powerful :)
      //
      // killough 3/15/98: Add acceleration. Types 214-218 are the same but
      // are accelerative.

      if (special >= 245 && special <= 249)         // displacement scrollers
        {
          special += 250-245;
          control = sides[*l->sidenum].sector - sectors;
        }
      else
        if (special >= 214 && special <= 218)       // accelerative scrollers
          {
            accel = 1;
            special += 250-214;
            control = sides[*l->sidenum].sector - sectors;
          }

      switch (special)
        {
          register int s;

        case 250:   // scroll effect ceiling
          for (s=-1; (s = P_FindSectorFromLineTag(l,s)) >= 0;)
            Add_Scroller(sc_ceiling, -dx, dy, control, s, accel);
          break;

        case 251:   // scroll effect floor
        case 253:   // scroll and carry objects on floor
          for (s=-1; (s = P_FindSectorFromLineTag(l,s)) >= 0;)
            Add_Scroller(sc_floor, -dx, dy, control, s, accel);
          if (special != 253)
            break;

        case 252: // carry objects on floor
          dx = FixedMul(dx,CARRYFACTOR);
          dy = FixedMul(dy,CARRYFACTOR);
          for (s=-1; (s = P_FindSectorFromLineTag(l,s)) >= 0;)
            Add_Scroller(sc_carry, dx, dy, control, s, accel);
          break;

          // killough 3/1/98: scroll wall according to linedef
          // (same direction and speed as scrolling floors)
        case 254:
          for (s=-1; (s = P_FindLineFromLineTag(l,s)) >= 0;)
            if (s != i)
              Add_WallScroller(dx, dy, lines+s, control, accel);
          break;

        case 255:    // killough 3/2/98: scroll according to sidedef offsets
          s = lines[i].sidenum[0];
          Add_Scroller(sc_side, -sides[s].textureoffset,
                       sides[s].rowoffset, -1, s, accel);
          break;

        case 48:                  // scroll first side
          Add_Scroller(sc_side,  FRACUNIT, 0, -1, lines[i].sidenum[0], accel);
          break;

        case 85:                  // jff 1/30/98 2-way scroll
          Add_Scroller(sc_side, -FRACUNIT, 0, -1, lines[i].sidenum[0], accel);
          break;
        }
    }
}


void Add_WallScroller(int64_t dx, int64_t dy, const line_t *l,
                             int control, int accel)
{
  fixed_t x = abs(l->dx), y = abs(l->dy), d;
  if (y > x)
    d = x, x = y, y = d;
  d = FixedDiv(x, finesine[(tantoangle[FixedDiv(y,x) >> DBITS] + ANG90)
                          >> ANGLETOFINESHIFT]);

  x = (fixed_t)((dy * -l->dy - dx * l->dx) / d);  // killough 10/98:
  y = (fixed_t)((dy * l->dx - dx * l->dy) / d);   // Use long long arithmetic
  Add_Scroller(sc_side, x, y, control, *l->sidenum, accel);
}


int P_FindLineFromLineTag (const line_t *line, int start)
{
  int i;
  for (i = start + 1; i < numlines; i++)
    if (lines[i].tag == line->tag)
      return i;
  return -1;
}
