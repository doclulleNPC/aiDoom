// (H) Heretic artifact inventory -- see files/p_inv_heretic.c.
//
// Eight Heretic artifacts share the generic player_t.inventory[] list (the same
// array the DOOM "overflow" inventory in p_invent.c uses), but their EFFECTS and
// on-floor pickup actors live here.  Effect values are ported from crispy-doom's
// heretic/{p_user.c,p_inter.c,info.c}.  Wings of Wrath and the Morph Ovum are
// deferred (they need flight / chicken subsystems we don't have yet).
#ifndef __P_INV_HERETIC_H__
#define __P_INV_HERETIC_H__

#include "d_player.h"		// player_t, artitype_t
#include "p_mobj.h"		// mobj_t

// Fill the appended Heretic pickup states/mobjinfo (call once at startup, next to
// RevMarine_Init in d_main.c).  Safe with or without hereticstuff.wad.
void		HereticInv_Init (void);

// True once hereticstuff.wad's artifact sprites are loaded (Hexen_Available-style).
// The EFFECTS work without it (console-give); only the on-floor icon needs the wad.
int		HereticInv_Available (void);

// Apply the effect of Heretic artifact `a` (a >= h_arti_flask).  Returns true if
// the artifact was consumed (and sets player->message); false if it refused
// (e.g. P_GiveBody failed at full health).  Called from p_invent.c's dispatcher.
boolean		ApplyHereticArtifact (player_t* player, artitype_t a);

// A placed/spawned MT_HARTI_* artifact was touched: pocket it into inventory[].
// Returns true if handled (so P_TouchSpecialThing can stop), false otherwise.
boolean		P_TouchHereticArtifact (player_t* player, mobj_t* special);

#endif
