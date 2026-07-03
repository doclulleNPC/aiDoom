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
    fflush (NULL);				// flush buffered [INVIS]/debug logs before the backtrace
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
    // Run the handler on a dedicated ALTERNATE signal stack: a STACK-OVERFLOW (runaway
    // recursion) faults with SIGSEGV but leaves no usable stack, so a normal handler can't
    // run and the process dies silently with NO backtrace -- exactly what we saw on the
    // invis-pickup crash.  SA_ONSTACK + sigaltstack lets the backtrace still print, naming
    // the recursive function.
    static char altstk[64*1024];
    stack_t ss;
    struct sigaction sa;
    ss.ss_sp = altstk; ss.ss_size = sizeof altstk; ss.ss_flags = 0;
    sigaltstack (&ss, NULL);
    sa.sa_handler = I_CrashHandler;
    sigemptyset (&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK;
    sigaction (SIGSEGV, &sa, NULL);
    sigaction (SIGABRT, &sa, NULL);
    sigaction (SIGFPE,  &sa, NULL);
    sigaction (SIGBUS,  &sa, NULL);
}
#elif defined(_WIN32)
// Windows has no signals for hardware faults; a bad pointer raises a Structured
// Exception instead.  A top-level SetUnhandledExceptionFilter catches the access
// violation (etc.) that would otherwise just close the (now windowless) game with
// no feedback -- it names the fault + address in an SDL dialog and writes it to
// stderr (-> run/aidoom_stderr.log via the launcher).  Best-effort: on a stack
// overflow the handler runs on a nearly-exhausted stack, so keep it minimal
// (static buffer, no allocation).
#define WIN32_LEAN_AND_MEAN	// keep rpcndr.h/wtypesbase.h out: their boolean/BOOLEAN clash with doomtype.h (cf. i_net.c)
#include <windows.h>
#include <stdio.h>
static LONG WINAPI I_Win32CrashFilter (EXCEPTION_POINTERS* ep)
{
    static char	msg[512];			// static: don't lean on the trashed stack
    DWORD	code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode    : 0;
    void*	addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : NULL;
    const char*	name =
	code == EXCEPTION_ACCESS_VIOLATION    ? "ACCESS_VIOLATION"       :
	code == EXCEPTION_STACK_OVERFLOW      ? "STACK_OVERFLOW"         :
	code == EXCEPTION_ILLEGAL_INSTRUCTION ? "ILLEGAL_INSTRUCTION"    :
	code == EXCEPTION_INT_DIVIDE_BY_ZERO  ? "INT_DIVIDE_BY_ZERO"     :
	code == EXCEPTION_PRIV_INSTRUCTION    ? "PRIVILEGED_INSTRUCTION" :
	code == EXCEPTION_IN_PAGE_ERROR       ? "IN_PAGE_ERROR"          :
					        "unhandled exception";
    snprintf (msg, sizeof msg,
	      "aiDoom crashed.\n\n%s (0x%08lX) at address 0x%p.\n\n"
	      "A log was written to run\\aidoom_stderr.log.",
	      name, (unsigned long)code, addr);
    fprintf (stderr, "\n*** aiDoom CRASH: %s ***\n", msg);
    fflush (stderr);
    // SDL's message box is the native Win32 dialog underneath; safe with parent = NULL
    // even after a crash / with no game window.
    SDL_ShowSimpleMessageBox (SDL_MESSAGEBOX_ERROR, "aiDoom crashed", msg, NULL);
    return EXCEPTION_EXECUTE_HANDLER;		// let the process terminate
}
static void I_InstallCrashHandler (void)
{
    SetUnhandledExceptionFilter (I_Win32CrashFilter);
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
