# HERETIC_SUPPORT_PLAN.md — plan for full Heretic support

This is the forward-looking plan for turning the current additive Heretic actor/artifact support into a real Heretic game mode. The current source already has a phase-1 boot/survival path and an additive actor pack; the work below must not be read as saying those pieces are absent.

## Current status

### Implemented foundation

- `files/d_main.c` detects an IWAD whose basename contains `heretic` and sets `heretic_mode`.
- Heretic map thing numbers are resolved through `P_HereticThingType` in `files/heretic.c`; unsupported things are skipped instead of aborting.
- `Heretic_Init` appends the current Heretic actors; `HereticInv_Init` appends the artifact pickup actors/effects.
- Native Heretic sprite names are remapped before `R_Init` builds sprite frames.
- Missing DOOM-only sound lumps fall back to silence; missing weapon/status/switch art is guarded to avoid startup crashes.
- The current artifact module appends ten pickup actors/effects (`files/p_inv_heretic.c`); full Heretic map placement and the always-on native inventory UI remain planned.
### Explicit boundary

`heretic_mode` is not yet a complete mission/game-mode switch. The current engine still uses the DOOM player, weapons, status bar, menu, specials and progression paths in places where Heretic-specific behavior is required. Some Heretic map content is therefore skipped or falls back safely.

## Target

Support `heretic.wad` as a real playable game mode while keeping ordinary DOOM startup behavior unchanged. The safest architecture is still a mission-selected active data/subsystem set, but the existing additive installer pattern should be reused only where it does not create hard-coded enum/table conflicts.

## Phases

### 1. Detection and boot hardening — substantially present

Keep the current detection and skip/fallback guards, but make the mode explicit in startup diagnostics and centralize the mission decision. The mode must be selected before sprite/sound/player tables are initialized.

### 2. Full Heretic data tables

Port or integrate the complete Heretic `states[]`, `mobjinfo[]`, sprite-name and sound tables, including player, weapons, items, decorations and map-placeable things. Avoid using the additive monster enum numbers as if they were a full alternate Heretic table.

Required decisions:

- active table pointers versus a separate mission namespace;
- how DeHackEd/DSDHacked applies in Heretic mode;
- how renderer sprite indices remain valid;
- savegame/demo versioning for a different mission.

### 3. Corvus player and weapons

Port the Heretic player fields, ammo types, weapon ownership/switching, eight weapons, Tome of Power modes and projectiles from the C reference source. The current artifact system must become the always-on Heretic inventory path rather than a console-only additive convenience.

### 4. Artifact/map integration

Assign the real Heretic doomed numbers to map-placeable artifacts in Heretic mode, while retaining collision-safe additive numbers in ordinary DOOM mode. Wire inventory selection/use/drop, Wings, Morph Ovum and mode-specific HUD behavior to the active mission.

### 5. Level specials and map semantics

Port Heretic wind/current, friction/scroll, door/lift timing, ambient/flicker and related sector/line differences. Keep fixed-point/tic-locked semantics and do not silently apply Heretic specials to ordinary DOOM maps.

### 6. Status bar and HUD

Add the Corvus chain health display, ammo/artifact ribbon, Tome flash and mission-specific HUD assets. The current `HU_Buddy`/inventory overlay is not a substitute for the Heretic status bar.

### 7. Menus, intermission, finale and title

Add Heretic title/help/menu graphics, episode/intermission flow and finale pages behind `heretic_mode`. Guard DOOM-only lump lookups; do not solve missing art by masking every lookup with a random DOOM patch.

### 8. Sound and music

Complete the Heretic SFX table and map every native sound name. Reuse the current MUS/FM playback only where the format/patch semantics match; validate music lookup separately from SFX fallback.

### 9. Episodes, skill and saves

Implement Heretic episode/skill names, progression and warp rules. Define a mission-aware save signature and reject incompatible saves cleanly rather than loading a DOOM player/table layout into Heretic state.

## Stop-points

Each phase should leave a runnable artifact:

1. mode detection + safe boot;
2. active Heretic tables and a walkable map;
3. Corvus/weapons;
4. status/inventory;
5. specials;
6. shell/progression/finale;
7. save/demo compatibility.

Do not claim “full Heretic” until a real Heretic map can be played from start through progression with native weapons, status, specials and audio.

## Risks

- One binary with alternate active tables touches many hard-coded DOOM enum accesses.
- Appended additive actors and mission-native actors can collide in editor numbers/sprite names.
- `mobj_t` extras such as `special1/2` affect savegame layout and require the auto-versioning process.
- Heretic's renderer palette and status art cannot be treated as optional if visual correctness is the goal.
- DOOM regression testing must remain a separate gate after every mission change.

## Related

- `docs/HERETIC_HEXEN.md` — additive actor/asset status.
- `docs/INVENTORY.md` — artifact implementation boundary.
- `files/heretic.c`, `files/p_inv_heretic.c`, `files/d_main.c` — current foundation.
- `CLAUDE.md` — build, save-version and engine architecture constraints.
