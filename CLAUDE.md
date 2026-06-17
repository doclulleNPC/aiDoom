# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`aiDoom` (originally `sdldoom-1.10-mod`) — a fork of Sam Lantinga's 1998 SDL port
of id Software's DOOM (the original 1993 C engine, id Tech 1). The goal of the
fork is to make the old SDL Doom build and run on modern 64-bit Linux with current
SDL, then add modifications (hi-res rendering, a video-options menu, …). All source
lives in `files/`. The window title / app name is set to "aiDoom" in
`i_video.c` (`SDL_WM_SetCaption`); the build produces a binary named `aidoom`
(`bin_PROGRAMS` in the Makefile).

## Build & run

Uses the original autotools setup. The README describes a **32-bit** build
(`-m32` against 32-bit SDL 1.2); use that only if 32-bit SDL 1.2 is actually
installed. On a typical modern x86-64 box only **64-bit SDL 1.2** is present, so
build natively 64-bit instead. Either way the legacy K&R-ish C needs permissive
flags for modern gcc (gcc 14 makes implicit-int / implicit-function-declaration /
int-conversion hard errors), and `-fno-strict-aliasing` is **required** — the
engine type-puns all over (e.g. `*(int *)lump->name`) and `-O2` will miscompile
it otherwise (manifests as bogus runtime errors like "missing rotations").

Build natively 64-bit from inside `files/` (note: this old `configure` takes
`CFLAGS` from the environment, not as a `./configure CFLAGS=...` argument):

```sh
export CFLAGS="-O2 -g -fcommon -fno-strict-aliasing -std=gnu89 \
  -Wno-implicit-int -Wno-implicit-function-declaration \
  -Wno-int-conversion -Wno-return-mismatch"
./configure --disable-sdltest
make            # produces ./aidoom
```

The original 32-bit recipe (only works with 32-bit SDL 1.2 installed):

```sh
./configure --disable-sdltest \
  && sed -i s/'^CPPFLAGS ='/'CPPFLAGS = -m32'/g Makefile \
  && sed -i s/'^LDFLAGS = '/'LDFLAGS = -m32'/g Makefile
make
```

`make` does **not** track `CFLAGS` changes — after editing flags, `make clean`
(or `rm -f config.cache` and re-`configure`) before rebuilding. If `configure` is
missing or `*.in` files change, regenerate with `autoreconf` (autotools:
`configure.in`, `acinclude.m4`/`aclocal.m4`, `Makefile.in`). The single output is
the `doom` executable (`bin_PROGRAMS = doom`, `-lm`).

### 64-bit portability (LP64) caveats

The code assumes `sizeof(ptr) == sizeof(int) == 4` (1996 DOS/x86). Building 64-bit
required fixing these; the **same class of bug still lurks** in the netcode
(`d_net.c`) — watch the `-Wpointer-to-int-cast` / `-Wint-to-pointer-cast`
warnings, each is a real bug:
- Pointers stored in `int` then cast back → use `intptr_t`/`uintptr_t` or store
  the real pointer type (fixed: WAD file handle in `lumpinfo_t.handle`).
- Pointer-alignment via `(int)ptr` truncation (fixed: `colormaps`,
  `translationtables`).
- **On-disk WAD structs must match the 32-bit layout.** `maptexture_t` had a
  `void **columndirectory` field (8 bytes on 64-bit) that shifted every following
  field off the on-disk format → garbage; it must be a 4-byte placeholder.
- Per-pointer-array allocations hardcoded `n*4` (DOS pointer size) → use
  `n*sizeof(*p)` (fixed: the `texture*` arrays in `R_InitTextures`, and
  `linebuffer` in `p_setup.c` — the latter caused zone-heap corruption / a crash
  in `P_LoadThings` on every level load until fixed).
- **Savegames** (`p_saveg.c`): the index⇄pointer swizzle now uses `intptr_t`, and
  the save buffer (`screens[1]+0x4000`) is large because `screens[]` are allocated
  at `MAXWIDTH*MAXHEIGHT`. Save+load is verified working on 64-bit.

### IMPORTANT build gotcha: header changes need `make clean`

