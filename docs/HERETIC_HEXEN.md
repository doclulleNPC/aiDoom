# Heretic / Hexen content in aiDoom — current status

**Source audit:** 2026-07-22. This document describes the additive content pack in the current DOOM engine. It is not a claim that aiDoom is a complete Heretic or Hexen game-mode port.

## 1. Architecture

Heretic and Hexen actors are appended to the existing `states[]`, `mobjinfo[]` and sprite tables at runtime:

- `files/heretic.c` / `heretic.h` — Heretic actors and native-sprite remapping;
- `files/hexen.c` / `hexen.h` — Hexen actors;
- `files/p_inv_heretic.c` — Heretic artifact pickups/effects;
- `files/freedoom.c` — FreeDoom DOOM2 actor clones.

The installers run early in `files/d_main.c`, before DeHackEd application and renderer sprite initialization. This keeps the generated DOOM tables intact and lets DEH/DSDHacked edits win when editor-number slots overlap.

## 2. Heretic additive pack

### Implemented

All ten current Heretic monster actors are present in the additive table:

- Golem / mummy;
- Sabreclaw / clink;
- Gargoyle / imp;
- Undead warrior / knight;
- Weredragon / beast;
- Disciple / wizard;
- Ophidian / snake;
- Maulotaur / minotaur;
- Iron Lich;
- D'Sparil (simplified).

They use the ported state/action definitions in `files/heretic.c`, can be spawned through the current summon/director paths, and have the implemented attack/death simplifications documented in the source. Ranged actors use the corresponding appended projectile actors.

The Heretic additive actor set is available when the required parsed sprite assets are present. `P_Director_HereticAvailable` checks the installed sprite frames before the director adds the pool.

### Assets and sprites

The tooling can produce a palette-converted `hereticstuff.wad`, but the current Heretic IWAD path also remaps the appended H* sprite names to native Heretic four-character sprite codes before `R_Init` (`Heretic_RemapNativeSprites` in `files/heretic.c`). Do not describe the current path as “always invisible” or as requiring only the old renamed overlay.

Heretic sounds are resolved through the native Heretic names in `heretic_mode` with a `ds<name>` fallback; missing DOOM-only sounds degrade to silence instead of aborting (`files/i_sound.c`).

### Artifacts

The artifact module currently has ten pickup actors/states, but the original source comment still says “eight”; the runtime enum/initializer includes flask, urn, tome, torch, bomb, ring, shadow, chaos, wings and egg. The effects include healing, berserk/tome behavior, infrared, invulnerability, invisibility, teleport, time bomb, flight support and morph-egg/chicken behavior where the generic subsystem is active.

The additive artifact inventory is usable from the current inventory/console paths. The pickup actors intentionally use `doomednum=-1`, so the items are not map-placed in ordinary DOOM/Heretic maps yet; full mode-specific map placement and always-on Heretic UI remain part of the plan.

## 3. Hexen additive pack

`files/hexen.c` currently installs ten Hexen actors:

| Actor | Current behavior |
|---|---|
| Ettin (`MT_XETTIN`) | Melee brute |
| Centaur (`MT_XCENTAUR`) | Melee brute |
| Slaughtaur (`MT_XSLAUGHTAUR`) | Melee plus projectile |
| Chaos Serpent (`MT_XDEMON`) | Melee plus fire projectile |
| Afrit (`MT_XFIREDEMON`) | Flying ranged actor |
| Reiver/Wraith (`MT_XWRAITH`) | Floating melee/ranged actor |
| Dark Bishop (`MT_XBISHOP`) | Floating caster |
| Wendigo/Ice Guy (`MT_XICEGUY`) | Floating ice projectile actor |
| Stalker (`MT_XSTALKER`) | Ambush/melee/spit actor |
| Death Wyvern (`MT_XDRAGON`) | Flying boss-style guard actor |

Each current ranged actor has an appended projectile. Multi-stage rituals, teleport/summon behavior and full Hexen `special1/2` semantics are simplified or omitted. Heresiarch/Korax are not full boss implementations, and the Hexen weapon/player system is not ported.

The current rule director selects Hexen trash from `MT_XETTIN` through `MT_XSTALKERBOSS` when `SPR_XETT` is available; it can select `MT_XDRAGON` as a rare objective guard. The asset probe is sprite-based, not a launcher-state API.

## 4. Asset extraction

The repository tools can extract/rename palette-converted assets for the additive pack:

```sh
python3 tools/extract_heretic_monsters.py
python3 tools/extract_hexen.py
```

The Hexen extractor writes the collision-free X* sprite namespace and the map used by the C table. Sounds are reused/loaded according to the current source path; the Hexen actor file explicitly notes that DOOM SFX are still reused for Hexen.

These tools are asset preparation, not a replacement for the behavior/player/game-mode work.

## 5. Still not a full Heretic/Hexen mode

The following remain incomplete:

- Corvus player and Heretic weapon set/tome modes;
- complete Heretic/Hexen status bars, menus, intermissions and finales;
- complete Heretic/Hexen line/sector specials and level progression;
- Hexen classes, weapons, artifacts and ACS/polyobject behavior;
- full Heresiarch/Korax and D'Sparil multi-phase behavior;
- all map-placeable content and native mission-specific UI.

See `docs/HERETIC_SUPPORT_PLAN.md` for the full game-mode plan and `docs/INVENTORY.md` for the current artifact boundary.

## Source map

- Heretic actors/remap: `files/heretic.c`, `files/heretic.h`.
- Hexen actors: `files/hexen.c`, `files/hexen.h`.
- Artifacts/morph: `files/p_inv_heretic.c`, `files/p_morph.c`.
- Map thing resolution: `files/p_mobj.c`, `files/heretic.c`.
- Sound fallback/native lookup: `files/i_sound.c`.
- Director pools/availability: `files/p_ai_director.c`.
- Startup ordering: `files/d_main.c`.
