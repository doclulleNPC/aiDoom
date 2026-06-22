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

## 2. Behaviour — the C port (large, staged; NOT done)

To actually *spawn and fight* a Heretic gargoyle or fire the Elven Wand, the engine
needs that actor's data + code, the DOOM way:

- **`SPR_*`** sprite enum entries + the `sprnames[]` strings for the new sprite
  names (e.g. Heretic `IMPX`, `MUMM`, `WIZD`, weapon `WAND`/`CrBow`…).
- **`states[]`** — the animation/behaviour frame table (sprite, frame, tics, action,
  next state) for every actor.
- **`mobjinfo[]`** — the actor definition (health, speed, radius, sounds, the state
  ids above, flags).
- **`sfx_*`** enum + `S_sfx[]` rows pointing at the copied sound lump names.
- **Action functions** (`A_*`) for behaviours DOOM doesn't have (e.g. Heretic's
  staff/wand/firemace, the gargoyle, the disciple's homing, weapon "tomed" modes).

This is the content of Heretic's/Hexen's own `info.c` + `p_*` action code — hundreds
of states and dozens of actors **per game**, i.e. a multi-week port, not a single
drop-in file. Doing it blindly would produce thousands of lines that don't build.

### Reference
gzDoom defines these as ZScript under
`../gzdoom-g4.14.2/wadsrc/static/zscript/actors/heretic/` (and `…/hexen/`) —
`mummy.zs`, `wizard.zs`, `clink.zs`, `weaponcrossbow.zs`, `weaponblaster.zs`, … —
which give the exact stats/states/sounds/behaviour to translate into the DOOM-style
tables above. (crispy-doom has no Heretic/Hexen; crispy-heretic/-hexen are separate.)

### Suggested staging (one vertical slice at a time, each builds + runs)
1. Sound table rows for the copied lumps + load `*_assets.wad`.
2. One simple **monster** end-to-end (Heretic gargoyle / mummy): `SPR_*`, `states[]`,
   `mobjinfo[]`, reuse DOOM action funcs where possible; let the director spawn it.
3. One **weapon** (Elven Wand) with its fire/raise/lower states.
4. Iterate outward (more monsters, the tomed weapon modes, Hexen's mana/inventory).

New engine files would land as `files/heretic.c` / `files/hexen.c` (compiled by
`build.sh`'s `*.c` glob), each a self-contained block of the above tables — added
incrementally so the build stays green.
