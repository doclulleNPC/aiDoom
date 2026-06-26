# Heretic / Hexen content in aiDoom — plan & status

Goal: bring Heretic/Hexen (and Freedoom) **weapons, monsters, sounds** into this
DOOM engine. This splits into two very different halves.

## 1. Assets — DONE (tooling)

`tools/extract_game_assets.py` reads another game's IWAD and **palette-converts**
its sprites into the DOOM palette, plus copies its sounds, into a PWAD:

```sh
python3 tools/extract_game_assets.py --src ID0/heretic.wad   --out ID0/heretic_assets.wad
python3 tools/extract_game_assets.py --src ID0/hexen.wad     --out ID0/hexen_assets.wad
python3 tools/extract_game_assets.py --src ID0/freedoom2.wad --out ID0/freedoom2_assets.wad   # identity palette
```

- Builds a 256→256 nearest-colour map from the source `PLAYPAL` to DOOM's, then
  remaps **only the pixel bytes** in each sprite's column posts (structure/offsets
  unchanged → exact, handles tall patches). Freedoom uses the DOOM palette, so it's
  an identity copy.
- Sounds (DMX format-3 lumps) are copied verbatim (audio is palette-independent).
- Verified: `heretic.wad` → 1492 sprites recoloured + 148 sounds, ~5 MB, <1 s.

Result is a palette-correct WAD of the graphics/sounds. **This does not make them
behave** — that's half 2.

## 3. Freedoom DOOM2 monsters -- DONE (free art, clone approach)

`tools/extract_freedoom2.py` renames the DOOM2-exclusive monster/projectile sprites
(SKEL->FSKE ...) from a free freedoom2.wad into `freedoomstuff.wad` (no collision with
DOOM/doom2stuff). `files/freedoom.c` clones each DOOM2 actor (revenant, mancubus,
arch-vile, arachnotron, chaingunner, hell knight, pain elemental, SS, keen) + its
projectiles into `MT_FD_*` at startup -- deep-copying the state graph with the sprite
remapped + missile-spawn actions re-pointed at cloned F* projectiles (the DOOM2
states/funcs already exist in the engine). `summon revenant|mancubus|...`. Coexists with
doom2stuff. The bosses (brain/spawner) are still TODO.

## 2. Behaviour — the C port (foundation + first monster DONE; rest staged)

### Status
- **Infrastructure DONE** (`files/heretic.c` + `heretic.h`): Heretic monsters are
  appended to the engine's `states[]`/`mobjinfo[]` at runtime via `Heretic_Init()`
  (called from `D_DoomMain` after `P_Init`), so info.c's giant generated initializers
  stay untouched. Enum slots (`SPR_HMUM`, `S_HMUM_*`, `MT_HMUMMY`) live at the end of
  `spritenum_t`/`statenum_t`/`mobjtype_t`; `"HMUM"` added to `sprnames[]`. Sprites come
  from `hereticstuff.wad` (renamed `H*`). Safe without the wad (0-frame sprite, gated spawn).
- **Mummy + Sabreclaw + Gargoyle + Knight + Weredragon + Disciple + Ophidian DONE**: ported from crispy `heretic/info.c` +
  `p_enemy.c`. Mummy (`A_MummyAttack` melee), Sabreclaw/clink (`A_ClinkAttack` melee,
  no blood), Gargoyle/imp (`A_ImpMeAttack` melee + `A_ImpMsAttack` skull-fly dive +
  `A_ImpDeath`; MF_FLOAT flyer), Knight/undead warrior (`A_KnightAttack`: melee
  HITDICE(3) or hurls a spinning `MT_HKNIGHTAXE` projectile via `P_SpawnMissile` — the
  first ranged Heretic monster + projectile actor, `A_ContMobjSound`). Spawn via
  `heretic [mummy|clink|imp|knight]` / `summon <name>`. Verified: boots with/without the
  wad; all spawn, render, fight, die (the knight throws + the axe flies/explodes).

### Remaining (each monster = the same pattern; weapons are a bigger stage)
- **Maulotaur DONE**: hammer swing + slam-charge (MF_SKULLFLY, engine-driven) + mace
  ball (MT_HMINOTAURFX); 3000 hp miniboss, in the director's exit-guard pool.