The generated `Makefile` has **no header dependencies** — editing a `.h` (e.g.
`doomdef.h`, `r_defs.h`) does **not** trigger recompilation of the `.c` files that
include it. After any header edit you must `make clean && make`, or you will run a
binary built from stale objects and chase phantom bugs. (This bit hard during the
hi-res port: a `visplane_t` struct change in `r_defs.h` silently didn't take.)

Run it from a directory containing an IWAD, or point `DOOMWADDIR`/`-iwad` at one:

```sh
./aidoom               # auto-detects doom.wad/doom1.wad/doom2.wad/plutonia.wad/tnt.wad
DOOMWADDIR=/path/to/wads ./aidoom
./aidoom -iwad DOOM1.WAD -warp 1 1 -skill 4 -fullscreen
```

Only the stock commercial/shareware IWADs are supported; this port does **not**
load arbitrary custom WADs as IWADs (PWADs via `-file` work). There is **no test
suite** — verification is done by running the game.

## Architecture

### LLM "AI Director" for monsters (flag-gated)

`files/p_ai_llm.c`/`.h` lets an external director (LLM/script) drive monster
*tactics* via a TCP line protocol, or a built-in `-aidemo` director. Off unless
`-aidirector [port]` or `-aidemo` is passed — vanilla AI otherwise. Hooks:
`A_Chase` (`p_enemy.c`) diverts to `A_LLMChase` for directed monsters,
`P_Ticker` calls `P_AI_Ticker`, `P_SetupLevel` calls `P_AI_Reset`. Directives are
kept in a side-table keyed by `mobj_t*` (no struct/savegame change). Full design
and protocol: **`AGENT_CONTROL.md` §12–13** (player control is §1–11).

### Variable internal resolution (hi-res renderer + Video menu)

The software renderer draws **natively at a runtime-variable resolution**, not an
upscale of 320x200. Key design (ported from `../sdldoom-1.10`, adapted to SDL 1.2):

- `SCREENWIDTH`/`SCREENHEIGHT`/`hires` are **runtime variables** (`doomdef.c`),
  declared `extern` in `doomdef.h`, equal to `BASE_WIDTH*hires` x `BASE_HEIGHT*hires`
  (`hires` 1..6 → 320x200 … 1920x1200). Renderer static tables and the `screens[]`
  buffers are sized for `MAXWIDTH`x`MAXHEIGHT` (1920x1200), not the current size.
- **All 2D drawing is authored in 320x200 (`BASE_*`) coordinates** and scaled up by
  `hires` inside the `V_*` functions (`v_video.c`: `V_DrawPatch`, `V_CopyRect`,
  `V_DrawPatchFlipped`). The 3D view, automap and screen wipe render natively at
  `SCREENWIDTH`/`SCREENHEIGHT`. So HUD/menu/status-bar/intermission/finale positions
  must use `BASE_WIDTH`/`BASE_HEIGHT`; the view-border code (`r_draw.c`) divides its
  hi-res view rect back to base coords before calling `V_*`. The status bar buffer
  (`screens[4]`) is `SCREENWIDTH*ST_HEIGHT*hires` (`ST_SetRes` in `st_stuff.c`).
- `V_SetRes(scale)` (`i_video.c`) changes resolution at runtime: updates the globals,
  re-creates the SDL surface at the new size (so the window grows with the
  resolution), reallocates the status-bar buffer (`ST_SetRes`), and flags a renderer
  rebuild via `R_SetViewSize`. Reached from **Options → Video** (`m_menu.c`,
  `M_DrawVideo`/`M_VideoRes`/`M_VideoFullscreen`, drawn as text — no graphic lumps),
  or `-1`/`-2`/`-3`/`-4` / `-render N` at startup. Persisted in the config as
  `screen_resolution` and `fullscreen` (`m_misc.c` `defaults[]`).
- **`visplane_t.top/bottom` (`r_defs.h`) are `unsigned short`, not `byte`** — they
  hold screen row numbers, which exceed 255 above hires=1; the "unset" sentinel is
  `0xffff` (in `r_plane.c`). A `byte` here silently truncated rows >255 and broke
  all floor/ceiling visplanes at hi-res (symptom: 3D view collapsed to a ~200px band).
- `R_ExecuteSetViewSize` (`r_main.c`) multiplies `scaledviewwidth`/`viewheight` by
  `hires`; `pspritescale`/`pspriteiscale` use `BASE_WIDTH` (weapon sprites are
  authored in base coords). The fuzz table (`r_draw.c`) holds unit offsets scaled by
  `SCREENWIDTH` at the use site (was a compile-time `SCREENWIDTH`, now a variable).

The engine is organized by **two-letter module prefixes**. The split that matters
most: the `i_*` files are the only platform-dependent layer; everything else is
portable game code that calls into them through the `I_*` interface.

- **Platform layer (`i_*`)** — the SDL implementation of the abstract `I_*` API.
  `i_video.c` (SDL_Surface framebuffer at the current resolution, `xlatekey`
  keymap, mouse grab, and `V_SetRes` runtime resolution switching — see below),
  `i_sound.c` (SDL_audio callback mixer, software channel mixing),
  `i_system.c` (timing, zone base alloc, exit), `i_net.c` (sockets), `i_main.c`
  (`main()` → sets `myargc`/`myargv` → `D_DoomMain()`). To port or change OS/SDL
  behavior, you almost always touch only these files.

- **Top level (`d_*`)** — `d_main.c` holds `D_DoomMain()` (startup: arg parsing,
  IWAD detection in `IdentifyVersion`, WAD init, subsystem init) and `D_DoomLoop()`
  (the game loop, never returns). `d_net.c` is the tic-synchronized netcode that
  drives `TryRunTics`.

- **Game logic (`g_game.c`)** — ticcmd building from input, game state machine,
  save/load, demo record/playback, level transitions.

- **Play simulation (`p_*`)** — the simulation, advanced one 1/35s tic at a time
  via `p_tick.c` (thinker list). `p_mobj` (map objects/actors), `p_enemy` (AI),
  `p_map`/`p_maputl`/`p_sight` (movement, collision, blockmap, line-of-sight),
  `p_user`/`p_pspr` (player + weapon sprites), `p_inter` (damage/pickups),
  `p_spec` + `p_doors`/`p_floor`/`p_ceilng`/`p_plats`/`p_lights`/`p_switch`/`p_telept`
  (sector/line specials), `p_setup` (level loading), `p_saveg` (savegame serialization).

- **Renderer (`r_*`)** — software BSP renderer. `r_main` (frame setup, view),
  `r_bsp` (BSP traversal, visplane/drawseg setup), `r_segs`/`r_plane`/`r_things`
  (walls, flats, sprites/masked columns), `r_data` (texture/flat composition),
  `r_draw` (the inner column/span pixel loops), `r_sky`. Output is the 320x200
  8-bit paletted buffer that `i_video.c` blits.

- **WAD & memory** — `w_wad.c` is the lump archive loader (the only file access
  abstraction; everything loads "lumps" by name/number). `z_zone.c` is the custom
  tagged zone allocator (`Z_Malloc` with `PU_*` purge tags) — all runtime
  allocation goes through it, not raw `malloc`.

- **UI & misc** — `m_menu` (in-game menus), `m_misc` (config file, screenshots),
  `m_cheat` (cheat-code FSM), `m_argv` (`M_CheckParm`), `m_random` (the fixed
  RNG table), `hu_*` (heads-up text/messages), `st_*` (status bar), `am_map`
  (automap), `wi_stuff` (intermission), `f_finale`/`f_wipe` (end screens, screen
  melt), `s_sound` (sound/music driver, calls into `i_sound`), `v_video` (screen
  buffer / patch drawing primitives), `info.c`/`tables.c` (huge generated data:
  actor state machine and the finesine/tangent lookup tables).

## Conventions specific to this codebase

- **Fixed-point math everywhere.** Positions, velocities, and most geometry use
  `fixed_t` (16.16) from `m_fixed.c`; angles are 32-bit BAM values indexing the
  trig tables in `tables.c`. Don't introduce floats into the playsim — it must stay
  deterministic so demos and netplay stay in sync.
- **The playsim is deterministic and tic-locked.** Game state advances only in
  whole 1/35s tics through the thinker list; anything affecting gameplay must run
  inside that tic flow (via ticcmds), never off rendering or wall-clock time.
- **No `malloc`/`free` for game data** — use the zone allocator (`Z_Malloc`/`Z_Free`
  with `PU_` purge tags) and load data as WAD lumps via `W_*`.
- Files carry the id Software DOOM Source License header (`DOOMLIC.TXT`); the SDL
  port additions are by Sam Lantinga. Keep new code in the existing C style.
- `files/FILES`/`files/FILES2` are historical manifests from the original id
  source drop and list many files (asm, DOS/X11 backends) that are **not** part of
  this SDL build — trust the actual `files/*.c` set, not those lists.
