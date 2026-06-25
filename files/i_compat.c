// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	MSVC compatibility shims for POSIX functions the engine uses but the
//	Windows CRT spells differently.  On non-MSVC toolchains (gcc/clang via
//	build.sh) this translation unit is effectively empty.
//
//	strcasecmp / strncasecmp: the case-insensitive lump / sprite / texture
//	name compares (r_data.c, w_wad.c, m_argv.c, i_system.c, ...) are POSIX;
//	MSVC names them _stricmp / _strnicmp.  Provide the POSIX names here so the
//	whole engine links on the Windows build without per-file #defines.
//
//-----------------------------------------------------------------------------

// Keep the translation unit non-empty on non-MSVC builds.
typedef int i_compat_translation_unit_not_empty;

#ifdef _MSC_VER
#include <string.h>

int strcasecmp (const char* a, const char* b)
{
    return _stricmp (a, b);
}

int strncasecmp (const char* a, const char* b, size_t n)
{
    return _strnicmp (a, b, n);
}
#endif
