# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`BuddyDoom` (originally `sdldoom-1.10-mod`) — a fork of Sam Lantinga's 1998 SDL port
of id Software's DOOM (the original 1993 C engine, id Tech 1). The goal of the
fork is to make the old SDL Doom build and run on modern 64-bit Linux with current
SDL, then add modifications (hi-res rendering, a video-options menu, …). All source
lives in `files/`. The window title / app name is set to "BuddyDoom" in
`i_video.c` (`SDL_WM_SetCaption`); the build produces a binary named `buddydoom`
(`bin_PROGRAMS` in the Makefile).

## Build & run

This is the **SDL3** codebase (`files/i_*.c` use `<SDL3/SDL.h>`). The legacy
autotools files (`configure.in`, `Makefile.am/.in`) are **stale** — they target
SDL 1.x and won't link SDL3. Use one of:

- **Linux/macOS:** `./build.sh` — compiles `files/*.c` against system SDL3
  (pkg-config) and **copies the `buddydoom` binary into `run/`** (so the launcher
  finds it there). Requires the SDL3 dev package.
- **Windows:** `nmake /f files\Makefile.msvc` (VS 2019 + SDL3 SDK) → `buddydoom.exe`
  + `SDL3.dll`, with the exe icon from `files/buddydoom.rc`. Or
  **`build_all_win.bat`** (repo root) which finds VS via `vswhere`, sets up the
  **x86** env, and builds the game **plus** the `tools/` apps (`buddydoom_config`,
  `gpumon`, `launcher`), copying every exe + `SDL3.dll` into `run/`.
- **Any platform (CMake):** `cmake -B build && cmake --build build` —
  `CMakeLists.txt` builds the game and the `buddydoom_config`/`gpumon` tools, finds
  SDL3 via `find_package` (a sibling `../SDL3` SDK on Windows, else a system
  install), and stages all binaries + `SDL3.dll` into `run/`.

The game and the **`tools/`** apps (`launcher`, `buddydoom_config`, `gpumon_sdl`,
`director`) are **separate SDL3 programs**, each with its own `tools/Makefile.msvc`
target and `tools/build_*_{win,}.sh` script. They share `run/buddydoom.cfg` and are
staged into `run/`; see `run/README.md` for what each one does and how the
launchers wire the game to the AI director.

`build.sh` is just this (and a `cp` to `run/`):

```sh
cd files && gcc -O2 -g -fcommon -fno-strict-aliasing -std=gnu11 \
  -Wno-implicit-int -Wno-implicit-function-declaration \
  -Wno-int-conversion -Wno-return-mismatch \
  -DSDL_MAIN_HANDLED $(pkg-config --cflags sdl3) \
  *.c -o buddydoom $(pkg-config --libs sdl3) -lm
```

Why the odd flags: the 1996 id source is K&R-ish, so modern gcc needs the
`-Wno-*` set (gcc 14 makes implicit-int / implicit-function-declaration /
int-conversion hard errors), and **`-fno-strict-aliasing` is required** — the
engine type-puns all over (e.g. `*(int *)lump->name`) and `-O2` miscompiles it
otherwise (symptom: bogus errors like "missing rotations"). `-DSDL_MAIN_HANDLED`
because `i_main.c` owns `main()`. `build.sh` recompiles every `.c`, so there's no
header-dependency-tracking pitfall.

**Auto-versioning (in `build.sh`).** Two version numbers bump automatically:
- **Fork version** (`files/buddydoom_version.h`, `BUDDYDOOM_VERSION`, shown in the window
  title) — the patch field is bumped **+0.0.1 on every build**. Bump major/minor by
  hand when cutting a release tag. The file is auto-generated; expect it dirty after
  every build.
- **Engine version** (`VERSION_NUM` in `doomdef.h`, the `1.xx` stamped in savegame
  headers) — `build.sh` fingerprints the sizes of every struct the savegame
  memcpy's (`p_saveg.c`: `player_t`, `mobj_t`, `ceiling_t`, `vldoor_t`,
  `floormove_t`, `plat_t`, `lightflash_t`, `strobe_t`, `glow_t`) into
  `files/buddydoom_saveg.sig`; when that changes it bumps `VERSION_NUM` **+1 (=+0.01)**
  so stale saves are cleanly **rejected** ("bad version") instead of crashing on
  load. So: change any of those structs → engine version auto-bumps.

