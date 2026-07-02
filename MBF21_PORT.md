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
- [x] **2. DeHackEd/BEX parser** — ported `../winmbf/Source/d_deh.c` -> `files/d_deh.{c,h}` (adapted
  to aiDoom: DEHFILE lump reader via `W_*`, actionf_t union, string compat, 64-bit); wired
  `D_ProcessDehInWads()` into `d_main.c` (every `DEHACKED` lump + `-deh <file>`). Live sections:
  Thing / Frame / Pointer+`[CODEPTR]` / Weapon / Ammo / Sounds / Sprite / `[PARS]`. MBF codepointer
  stubs in `files/p_mbf.c` (real impls in M4). **Bounds-safe:** DSDHacked frames/things beyond the
  tables are skipped (no OOB crash) until M3. Verified: a vanilla thing/frame DEH loads + applies;
  `Crispy and Brutal.wad` no longer crashes the parser (now stops at `R_InitSprites` -- a sprite
  rotation-leniency issue -> M3). **Deferred to M2b:** `[STRINGS]`/`Text`/`Misc`/`Cheat` (need the
  string-table refactor + aiDoom's cheat map).
- [x] **3. DSDHacked** — **done.** `states`/`mobjinfo`/`sprnames`/`S_sfx` are growable pointers onto
  the built-in arrays with runtime counts (`num_states`/`num_mobjtypes`/`num_sprites`/`num_sfx`);
  `files/dsdhacked.c` realloc-grows them on demand, wired into `deh_procFrame`/`deh_procThing` and
  the `[SPRITES]`/`[SOUNDS]` sections (`deh_procSprites`/`deh_procSoundsList`). Sprite loading made
  non-fatal (`R_InstallSpriteLump`, `R_InitSpriteDefs` "no patches", `R_ProjectSprite`/
  `R_DrawPSprite` out-of-range → skip/later-wins). `P_SetMobjState` uses Killough's seenstate/
  recursion form so zero-tic + jumping (`A_RandomJump`) chains don't hang or clobber. **Grown states
  default to the invisible `SPR_TNT1` placeholder** (added to `info.h`/`info.c`), not sprite 0 — an
  unset DSDHacked sprite must render nothing, never SPR_TROO (the imp). Verified: `Crispy and
  Brutal.wad`'s 471 KB patch loads fully (6257 frames to #72138, 130 things, numsprites 199→8100)
  and monsters/gore/sounds render correctly.
- [x] **4. MBF21 codepointers** — done (M4a-e): args parsing; A_SpawnObject; the MBF classics
  (RandomJump/PlaySound/Spawn/Turn/Face/Detonate/Die/Scratch/Mushroom); monster+weapon projectiles
  (A_MonsterProjectile/A_WeaponProjectile); weapon Sound/ConsumeAmmo/GunFlashTo/RefireTo/Bullet+
  MeleeAttack; A_RadiusDamage/MonsterMeleeAttack; the flags2 Jump/Flag family (A_AddFlags/
  RemoveFlags/JumpIf*, NoiseAlert, ClearTracer); A_SeekTracer/FindTracer (homing); A_HealChase
  (chase fallback). MBF21 thing fields parse into mobjinfo (MBF21 Bits->flags2, groups, dropped
  item, fast speed, melee range). Only A_LineEffect is still a stub. **Not yet wired:** the flags2
  *playsim effects* (RIP/BOUNCE/...) + infighting/projectile/splash *group* logic, and
  [SOUNDS]/S_sfx growth so custom sounds play. — the ~20 new codepointers (`A_SpawnObject`, `A_MonsterProjectile`,
  `A_WeaponProjectile`, `A_WeaponSound`, `A_MonsterMeleeAttack`, `A_SeekTracer`, `A_AddFlags`,
  `A_JumpIfFlagsSet`, `A_HealChase`, …) in `p_enemy.c`/`p_pspr.c`; MBF21 thing flags (`flags2`) in
  `p_mobj.c`/`p_map.c`; projectile/splash/infighting **groups** (infighting logic in `p_map.c`);
  fast-speed / DEH `Args`. Wire the `[CODEPTR]` names + flag mnemonics into the parser.
  **flags2 playsim effects wired** (`p_mobj.c`/`p_map.c`): `MF2_LOGRAV`, `MF2_RIP`,
  `MF2_NORADIUSDMG`, `MF2_FULLVOLSOUNDS`; **groups wired** (`p_map.c`/`p_inter.c`):
  infighting/projectile/splash. **[SOUNDS]/S_sfx growth done** (custom sounds play). Weapon
  codepointers completed incl. `A_CheckAmmo`/`A_WeaponJump`/`A_WeaponAlert` (a missing `A_CheckAmmo`
  on a DSDHacked readystate frame is what stopped Brutal's weapons from firing). Only `A_LineEffect`
  is still a stub; `[STRINGS]`/`Text`/`Misc`/`Cheat` string replacement deferred to M2b.
- [x] **5. Verify** — **`Crispy and Brutal.wad` loads and plays**: weapons fire, gore/gibs spawn,
  monsters die with correct animations + sprites, custom sounds play.

## Status: MBF21 / DeHackEd port complete (gameplay level)

aiDoom runs modern DeHackEd/DSDHacked/MBF21 gameplay mods (verified end-to-end with
`Crispy and Brutal.wad`). The **map/level** side of Boom/MBF (generalized specials, extended nodes,
deep water/friction/wind/scrollers, `ANIMATED`/`SWITCHES`) is a **separate** effort — see
`BOOM_COMPAT.md`.

### Debugging log — the subtle bugs that took real digging (all fixed)
- **Weapons wouldn't fire** — `[CODEPTR]`/`Pointer` bounds-checked the frame index against the fixed
  `NUMSTATES` instead of the runtime `num_states`, so every codepointer on a DSDHacked frame (70000+)
  was rejected and left `NULL`; the pistol's readystate `A_CheckAmmo` and firing actions did nothing.
- **Monsters glitched into each other's poses** — `dsdh_EnsureStatesCapacity` left the base
  `seenstate_tab` entries uninitialised on the first grow (realloc-from-NULL); `P_SetMobjState`'s
  cycle guard read garbage and cut vanilla-state transitions short.
- **Gore rendered as standing imps; monsters "froze" then vanished** — grown DSDHacked states
  defaulted to sprite 0 = `SPR_TROO` (the imp). Brutal's blood-splat frames leave the sprite unset;
  they rendered as a standing imp (`TROOA`) for their linger duration. Fixed by defaulting grown
  states to the invisible `SPR_TNT1`.
- **PWAD with spaces didn't load** — the launcher passed `Crispy and Brutal.wad` unquoted, so it
  reached the game as three failed `-file` args (looked exactly like vanilla).

## Notes
- Keep the playsim deterministic (fixed-point, tic-locked) — DeHackEd only *rewrites the tables*,
  it must not add wall-clock/float logic.
- aiDoom's death-drops are hardcoded in the death states (not `droppeditem`); MBF21 `droppeditem`
  is additive and defaults to 0 (no drop) until step 4 wires it.

## M2 execution plan (as scoped)

**Port base: `../winmbf/Source/d_deh.c`** (Lee Killough's classic MBF parser, 2755 lines) — closest
to aiDoom's 1993 structures, and it already carries the MBF codepointers (in `../winmbf/Source/p_enemy.c`).
Nugget-Doom (`/home/dulli/Source/Nugget-Doom`) is the reference for the MBF21/DSDHacked layers (M3/M4).

Verified **64-bit safe**: `deh_procFrame` assigns `state_t` fields explicitly (no int-array pun over
the 8-byte `action` pointer); `deh_procThing`'s `((int*)&mobjinfo[i])[ix]` offset trick is fine
because `mobjinfo_t` is all-`int` (including the new mbf21 fields).

Steps:
1. **`files/d_deh.{c,h}`** — bring in d_deh.c; adapt includes to aiDoom (`w_wad.h`, `info.h`,
   `sounds.h`, `m_cheat.h`, `dstrings.h`), inline the `DEHFILE` lump/file reader.
2. **Wire loading** — `DEH_LoadLumps()` in `d_main.c`: after `W_InitMultipleFiles`, scan for every
   `DEHACKED` lump and `ProcessDehFile(NULL,NULL,lumpnum)`; add `-deh <file>` arg.
3. **Name/mnemonic tables** — `deh_mobjinfo[]` (23 vanilla `mobjinfo_t` field names, order-matched),
   `deh_state[]`, `deh_sfxinfo[]`, `deh_ammo[]`, `deh_weapon[]`, `deh_misc[]`, `deh_mobjflags[]`
   (Bits mnemonics), `deh_bexptrs[]` (codepointer name→`A_*`), `deh_codeptr[NUMSTATES]`.
4. **Sections** (procs): Thing, Frame, Pointer/`[CODEPTR]`, Sounds, Ammo, Weapon, Sprite, Misc,
   Cheat, `[PARS]`. All operate on aiDoom's existing `mobjinfo`/`states`/`weaponinfo`/`S_sfx`/cheats.
5. **Defer to M2b**: `[STRINGS]` / `Text` string replacement — aiDoom's strings are compile-time
   `#define`s (dstrings.h), not a runtime `deh_strlookup` table. Parse-and-ignore first; the
   string-table refactor (route message strings through a replaceable table) is a follow-up.
   (Gameplay DEH — things/frames/pointers/weapons — needs none of this.)

After M2, layer DSDHacked (M3) and the MBF21 codepointers/flags/groups (M4).
