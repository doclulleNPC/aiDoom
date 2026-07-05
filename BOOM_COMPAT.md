# Boom / MBF map compatibility

The MBF21/DeHackEd **gameplay** port (actors, codepointers, flags — see `MBF21_PORT.md`) is done.
This doc tracks the **map/level** side of Boom/MBF: the features a Boom-format WAD's *maps* use.
These live in the map loader (`p_setup.c`), the sector/line specials (`p_spec.c` + friends) and the
renderer, not in DeHackEd.

Reference ports (siblings): **`../Nugget-Doom`** (Woof/MBF lineage — full Boom), and
**`../crispy-doom`** (limited Boom — has generalized specials + some transfers, but not extended
nodes). `../winmbf` is the original MBF (has the specials, not extended nodes).

## Current state (baseline)

- **Nodes:** vanilla only — `P_LoadNodes` reads `mapnode_t` directly, no `ZNOD`/`XNOD`/`ZGLN`/
  DeePBSP detection. Maps built with ZDBSP extended nodes (most large modern maps) won't load.
- **Line specials:** vanilla set only (up to ~141). No Boom **generalized** specials (`>= 0x2000`:
  GenFloor/GenCeiling/GenDoor/GenLockedDoor/GenLift/GenStairs/GenCrusher).
- **Sector specials:** vanilla cases only (1–17). No Boom **generalized** sector types (bitfielded:
  the low 5 bits are the damage/light type, plus SECRET/FRICTION/PUSH/… bits).
- **Transfers / thinkers:** vanilla wall scroll only. No deep water (242), friction (223), wind/
  current/pusher (224/225/226), floor/ceiling/wall scrollers (245–255), transfer-height, colormap.
- **Lumps:** `ANIMATED`/`SWITCHES` (Boom custom animated flats/textures + switch pairs) not loaded —
  `P_InitPicAnims` uses the hardcoded vanilla table.

## Plan (phased, each a testable milestone)

- [x] **B1. Extended nodes** -- DONE (ZDBSP XNOD). — detect + load ZDBSP `XNOD`/`ZNOD` (and DeePBSP `xNd4`) in
  `P_LoadNodes`/`P_LoadSubsectors`/`P_LoadSegs`: 32-bit subsector/seg indices, rebuilt vertices.
  The loading gate for large maps. (crispy lacks this; port from Nugget/dsda.)
- [x] **B2. Generalized linedef specials** -- DONE (ported p_genlin.c). — recognise `line->special >= 0x2000` and decode the
  bitfields (type + speed + model + direction + delay + target/change) into the existing
  door/plat/floor/ceiling/stair/crusher builders. Boom maps' doors/lifts/floors.
- [x] **B3. Generalized + extended sector types** -- DONE. — treat `sector->special` as bitfielded when
  `>= 32`: low bits = damage/light preset, plus SECRET/FRICTION/PUSHPULL bits; keep vanilla ≤17.
- [x] **B4. Boom transfers & thinkers** -- DONE (scrollers, friction, pushers, flat-scroll render, and 242 transfer-heights). — scrollers
  (245–255, wall + carry), **friction (223)**, **wind/current/point-push-pull (224–226)** and the
  **visual flat scroll (250/251/253)** are done. Friction/pushers: `p_boomsp.c`
  (`P_SpawnFriction`/`P_SpawnPushers`/`T_Pusher`); `p_map.c` (`P_GetFriction`/`P_GetMoveFactor`
  hooked into `P_XYMovement` coast-down and `P_MovePlayer` thrust); `MT_PUSH`/`MT_PULL` things +
  `S_TNT1` idle state in `info.{h,c}`. aiDoom has no msecnode touching-sector list, so friction uses
  the object's **centre sector** and constant pushers scan `sector->thinglist`. Flat scroll: the
  `floor_xoffs`/`ceiling_xoffs` the scroll thinker accumulates are now carried on `visplane_t`
  (`R_FindPlane` keys on them so differently-scrolled planes don't merge) and added to the span
  texture coords in `R_MapPlane`. **Deep water / fake floor+ceiling / glass floor (242)** now works:
  `sector_t.heightsec` points at the control sector (set in `P_SpawnSpecials`); `R_FakeFlat`
  (`r_bsp.c`, ported from `../winmbf`) swaps in the control sector's heights/flats/offsets for
  rendering, wired into `R_Subsector` (frontsector) and `R_AddLine` (backsector). Purely visual --
  collision uses the real heights. Verified against BOOMEDIT (no more HOM; the fake flat renders).
- [x] **B5. ANIMATED / SWITCHES lumps** -- DONE. — load Boom's custom flat/texture animation + switch
  tables in `P_InitPicAnims` / the switch init, falling back to the vanilla table when absent.
  (Fix: `p_switch.c` needed `#include "w_wad.h"` — without it `W_CacheLumpName` was implicitly
  `int`-declared and its 64-bit lump pointer was truncated to 32 bits → `P_InitSwitchList` segfault.)
- [x] **B6. Verify** -- DONE. Vanilla DOOM/DOOM2 + Brutal.wad unaffected; `BOOMEDIT.WAD` (TeamTNT's
  Boom test map: generalized specials + `ANIMATED` + `SWITCHES`) loads and runs on `doom2.wad`; XNOD
  extended-node maps load/render. Full Boom-map *behaviour* (deep water, friction, pushers) still
  needs the B4 renderer/thinker hooks below to be visible in play.

## Extra Boom specials wired (beyond B1–B6)
- **Silent teleporters** (`p_telept.c` `EV_SilentTeleport`/`EV_SilentLineTeleport`, dispatched in
  `P_CrossSpecialLine`): thing-exit 207/208 + monster 268/269, and linedef-to-linedef 243/244
  (+ reversed 262/263, monster 264–267). No fog/sound, angle+height preserved and **momentum
  rotated**, so a 252 conveyor can carry things (BOOMEDIT's candles) through a teleporter and have
  them re-emerge still moving — the "loop the belt" effect. Ported from `../winmbf` (Killough).
- **Generalized-switch feedback**: `P_DoGenLineSpecial` now calls `P_ChangeSwitchTexture` on a
  switch/use (`actclass==1`) activation, so Boom generalized lifts/doors/etc. toggle their switch
  texture + play the switch sound instead of silently doing nothing ("use does nothing").

## Notes
- Keep the playsim deterministic (fixed-point, tic-locked). Boom specials are all integer.
- Boom bumped several **on-disk limits** (segs/subsectors/nodes) to 32-bit; the vanilla loaders use
  16-bit `short` indices — extended-node loading must use 32-bit and mark `NF_SUBSECTOR` at bit 31
  (not the vanilla bit 15).
- Complevel note: this targets **Boom (cl 9) / MBF (cl 11)** map features, not ZDoom/UDMF.