**App icon:** the live window/taskbar icon is embedded from `files/buddydoom.ico`
into `files/buddydoom_icon.h` (a 64×64 RGBA array) and set via `SDL_SetWindowIcon`
in `i_video.c`; the Windows `.exe` icon comes from `files/buddydoom.rc`. Regenerate
the header from the `.ico` with ImageMagick if the icon art changes.

Output binary is `buddydoom` (`-iwad <wad>` / auto-detected IWAD; **bring your own**
IWAD). **Game WADs + savegames live in `run/ID0/`** — the engine/launcher/tools
search there first, so bare names (`-iwad DOOM.WAD`, `-file doom2stuff.wad`)
resolve without a path (`d_main.c` IWAD dirs + `w_wad.c` `W_AddFile` ID0/ fallback;
savegames via `SAVEGAMENAME` in `dstrings.h`). To launch, use the **`run/launcher`
GUI** (the old `start_*` scripts are obsolete, kept as a backup in `tools/scripts/`)
— see `run/README.md`.

### 64-bit portability (LP64) caveats

> A full running log of age-of-the-code fixes (64-bit, modern compiler, hi-res,
> undersized buffers, tooling) lives in **`docs/LEGACY_FIXES.md`** — add new ones there.

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
./buddydoom               # auto-detects doom.wad/doom1.wad/doom2.wad/plutonia.wad/tnt.wad
DOOMWADDIR=/path/to/wads ./buddydoom
./buddydoom -iwad DOOM1.WAD -warp 1 1 -skill 4 -fullscreen
```

Only the stock commercial/shareware IWADs are supported; this port does **not**
load arbitrary custom WADs as IWADs (PWADs via `-file` work). There is **no test
suite** — verification is done by running the game.

## Architecture

### The AI / "Director" systems

**Defaults:** a plain no-flag launch now runs the **rule-based L4D spawn director**
(`-director`) **plus the AI co-op buddy** (`-coop`, player 2) — both are ON by default
(gated in `P_Director_Init` / `P_AICoop_Init` on `vanilla_mode`, `netgame`, and
`-record`). **`-vanilla`** turns them (and every other modern deviation) off for the
bare 1993 game. Demo playback is unaffected: the director is gated on `!demoplayback`
in `p_tick.c`, and the buddy self-disables because `G_DoPlayDemo` restores
`playeringame[]` from the demo header. The LLM variants (`-aidirector`, `-aicoop`,
`-aiplayer`) are still opt-in. The independent systems — keep them clearly distinct:

- **LLM "AI Director"** (`files/p_ai_llm.c`/`.h`, `-aidirector [port]` or the
  built-in scripted `-aidemo`) — an external director (an LLM, a script, or the
  demo) drives monster *tactics* (flank, fall back, focus-fire, …) over a TCP
  line protocol. Hooks: `A_Chase` (`p_enemy.c`) diverts to `A_LLMChase` for
  directed monsters, `P_Ticker` calls `P_AI_Ticker`, `P_SetupLevel` calls
  `P_AI_Reset`. Directives live in a side-table keyed by `mobj_t*` (no
  struct/savegame change). Protocol + design: **`docs/AGENT_CONTROL.md` §12–13**
  (player control is §1–11). The ready-to-run client is the native SDL3
  `tools/director.c` (talks to Ollama; no Python).
- **L4D-style rule director** (`files/p_ai_director.c`/`.h`, `-director`) —
  *offline/rule-based*, no LLM. Tracks per-tic player STRESS (damage bursts,
  close kills, low ammo) and runs a build-up → peak → relax spawn cycle: spawns
  monsters out of sight behind the player while building tension, drops items
  during relax. The LLM variant reads the same intensity in its `observe`.
  **Exit-aware pressure:** as the player nears the level exit (located by scanning
  for exit specials) spawns get faster, the relax lull shrinks, and more monsters
  are routed *into the exit room*; but when a survivor is critically low on HP **or**
  ammo (`P_Director_Stressed`) hordes and special/boss spawns are suppressed so it
  never death-spirals. **Spoken game-master voice:** a separate ElevenLabs persona
  (`DD*` lumps, `P_Director_Voice`/`P_Director_Say`) narrates spawns/phases/items
  — see **`docs/BUDDY_VOICE.md`**.
- **AI co-op companion / "buddy"** (`files/p_ai_coop.c`/`.h`, `-aicoop`) — a
  second marine (player 2) filled by a small built-in bot each tic: acquires the
  nearest visible monster and fires, seeks health when hurt, else follows you.
  A real co-op player (weapons/damage/pickups/reborn all work). Has its own HUD
  (`hu_buddy.c`, top-of-screen strip, config `show_buddy_hud`) and an optional
  spoken voice (see below). Design docs: **`docs/BUDDY_HUD.md`**, **`docs/BUDDY_PORTING.md`**.
- **Pack-hunt monster AI** (`p_enemy.c`, config `monster_pack 1`) — monsters
  acquire the player on spawn (even with no LoS) and steer toward nearby allies,
  so they gather and assault in groups.
- **LLM/agent player control** (`files/g_agent.c`/`.h`, `-aiplayer [port|demo]`) — drives
  the *human marine* (player 1) instead of the keyboard. A slow external BRAIN (an LLM via
  `run/llm_player.py`, or the built-in `demo` brain) issues high-level intents over a TCP
  line protocol (`map`/`observe` + `goto/target/attack/use/…`); a 35 Hz C REFLEX in
  `G_AgentBuildTiccmd` turns them into ticcmds (buddy-grade nav, kiting, door-use, weapon-up).
  Hook: `G_BuildTiccmd` (`g_game.c`). Full as-built reference: **`docs/AIPLAYER.md`**.

Related gameplay flags worth knowing: `-infight` (same-species infighting),
`-nofriendlyfire`/`-noff` (player ↔ buddy don't hurt each other), `-infinitetall`
(revert to vanilla "infinitely tall actors" — over/under 3D object clipping is **on by
default**, `over_under` in `p_map.c` `PIT_CheckThing`: walk under flying things / stand on
top of things), `-autoaim` (restore vanilla vertical aim-assist — it is **off by default**
now, `autoaim` in `p_pspr.c`/`p_mobj.c`: the human shoots straight along the free-look pitch
so shots can be placed/headshot; the AI buddy keeps autoaim), plus free-look (mouse pitch,
`r_main.c`/`p_user.c`) and jump (`g_game.c`/`p_user.c`). Console cheats include `notarget`
(monsters ignore the human).

### Other major subsystems added by the fork

- **Quake-style console** (`files/c_console.c`/`.h`) — toggle with **F12** or
  **`` ` ``** (backquote); scrollback + input line over a dimmed view. Note the
  drawer is split: `C_Responder` handles input and `C_Printf`/`C_GetLine` own the
  text, but the actual pixels are drawn by the **SDL overlay in `i_video.c`**
  (`C_Drawer` is a legacy no-op). Commands: `help clear echo quit god noclip give
  map`/`warp`, plus buddy commands `where come wait attack report buddygod buddyarm
  buddyhome` (the last teleports the buddy back to its map spawn point).
