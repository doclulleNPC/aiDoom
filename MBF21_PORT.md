# MBF21 / DeHackEd port

Goal: make aiDoom a **MBF21-compatible** port so it runs modern DeHackEd mods. Target test case:
**`run/ID0/Crispy and Brutal.wad`** — its `DEHACKED` lump is a 471 KB DECOHack patch,
`Doom version = 2021, Patch format = 6` (**MBF21**), **6257 frames** with state numbers up to
**72138** (**DSDHacked**-level table expansion), 130 things, MBF21 projectile/splash groups.

## References (siblings)
- **`../crispy-doom`** — clean *modular* DeHackEd (Chocolate lineage): `deh_main/io/str/mapping.c`
  + `doom/deh_{thing,frame,weapon,ptr,sound,ammo,cheat,misc,text}.c` + BEX (`deh_bex*.c`).
  Vanilla + Boom only (no MBF21/DSDHacked). aiDoom shares the 1993 id data structures, so this
  ports cleanly — it is the **parser base**.
- **`../Nugget-Doom`** — Woof lineage, **full MBF21 + DSDHacked** (`d_deh.c` 132 KB, `dsdhacked.c`,
  `info.h` with the mbf21 struct fields, the mbf21 codepointers in `p_enemy/p_pspr`). The
  reference for the MBF21/DSDHacked layers laid on top of the crispy parser.

## Plan / progress

- [x] **1. Struct foundation** — extend `state_t` (`args[MAXSTATEARGS]`, `flags`) and `mobjinfo_t`
  (`flags2`, `infighting_group`, `projectile_group`, `splash_group`, `altspeed`, `meleerange`,
  `droppeditem`) in `files/info.h`. Positional initializers in `info.c` stay valid (new fields → 0).
- [ ] **2. DeHackEd/BEX parser** — port crispy's modular `deh_*.c` to `files/` and adapt to
  aiDoom's globals (`states`, `mobjinfo`, `weaponinfo`, `sprnames`, `S_sfx`, cheats, pars).
  Sections: Thing, Frame, Pointer/`[CODEPTR]`, Weapon, Sound, Ammo, Misc, Cheat, `[PARS]`, Text,
  `[STRINGS]`. Load the `DEHACKED` lump (+ `-deh file`) in `d_main.c` after WAD init. *Testable
  with any classic/Boom DEH.*
- [ ] **3. DSDHacked** — make `states` / `mobjinfo` / `sprnames` / `S_sfx` **dynamically growable**
  (state/thing numbers into the tens of thousands). Fixed arrays → grown pointers; `NUMSTATES`
  etc. become runtime counts. (port `dsdhacked.c`.)
- [ ] **4. MBF21** — the ~20 new codepointers (`A_SpawnObject`, `A_MonsterProjectile`,
  `A_WeaponProjectile`, `A_WeaponSound`, `A_MonsterMeleeAttack`, `A_SeekTracer`, `A_AddFlags`,
  `A_JumpIfFlagsSet`, `A_HealChase`, …) in `p_enemy.c`/`p_pspr.c`; MBF21 thing flags (`flags2`) in
  `p_mobj.c`/`p_map.c`; projectile/splash/infighting **groups** (infighting logic in `p_map.c`);
  fast-speed / DEH `Args`. Wire the `[CODEPTR]` names + flag mnemonics into the parser.
- [ ] **5. Verify** — `Crispy and Brutal.wad` loads and plays.

## Notes
- Keep the playsim deterministic (fixed-point, tic-locked) — DeHackEd only *rewrites the tables*,
  it must not add wall-clock/float logic.
- aiDoom's death-drops are hardcoded in the death states (not `droppeditem`); MBF21 `droppeditem`
  is additive and defaults to 0 (no drop) until step 4 wires it.
