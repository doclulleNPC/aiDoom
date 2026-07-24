// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.  (BuddyDoom fork additions)
//
// DESCRIPTION:
//	In-game key-bindings screen.  Reached from Options -> Controls; rebinds
//	the same key_* globals the config file and the buddydoom_config tool use,
//	and persists them straight to buddydoom.cfg (M_SaveDefaults).  Rendered as
//	an SDL overlay with the baked TrueType font atlas (see i_video.c
//	I_DrawControlsOverlay) rather than through the 8-bit paletted V_DrawPatch
//	path, so the text is crisp at any internal resolution.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "doomdef.h"
#include "d_event.h"
#include "m_controls.h"

// From m_misc.c / m_menu.c: persist to buddydoom.cfg, reset one value to its
// compiled-in default, and the menu-active flag.
extern void	M_SaveDefaults (void);
extern void	M_ResetDefault (int* location);
extern boolean	menuactive;

// The rebindable key globals (g_game.c).  We store the game's internal KEY_*
// codes (already translated from SDL by xlatekey in i_video.c).
extern int	key_right, key_left, key_up, key_down;
extern int	key_strafeleft, key_straferight, key_fire, key_use, key_strafe, key_speed;
extern int	key_nextweapon, key_prevweapon, key_jump, key_turret, key_console;
extern int	key_buddy_come, key_buddy_attack, key_buddy_stay, key_buddy_mode, key_spy;
extern int	key_inv_left, key_inv_right, key_inv_use, key_inv_drop;

typedef struct { const char* label; int* key; } binding_t;

static binding_t bindings[] =
{
    { "Move forward",   &key_up          },
    { "Move back",      &key_down         },
    { "Turn left",      &key_left         },
    { "Turn right",     &key_right        },
    { "Strafe left",    &key_strafeleft   },
    { "Strafe right",   &key_straferight  },
    { "Fire",           &key_fire         },
    { "Use / open",     &key_use          },
    { "Strafe (hold)",  &key_strafe       },
    { "Run",            &key_speed        },
    { "Jump",           &key_jump         },
    { "Next weapon",    &key_nextweapon   },
    { "Prev weapon",    &key_prevweapon   },
    { "Deploy turret",  &key_turret       },
    { "Console",        &key_console      },
    { "Buddy: come",    &key_buddy_come   },
    { "Buddy: attack",  &key_buddy_attack },
    { "Buddy: stay",    &key_buddy_stay   },
    { "Buddy: mode",    &key_buddy_mode   },
    { "Buddy: view",    &key_spy          },
    { "Inventory prev", &key_inv_left     },
    { "Inventory next", &key_inv_right    },
    { "Inventory use",  &key_inv_use      },
    { "Inventory drop", &key_inv_drop     },
};
#define NBIND	((int)(sizeof(bindings) / sizeof(bindings[0])))

static boolean	active;
static int	sel;
static boolean	capturing;

void M_Controls_Open (void)
{
    active    = true;
    capturing = false;
    sel       = 0;
    // menuactive stays true (we were launched from the Options menu): the mouse is
    // released and the classic menu drawer is suppressed while M_Controls_Active()
    // (see M_Drawer); Esc just drops back to the Options menu.
}

boolean M_Controls_Active (void)    { return active; }
int     M_Controls_Count (void)     { return NBIND; }
int     M_Controls_Sel (void)       { return sel; }
boolean M_Controls_Capturing (void) { return capturing; }

const char* M_Controls_Label (int i)
{
    return (i >= 0 && i < NBIND) ? bindings[i].label : "";
}

// Game KEY_* code -> short display label (port of buddydoom_config's keyname()).
void M_Controls_KeyName (int i, char* out, int n)
{
    int k = (i >= 0 && i < NBIND) ? *bindings[i].key : 0;
    if (!n) return;
    if (k <= 0) { out[0] = 0; return; }		// unbound -> blank
    switch (k)
    {
      case KEY_LEFTARROW:  snprintf (out, n, "Left");       return;
      case KEY_RIGHTARROW: snprintf (out, n, "Right");      return;
      case KEY_UPARROW:    snprintf (out, n, "Up");         return;
      case KEY_DOWNARROW:  snprintf (out, n, "Down");       return;
      case KEY_RCTRL:      snprintf (out, n, "Ctrl");       return;
      case KEY_RSHIFT:     snprintf (out, n, "Shift");      return;
      case KEY_RALT:       snprintf (out, n, "Alt");        return;
      case KEY_MWHEELUP:   snprintf (out, n, "Wheel Up");   return;
      case KEY_MWHEELDOWN: snprintf (out, n, "Wheel Down"); return;
      case KEY_MOUSE1:     snprintf (out, n, "Mouse L");    return;
      case KEY_MOUSE2:     snprintf (out, n, "Mouse R");    return;
      case KEY_MOUSE3:     snprintf (out, n, "Mouse M");    return;
      case ' ':            snprintf (out, n, "Space");      return;
      case KEY_ENTER:      snprintf (out, n, "Enter");      return;
      case KEY_TAB:        snprintf (out, n, "Tab");        return;
      case KEY_BACKSPACE:  snprintf (out, n, "Bksp");       return;
      case KEY_ESCAPE:     snprintf (out, n, "Esc");        return;
    }
    if (k >= KEY_F1 && k <= KEY_F1 + 9) { snprintf (out, n, "F%d", k - KEY_F1 + 1); return; }
    if (k == KEY_F11) { snprintf (out, n, "F11"); return; }
    if (k == KEY_F12) { snprintf (out, n, "F12"); return; }
    if (k > 32 && k < 127) snprintf (out, n, "%c", toupper (k));
    else                   snprintf (out, n, "key %d", k);
}

boolean M_Controls_Responder (event_t* ev)
{
    if (!active)
	return false;
    if (ev->type != ev_keydown)
	return true;			// modal: swallow everything else too

    int k = ev->data1;

    if (capturing)
    {
	if (k == KEY_ESCAPE)		// cancel, keep the old binding
	    capturing = false;
	else				// bind it (Esc excepted) and save
	{
	    *bindings[sel].key = k;
	    capturing = false;
	    M_SaveDefaults ();
	}
	return true;
    }

    // NBIND action rows + one virtual "Reset all to defaults" row (index NBIND).
    {
	int total = NBIND + 1;
	switch (k)
	{
	  case KEY_UPARROW:   sel = (sel - 1 + total) % total; break;
	  case KEY_DOWNARROW: sel = (sel + 1)         % total; break;
	  case KEY_ENTER:
	    if (sel == NBIND)						// reset every binding
	    {
		int i;
		for (i = 0; i < NBIND; i++)
		    M_ResetDefault (bindings[i].key);
		M_SaveDefaults ();
	    }
	    else
		capturing = true;					// rebind the selected action
	    break;
	  case KEY_BACKSPACE:						// clear the binding
	    if (sel < NBIND) { *bindings[sel].key = 0; M_SaveDefaults (); }
	    break;
	  case KEY_ESCAPE:						// back to the Options menu
	    active = false;
	    break;
	  default: break;
	}
    }
    return true;
}
