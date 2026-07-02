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
- [~] **B4. Boom transfers & thinkers** -- scrollers DONE (wall + carry); flat-scroll render, friction, pushers, deep-water remain. — deep water (242 transfer-height), friction (223), wind/
  current/pusher (224–226), scrollers (245–255), and the associated `p_spec` thinkers. Renderer
  hooks for transfer-height (fake floor/ceiling) are the hard part.
- [x] **B5. ANIMATED / SWITCHES lumps** -- DONE. — load Boom's custom flat/texture animation + switch
  tables in `P_InitPicAnims` / the switch init, falling back to the vanilla table when absent.
- [~] **B6. Verify** -- structural: vanilla unaffected + XNOD map loads/renders; full Boom-map behaviour needs real Boom WADs. — a Boom-format test map (generalized specials + extended nodes) loads and
  plays.

## Notes
- Keep the playsim deterministic (fixed-point, tic-locked). Boom specials are all integer.
- Boom bumped several **on-disk limits** (segs/subsectors/nodes) to 32-bit; the vanilla loaders use
  16-bit `short` indices — extended-node loading must use 32-bit and mark `NF_SUBSECTOR` at bit 31
  (not the vanilla bit 15).
- Complevel note: this targets **Boom (cl 9) / MBF (cl 11)** map features, not ZDoom/UDMF.