- **Iron Lich DONE**: floating 700-hp boss -- ice ball (MT_HHEADFX1) + a HOMING whirlwind
  (MT_HWHIRLWIND, steered by the engine's A_Tracer via tracer); sprites HIRO/HIRB/HIRX.
  In the director guard pool. (Fire-column 3rd attack omitted.)
- **D'Sparil DONE** (simplified): the phase-2 sorcerer -- floats and hurls exploding blue
  bolts (MT_HDSPARILFX + A_Explode), 3500 hp; sprites HSR2/HSRB. The serpent phase 1,
  teleport and wizard-summon are omitted, and (no SDTH sprite extracted) the death is a
  brief body-frame fade.

**All 10 Heretic monsters are now in, with authentic Heretic sounds.** Remaining Heretic
work: the weapons.  Then **Hexen** (the bigger game) -- see the asset tool below.

### Hexen assets (`tools/extract_hexen.py` -> `run/ID0/hexenstuff.wad`)

The Hexen counterpart of `extract_heretic_monsters.py`: it extracts the Hexen **monsters
and weapons** (sprites palette-converted to the DOOM palette, plus their DMX sounds) into
`run/ID0/hexenstuff.wad`, ready for a future `files/hexen.c` port.
- **Sprites** (98 codes: ettin, centaur/slaughtaur, both chaos serpents, reiver, minotaur,
  stalker, dark bishop, death wyvern, afrit, wendigo, heresiarch, korax, pig + all three
  classes' weapons & the 4th-weapon pieces) are renamed into a collision-free **`X`**
  (heXen) namespace -- the full code map is printed and written to
  `tools/hexen_sprite_map.txt` so the C port uses the SAME codes.
- **Sounds** are copied **verbatim** (Hexen lump names are descriptive and up to 8 chars,
  so they can't take a `DS` prefix like Heretic's 6-char names did); choosing their
  engine-facing names is a porting-step decision.
- Run: `python3 tools/extract_hexen.py` (auto-detects a DOOM IWAD in ID0 for the target
  palette).  Verified: the wad loads under DOOM with no crash and the sprites render
  correctly (ettin / serpent / fighter-axe checked).

#### Hexen monster port -- 10 monsters in (`files/hexen.c` + `hexen.h`)

Same additive mechanism as `files/heretic.c`: `Hexen_Init()` (called from `D_DoomMain`)
appends the Hexen monsters' states/mobjinfo at runtime; enums (`SPR_X*`, `S_X*_*`, `MT_X*`)
live at the end of `spritenum_t`/`statenum_t`/`mobjtype_t`; the `X*` codes are added to
`sprnames[]` in lock-step (the count of SPR_* enum entries MUST equal the sprnames[] strings).
Sprites come from `hexenstuff.wad` (the `X*` codes in `hexen_sprite_map.txt`); sounds reuse
DOOM SFX for now.  Gated by `Hexen_Available()` (probes `SPR_XETT`), safe without the wad.

**10 monsters DONE** (all verified: spawn / chase / attack / die, no crash; each ranged one
has its own projectile actor; multi-stage rituals / homing / teleports were simplified to a
clean single death + straight projectile, no `mobj_t` special1/2):

| Monster | Type | Behaviour | Summon |
|---|---|---|---|
| Ettin | `MT_XETTIN` | melee brute, HITDICE(2) | `ettin` |
| Centaur | `MT_XCENTAUR` | melee brute | `centaur` |
| Slaughtaur | `MT_XSLAUGHTAUR` | melee + lobbed bolt | `slaughtaur` |
| Chaos Serpent | `MT_XDEMON` | melee + fire breath | `serpent` |
| Fire Demon / Afrit | `MT_XFIREDEMON` | flying, fireballs | `afrit` |
| Reiver / Wraith | `MT_XWRAITH` | floating, drain melee + bolt | `reiver` |
| Dark Bishop | `MT_XBISHOP` | floating caster | `bishop` |
| Wendigo / Ice Guy | `MT_XICEGUY` | floating, ice shard | `wendigo` |
| Stalker | `MT_XSTALKER` | ambusher, melee + spit | `stalker` |
| Death Wyvern | `MT_XDRAGON` | flying boss (640 hp), fireball | `dragon` |

- **Director:** all 10 are in `dir_hexen[]` (`p_ai_director.c`), mixed into the director's
  trash tier (~30%) when the Hexen pack is loaded; the Death Wyvern is also a rare exit guard
  (`P_Director_PickGuard`).  Gated by `P_Director_HexenAvailable()`.
- **Launcher:** the **Hexen** checkbox (next to FreeDoom/Heretic) adds `-file hexenstuff.wad`.
- Remaining: the **bosses** Heresiarch + Korax (need Hexen `special1/2` / teleport / summon
  mechanics), and the **pig** morph target (the generic morph subsystem from the Heretic egg
  is ready — see `INVENTORY.md`).
- Authentic Heretic **sounds DONE**: `extract_heretic_monsters.py` now copies the Heretic
  SFX with a `DS` prefix (so the engine's `ds%s` lookup finds them); 51 `sfx_h_*` rows in
  sounds.h/.c, and every Heretic monster's see/attack/pain/death/active sounds are wired to
  them.  **Re-run the extractor** to refresh `hereticstuff.wad` after this change.
- **Weapons** (staff/gauntlets/wand/crossbow/dragonclaw/hellstaff/phoenix/firemace +
  tomed modes) from `heretic/p_pspr.c` — a separate, larger stage.
- Director wiring so it spawns the Heretic monsters alongside DOOM ones.

### Original plan (kept for reference)

### Reference: crispy-doom's C source (NOT gzdoom ZScript)

`../crispy-doom/src/heretic/` and `…/src/hexen/` contain the **complete Heretic &
Hexen game code in C**, same Chocolate/Doom lineage as this engine:

- `heretic/info.c` (~5.6k lines): `state_t states[]` + `mobjinfo_t mobjinfo[]` —
  the exact frame tables and actor definitions, in the SAME `{SPR_x, frame, tics,
  A_action, nextstate, ...}` form aiDoom's `info.c` uses.
- `heretic/p_enemy.c` (~2.7k), `p_pspr.c`, `p_inter.c`: the `A_*` action functions
  (monster AI + weapons).
- `hexen/` ditto (info.c ~13.7k — much bigger; Hexen also brings ACS/polyobjects).

So the behaviour is **already C we can adapt**, not something to invent. That's the
right source.

### The real integration hurdles (why it's still a port, not a copy)

Heretic/Hexen are separate games whose tables can't just be dropped in:

1. **Enum collisions.** Heretic has its own `SPR_*`, `S_*` (states), `MT_*` (mobj
   types), `sfx_*` — all colliding with DOOM's. To coexist in one binary they must be
   **namespaced** (e.g. append Heretic entries with new values: `MT_H_MUMMY`,
   `S_H_MUMMY_*`, `SPR_H_MUMM`, `sfx_h_*`) and the copied sprite lumps likewise can't
   clash with DOOM sprite names of the same 4-letter code.
2. **Action-function signature.** Heretic uses one unified
   `void A_Foo(mobj_t*, player_t*, pspdef_t*)` for ALL actions; DOOM uses
   `A_Chase(mobj_t*)` / `A_FireFoo(player_t*, pspdef_t*)`. The ported funcs need a
   shim/dispatch to aiDoom's `actionf_t`.
3. **`mobj_t` extras.** Heretic adds `special1/2` (and Hexen more); aiDoom's `mobj_t`
   lacks them — the monster AI uses them, so add the fields (or a side-table).
4. **Engine deltas.** Heretic's `A_Chase`, `P_DamageMobj`, ambient sounds, flend/
   terrain, etc. differ subtly from DOOM's — port the few the chosen actors touch.

### Architecture choice (decide before coding)

- **(A) Additive "Heretic monsters in DOOM"** *(recommended, matches aiDoom)* — append
  a handful of Heretic actors to DOOM's tables with namespaced enums + ported action
  funcs in a new `files/heretic.c`; load `heretic_assets.wad`; the director spawns
  them alongside DOOM monsters (like doom2stuff). Per-actor manual adaptation, but
  each slice builds + runs and you get the variety immediately.
- **(B) Full Heretic game-mode** — select Heretic's whole `info.c`/`p_*` instead of
  DOOM's at startup when `heretic.wad` is the IWAD (what chocolate-heretic does as a
  separate binary). Cleanest/most complete, but a much bigger dual-mode refactor.

### Suggested first slice (approach A, grounded in crispy's code)
1. Add namespaced sound rows (`sfx_h_*`) for the copied lumps; load `heretic_assets.wad`.
2. One monster end-to-end — the **Mummy** (`MT_MUMMY`, states `S_MUMMY_*`, funcs
   `A_Look`/`A_Chase`/`A_FaceTarget`/`A_MummyAttack` from crispy `heretic/info.c` +
   `p_enemy.c`): copy its states/mobjinfo with `_H_` enums, shim the action sigs,
   add `special1/2` to `mobj_t`, let the director spawn it.
3. One weapon (Elven Wand) from `heretic/p_pspr.c`.
4. Iterate outward; then tackle Hexen (bigger) the same way.

New engine files would land as `files/heretic.c` / `files/hexen.c` (compiled by
`build.sh`'s `*.c` glob), each a self-contained block of the above tables — added
incrementally so the build stays green.
