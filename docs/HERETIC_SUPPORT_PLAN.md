# HERETIC_SUPPORT_PLAN.md — plan for **full** Heretic support

Where we are vs. where "full Heretic" is, and a phased path between them.

## Status
- **Phase 1 — DONE** (commit `557a620`): `heretic.wad` is detected (`heretic_mode`), and a
  Heretic level now **loads and runs without crashing** — `P_SpawnMapThing` resolves the
  Heretic doomednums of our 10 ported monsters + 10 artifacts (`P_HereticThingType`) and
  skips unported things instead of `I_Error`-ing, plus a chain of `heretic_mode`-gated
  boot-survival guards (missing DOOM sfx → silence, missing music/switch/HUD/status-bar/
  sprite lumps → skip). 39 monsters spawn on E1M1; **DOOM is byte-for-byte unaffected**.
  *Caveat:* the Heretic content is still **invisible/silent** — the native Heretic sprites,
  palette, weapons, status bar, keys, specials, sound and music are not wired yet (below).
- **Phases 2–9 — TODO.** Next highest-value: make the content visible (Phase 2 data tables:
  native Heretic sprite names + the Heretic palette) and the player armed (Phase 3 weapons).

## Where we are now (the additive pack)
aiDoom plays **DOOM**; Heretic content is bolted on as an optional *pack* over the DOOM engine
(approach A in `HERETIC_HEXEN.md`):
- **Monsters** — all 10, appended at runtime (`files/heretic.c`, `Heretic_Init`), summonable +
  director-spawned when `hereticstuff.wad` is loaded.
- **Artifacts** — all 10, the Heretic inventory effects (`files/p_inv_heretic.c`); flight +
  chicken-morph are generic subsystems (`INVENTORY.md`). Currently console-given.
- **Sounds** — the Heretic SFX (`DS`-prefixed, `sfx_h_*`).
- **Assets** — `tools/extract_heretic_monsters.py` palette-converts Heretic sprites/sounds
  into `run/ID0/hereticstuff.wad`.

What's missing for **full** Heretic = playing `heretic.wad` as a real game: the Corvus player,
the 8 weapons, Heretic levels/specials, the status bar, ammo/episode/menu/finale — i.e. a
**Heretic game mode**, not a DOOM level with Heretic monsters sprinkled in.

## Target: a Heretic GAME MODE (approach B)
Chocolate-Heretic ships as a *separate binary* with Heretic's own `info.c`/`p_*`/`sb_bar`/…
Doing it **in one binary** means selecting Heretic's data + a few behavior deltas at startup
when `heretic.wad` is the IWAD, while DOOM stays the default. The engines share the same
Chocolate/Doom lineage (BSP renderer, blockmap, thinkers, fixed-point), so the renderer,
collision, and most of `p_map`/`p_mobj` are reusable — the work is **data tables + the
subsystems Heretic does differently**.

## The pieces (each is a phase)

### 1. Game-mode detection & boot
- `IdentifyVersion` (`d_main.c`): recognise `heretic.wad` (its `E1M1`+the `_` lumps / the
  Heretic IWAD header) → set a new `gamemission == heretic` (and a `GameMode`/episode count).
- Pick Heretic vs DOOM **data tables** at boot (see 2). Gate the existing DOOM-only paths
  (status bar, finale, menu graphics) on the mission.

### 2. Data tables (states / mobjinfo / sprites / sounds)
- Heretic's full `states[]` + `mobjinfo[]` (player, weapons, all items/decorations, not just
  the 10 monsters we have). Cleanest: a **second set of tables** selected at boot (like the
  Heretic monster `*_Init` but for the *whole* game), OR compile crispy's `heretic/info.c`
  into a parallel `hinfo.c` and point `states`/`mobjinfo`/`sprnames`/`S_sfx` at it in Heretic
  mode. Enum collisions are the catch — Heretic mode wants its OWN `SPR_`/`S_`/`MT_`/`sfx_`
  numbering, so the engine must index through the *active* table, not hard-coded DOOM enums.
