// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// DESCRIPTION:
//	DOOM graphics stuff for the SDL3 library.
//
//	The software renderer produces an 8-bit palettized SCREENWIDTH x
//	SCREENHEIGHT framebuffer in screens[0].  SDL3 no longer exposes
//	palettized display surfaces, so each frame we expand that buffer through
//	a 32-bit palette into a streaming texture and let the renderer scale it
//	to the window (SDL_SetRenderLogicalPresentation handles the scaling).
//
//	Ported from the SDL 1.2 backend; see ../sdldoom-sdl3 for the reference.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "m_swap.h"
#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"

#include "doomdef.h"

#include "buddydoom_icon.h"
#include "buddydoom_version.h"		// BUDDYDOOM_VERSION (auto-bumped by build.sh)
#include "c_console.h"			// console overlay state (C_Active / C_GetLine)
#include "m_controls.h"			// in-game key-bindings screen (overlay drawn here)
#include "m_menu.h"			// M_Video_* -- the Video settings overlay (drawn here)
#include "../tools/font_atlas.h"		// baked DejaVuSansMono atlas (TTF console font)


static SDL_Window*	window = NULL;
static SDL_Renderer*	renderer = NULL;
static SDL_Texture*	texture = NULL;

// Expanded 32-bit (ARGB8888) palette, rebuilt by I_SetPalette.
static Uint32		palette[256];

// Video options (Options -> Video; persisted in config).
int			scale_mode = 0;       // 0 = Nearest, 1 = Linear
int			vsync = 1;            // 0 = Off, 1 = On (default On)
int			integer_scale = 0;    // 0 = Letterbox, 1 = Integer Scale
int			render_backend = 0;   // 0 = Auto; 1..N = the Nth VERIFIED render driver

// Options -> Video -> Backend.  SDL_GetRenderDriver() lists the drivers SDL was
// *built* with, but selecting one doesn't guarantee it actually creates a renderer on
// this machine (e.g. "vulkan"/"direct3d12" with no working ICD).  So at startup we
// PROBE each driver on a hidden window and keep only the ones that really create a
// renderer -- that verified list is what the menu offers and what the real renderer is
// built from, so a listed backend is always one that works here.  Index 0 = "Auto".
#define MAX_BACKENDS	16
static const char*	ok_backends[MAX_BACKENDS];
static int		ok_backend_count = -1;	// -1 = not probed yet

static void I_ProbeBackends (void)
{
    int i, n;
    if (ok_backend_count >= 0)
	return;					// probe once
    ok_backend_count = 0;
    n = SDL_GetNumRenderDrivers ();
    for (i = 0; i < n && ok_backend_count < MAX_BACKENDS; i++)
    {
	const char*	d = SDL_GetRenderDriver (i);
	SDL_Window*	w = SDL_CreateWindow ("probe", 64, 64, SDL_WINDOW_HIDDEN);
	if (w)
	{
	    SDL_Renderer* r = SDL_CreateRenderer (w, d);
	    if (r) { ok_backends[ok_backend_count++] = d; SDL_DestroyRenderer (r); }
	    SDL_DestroyWindow (w);
	}
    }
}

int I_RenderBackendCount (void)			// verified drivers + the Auto entry
{
    I_ProbeBackends ();
    return ok_backend_count + 1;
}
const char* I_RenderBackendName (int i)
{
    if (i <= 0) return "Auto";
    I_ProbeBackends ();
    return (i - 1 < ok_backend_count) ? ok_backends[i - 1] : "Auto";
}

// Gamepad integration
float gamepad_left_x = 0.0f;
float gamepad_left_y = 0.0f;
float gamepad_right_x = 0.0f;
float gamepad_right_y = 0.0f;
static SDL_Gamepad* gamepad = NULL;

extern int mousex, mousey;
extern int key_use;
extern int key_fire;
extern int key_jump;
extern int key_strafeleft;
extern int key_straferight;

