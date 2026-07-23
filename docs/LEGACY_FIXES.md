# Legacy fixes — making 1996 DOOM behave on 2020s hardware

This is the source-backed running log of portability fixes in the current aiDoom tree. The entries cover 64-bit layout, modern compilers, variable internal resolution, large modern WADs, SDL3 tooling and the new AI/asset subsystems. It is a historical fix log: an entry can describe a bug that is already fixed, while the final section gives the current diagnostic symptom.

**Last source audit:** 2026-07-22. When adding a new entry, record symptom → root cause → fix → files and distinguish a historical regression from a still-open limitation.

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
| AI-buddy **voice crash a few minutes into a game** | `lumpcache[]` is `malloc`'d **once** in `W_InitMultipleFiles`, sized to `numlumps` at that moment — the 1996 engine only ever `W_AddFile`s *before* that point. Adding `aidoom.wad` at **runtime** (`I_Voice_Init`) grows `numlumps` but **not** `lumpcache`, so `W_CacheLumpNum` on a buddy lump writes past the array → heap corruption (latent → crash later) | grow `lumpcache` (`realloc` + zero the new slots) right after the runtime `W_AddFile` | `i_voice.c` |
| **HOM / non-continuous edges** on busy hi-res views | `MAXSEGS` (solid-seg clip list) was **32** — far below the worst case (≈viewwidth/2); a complex/hi-res view overruns `solidsegs[]` and corrupts memory. Plus `drawsegs[]` was a fixed `MAXDRAWSEGS` array that **silently dropped** wall segments once full → missing-wall HOM | `MAXSEGS = MAXWIDTH/2 + 8`; make `drawsegs` grow on demand (`realloc`; `MAXDRAWSEGS` = initial cap) | `r_bsp.c`, `r_bsp.h`, `r_segs.c`, `r_plane.c` |

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

## 7. `realloc`-moving an array the zone owns back-pointers into

> **Not strictly a legacy bug.** Vanilla DOOM allocates `lumpcache` exactly once
> and never moves it, so the stock engine never trips this. The *bug* was
> introduced by the fork (the `aidoom.wad` late-load); what's legacy is the
> **invariant it violated** — the zone's owner-back-pointer scheme below. It's
> logged here because that invariant is the reusable lesson and the symptom looks
> exactly like the age-of-code corruption bugs above (§2/§4).

The zone allocator stores, per block, the **address of the owner's pointer**
(`memblock_t.user`) and writes `NULL` through it when the block is purged/freed
(`Z_Free`: `*block->user = 0`). `W_CacheLumpNum` caches every lump with
`Z_Malloc(..., &lumpcache[lump])`, so each cached block's `user` points *into the
`lumpcache` array*. (Modern ports sidestep the whole trap: GZDoom's filesystem
returns RAII `FileData` value objects that own their memory — there is no parallel
`void** lumpcache` with back-pointers to invalidate, so resizing its record array
is harmless.)

| Symptom | Root cause | Fix | Files |
|---|---|---|---|
| Reproducible crash ~5 s into any level, **only with `aidoom.wad` present**, only at `-O2` (vanishes at `/Od` or under a debugger's debug heap) — access violation in `R_DrawSprite`→`R_PointOnSegSide` reading a NULL `drawseg->curline` | `I_Voice_Init` adds `aidoom.wad` *after* `R_Init` has cached lumps, then `realloc`s `lumpcache`. The realloc **moves** the array, leaving every already-cached lump's `block->user` dangling into the freed old array; the next zone purge does `*block->user = 0` into freed/reused heap → corruption that surfaces much later as a NULL drawseg | after the realloc, if the array moved, re-point each live block's owner via new `Z_ChangeUser(lumpcache[i], &lumpcache[i])` | `i_voice.c` (`I_Voice_Init`), `z_zone.c`/`.h` (`Z_ChangeUser`) |

Rule of thumb: **any array that zone blocks hold `user` back-pointers into must
never be `realloc`'d without fixing those back-pointers** (or grow it *before* the
first `Z_Malloc(..., &arr[i])`). The classic tell is a heisenbug — corruption that
only reproduces in the optimized build and disappears under the debugger.

