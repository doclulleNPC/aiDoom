// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	BEX [STRINGS] support for the pickup messages.  BuddyDoom's HUD strings are
//	compile-time #defines (d_englsh.h); DEHACKED/BEX patches (e.g. Legacy of
//	Rust renames the plasma/BFG pickups) replace them at runtime by mnemonic.
//	We keep a runtime `char* deh_<MNEMONIC>` per pickup string, registered in
//	d_deh.c's deh_strlookup[], and consumers (p_inter.c) use those instead of
//	the raw #define so a [STRINGS] entry actually takes effect.
//
//-----------------------------------------------------------------------------
#ifndef __DSTRINGS_BEX_H__
#define __DSTRINGS_BEX_H__

// The full set of vanilla pickup messages, as X(mnemonic).  Extend as needed;
// every entry becomes a runtime string variable and a BEX [STRINGS] key.
#define BEX_PICKUP_STRINGS(X) \
    X(GOTARMOR)   X(GOTMEGA)     X(GOTHTHBONUS) X(GOTARMBONUS) X(GOTSTIM)    \
    X(GOTMEDINEED)X(GOTMEDIKIT)  X(GOTSUPER)    X(GOTMSPHERE)  X(GOTSUIT)    \
    X(GOTINVIS)   X(GOTINVUL)    X(GOTVISOR)    X(GOTBERSERK)  X(GOTMAP)     \
    X(GOTCLIP)    X(GOTCLIPBOX)  X(GOTROCKET)   X(GOTROCKBOX)  X(GOTCELL)    \
    X(GOTCELLBOX) X(GOTSHELLS)   X(GOTSHELLBOX) X(GOTBACKPACK)               \
    X(GOTBFG9000) X(GOTCHAINGUN) X(GOTCHAINSAW) X(GOTLAUNCHER)               \
    X(GOTPLASMA)  X(GOTSHOTGUN)  X(GOTSHOTGUN2)                              \
    X(GOTBLUECARD)X(GOTYELWCARD) X(GOTREDCARD)                              \
    X(GOTBLUESKUL)X(GOTYELWSKUL) X(GOTREDSKULL)

#define X(name) extern char* deh_##name;
BEX_PICKUP_STRINGS(X)
#undef X

#endif
