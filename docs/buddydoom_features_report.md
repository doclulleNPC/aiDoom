# Source-audit snapshot: BuddyDoom / Woof / ZDoom

This document is a source-backed audit snapshot, not a dated generated report. It compares the current BuddyDoom tree with the external Woof/ZDoom references named in the table; external version cells are intentionally qualified because they were not re-audited in this pass.

- `BUDDYDOOM_VERSION "0.10.26"` (`files/buddydoom_version.h`);
- `VERSION_NUM = 118` and `DEMOVERSION = 109` (`files/doomdef.h`).

The comparison rows below retain the useful audit conclusions while correcting the old version metadata and the extended-node boundary.

| Category | BuddyDoom `files/` | Woof `src/` | ZDoom `src/` |
|---|---|---|---|
| Stand/version | Current tree: `BUDDYDOOM_VERSION 0.10.26`; savegame version `118`; stock demo version `109`. | External comparison snapshot; refresh independently. | External comparison snapshot; refresh independently. |
| MBF21 | DeHackEd/DSDHacked runtime growth and several MBF/MBF21 codepointers are present. `A_SpawnObject`, weapon sound/alert and related pointers are implemented; `A_LineEffect` remains a stub. Flags2/group gameplay effects and the full MBF21 surface are not complete. | External comparison snapshot. | External comparison snapshot. |
| Boom | Generalized specials, scrollers/friction/pushers, sky/silent teleport and translucency paths are present. Extended BSP support is **uncompressed XNOD only**; compressed ZNOD and GL variants are not handled. | External comparison snapshot. | External comparison snapshot. |
| DEH/BEX/MapInfo | Text DEH/BEX loading, embedded `DEHACKED` processing, `[CODEPTR]`, MBF21 thing fields and UMAPINFO support exist. BEX string replacement remains limited/stubbed; no ZDoom DECORATE/ZScript/MAPINFO stack. | External comparison snapshot. | External comparison snapshot. |
| ID24 | Generated ID24 tables and selected `GAMECONF`, `DEMOLOOP`, `SKYDEFS`, LoR SBARDEF and inventory/special handling are present. This is a subset, not a full ID24 compliance claim. | External comparison snapshot. | External comparison snapshot. |
| Engine limits | `spechit` and several map/render structures grow dynamically; current source values include `MAXVISSPRITES=128`, `MAXVISPLANES=1024`, `MAXPLATS=8192`, with drawsegs growth. These are current implementation limits, not universal modern-map support. | External comparison snapshot. | External comparison snapshot. |
| Other formats/features | No ACS, DECORATE or ZScript parser. UMAPINFO exists. No claim of ZNOD/GL node support or full MUSINFO parser. Savegames use `VERSION_NUM`; demos use pinned `DEMOVERSION`. | External comparison snapshot. | External comparison snapshot. |

## Source anchors

- Version: `files/buddydoom_version.h`, `files/doomdef.h`.
- DeHackEd/DSDHacked: `files/d_deh.c`, `files/dsdhacked.c`, `files/p_mbf.c`, `files/p_pspr.c`.
- Boom/generalized specials: `files/p_genlin.c`, `files/p_boomsp.c`, `files/p_spec.c`, `files/p_telept.c`.
- Extended nodes: `files/p_setup.c` (`XNOD` path; `ZNOD`/GL not handled).
- ID24: `files/id24.c`, `files/id24_gen.h`, `files/d_main.c`, `files/r_skydefs.c`, `files/st_sbardef.c`.
- Renderer limits: `files/r_things.h`, `files/r_plane.c`, `files/r_segs.c`.

## How to refresh this snapshot

1. Run `git describe --always --dirty` from the repository root.
2. Read `files/buddydoom_version.h` and `files/doomdef.h`.
3. Re-check every source symbol named in the BuddyDoom column with `search_files`/`read_file`.
4. Keep external Woof/ZDoom versions qualified unless their source trees were audited in the same pass.
5. Do not copy a generated version number from a previous build: `build.sh` increments the fork patch field on rebuild.
