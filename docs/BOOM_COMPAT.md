# BOOM / MBF map compatibility

This document describes the Boom/MBF-era map and gameplay support that is present in the current BuddyDoom source. It is an as-built compatibility note, not a claim of full ZDoom, UDMF, or extended-node compatibility.

## Current state

### Implemented

- Boom generalized line specials: floors, ceilings, doors, locked doors, lifts, stairs and crushers (`files/p_genlin.c`, `files/p_spec.c`).
- Generalized sector effects: friction, wind, push/pull and related Boom sector bitfields (`files/p_spec.c`, `files/p_boomsp.c`).
- Scrollers and carry behavior, friction and pushers (`files/p_boomsp.c`, `files/p_spec.c`).
- Deep-water and sky-transfer specials supported by the current special tables.
- Silent teleports and the current Boom/ID24 special extensions (`files/p_telept.c`, `files/p_boomsp.c`).
- Translucency through the palette/`TRANMAP` path (`files/r_data.c`, `files/r_draw.c`, `files/r_things.c`).
- `ANIMATED` and `SWITCHES` loading, with a larger runtime switch table than vanilla. `MAXSWITCHES` is currently `128` (`files/p_spec.h`, `files/p_switch.c`).

### Extended nodes

The loader supports **uncompressed ZDBSP `XNOD`** / the corresponding extended node form used by supported PWADs. It reads 32-bit node-related indices and rebuilds the extended vertex/seg data in `files/p_setup.c`.

The current loader explicitly does **not** handle compressed `ZNOD` or GL node variants. Do not describe those formats as supported until `P_LoadNodes_Extended` grows a decompression/GL path.

This distinction matters: a map can contain Boom specials and still fail because its node format is not one of the forms the loader accepts. The launcher should therefore report node-format failures separately from gameplay-special failures.

## Plan and implementation notes

The compatibility target is the Boom/MBF style of fixed-point, tic-locked gameplay. The source does not implement a universal compatibility-level switch for every port-specific quirk. It also does not claim complete MBF21, ID24, UMAPINFO, or ZDoom behavior; see `docs/MBF21_PORT.md` and `docs/buddydoom_features_report.md` for those boundaries.

### Limits that are not vanilla hard caps anymore

Several old arrays are dynamic or enlarged:

- `spechit` grows dynamically rather than stopping at the vanilla eight-entry overflow point (`files/p_map.c`).
- Lines and subsectors are allocated from lump sizes; supported `XNOD` data uses 32-bit counts/indices (`files/p_setup.c`).
- `MAXPLATS` is `8192` in the current special subsystem (`files/p_spec.h`).
- The renderer uses `MAXVISSPRITES=128`, `MAXVISPLANES=1024`, and dynamically grows drawsegs beyond their initial capacity (`files/r_things.h`, `files/r_plane.c`, `files/r_segs.c`).

These are implementation limits, not promises that every large modern map will load. Memory, malformed lumps, unsupported nodes and unsupported gameplay constructs can still reject a map.

## Compatibility boundary

This port targets Boom/MBF-style maps and selected modern extensions. It is not a generic UDMF engine and does not provide ZDoom's DECORATE, ZScript, ACS or full MAPINFO stack. UMAPINFO support exists as a separate, narrower loader (`files/u_mapinfo.c`).

`files/p_boomsp.c` and the generalized-special code must remain deterministic: fixed-point arithmetic and the 35 Hz tic flow are part of the compatibility contract.

## Source map

- Extended BSP loading: `files/p_setup.c` (`P_LoadNodes_Extended`, extended subsector/seg loaders).
- Generalized line specials: `files/p_genlin.c`, dispatched by `files/p_spec.c`.
- Scrollers, friction and pushers: `files/p_boomsp.c`, `files/p_spec.c`.
- Teleports: `files/p_telept.c`.
- Switch and animation lumps: `files/p_switch.c`.
- Renderer extensions: `files/r_data.c`, `files/r_draw.c`, `files/r_plane.c`, `files/r_segs.c`, `files/r_things.c`.

## Practical testing

Use a supported IWAD and load the test PWAD through `run/ID0/` or `-file`. Verify in-game rather than treating a successful binary build as map compatibility. For a node-format issue, inspect the map's `NODES`/extended-node lumps; for a special issue, reduce the map to the relevant generalized line/sector feature.
