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

## 2. Behaviour — the C port (foundation + first monster DONE; rest staged)

### Status
- **Infrastructure DONE** (`files/heretic.c` + `heretic.h`): Heretic monsters are
  appended to the engine's `states[]`/`mobjinfo[]` at runtime via `Heretic_Init()`
  (called from `D_DoomMain` after `P_Init`), so info.c's giant generated initializers
  stay untouched. Enum slots (`SPR_HMUM`, `S_HMUM_*`, `MT_HMUMMY`) live at the end of
  `spritenum_t`/`statenum_t`/`mobjtype_t`; `"HMUM"` added to `sprnames[]`. Sprites come
  from `hereticstuff.wad` (renamed `H*`). Safe without the wad (0-frame sprite, gated spawn).
- **Mummy + Sabreclaw + Gargoyle DONE**: ported from crispy `heretic/info.c` +
  `p_enemy.c`. Mummy (`A_MummyAttack` melee), Sabreclaw/clink (`A_ClinkAttack` melee,
  no blood), Gargoyle/imp (`A_ImpMeAttack` melee + `A_ImpMsAttack` skull-fly dive +
  `A_ImpDeath`; MF_FLOAT flyer). Spawn via `heretic [mummy|clink|imp]`. Verified: boots
  with/without the wad; all spawn, render, fight, die.

### Remaining (each monster = the same pattern; weapons are a bigger stage)
- More monsters: gargoyle (imp), sabreclaw (clink), undead warrior (knight), weredragon
  (beast), disciple (wizard) [melee/simple first], then ranged/boss (ophidian, iron lich,
  maulotaur, d'sparil) which need their projectile actors + custom A_* (homing, etc.).
- Authentic Heretic **sounds** (currently DOOM SFX are reused): extend `sfxenum_t` +
  `S_sfx[]` with `sfx_h_*` rows for the lumps `extract_heretic_monsters.py` copied.
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
