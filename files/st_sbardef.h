// ID24 SBARDEF status-bar definition -- see files/st_sbardef.c.
#ifndef __ST_SBARDEF__
#define __ST_SBARDEF__

#include "doomtype.h"

void	ST_SBARDEF_Init (void);			// parse the SBARDEF lump if present
int	ST_SBARDEF_Active (void);		// true if loaded AND enabled (-sbardef)
void	ST_SBARDEF_Draw (boolean fullscreen);	// draw the data-driven status bar

#endif
