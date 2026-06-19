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

#include <SDL3/SDL.h>

#include "m_swap.h"
#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"

#include "doomdef.h"

#include "aidoom_icon.h"
#include "c_console.h"			// console overlay state (C_Active / C_GetLine)
#include "../tools/font_atlas.h"		// baked DejaVuSansMono atlas (TTF console font)


static SDL_Window*	window = NULL;
static SDL_Renderer*	renderer = NULL;
static SDL_Texture*	texture = NULL;

// Expanded 32-bit (ARGB8888) palette, rebuilt by I_SetPalette.
static Uint32		palette[256];

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
	event.data1 = xlatekey(Event->key.key);
	D_PostEvent(&event);
	break;

      case SDL_EVENT_KEY_UP:
	event.type = ev_keyup;
	event.data1 = xlatekey(Event->key.key);
	D_PostEvent(&event);
	break;

      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
	event.type = ev_mouse;
	event.data1 = I_MouseButtons(SDL_GetMouseState(NULL, NULL));
	event.data2 = event.data3 = 0;
	D_PostEvent(&event);
	break;

      case SDL_EVENT_MOUSE_MOTION:
	event.type = ev_mouse;
	event.data1 = I_MouseButtons(Event->motion.state);
	event.data2 =  ((int)Event->motion.xrel) << 2;
	event.data3 = -((int)Event->motion.yrel) << 2;
	D_PostEvent(&event);
	break;

      case SDL_EVENT_MOUSE_WHEEL:
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

static void I_DrawConsoleOverlay (void)
{
    float	W, H, conH, fs, cw, ch, y;
    int		row;
    const char*	line;

    if (!C_Active())
	return;

    if (!confont)
    {
	Uint32* px = malloc (FONT_AW*FONT_CH*4);
	int i;
	for (i=0 ; i<FONT_AW*FONT_CH ; i++)
	    px[i] = 0x00FFFFFFu | ((Uint32)font_alpha[i] << 24);	// white, alpha=coverage
	SDL_Surface* s = SDL_CreateSurfaceFrom (FONT_AW, FONT_CH, SDL_PIXELFORMAT_ARGB8888, px, FONT_AW*4);
	confont = SDL_CreateTextureFromSurface (renderer, s);
	SDL_SetTextureBlendMode (confont, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode (confont, SDL_SCALEMODE_LINEAR);
	SDL_DestroySurface (s); free (px);
    }

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

    for (y=0 ; y<SCREENHEIGHT ; y++)
    {
	Uint32*		dst = (Uint32 *)((Uint8 *)pixels + y*pitch);
	unsigned char*	src = screens[0] + y*SCREENWIDTH;
	for (x=0 ; x<SCREENWIDTH ; x++)
	    dst[x] = palette[src[x]];
    }

    SDL_UnlockTexture(texture);

    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, NULL, NULL);
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
static void I_CreateTexture(void)
{
    if (texture)
	SDL_DestroyTexture(texture);

    // Render at the internal resolution; SDL scales (aspect-preserving) to the
    // window.  Letterbox keeps square pixels with bars rather than distorting.
    SDL_SetRenderLogicalPresentation(renderer, SCREENWIDTH, SCREENHEIGHT,
				     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
				SDL_TEXTUREACCESS_STREAMING,
				SCREENWIDTH, SCREENHEIGHT);
    if ( texture == NULL )
	I_Error("Could not create texture: %s", SDL_GetError());
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);	// crisp pixels
}


// Cross-module hooks used when changing resolution.
extern int	screenblocks;
extern int	detailLevel;
void		R_SetViewSize (int blocks, int detail);
void		ST_SetRes (void);

//
// V_SetRes
// Change the internal rendering resolution at runtime (scale = 1..6).
// Recreates the SDL texture and rebuilds the renderer / status-bar state.
//
void V_SetRes(int scale)
{
    if (scale < 1) scale = 1;
    if (scale > 6) scale = 6;
    if (BASE_WIDTH*scale > MAXWIDTH || BASE_HEIGHT*scale > MAXHEIGHT)
	return;

    hires        = scale;
    SCREENWIDTH  = BASE_WIDTH  * scale;
    SCREENHEIGHT = BASE_HEIGHT * scale;

    if (renderer)
	I_CreateTexture();

    // Rebuild resolution-dependent state.  R_SetViewSize sets setsizeneeded so
    // the next D_Display rebuilds the view tables and repaints the border;
    // ST_SetRes resizes (and flags a full redraw of) the status-bar buffer.
    ST_SetRes();
    R_SetViewSize (screenblocks, detailLevel);

    // Grow/shrink the window to match the new resolution (windowed only).
    if (window && !fullscreen_mode)
    {
	SDL_SetWindowSize(window, SCREENWIDTH, SCREENHEIGHT);
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

    if (!fullscreen_mode)
    {
	SDL_SetWindowSize(window, SCREENWIDTH, SCREENHEIGHT);
	SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
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
    if (startscale > 6) startscale = 6;

    if (fullscreen_mode || M_CheckParm("-fullscreen"))
    {
	window_flags |= SDL_WINDOW_FULLSCREEN;
	fullscreen_mode = 1;
    }

    window = SDL_CreateWindow("aiDoom",
			      BASE_WIDTH*startscale, BASE_HEIGHT*startscale,
			      window_flags);
    if ( window == NULL )
	I_Error("Could not create window: %s", SDL_GetError());

    // Application/window icon (embedded from aidoom.ico; see aidoom_icon.h).
    // On Windows the .exe icon comes from aidoom.rc; this sets the live
    // window/taskbar icon on every platform.
    {
	SDL_Surface* icon = SDL_CreateSurfaceFrom(
	    AIDOOM_ICON_W, AIDOOM_ICON_H, SDL_PIXELFORMAT_RGBA32,
	    (void *)aidoom_icon_rgba, AIDOOM_ICON_W*4);
	if (icon)
	{
	    SDL_SetWindowIcon(window, icon);
	    SDL_DestroySurface(icon);
	}
    }

    renderer = SDL_CreateRenderer(window, NULL);
    if ( renderer == NULL )
	I_Error("Could not create renderer: %s", SDL_GetError());

    // VSync on: SDL3 defaults it OFF, which tears badly on fast strafes -- the
    // high-contrast textures (e.g. the blue computer wall) showed as a horizontal
    // "smear". Sync presentation to the refresh to kill the tearing.
    SDL_SetRenderVSync(renderer, 1);

    I_CreateTexture();

    SDL_HideCursor();
    I_ApplyMouseGrab();

    // screens[0..3] are allocated by V_Init at MAXWIDTH x MAXHEIGHT; the view,
    // HUD etc. render into screens[0] at the current SCREENWIDTH.  V_SetRes
    // sizes the texture + window to the chosen resolution.
    V_SetRes(startscale);
}
