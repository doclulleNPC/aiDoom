// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	Main program, simply calls D_DoomMain high level loop.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_main.c,v 1.4 1997/02/03 22:45:10 b1 Exp $";


#include <SDL3/SDL.h>

// The build defines SDL_MAIN_HANDLED, so SDL does not hijack main(); we own the
// console entry point (keeps stdout for the AI director logs) and just tell SDL
// the main thread is ready before any SDL call.

#include "doomdef.h"

#include "m_argv.h"
#include "d_main.h"

// Crash diagnostics: on a fatal signal, dump a backtrace to stderr (the launcher
// captures it to run/aidoom_stderr.log) so a silent segfault during play -- e.g. a
// bad sprite/sound from the doom2stuff overlay, or a playsim pointer bug -- names the
// function that faulted instead of vanishing.  glibc/Linux only; async-signal-safe
// (backtrace_symbols_fd, no malloc).
#if defined(__linux__) && defined(__GLIBC__)
#include <execinfo.h>
#include <signal.h>
#include <unistd.h>
static void I_CrashHandler (int sig)
{
    void* bt[64];
    int   n = backtrace (bt, 64);
    static const char hdr[] = "\n*** aiDoom CRASH (signal ";
    char  num[4] = { (char)('0' + (sig/10)%10), (char)('0' + sig%10), ')', '\n' };
    write (2, hdr, sizeof hdr - 1);
    write (2, num, 4);
    backtrace_symbols_fd (bt, n, 2);		// fd 2 = stderr
    signal (sig, SIG_DFL);			// restore default + re-raise for a core dump
    raise (sig);
}
static void I_InstallCrashHandler (void)
{
    signal (SIGSEGV, I_CrashHandler);
    signal (SIGABRT, I_CrashHandler);
    signal (SIGFPE,  I_CrashHandler);
    signal (SIGBUS,  I_CrashHandler);
}
#else
static void I_InstallCrashHandler (void) {}
#endif

int
main
( int		argc,
  char**	argv )
{
    SDL_SetMainReady();
    I_InstallCrashHandler ();

    myargc = argc;
    myargv = argv;

    D_DoomMain ();

    return 0;
}
