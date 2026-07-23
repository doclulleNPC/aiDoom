# MBF21 / DeHackEd — current source status

## Scope

aiDoom contains a substantial DeHackEd/BEX/DSDHacked/MBF21 gameplay layer. This document records the current source boundary; it is not a claim that every MBF21 or modern mod feature is complete.

The large fixture historically used for development is `run/ID0/Crispy and Brutal.wad`, whose embedded `DEHACKED` data exercises expanded DSDHacked frame/thing tables and MBF21 codepointers. Treat a successful run of that fixture as a project test result, not as a universal compatibility guarantee for unrelated mods.

## Implemented layers

### DeHackEd/BEX loading

`files/d_deh.c` / `.h` and the BEX helper files process:

- Thing, Frame, Pointer and `[CODEPTR]` sections;
- Weapon and Ammo sections;
- Sounds and Sprite sections;
- `[PARS]` timing;
- every embedded `DEHACKED` lump in load order plus `-deh <file>` through `D_ProcessDehInWads`.

The runtime installer ordering in `files/d_main.c` is important: appended builtin actors (`Heretic_Init`, `Hexen_Init`, `Freedoom_Init`, `RevMarine_Init`, `Morph_Init`, `HereticInv_Init`) are filled before DeHackEd is applied, so patch edits can overwrite colliding editor-number slots.

BEX string/text replacement remains limited. The source's compile-time `dstrings.h` layout is not a full replaceable runtime string table.

### DSDHacked growth

`files/dsdhacked.c` grows the runtime table pointers and counts for states, mobj types, sprites and sounds. The renderer and state machine contain bounds/skip handling for expanded states/sprites rather than assuming every index belongs to the generated vanilla arrays.

Grown states default to the invisible `SPR_TNT1` placeholder when a patch leaves their sprite unset. `P_SetMobjState` uses cycle protection for zero-tic/jump chains.

### MBF21 codepointers and fields

The current source includes the main MBF21 action families, including:

- spawn/object and projectile actions;
- random/jump/flag actions;
- monster/weapon sound and ammo actions;
- melee, radius damage and detonation helpers;
- tracer seek/find actions;
- noise/alert and clear-tracer actions;
- `A_HealChase` fallback behavior;
- weapon check-ammo/jump/alert actions.

MBF21 thing fields are parsed into `mobjinfo_t`, including `flags2`, infighting/projectile/splash groups, fast speed, melee range and dropped-item data. The current playsim paths wire the implemented flag effects and group behavior where present.

`A_LineEffect` remains a stub. Do not infer complete line-special behavior from the presence of the parser field.

## Compatibility boundary

The port is gameplay-level MBF21/DSDHacked support layered on the classic Doom tables. It is not a full ZDoom actor language and does not provide DECORATE, ZScript or ACS.

The Boom map/special side—generalized specials, extended nodes, scrollers, friction, pushers and large `SWITCHES`—is documented separately in `docs/BOOM_COMPAT.md`.

The parser must remain fixed-point/tic-locked. DeHackEd changes tables and codepointer selection; it must not introduce wall-clock or floating-point simulation behavior.

## Known source pitfalls and historical fixes

- Bounds checks must use runtime `num_states`/`num_mobjtypes`, not generated vanilla counts.
- Expanded state-cycle guards must be initialized for newly grown ranges.
- Empty DSDHacked sprite fields must remain invisible (`SPR_TNT1`), not sprite 0.
- Custom sound indices must be checked against runtime `num_sfx`.
- A PWAD path containing spaces must reach the launcher/game as one quoted argument.
- Appended aiDoom actor installers must run before DeHackEd if a patch owns the overlapping editor-number range.

These are implementation lessons; they should be rechecked against current source when changing the parser.

## Current open work

- complete `[STRINGS]`/`Text`/`Misc`/`Cheat` replacement through a runtime string table;
- any remaining MBF21 codepointer or flags edge cases not represented by the current fixture;
- broader mod compatibility testing across different DeCOHack/DSDHacked producers;
- a clearer compatibility-level policy for individual Boom/MBF/MBF21 quirks.

## Source map

- Parser/sections: `files/d_deh.c`, `files/d_deh.h`, `files/deh_*.c`.
- Runtime table growth: `files/dsdhacked.c`, `files/info.c`, `files/info.h`.
- MBF21 actions/fields: `files/p_mbf.c`, `files/p_enemy.c`, `files/p_pspr.c`, `files/p_mobj.c`, `files/p_map.c`, `files/p_inter.c`.
- Startup ordering: `files/d_main.c`.
- Related map compatibility: `docs/BOOM_COMPAT.md`.