## 8. AI Director TCP link dropped on every map change (and SIGPIPE on disconnect)

Not age-of-the-code, but the same "two halves conspire" shape, logged here for the
AI Director transport.

| Symptom | Root cause | Fix | Files |
|---|---|---|---|
| The external `tools/director.c` client disconnects on every level transition and never reconnects | The engine only services the director socket in `GS_LEVEL` (via `P_Ticker`→`P_AI_Ticker`); during the inter-map `GS_INTERMISSION` it stops answering `observe`, so the director's 5 s `SO_RCVTIMEO` elapses and its worker `break`s out **with no outer reconnect loop** | (engine) service the socket in **every** gamestate via a new `P_AI_NetService()` called from `G_Ticker`; (client) `recv_line` distinguishes `RECV_TIMEOUT` from `RECV_EOF` and the worker is wrapped in a `reconnect:` loop that retries forever | `p_ai_llm.c`/`.h`, `g_game.c`, `tools/director.c` |
| Game dies with exit 141 when the director client drops mid-protocol | a `write()` to the closed socket raises `SIGPIPE` (default action = terminate) | `signal(SIGPIPE, SIG_IGN)` in `P_AI_Init` (non-Windows) so the write fails with `EPIPE` instead | `p_ai_llm.c` |

Rule of thumb: **a long-lived socket peer must tolerate the other side going quiet
(distinguish a timeout from an EOF) and must reconnect; never let a transient stall
end the session permanently. And always `SIG_IGN` SIGPIPE in anything that writes to
a socket the peer may have closed.**

## 9. Vanilla playsim bugs we deliberately KEEP (demo / map compat)

These are *original* DOOM logic bugs, not age-of-the-code ones — but they belong
here for the same reason as §6: so you don't "fix" one and silently break demo
playback and the maps that rely on the exact 1993 behaviour. The playsim is
tic-locked and deterministic (see `CLAUDE.md`); changing any of it desyncs demos and
netplay. If you ever *want* the corrected behaviour, gate it behind a compatibility
flag — this is exactly what Boom's `comp_stairs` does — and never change the default.