// Re-apply the texture scale mode.
void I_ApplyVideoFilter (void)
{
    if (texture)
	SDL_SetTextureScaleMode (texture,
	    scale_mode ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
}

// Re-apply logical presentation.
void I_ApplyLogicalPresentation (void)
{
    if (renderer)
    {
	SDL_SetRenderLogicalPresentation(renderer, SCREENWIDTH, I_OutHeight(),
	    integer_scale ? SDL_LOGICAL_PRESENTATION_INTEGER_SCALE : SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }
}

// Re-apply VSync.
void I_ApplyVSync (void)
{
    if (renderer)
	SDL_SetRenderVSync(renderer, vsync);
}

// Non-zero when displaying fullscreen.  Saved/restored via the config file.
int			fullscreen_mode = 0;

// Grab (relative-motion) the mouse while actually playing; default on.
boolean			grabMouse = true;

static int		window_focused = 1;

// Grab the mouse only while playing -- release it when the menu is up (so the
// OS cursor is usable) or we lose focus.  SDL3 relative mode also recenters the
// cursor internally, which is what makes turning work without the old warp hack.
static void I_ApplyMouseGrab (void)
{
    static int	applied = -1;
    int		want = grabMouse && window_focused && !menuactive;

    if (window && want != applied)
    {
	SDL_SetWindowRelativeMouseMode (window, want ? true : false);
	applied = want;
    }
}


//
//  Translates the SDL3 keycode to a DOOM key.
//
int xlatekey(SDL_Keycode sym)
{
    int rc;

    switch(sym)
    {
      case SDLK_LEFT:	rc = KEY_LEFTARROW;	break;
      case SDLK_RIGHT:	rc = KEY_RIGHTARROW;	break;
      case SDLK_DOWN:	rc = KEY_DOWNARROW;	break;
      case SDLK_UP:	rc = KEY_UPARROW;	break;
      case SDLK_ESCAPE:	rc = KEY_ESCAPE;	break;
      case SDLK_RETURN:	rc = KEY_ENTER;		break;
      case SDLK_TAB:	rc = KEY_TAB;		break;
      case SDLK_F1:	rc = KEY_F1;		break;
      case SDLK_F2:	rc = KEY_F2;		break;
      case SDLK_F3:	rc = KEY_F3;		break;
      case SDLK_F4:	rc = KEY_F4;		break;
      case SDLK_F5:	rc = KEY_F5;		break;
      case SDLK_F6:	rc = KEY_F6;		break;
      case SDLK_F7:	rc = KEY_F7;		break;
      case SDLK_F8:	rc = KEY_F8;		break;
      case SDLK_F9:	rc = KEY_F9;		break;
      case SDLK_F10:	rc = KEY_F10;		break;
      case SDLK_F11:	rc = KEY_F11;		break;
      case SDLK_F12:	rc = KEY_F12;		break;

      case SDLK_BACKSPACE:
      case SDLK_DELETE:	rc = KEY_BACKSPACE;	break;

      case SDLK_PAUSE:	rc = KEY_PAUSE;		break;

      case SDLK_EQUALS:	rc = KEY_EQUALS;	break;

      case SDLK_KP_MINUS:
      case SDLK_MINUS:	rc = KEY_MINUS;		break;

      case SDLK_LSHIFT:
      case SDLK_RSHIFT:	rc = KEY_RSHIFT;	break;

      case SDLK_LCTRL:
      case SDLK_RCTRL:	rc = KEY_RCTRL;		break;

      case SDLK_LALT:
      case SDLK_LGUI:
      case SDLK_RALT:
      case SDLK_RGUI:	rc = KEY_RALT;		break;

      default:		rc = sym;		break;
    }

    return rc;
}

void I_ShutdownGraphics(void)
{
    if (texture)  { SDL_DestroyTexture(texture);   texture = NULL;  }
    if (renderer) { SDL_DestroyRenderer(renderer); renderer = NULL; }
    if (window)   { SDL_DestroyWindow(window);     window = NULL;   }
    SDL_Quit();
}


//
// I_StartFrame
//
void I_StartFrame (void)
{
}

//
// Build a DOOM mouse-button mask from the SDL mouse button state.
//
static int I_MouseButtons(SDL_MouseButtonFlags state)
{
    return 0
	| ((state & SDL_BUTTON_MASK(SDL_BUTTON_LEFT))   ? 1 : 0)
	| ((state & SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE)) ? 2 : 0)
	| ((state & SDL_BUTTON_MASK(SDL_BUTTON_RIGHT))  ? 4 : 0);
}

/* This processes SDL events */
void I_GetEvent(SDL_Event *Event)
{
    event_t event;

    switch (Event->type)
    {
      case SDL_EVENT_KEY_DOWN:
	event.type = ev_keydown;
	// The console-toggle key is the one LEFT of "1" -- detect it by physical
	// SCANCODE (SDL_SCANCODE_GRAVE), not the produced character, so it works on
	// non-US layouts (German `^`, etc.) where that key isn't backquote. (GZDoom
	// does the same: keycode first, grave scancode as the layout-proof fallback.)
	event.data1 = (Event->key.scancode == SDL_SCANCODE_GRAVE)
		      ? KEY_BACKQUOTE : xlatekey(Event->key.key);
	D_PostEvent(&event);
	break;

      case SDL_EVENT_KEY_UP:
	event.type = ev_keyup;
	event.data1 = (Event->key.scancode == SDL_SCANCODE_GRAVE)
		      ? KEY_BACKQUOTE : xlatekey(Event->key.key);
	D_PostEvent(&event);
	break;

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
	if (menuactive)
	    break;
	event.type = ev_mouse;
	event.data1 = I_MouseButtons(SDL_GetMouseState(NULL, NULL));
	event.data2 = event.data3 = 0;
	D_PostEvent(&event);
	// Also emit the specific button as a bindable key press, so the config
	// tool / `bind` can map a mouse button to an action (jump, spy, ...).
	{
	    int mk = (Event->button.button == SDL_BUTTON_LEFT)   ? KEY_MOUSE1
		   : (Event->button.button == SDL_BUTTON_RIGHT)  ? KEY_MOUSE2
		   : (Event->button.button == SDL_BUTTON_MIDDLE) ? KEY_MOUSE3 : 0;
	    if (mk)
	    {
		event.type  = (Event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? ev_keydown : ev_keyup;
		event.data1 = mk;
		event.data2 = event.data3 = 0;
		D_PostEvent(&event);
	    }
	}
	break;

      case SDL_EVENT_MOUSE_MOTION:
	if (menuactive)
	    break;
	event.type = ev_mouse;
	event.data1 = I_MouseButtons(Event->motion.state);
	event.data2 =  ((int)Event->motion.xrel) << 2;
	event.data3 = -((int)Event->motion.yrel) << 2;
	D_PostEvent(&event);
	break;

      case SDL_EVENT_MOUSE_WHEEL:
	if (menuactive)
	    break;
	// wheel up/down -> a one-shot key press (default: next/prev weapon)
	if (Event->wheel.y != 0)
	{
	    event.type = ev_keydown;
	    event.data1 = (Event->wheel.y > 0) ? KEY_MWHEELUP : KEY_MWHEELDOWN;
	    D_PostEvent(&event);
	    event.type = ev_keyup;
	    D_PostEvent(&event);
	}
	break;

      case SDL_EVENT_GAMEPAD_ADDED:
	if (!gamepad)
	{
	    gamepad = SDL_OpenGamepad(Event->gdevice.which);
	    if (gamepad)
		fprintf(stderr, "Gamepad connected: %s\n", SDL_GetGamepadName(gamepad));
	}
	break;

      case SDL_EVENT_GAMEPAD_REMOVED:
	if (gamepad && Event->gdevice.which == SDL_GetGamepadID(gamepad))
	{
	    fprintf(stderr, "Gamepad disconnected\n");
	    SDL_CloseGamepad(gamepad);
	    gamepad = NULL;
	    gamepad_left_x = 0.0f;
	    gamepad_left_y = 0.0f;
	    gamepad_right_x = 0.0f;
	    gamepad_right_y = 0.0f;
	}
	break;

      case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
      case SDL_EVENT_GAMEPAD_BUTTON_UP:
	{
	    int doom_key = 0;
	    switch (Event->gbutton.button)
	    {
		case SDL_GAMEPAD_BUTTON_SOUTH:        doom_key = key_jump; break;
		case SDL_GAMEPAD_BUTTON_WEST:         doom_key = key_use; break;
		case SDL_GAMEPAD_BUTTON_EAST:         doom_key = key_fire; break;
		case SDL_GAMEPAD_BUTTON_NORTH:
		    {
			if (Event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)
			{
			    event.type = ev_keydown;
			    event.data1 = KEY_MWHEELUP;
			    event.data2 = event.data3 = 0;
			    D_PostEvent(&event);
			    event.type = ev_keyup;
			    D_PostEvent(&event);
			}
		    }
		    break;
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  doom_key = key_strafeleft; break;
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: doom_key = key_straferight; break;
		case SDL_GAMEPAD_BUTTON_START:        doom_key = KEY_ESCAPE; break;
		case SDL_GAMEPAD_BUTTON_BACK:         doom_key = KEY_TAB; break;
		case SDL_GAMEPAD_BUTTON_DPAD_UP:      doom_key = KEY_UPARROW; break;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN:    doom_key = KEY_DOWNARROW; break;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT:    doom_key = KEY_LEFTARROW; break;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:   doom_key = KEY_RIGHTARROW; break;
	    }
	    if (doom_key)
	    {
		event.type  = (Event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) ? ev_keydown : ev_keyup;
		event.data1 = doom_key;
		event.data2 = event.data3 = 0;
		D_PostEvent(&event);
	    }
	}
	break;

      case SDL_EVENT_GAMEPAD_AXIS_MOTION:
	{
	    float val = (float)Event->gaxis.value / 32767.0f;
	    switch (Event->gaxis.axis)
	    {
		case SDL_GAMEPAD_AXIS_LEFTX:
		    gamepad_left_x = val;
		    break;
		case SDL_GAMEPAD_AXIS_LEFTY:
		    gamepad_left_y = val;
		    break;
		case SDL_GAMEPAD_AXIS_RIGHTX:
		    gamepad_right_x = val;
		    break;
		case SDL_GAMEPAD_AXIS_RIGHTY:
		    gamepad_right_y = val;
		    break;
		case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
		    {
			static boolean lt_pressed = false;
			boolean pressed = (Event->gaxis.value > 16384);
			if (pressed != lt_pressed)
			{
			    lt_pressed = pressed;
			    event.type = pressed ? ev_keydown : ev_keyup;
			    event.data1 = KEY_RSHIFT; // Speed
			    event.data2 = event.data3 = 0;
			    D_PostEvent(&event);
			}
		    }
		    break;
		case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
		    {
			static boolean rt_pressed = false;
			boolean pressed = (Event->gaxis.value > 16384);
			if (pressed != rt_pressed)
			{
			    rt_pressed = pressed;
			    event.type = pressed ? ev_keydown : ev_keyup;
			    event.data1 = key_fire; // Fire
			    event.data2 = event.data3 = 0;
			    D_PostEvent(&event);
			}
		    }
		    break;
	    }
	}
	break;

      case SDL_EVENT_WINDOW_FOCUS_GAINED:
	window_focused = 1;
	I_ApplyMouseGrab();
	break;

      case SDL_EVENT_WINDOW_FOCUS_LOST:
	window_focused = 0;
	I_ApplyMouseGrab();
	break;

      case SDL_EVENT_QUIT:
	I_Quit();
    }
}

//
// I_StartTic
//
void I_StartTic (void)
{
    SDL_Event Event;

    while ( SDL_PollEvent(&Event) )
	I_GetEvent(&Event);

    // Apply right stick analog looking/turning to mouse accumulators
    if (gamepad_right_x < -0.1f || gamepad_right_x > 0.1f) {
        // Horizontal turn
        mousex += (int)(gamepad_right_x * 8.0f * (mouseSensitivity + 5));
    }
    if (gamepad_right_y < -0.1f || gamepad_right_y > 0.1f) {
        // Vertical look (inverted look: right stick down/positive Y is looking down, mousey negative is looking down)
        mousey -= (int)(gamepad_right_y * 8.0f * (mouseSensitivity + 5));
    }
}


//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
}

//
// I_FinishUpdate
//
//
// Console overlay -- drawn with SDL using the baked DejaVuSansMono atlas, so the
// console text is crisp/anti-aliased over a translucent panel (drawn in the
// renderer's logical space = SCREENWIDTH x SCREENHEIGHT, after the game blit).
//
static SDL_Texture* confont = NULL;

static void I_ConDrawText (float x, float y, const char* s, float cw, float ch)
{
    if (!s) return;
    for ( ; *s ; s++)
    {
	int c = (unsigned char)*s;
	if (c < FONT_FIRST || c >= FONT_FIRST+FONT_COUNT) c = '?';
	SDL_FRect src = { (float)((c-FONT_FIRST)*FONT_CW), 0, FONT_CW, FONT_CH };
	SDL_FRect dst = { x, y, cw, ch };
	SDL_RenderTexture (renderer, confont, &src, &dst);
	x += cw;
    }
}

// Build the baked-atlas font texture once (shared by the console + the Controls screen).
static void I_EnsureConFont (void)
{
    Uint32*	px;
    SDL_Surface* s;
    int		i;
    if (confont)
	return;
    px = malloc (FONT_AW*FONT_CH*4);
    for (i=0 ; i<FONT_AW*FONT_CH ; i++)
	px[i] = 0x00FFFFFFu | ((Uint32)font_alpha[i] << 24);	// white, alpha=coverage
    s = SDL_CreateSurfaceFrom (FONT_AW, FONT_CH, SDL_PIXELFORMAT_ARGB8888, px, FONT_AW*4);
    confont = SDL_CreateTextureFromSurface (renderer, s);
    SDL_SetTextureBlendMode (confont, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode (confont, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface (s); free (px);
}

static void I_DrawConsoleOverlay (void)
{
    float	W, H, conH, fs, cw, ch, y;
    int		row;
    const char*	line;

    if (!C_Active())
	return;

    I_EnsureConFont ();

    W = SCREENWIDTH; H = SCREENHEIGHT;
    conH = H * 0.55f;

    // translucent panel + red separator
    SDL_SetRenderDrawBlendMode (renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor (renderer, 8, 10, 16, 205);
    { SDL_FRect p = {0, 0, W, conH};        SDL_RenderFillRect (renderer, &p); }
    SDL_SetRenderDrawColor (renderer, 180, 40, 40, 255);
    { SDL_FRect b = {0, conH-2, W, 2};      SDL_RenderFillRect (renderer, &b); }

    // size the font so ~13 lines fit; advance keeps the monospace aspect
    ch = H / 24.0f; if (ch < 8) ch = 8;
    fs = ch / FONT_CH;
    cw = FONT_CW * fs;

    SDL_SetTextureColorMod (confont, 255, 236, 160);	// amber text
    y = conH - 4 - ch;					// input line at the bottom
    I_ConDrawText (cw*0.5f, y, C_GetLine(0), cw, ch);

    for (row = 1, y -= ch + 2 ; y > 2 ; y -= ch + 2, row++)
    {
	line = C_GetLine (row);
	if (!line) break;
	I_ConDrawText (cw*0.5f, y, line, cw, ch);
    }
}

// Options -> Controls: the key-bindings screen, drawn with the baked TTF atlas
// (state/input live in m_controls.c).  Logical space is SCREENWIDTH x SCREENHEIGHT.
static void I_DrawControlsOverlay (void)
{
    float	W, H, ch, cw, rowh, lx, kx, y, tw;
    int		n, i, sel;
    char	keybuf[32];
    const char*	title = "CONTROLS";

    if (!M_Controls_Active())
	return;

    I_EnsureConFont ();

    W = SCREENWIDTH; H = SCREENHEIGHT;
    n   = M_Controls_Count ();
    sel = M_Controls_Sel ();

    // dim the whole frame
    SDL_SetRenderDrawBlendMode (renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor (renderer, 12, 6, 6, 236);	// warm near-black dim panel
    { SDL_FRect p = {0, 0, W, H}; SDL_RenderFillRect (renderer, &p); }

    // rows fill ~82% of the height between a title band and a footer -> adapts to any res
    // (+1 for the "Reset all to defaults" row below the bindings)
    rowh = (H * 0.82f) / (float)(n + 1);
    ch   = rowh * 0.74f; if (ch < 6) ch = 6;
    cw   = FONT_CW * (ch / FONT_CH);
    lx   = W * 0.20f;			// label column
    kx   = W * 0.60f;			// key column

    // title, centred and a bit larger
    tw = (float)strlen(title) * (cw*1.7f);
    SDL_SetTextureColorMod (confont, 216, 44, 28);	// DOOM-red title
    I_ConDrawText ((W - tw)*0.5f, H*0.03f, title, cw*1.7f, ch*1.7f);

    y = H * 0.12f;
    for (i = 0; i < n; i++, y += rowh)
    {
	boolean issel = (i == sel);
	if (issel)
	{
	    SDL_SetRenderDrawColor (renderer, 78, 14, 14, 235);	// dark-red highlight bar
	    { SDL_FRect hb = { lx - cw, y - rowh*0.12f, (kx + W*0.16f) - (lx - cw), rowh }; SDL_RenderFillRect (renderer, &hb); }
	}
	SDL_SetTextureColorMod (confont, issel?255:184, issel?96:34, issel?64:24);	// DOOM red (bright when selected)
	I_ConDrawText (lx, y, M_Controls_Label (i), cw, ch);

	if (issel && M_Controls_Capturing ())
	{
	    SDL_SetTextureColorMod (confont, 255, 132, 84);	// "< press a key >" -- bright red, active
	    I_ConDrawText (kx, y, "< press a key >", cw, ch);
	}
	else
	{
	    M_Controls_KeyName (i, keybuf, sizeof keybuf);
	    if (!keybuf[0]) { keybuf[0]='-'; keybuf[1]='-'; keybuf[2]='-'; keybuf[3]=0; }
	    SDL_SetTextureColorMod (confont, 236, 104, 64);	// value: warmer/lighter red
	    I_ConDrawText (kx, y, keybuf, cw, ch);
	}
    }

    // "Reset all to defaults" row (virtual index n)
    {
	boolean issel = (sel == n);
	if (issel)
	{
	    SDL_SetRenderDrawColor (renderer, 90, 16, 16, 235);	// reset row: dark-red bar
	    { SDL_FRect hb = { lx - cw, y - rowh*0.12f, (kx + W*0.16f) - (lx - cw), rowh }; SDL_RenderFillRect (renderer, &hb); }
	}
	SDL_SetTextureColorMod (confont, issel?255:206, issel?96:48, issel?64:34);	// reset row: red
	I_ConDrawText (lx, y, "Reset all to defaults", cw, ch);
    }

    // footer hint
    SDL_SetTextureColorMod (confont, 156, 78, 66);	// dim red footer
    I_ConDrawText (lx, H*0.955f,
		   "Up/Down: move   Enter: rebind   Bksp: clear   Esc: back",
		   cw*0.85f, ch*0.85f);
}

// Options -> Video: the settings screen, same TTF-overlay style as Controls.
static void I_DrawVideoOverlay (void)
{
    float	W, H, ch, cw, rowh, lx, vx, y, tw;
    int		n, i, sel;
    char	valbuf[40];
    const char*	title = "VIDEO";

    if (!M_Video_Active())
	return;

    I_EnsureConFont ();

    W = SCREENWIDTH; H = SCREENHEIGHT;
    n   = M_Video_Count ();
    sel = M_Video_Sel ();

    SDL_SetRenderDrawBlendMode (renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor (renderer, 12, 6, 6, 236);	// warm near-black dim panel
    { SDL_FRect p = {0, 0, W, H}; SDL_RenderFillRect (renderer, &p); }

    rowh = (H * 0.74f) / (float)n;		// fewer rows than Controls -> a bit larger
    ch   = rowh * 0.72f; if (ch < 6) ch = 6;
    cw   = FONT_CW * (ch / FONT_CH);
    lx   = W * 0.22f;
    vx   = W * 0.60f;

    tw = (float)strlen(title) * (cw*1.7f);
    SDL_SetTextureColorMod (confont, 216, 44, 28);	// DOOM-red title
    I_ConDrawText ((W - tw)*0.5f, H*0.05f, title, cw*1.7f, ch*1.7f);

    y = H * 0.17f;
    for (i = 0; i < n; i++, y += rowh)
    {
	boolean issel = (i == sel);
	if (issel)
	{
	    SDL_SetRenderDrawColor (renderer, 78, 14, 14, 235);	// dark-red highlight bar
	    { SDL_FRect hb = { lx - cw, y - rowh*0.12f, (vx + W*0.20f) - (lx - cw), rowh }; SDL_RenderFillRect (renderer, &hb); }
	}
	SDL_SetTextureColorMod (confont, issel?255:184, issel?96:34, issel?64:24);	// DOOM red (bright when selected)
	I_ConDrawText (lx, y, M_Video_Label (i), cw, ch);

	M_Video_Value (i, valbuf, sizeof valbuf);
	SDL_SetTextureColorMod (confont, 236, 104, 64);	// value: warmer/lighter red
	if (issel)		// draw < value > to hint left/right cycling
	{
	    char withar[48];
	    snprintf (withar, sizeof withar, "< %s >", valbuf);
	    I_ConDrawText (vx - cw*2, y, withar, cw, ch);
	}
	else
	    I_ConDrawText (vx, y, valbuf, cw, ch);
    }

    SDL_SetTextureColorMod (confont, 156, 78, 66);	// dim red footer
    I_ConDrawText (lx, H*0.955f,
		   "Up/Down: move   Left/Right: change   Esc: back",
		   cw*0.85f, ch*0.85f);
}

void I_FinishUpdate (void)
{
    static int	lasttic;
    int		tics;
    int		i;
    int		x, y;
    void*	pixels;
    int		pitch;

    // grab while playing, release in the menu
    I_ApplyMouseGrab();

    // draws little dots on the bottom of the screen
    if (devparm)
    {
	i = I_GetTime();
	tics = i - lasttic;
	lasttic = i;
	if (tics > 20) tics = 20;

	for (i=0 ; i<tics*2 ; i+=2)
	    screens[0][ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0xff;
	for ( ; i<20*2 ; i+=2)
	    screens[0][ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0x0;
    }

    // Expand the 8-bit palettized frame into the 32-bit streaming texture.
    if ( !SDL_LockTexture(texture, NULL, &pixels, &pitch) )
	return;

    for (y=0 ; y<SCREENHEIGHT ; y++)		// fast path: expand straight to texture
    {
	Uint32*		dst = (Uint32 *)((Uint8 *)pixels + y*pitch);
	unsigned char*	src = screens[0] + y*SCREENWIDTH;
	for (x=0 ; x<SCREENWIDTH ; x++)
	    dst[x] = palette[src[x]];
    }

    SDL_UnlockTexture(texture);

    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
    I_DrawControlsOverlay();		// Options -> Controls key-bindings screen
    I_DrawVideoOverlay();		// Options -> Video settings screen
    I_DrawConsoleOverlay();		// crisp SDL/TTF console on top of the frame
    SDL_RenderPresent(renderer);
}


//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy (scr, screens[0], SCREENWIDTH*SCREENHEIGHT);
}


//
// I_SetPalette
//
void I_SetPalette (byte* pal)
{
    int i;

    for ( i=0; i<256; ++i ) {
	Uint8 r = gammatable[usegamma][*pal++];
	Uint8 g = gammatable[usegamma][*pal++];
	Uint8 b = gammatable[usegamma][*pal++];
	palette[i] = ((Uint32)0xff << 24) | (r << 16) | (g << 8) | b;
    }
}


//
// (Re)create the streaming texture + logical presentation to match the current
// internal resolution (SCREENWIDTH x SCREENHEIGHT).
//
// Output (display) height for the current aspect.  16:9 and 16:10 show the buffer
// 1:1; 4:3 reuses the 16:10 buffer but presents it into a 4:3 logical area, which
// stretches it vertically ~1.2x -- the authentic classic-Doom look.
int I_OutHeight(void)
{
    return (aspect == 0) ? SCREENWIDTH*3/4 : SCREENHEIGHT;
}

static void I_CreateTexture(void)
{
    if (texture)
	SDL_DestroyTexture(texture);

    I_ApplyLogicalPresentation();

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
				SDL_TEXTUREACCESS_STREAMING,
				SCREENWIDTH, SCREENHEIGHT);
    if ( texture == NULL )
	I_Error("Could not create texture: %s", SDL_GetError());
    
    I_ApplyVideoFilter();
}


// Cross-module hooks used when changing resolution.
extern int	screenblocks;
extern int	detailLevel;
void		R_SetViewSize (int blocks, int detail);
void		R_ExecuteSetViewSize (void);
void		ST_SetRes (void);
void		HU_Buddy_SetRes (void);

//
// V_SetRes
// Change the internal rendering resolution at runtime (scale = 1..6).
// Recreates the SDL texture and rebuilds the renderer / status-bar state.
//
void V_SetRes(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 7) scale = 7;
    if (BASE_WIDTH*scale > MAXWIDTH || BASE_HEIGHT*scale > MAXHEIGHT)
	return;

    widescreen   = (aspect == 1);		// only 16:9 widens the buffer (Hor+)
    hires        = scale;
    SCREENHEIGHT = BASE_HEIGHT * scale;
    NONWIDEWIDTH = BASE_WIDTH  * scale;		// the 16:10 reference width

    if (widescreen)
    {
	SCREENWIDTH = SCREENHEIGHT * 16 / 9;	// Hor+ 16:9 buffer
	if (SCREENWIDTH > MAXWIDTH) SCREENWIDTH = MAXWIDTH;
	SCREENWIDTH &= ~3;			// keep a multiple of 4
    }
    else
	SCREENWIDTH = NONWIDEWIDTH;		// 4:3 and 16:10 share the 320*hires buffer
						// (4:3 is the same buffer shown stretched)

    // half the extra width, in BASE (320) coords -- HUD edges shift by this
    WIDESCREENDELTA = ((SCREENWIDTH - NONWIDEWIDTH) / scale) / 2;

    if (renderer)
	I_CreateTexture();

    // Rebuild resolution-dependent state.  R_SetViewSize sets setsizeneeded so
    // the next D_Display rebuilds the view tables and repaints the border;
    // ST_SetRes resizes (and flags a full redraw of) the status-bar buffer.
    ST_SetRes();
    // Companion HUD is resolution-independent (uses V_DrawBlock + V_DrawPatch
    // which scale internally), but call its hook so it can reallocate any
    // per-resolution scratch buffers it may grow in the future.
    HU_Buddy_SetRes();
    R_SetViewSize (screenblocks, detailLevel);
    R_ExecuteSetViewSize ();

    // Grow/shrink the window to match the new resolution + output aspect (windowed).
    if (window && !fullscreen_mode)
    {
	SDL_SetWindowSize(window, SCREENWIDTH, I_OutHeight());
	SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

//
// Toggle borderless-desktop fullscreen at runtime (from the Video menu).
//
void I_SetFullscreen(int on)
{
    if (!window)
	return;

    fullscreen_mode = on ? 1 : 0;
    SDL_SetWindowFullscreen(window, fullscreen_mode ? true : false);

    // Completely rebuild scaling, texture, status bar, and viewport tables for the new mode.
    V_SetRes(hires);
}

int I_GetFullscreen(void)
{
    return fullscreen_mode;
}


void I_InitGraphics(void)
{
    static int	firsttime=1;
    Uint32	window_flags = SDL_WINDOW_RESIZABLE;
    int		startscale;

    if (!firsttime)
	return;
    firsttime = 0;

    if (M_CheckParm("-nomouse") || M_CheckParm("-nograb"))
	grabMouse = false;

    // Initial resolution scale (also drives the window size).  Defaults to the
    // saved value (loaded from the config); -1..-4 / -render N override.
    startscale = hires;
    if (M_CheckParm("-1")) startscale = 1;
    if (M_CheckParm("-2")) startscale = 2;
    if (M_CheckParm("-3")) startscale = 3;
    if (M_CheckParm("-4")) startscale = 4;
    {
	int p = M_CheckParm("-render");
	if (p && p < myargc-1)
	    startscale = atoi(myargv[p+1]);
    }
    if (startscale < 1) startscale = 1;
    if (startscale > 7) startscale = 7;

    if (fullscreen_mode || M_CheckParm("-fullscreen"))
    {
	window_flags |= SDL_WINDOW_FULLSCREEN;
	fullscreen_mode = 1;
    }

    window = SDL_CreateWindow("BuddyDoom " BUDDYDOOM_VERSION,
			      BASE_WIDTH*startscale, BASE_HEIGHT*startscale,
			      window_flags);
    if ( window == NULL )
	I_Error("Could not create window: %s", SDL_GetError());

    // Application/window icon (embedded from buddydoom.ico; see buddydoom_icon.h).
    // On Windows the .exe icon comes from buddydoom.rc; this sets the live
    // window/taskbar icon on every platform.
    {
	SDL_Surface* icon = SDL_CreateSurfaceFrom(
	    BUDDYDOOM_ICON_W, BUDDYDOOM_ICON_H, SDL_PIXELFORMAT_RGBA32,
	    (void *)buddydoom_icon_rgba, BUDDYDOOM_ICON_W*4);
	if (icon)
	{
	    SDL_SetWindowIcon(window, icon);
	    SDL_DestroySurface(icon);
	}
    }

    // render_backend: 0 = Auto (let SDL choose), 1..N = the Nth VERIFIED driver
    // (I_RenderBackendName; the list was probed above so every entry created OK).
    const char* driver_name =
	(render_backend >= 1 && render_backend <= I_RenderBackendCount()-1)
	    ? I_RenderBackendName(render_backend) : NULL;

    renderer = SDL_CreateRenderer(window, driver_name);
    if ( renderer == NULL && driver_name )		// chosen driver failed anyway -> Auto
    {
	fprintf (stderr, "I_InitGraphics: render backend '%s' failed (%s) -- falling back to Auto\n",
		 driver_name, SDL_GetError());
	renderer = SDL_CreateRenderer(window, NULL);
    }
    if ( renderer == NULL )
	I_Error("Could not create renderer: %s", SDL_GetError());

    // Sync presentation to the refresh rate if vsync is enabled.
    I_ApplyVSync();

    I_CreateTexture();

    SDL_HideCursor();
    I_ApplyMouseGrab();

    // screens[0..3] are allocated by V_Init at MAXWIDTH x MAXHEIGHT; the view,
    // HUD etc. render into screens[0] at the current SCREENWIDTH.  V_SetRes
    // sizes the texture + window to the chosen resolution.
    V_SetRes(startscale);
}