- This is the **biggest** item; it's what makes #3–#8 mostly "port the table + the few funcs".

### 3. The player & weapons (`p_pspr`, `p_user`, `d_items`)
- The Corvus player: Heretic `player_t` extras (the inventory is always-on; `weaponowned`,
  the 6 ammo types `am_goldwand…am_mace`, `maxammo`).
- 8 weapons + **Tome of Power** (each weapon has a normal + tomed firing func): Staff,
  Gauntlets, Elven Wand, Ethereal Crossbow, Dragon Claw, Hellstaff, Phoenix Rod, Firemace —
  port `heretic/p_pspr.c` (the `A_Fire*` funcs + their projectiles).
- Weapon-pickup/switch logic (slots differ from DOOM).

### 4. Inventory (mostly done)
- Reuse `p_inv_heretic.c`; in Heretic mode make the artifacts **map-placeable** with their
  real Heretic doomednums (they're `doomednum=-1` now to avoid DOOM collisions), turn the
  always-on inventory bar on, and wire the "fly"/"morph" the player can suffer (enemy egg).

### 5. Level data & specials (`p_setup`, `p_spec`, `p_floor`/`p_plats`/…)
- Heretic map things resolve through the Heretic `doomednum`→`mobjtype` table (#2).
- Heretic **line/sector specials** differ: wind/current sectors, the friction/scroll, the
  different door/lift speeds & the `P_AmbientSound`/flickering. Port `heretic/p_spec.c` deltas
  (a per-mission branch where they diverge).

### 6. Status bar & HUD (`sb_bar` ≈ our `st_stuff`)
- Heretic's status bar (the inventory ribbon, the health **chain**, the artifact count, the
  ammo icons, the tome flash). A Heretic `st_*` variant drawn in Heretic mode.

### 7. Menu / intermission / finale / title
- Heretic title/credits/help, the intermission tally, the finale text + the underwater/“E2 to
  be continued” screens, the menu graphics (`M_*` Heretic art). Mission-branch in `m_menu.c`,
  `wi_stuff.c`, `f_finale.c`, `d_main.c` page drawer.

### 8. Sound & music
- Heretic SFX names/ids (we have the clips; need the full `sfx_*` table in Heretic mode) +
  the Heretic MUS/MIDI music through the existing `i_mus.c` FM synth.

### 9. Episodes & skill
- Heretic episode/skill menu (3 episodes in `heretic.wad`, 5 in the extended `hexen.wad`-era
  "Shadow of the Serpent Riders"), the warp/`-skill` ("Thou art a Smite-Meister") names.

## Suggested phasing (each ships runnable)
1. **Boot + tables**: detect `heretic.wad`, select Heretic `states/mobjinfo/sprnames/sfx`,
   spawn the Corvus at a Heretic start, walk a Heretic level with the monsters we have
   (no weapons yet → use the staff stub). Proves the data-table switch.
2. **Weapons**: the 8 weapons + Tome (port `heretic/p_pspr.c`).
3. **Status bar + inventory bar** (Heretic `sb_bar`), artifacts map-placeable.
4. **Specials** (wind/scroll/flicker/ambient + the door/lift deltas).
5. **Menu / intermission / finale / music / episodes** — the shell.

## Risks / decisions
- **Dual data tables in one binary** is the crux: either runtime-select pointers
  (`states`/`mobjinfo`/… become `extern`-pointers set at boot) — touches every file that
  indexes them by a hard DOOM enum — or keep two binaries (chocolate-heretic style) and just
  share assets. The pointer approach is the "one aiDoom" goal but is the riskiest refactor.
- The renderer / collision / netcode are **reusable as-is** (same lineage).
- Source of truth is `../crispy-doom/src/heretic/` — it's the same C, so most of this is
  port-and-branch, not invent.

## Related
`HERETIC_HEXEN.md` (the additive pack + monster status), `INVENTORY.md` (the artifact +
flight/morph subsystems already built), `CLAUDE.md` (engine architecture).
