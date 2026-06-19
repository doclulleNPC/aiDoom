# Legacy fixes — making 1996 DOOM behave on 2020s hardware

This is a running log of bugs and limitations that stem purely from the **age of
the code**: the engine is id Software's 1993/1996 C, written for a 32-bit DOS/x86
world with a fixed 320×200 display, a small heap, K&R-ish C, and an early-90s
compiler. None of these are "real" gameplay bugs — they are assumptions that were
true then and are false now (64-bit pointers, modern compilers, hi-res displays,
gigabytes of RAM).

**When you fix another one of these, add an entry here** (symptom → root cause →
fix → files). Categories below.

---

## 1. 64-bit (LP64/ILP32) pointer assumptions

The code assumes `sizeof(void*) == sizeof(int) == sizeof(long) == 4`. On LP64
(Linux/macOS) and LLP64 (Win64) pointers are 8 bytes, so any pointer that travels
through an `int`, or any on-disk struct that embedded a pointer, silently corrupts.
Watch the `-Wpointer-to-int-cast` / `-Wint-to-pointer-cast` warnings — **each one
is a real bug.**

| Symptom | Root cause | Fix | Files |
|---|---|---|---|
| Crash reading lumps | WAD file handle (a `FILE*`) stored in an `int` field | store the real `FILE*` | `w_wad.c`, `lumpinfo_t` |
| Garbage colormaps / corrupt light | pointer aligned via `(int)ptr` truncation | use `uintptr_t`/`intptr_t` | `colormaps`, `translationtables` |
| Garbage textures / crash in `R_InitTextures` | on-disk `maptexture_t` had a `void **columndirectory` (8 B on 64-bit) that shifted every following field off the on-disk layout | make it a 4-byte placeholder | `r_data.c` |
| Zone-heap corruption → crash in `P_LoadThings` on every level load | pointer arrays allocated as `Z_Malloc(n*4)` (DOS pointer size) under-allocate | `Z_Malloc(n*sizeof(*p))` | `p_setup.c` (`linebuffer`), `r_data.c` (texture arrays) |
| Load a savegame → crash or corrupted actors/sectors | mobj/player/sector/state references are archived as array *indices* stuffed into the struct's pointer fields, then read back with `(int)` — which truncates the 8-byte field on 64-bit | swizzle both directions through `intptr_t`; save buffer is ample (`screens[]` are `MAXWIDTH*MAXHEIGHT`). **Verified: save+load works on 64-bit** | `p_saveg.c` |
| Spurious `Z_ChangeTag: an owner is required for purgable blocks` | the purge-owner test `(unsigned)block->user < 0x100` truncates a 64-bit `user` pointer to 32 bits before comparing | cast through `uintptr_t` | `z_zone.c` (`Z_ChangeTag2`) |

**Checked, benign:** `d_net.c` uses `(int)&((doomdata_t*)0)->field` — that's the
old `offsetof` idiom (a small offset, not a real pointer), so it's correct on
64-bit despite the cast warning. If real multiplayer with packed pointers is
revived, re-audit it.

**Load-bearing detail:** `doomtype.h` keeps `typedef int boolean` (4 bytes), *not*
1-byte `bool` — several on-disk/in-memory struct layouts depend on it.

---

## 2. Modern-compiler strictness (gcc 14 / C11+)

The source is K&R-ish C that a 2024 compiler rejects or miscompiles by default.

- **Hard errors that were warnings in 1996:** implicit-int, implicit function
  declarations, int↔pointer conversions. Built with
  `-Wno-implicit-int -Wno-implicit-function-declaration -Wno-int-conversion
  -Wno-return-mismatch` (see `build.sh`).
- **Strict aliasing miscompilation (subtle, `-O2` only).** The engine type-puns
  constantly, e.g. `*(int *)lumpinfo[l].name` to compare 4 chars as an int. With
  default strict aliasing `-O2` reorders/optimizes this wrong — the classic symptom
  was `R_InitSprites: Sprite TROO frame A is missing rotations` at startup.
  **`-fno-strict-aliasing` is mandatory.**
- `-fcommon` for the era's tentative-definition globals; `-DSDL_MAIN_HANDLED`
  because `i_main.c` owns `main()`.

---

## 3. Fixed 320×200 → variable hi-res resolution

The original renders into a hardcoded 320×200 8-bit buffer; this port makes the
internal resolution a **runtime** value (`SCREENWIDTH`/`SCREENHEIGHT` = `BASE_* *
hires`, 1–6 → up to 1920×1200). That exposes every place that baked in 320/200.

- **`SCREENWIDTH`/`SCREENHEIGHT` are now variables** (`doomdef.c`), so any
  *static array* or *static initializer* sized/seeded with them breaks at file
  scope → renderer tables are sized for `MAXWIDTH`×`MAXHEIGHT` instead.
- **2D coordinate convention:** all HUD/menu/status-bar/intermission/finale drawing
  is authored in **BASE (320×200)** coords and scaled by `hires` inside the `V_*`
  functions. Passing a `SCREENWIDTH`-relative coordinate to `V_DrawPatch` now
  overflows and is rejected ("`Patch at … exceeds LFB`").
- **`visplane_t.top/bottom` were `byte`** (max 255) but hold *screen row numbers*,
  which exceed 255 above hires=1 → floor/ceiling visplanes silently truncated (3D
  view collapsed to a ~200px band). Widened to `unsigned short`, sentinel `0xffff`.
  (`r_defs.h`, `r_plane.c`)
