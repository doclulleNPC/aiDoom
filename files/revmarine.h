// (G) Revived friendly marines -- see files/revmarine.c.
#ifndef __REVMARINE_H__
#define __REVMARINE_H__

#include "d_player.h"		// player_t

void		RevMarine_Init (void);			// fill MT_REVMARINE states/mobjinfo (D_DoomMain)
const char*	P_ReviveMarineNear (player_t* presser);	// USE near a dead marine -> friendly ally; NULL if none or not in buddy mode

#endif