- **MUS music playback** (`files/i_mus.c`/`.h`) — a from-scratch MUS sequencer +
  2-operator FM synth driven by the IWAD's `GENMIDI` patches (OPL/Adlib-style, no
  ZMusic/external dep). `MUS_Render` mixes into the same SDL audio path as SFX and
  the OGG music in `i_sound.c`. Output is 11025 Hz stereo S16.
- **AI buddy + Director voice** — pre-baked ElevenLabs clips packed into the
  `run/buddydoom.wad` PWAD by `tools/bake_buddy_voice.py` (one bake, two personas:
  buddy = **Joker-HL** `DS*` lumps, AI Director = **UT** `DD*` lumps). `i_voice.c`
  plays them by tag on **two separate SDL streams** (buddy positional via
  `I_Voice_Say`; Director "voice of god" via `I_Director_Say`), so both can talk at
  once. No live TTS at runtime. Mind the 8-byte lump-name rule (below). Design:
  **`docs/BUDDY_VOICE.md`**.
- **Multiplayer netcode client** (`files/d_netcl.c`/`.h`) — a clean-room
  reimplementation of the Chocolate/Crispy-Doom network protocol (connection state
  machine + reliable layer + GAMEDATA tic windows). Transport-side **only**;
  splicing it into `D_DoomLoop` is unfinished. Separate from the original
  peer-to-peer `d_net.c`.