- **`wi_stuff.c` (intermission) was never converted** — it positioned everything
  with `SCREENWIDTH`/`SCREENHEIGHT` (e.g. `SCREENWIDTH - SP_STATSX`). At hi-res
  those are ~1230, so `V_DrawPatch` rejected them: **the kills/items/secrets %,
  time and centered titles never drew**, with `bad patch (ignored)` spam at
  573,17 / 589,2. Fixed by switching positioning to `BASE_WIDTH`/`BASE_HEIGHT`
  (kept the real framebuffer `memcpy` in `WI_slamBackground` at `SCREEN*`).
- Fuzz/spectre table held a compile-time `SCREENWIDTH` offset → now unit offsets
  scaled by the runtime `SCREENWIDTH` at the use site (`r_draw.c`).
- `pspritescale`/`pspriteiscale` use `BASE_WIDTH` (weapon sprites authored in base
  coords) (`r_main.c`).
- **`D_Display` (`d_main.c`) hardcoded the base resolution** — `viewheight == 200`
  (fullscreen-HUD test) and `scaledviewwidth != 320` (border-redraw test). At
  hi-res `viewheight` is never 200 and `scaledviewwidth` never 320, so the
  status-bar refresh/fullscreen logic misfired. Use `SCREENHEIGHT`/`SCREENWIDTH`.
  Compounding it: the menu can **overdraw the status bar** (the 2x "Video" item in
  `M_DrawOptions` bleeds below base y=168 into the bar), and nothing forced a full
  bar repaint when the menu closed → leftover red "VIDEO" baked into the HUD over
  HEALTH/ARMS. Fix: set `redrawsbar = true` whenever the menu was active
  (`menuactivestate`), so `ST_Drawer` does a full `ST_refreshBackground`.

The reliable tell for this class: `Patch at X,Y exceeds LFB` where `X > 320`, or a
HUD/menu element that's off-screen or missing at hires > 1.

---

## 4. Undersized 1996-era fixed buffers

Constants chosen for 320×200 / a few MB of RAM are too small once the frame is
hi-res.

| Symptom | Root cause | Fix | File |
|---|---|---|---|
| `Z_Malloc: failed on allocation of 1024040 bytes` → crash at map-end stats screen | the screen wipe transposes a `SCREENWIDTH*SCREENHEIGHT` buffer (~1 MB at 1280×800, ~2.3 MB at 1920×1200) on top of level data, overflowing the **6 MB** zone heap | bump the zone to **32 MB** (`mb_used`) | `i_system.c` |
| status-bar background buffer | `screens[4]` sized for 320×32 | `SCREENWIDTH*ST_HEIGHT*hires`, reallocated in `ST_SetRes` | `st_stuff.c` |
| AI-buddy **voice crash a few minutes into a game** | `lumpcache[]` is `malloc`'d **once** in `W_InitMultipleFiles`, sized to `numlumps` at that moment — the 1996 engine only ever `W_AddFile`s *before* that point. Adding `buddy.wad` at **runtime** (`I_Voice_Init`) grows `numlumps` but **not** `lumpcache`, so `W_CacheLumpNum` on a buddy lump writes past the array → heap corruption (latent → crash later) | grow `lumpcache` (`realloc` + zero the new slots) right after the runtime `W_AddFile` | `i_voice.c` |

---

## 5. Build / tooling age (not code, but same root cause)

- The original **autotools** setup (`configure.in`, `Makefile.am`) targets SDL 1.x
  and is **stale** — it won't link SDL3. Use `build.sh` (Linux/macOS, system SDL3)
  or `files/Makefile.msvc` (Windows). See the top-level README / `CLAUDE.md`.
- The generated `Makefile` tracks **no header dependencies** — editing a `.h` does
  *not* recompile the `.c` files that include it. After a header change do
  `make clean && make` (or just use `build.sh`, which recompiles everything).
  This masked a `visplane_t` (`r_defs.h`) change during the hi-res work.
- `build.sh` compiles **every** `files/*.c`. A single-header library that is both
  compiled standalone (its own `.c` in the glob) *and* `#include`d with its
  implementation in another TU defines its symbols twice → `multiple definition`
  link error. Pull in only the API in the consumer (`#define STB_VORBIS_HEADER_ONLY`
  before `#include "stb_vorbis.c"`); let the standalone `stb_vorbis.c` be the single
  implementation. (`i_voice.c`, for the buddy-voice OGG decoder.)

---

## 6. Pre-existing portability scaffolding (already in the original — not bugs)

The 1996 source already carried portability workarounds for *its* era. These are
**not** things to "fix" — know they exist so you don't mistake them for bugs:

- **Endianness:** `m_swap.h` byte-swaps because WAD lumps are little-endian; the
  `SHORT()`/`LONG()` macros are no-ops on little-endian hosts. Keep using them on
  on-disk data.
- **`__BEOS__` / `__SVR4` / `linux` guards** in `w_wad.c`, `r_data.c`, `m_swap.h`,
  `doomtype.h` — old per-OS branches.
- **Solaris BUS-error workaround** (`r_data.c`): the texture name is copied
  byte-by-byte instead of `memcpy` because "memcpy() generates a BUS error on
  Solaris with optimization on". Harmless; leave it.
- **DOS remnants** in `i_sound.c`/`s_sound.c` (comments + dead 8-bit paths) — the
  audio is fully reimplemented in the `i_*` SDL layer; the comments are historical.

## How to spot the next one

- Compiler warnings `-Wpointer-to-int-cast` / `-Wint-to-pointer-cast` → §1.
- A startup/render glitch that only appears at `-O2` → strict aliasing, §2.
- `Patch … exceeds LFB`, missing HUD elements, a clamped/banded 3D view → §3.
- `Z_Malloc: failed …` or visual corruption that scales with resolution → §4.
- "It built but behaves weird after I edited a header" → §5 (`make clean`).
