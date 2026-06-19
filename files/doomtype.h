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
// DESCRIPTION:
//	Simple basic typedefs, isolated here to make it easier
//	 separating modules.
//    
//-----------------------------------------------------------------------------


#ifndef __DOOMTYPE__
#define __DOOMTYPE__


#ifndef __BYTEBOOL__
#define __BYTEBOOL__
// Fixed to use builtin bool type with C++.
#ifdef __cplusplus
typedef bool boolean;
#else
#ifdef __BEOS__	/* boolean is a builtin type for MWCC */
#define boolean D_BOOL
#undef false
#define false D_false
#undef true
#define true D_true
#endif
// SDL3 (via <stdbool.h>) defines true/false as macros, which would break the
// original `typedef enum {false, true} boolean;`.  Use stdbool instead while
// keeping the historic int-sized boolean ABI (load-bearing for the on-disk WAD
// and savegame struct layouts -- see CLAUDE.md).
//
// On Windows, <rpcndr.h> (transitively pulled in by <windows.h>) hard-codes
// `typedef unsigned char boolean;` with no #ifdef guard.  We can't redefine
// `boolean` to int after the SDK does, but we CAN pre-empt the SDK by
// aliasing the engine's `boolean` to a private tag name via #define, BEFORE
// <windows.h> is included anywhere.  All engine source continues to spell
// the type as `boolean`; the preprocessor turns it into `doom_boolean_t`
// consistently across all translation units.
#define boolean doom_boolean_t
typedef int doom_boolean_t;
#include <stdbool.h>
#ifndef false
#define false 0
#endif
#ifndef true
#define true 1
#endif
#endif
typedef unsigned char byte;
#endif


// Predefined with some OS.
#ifdef LINUX
#include <values.h>
#else
#ifndef MAXCHAR
#define MAXCHAR		((char)0x7f)
#endif
#ifndef MAXSHORT
#define MAXSHORT	((short)0x7fff)
#endif

// Max pos 32-bit int.
#ifndef MAXINT
#define MAXINT		((int)0x7fffffff)	
#endif
#ifndef MAXLONG
#define MAXLONG		((long)0x7fffffff)
#endif
#ifndef MINCHAR
#define MINCHAR		((char)0x80)
#endif
#ifndef MINSHORT
#define MINSHORT	((short)0x8000)
#endif

// Max negative 32-bit integer.
#ifndef MININT
#define MININT		((int)0x80000000)	
#endif
#ifndef MINLONG
#define MINLONG		((long)0x80000000)
#endif
#endif




#endif
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