There is a large set of design docs at the repo root (one per feature):
`docs/AGENT_CONTROL.md` (agent/LLM control *design*) and `docs/AIPLAYER.md` (the shipped `-aiplayer`
*as-built*), `docs/BUDDYDOOM_PARAMETERS.md`, `BUDDY_*.md`, `docs/GPUMON.md`,
`docs/Pathfinding.md`, `docs/VISIBILITY_CACHE.md`, `docs/YAPB_ARCHITECTURE.md`, `docs/Collision.md`,
`docs/HD_TEXTURES.md` (how the `../sdldoom-sdl3` sibling does true-color PNG texture/
sprite/voxel replacement — a porting reference, not yet implemented here),
plus `docs/LEGACY_FIXES.md` (the running log of age-of-the-code fixes). Consult the
matching doc before touching a subsystem.

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
- **Video filters** (Options → Video, default off, persisted as `antialiasing`/`blur`):
  `antialiasing` toggles the texture scale mode (`SDL_SCALEMODE_LINEAR` vs `NEAREST`,
  smooths the upscale to the window), `blur` runs a 1-2-1 separable soft blur
  (`I_SoftenFrame`, SWAR) over the 32-bit frame each present (`i_video.c`). VSync is
  forced on (`SDL_SetRenderVSync`) — SDL3 defaults it off, which tore the frame on fast
  strafes.
- **Widescreen (Hor+)** (Options → Video → Widescreen, default off, config `widescreen`):
  crispy-doom-style — `SCREENWIDTH` becomes 16:9 (`SCREENHEIGHT*16/9`, capped `MAXWIDTH`)
  while `NONWIDEWIDTH` stays the 16:10 reference. The 3D projection/focal length use the
  **non-wide** centre (`centerxfrac_nonwide`, `r_main.c`) so the per-column angle matches
  4:3 and the extra width shows *more world* at the sides (vertical FOV unchanged). HUD
  uses `WIDESCREENDELTA` (half the extra width, BASE coords): the status bar renders
  **centred over a full-height view** (so the game shows on both sides of it, not a stone
  bezel) — drawn after `R_RenderPlayerView` in `D_Display`, every-frame refresh; all `ST_*X`
  defines add `WIDESCREENDELTA` (0 in 16:10). `V_DrawPatch`/`V_CopyRect` X bound is
  `SCREENWIDTH/hires` (the wide base width), not `BASE_WIDTH`. In 16:10 every `*_nonwide`
  equals its wide value and `WIDESCREENDELTA==0`, so non-widescreen is unchanged.
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
  `i_sound.c` (SDL_audio callback mixer — software SFX channels + OGG music +
  the `i_mus.c` FM synth), `i_mus.c` (MUS music player),
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
  RNG table), `hu_*` (heads-up text/messages, incl. `hu_buddy` for the co-op
  companion HUD), `st_*` (status bar), `am_map` (automap), `wi_stuff`
  (intermission), `f_finale`/`f_wipe` (end screens, screen melt), `c_console`
  (developer console), `s_sound` (sound/music driver, calls into `i_sound`),
  `v_video` (screen buffer / patch drawing primitives), `info.c`/`tables.c` (huge
  generated data: actor state machine and the finesine/tangent lookup tables).

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
- **WAD lump names are 8 bytes max, with the trailing byte as NUL.** When you
  bake an asset into `*.wad` (e.g. via `tools/bake_buddy_voice.py` for the buddy
  voice PWAD in `run/buddydoom.wad`), every lump name is stored as exactly 8 bytes;
  if the human-readable name is shorter than 8 chars, the remaining bytes must be
  `\0`. A 7-char name in the WAD and the same 7-char name as a C string literal
  in the lookup table are equivalent (both pad with NUL), but an **8-char C
  literal** for the same WAD entry will not match because `W_CheckNumForName`
  treats the embedded NUL as the terminator and the WAD entry has its NUL one
  byte earlier. Always verify a hand-edited lump name against the on-disk PWAD
  before assuming a "silent asset" is a missing asset.