- **The stair-builder bug** (`EV_BuildStairs`, `p_floor.c`, ~L531/L537). A staircase
  raises the tagged sector, then walks to the next step by scanning the current step's
  two-sided lines for a neighbour that has this step as its **front** sector *and*
  shares the same floor flat (`floorpic`). Two genuine vanilla bugs live in that walk:
  - **Height is bumped *before* the "already moving" check.** `height += stairsize;`
    runs *before* `if (tsec->specialdata) continue;`. If the next candidate sector is
    already in motion (another stair, a closing door…), vanilla skips it but has
    **already advanced the running height** — so the *next* real step builds one
    `stairsize` too high. Some maps exploit this to make stairs skip a level on purpose.
  - **`secnum` is clobbered by the inner walk** (`secnum = newsecnum;`). The outer
    `while ((secnum = P_FindSectorFromLineTag(line, secnum)) >= 0)` then resumes the
    tag scan from the *last step's* sector index instead of the original trigger — so
    when one tag fires **several disjoint** staircases, the later ones build from the
    wrong place or get skipped. (Boom saves+restores `secnum` to fix this; vanilla
    doesn't, and neither do we.)
  aiDoom ships this code **unchanged on purpose.** Don't "tidy" the increment order or
    the `secnum` reuse — both are load-bearing for compatibility.

---

## 10. Dead Marine (Thing 15) Revival Interference

| Symptom | Root cause | Fix | Files |
|---|---|---|---|
| Player cannot open a door when a dead marine (Thing 15) lies near it; revival also occurs through closed doors/walls | The revival handler checked a 96-unit radius, lacked a line-of-sight check, and returned a warning message string (non-NULL) even when player resources were insufficient. This consumed the player's USE button, preventing door activations | Reduce range to 64 units, add line-of-sight checks (`P_CheckSight`), and return `NULL` on insufficient resources to let the USE action fall through | `revmarine.c`, `p_ai_coop.c` |

---

## 11. Missing Cases in Generalized Thinkers

| Symptom | Root cause | Fix | Files |
|---|---|---|---|
| Generalized doors, floors, and ceilings/crushers get stuck or fail to apply texture/type/speed changes | The dispatcher parsed the generalized specials, but the individual thinkers (`T_VerticalDoor`, `T_MoveFloor`, `T_MoveCeiling`) lacked case statements for the newly introduced generalized enums | Add cases for all generalized enums to handle direction changes, wait countdowns, texture/type transfers (including `oldspecial`/`newspecial`), and speed adjustments when blocked | `p_doors.c`, `p_floor.c`, `p_ceilng.c` |

---

## 12. Tall / large textures from modern PWADs (>254 rows, >64 KB patches)

The 1993 texture composer assumes every texture is ≤254 rows tall and every patch
lump fits in 64 KB — true for the stock IWADs, false for modern content. Legacy of
Rust's `ZZZGATE*` portal textures are **512×512**; the symptom was the **left half
of the portal rendering fine and the right half a scrambled mess of vertical
streaks** (see the LoR MAP01 gate).

| Symptom | Root cause | Fix | File |
|---|---|---|---|
| Right half of a tall/wide texture (e.g. `ZZZGATE1`, 512×512) renders as garbage columns | `texturecolumnofs[]` was `unsigned short` — but the **byte offset** of a column into a >64 KB patch lump exceeds 65535, so every column past ~col 126 truncated (mod 65536) and read the wrong data | make `texturecolumnofs` (and the local `colofs`) **32-bit** (`unsigned`); allocate `width*sizeof(unsigned)` | `r_data.c` |
| Rows below ~254 of a tall single-patch wall texture are garbage | a >254-row solid column can't be one post (post `length` is a byte), so it is stored as **multiple posts using the DeePsea tall-patch convention** (a post whose `topdelta` ≤ the previous is a *cumulative* continuation). The old code read the raw patch column directly as flat pixels, and `R_DrawColumnInCache` used `topdelta` absolutely | force any texture `height > 254` through the composite path (never the direct single-post shortcut), and make `R_DrawColumnInCache` track a running absolute `topdelta` | `r_data.c` |
| `R_GenerateLookup: texture is >64k` I_Error on a big texture | the composite-size guard capped a texture at 64 KB (16-bit `colofs` era) | with 32-bit offsets the cap is unnecessary — removed | `r_data.c` |
| A tall texture composited **correctly** but still drawn as a 128-row band tiled vertically (whole `ZZZGATE` gate scrambled, not just the right half) | every 8bpp column drawer hardcoded the vanilla 128-row wrap `dc_source[(frac>>FRACBITS)&127]`, so a 512-row composite column was only ever sampled in rows 0–127 and repeated 4× | add a per-column `dc_texheight` and a pow2/modulo height-mask in `R_DrawColumn`/`Low`/`Dither`/`TLColumn` (128 → the vanilla `&127`, so no change for stock textures); callers set `dc_texheight` to the real height (walls: `r_segs.c`; posted sprite/masked columns reset it to 128 in `R_DrawMaskedColumn`) | `r_draw.c`, `r_segs.c`, `r_things.c` |

---

## 13. DSDHacked thing numbers collide with aiDoom's appended builtin mobjtypes

A DSDHacked/DECOHack patch (Legacy of Rust's `id1.wad`) defines its new things
starting at **Thing 151** — the vanilla+MBF boundary in dsda-doom, where the patch
author assumed free slots. aiDoom instead packs Heretic/Hexen/Freedoom/etc. monsters
into builtin `mobjinfo[]` indices 138–208, so `Thing 151` lands on `MT_HMINOTAURFX`
and friends. Symptom: **LoR's Ghouls and 3100-range scenery all spawn as
`P_SpawnMapThing: unknown type` and vanish**, because the DEH set (e.g.)
`mobjinfo[150].doomednum = 3007` and then `Heretic_Init()`/`Hexen_Init()`/… — which
ran *after* the DEH — overwrote those slots with their own actors, wiping the
DEH-assigned editor numbers.

**Fix:** run the appended-mobjtype installers (`Heretic_Init`, `Hexen_Init`,
`Freedoom_Init`, `RevMarine_Init`, `Morph_Init`, `HereticInv_Init`) **before**
`D_ProcessDehInWads()` instead of after, so the DEHACKED's Thing edits win. They only
populate static tables, so they're safe that early (before `R_Init`/`P_Init`). File:
`d_main.c`.

---

## 14. Boom `SWITCHES` lump larger than `MAXSWITCHES`

`MAXSWITCHES` was 50; id1.wad's Boom `SWITCHES` lump ships **85 pairs**. The loader's
`index >= MAXSWITCHES*2 - 2` guard silently dropped everything past pair 49 — the
LoR-appended switches (`SW1GATE/SW2GATE`, `SW1BAS1`, `SW1RAIL1/2`, `SW1ROCK6`,
`SW1TC1`) among them. Symptom: **a switch works (the linedef action fires) but its
texture never swaps** because the pair isn't in `switchlist[]`. Fix: raise
`MAXSWITCHES` to 128 (the `alphSwitchList[]` fallback loop breaks at its NUL
terminator, so a larger cap is safe). File: `p_spec.h`.

---

## 15. Medikit "…that you REALLY need!" pickup message never showed

| Symptom | Root cause | Fix | Files |
|---|---|---|---|
| Picking up a medikit at low health always printed the normal "Picked up a medikit." — the "…that you REALLY need!" variant never appeared | id's original `P_TouchSpecialThing` gives the +25 HP (`P_GiveBody`) **before** testing `player->health < 25`, so the medikit's own 25 HP pushes health to ≥25 first and the special message is unreachable (the classic doomwiki bug) | Sample the health into a local **before** healing, then pick the message from that. Message-only / HUD-only, so demos and the playsim are unaffected (unlike the §9 bugs we keep) | `p_inter.c` (`SPR_MEDI` case) |

---

## How to spot the next one

- Compiler warnings `-Wpointer-to-int-cast` / `-Wint-to-pointer-cast` → §1.
- A startup/render glitch that only appears at `-O2` → strict aliasing, §2.
- `Patch … exceeds LFB`, missing HUD elements, a clamped/banded 3D view → §3.
- `Z_Malloc: failed …` or visual corruption that scales with resolution → §4.
- "It built but behaves weird after I edited a header" → §5 (`make clean`).
- A crash that **only happens in the `-O2`/release build and vanishes under a
  debugger or at `/Od`** → heap corruption; suspect an OOB write or a stale zone
  `user` back-pointer (§7). Repro under cdb with `-hd` + `_NO_DEBUG_HEAP=1`.
- **Stairs build to the wrong height, or a second staircase off the same switch is
  wrong/missing** → that's the *vanilla* stair-builder bug (§9). It's not yours to
  fix — it's kept for demo/map compat.
- **Half of a big wall texture is fine and the other half is scrambled vertical
  streaks** (modern/ID24 PWADs with 256/384/512-tall textures) → tall-texture /
  >64 KB-patch limits, §12. If the *whole* texture is a vertically-tiled 128-row
  band instead, the composite is fine but a drawer is still `&127`-wrapping → §12
  (`dc_texheight`).
- **A modern (DSDHacked/ID24) PWAD's new monsters/scenery spawn as
  `unknown type … skipped`** while its remapped vanilla things are fine → the new
  Thing numbers collide with aiDoom's appended builtin mobjtypes, §13.
- **A wall switch triggers its action but its graphic never flips** → the switch
  pair fell off the end of a `SWITCHES` lump bigger than `MAXSWITCHES`, §14.
