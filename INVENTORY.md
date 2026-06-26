# INVENTORY.md — artifact / item inventory systems

aiDoom adds a Heretic/Hexen-style **inventory** on top of the 1993 engine: a held list of
items you scroll through and use, drawn bottom-centre (`HU_Inventory_Drawer`, config
`show_inventory_hud`). The held counts live in `player_t.inventory[NUMARTIFACTS]` + the
selected slot `player_t.invslot` (so they ride along in the savegame's wholesale `player_t`
memcpy; changing the artifact set bumps the engine `VERSION_NUM` and cleanly rejects stale
saves).

There are **three independent item sets** sharing the same machinery (scroll / use / drop /
HUD). The generic engine is in `files/p_invent.c`; the Heretic effects in
`files/p_inv_heretic.c`; the generic flight + morph subsystems in `files/p_morph.c` and
`p_user.c`.

## Keys (configurable in `aidoom_config` / `aidoom.cfg`)
| Action | Default | Bind |
|---|---|---|
| Select previous item | `[` | `key_inv_left` |
| Select next item | `]` | `key_inv_right` |
| Use selected item | `Enter` | `key_inv_use` |
| Drop selected item | `d` | `key_inv_drop` |

Scrolling skips empty slots and wraps. **Drop** (`P_DropArtifact`) spawns the item's pickup
mobj on the ground a little in front of you (tossed, so you don't instantly re-grab it; it
stays at your feet if a wall blocks) and removes it from the inventory.

## 1. DOOM "overflow" inventory — **buddy-only**
Modelled as a co-op convenience: it is **only active when you play with the buddy**
(`-coop` / `-aicoop`; gated on `P_AICoop_Active()` in `P_StoreOverflow`). Solo play is
vanilla — a stimpack at full health just stays on the floor.

When you pick up a health/armor/ammo item while already **at its cap** (so vanilla DOOM
would waste/refuse it), the surplus is pocketed instead:

| Picked up at cap | Stored as | Use effect |
|---|---|---|
| Stimpack (HP ≥ 100) | `arti_stimpack` | +10 HP |
| Medikit (HP ≥ 100) | `arti_medikit` | +25 HP |
| Health bonus (HP ≥ 200) | `arti_healthbonus` | +1 HP (cap 200) |
| Armor bonus / green / blue (at cap) | `arti_armorbonus` / `_greenarmor` / `_bluearmor` | re-give the armor |
| Bullets / shells / rockets / cells (at max) | `arti_ammo_*` (stores the **amount**) | refill up to max |

Implemented as a uniform pattern in `P_TouchSpecialThing` (`p_inter.c`): if the normal
`P_GiveBody/Armor/Ammo` fails (at cap), `P_StoreOverflow(...)` pockets it, else the pickup
falls through to vanilla "leave it".

**Second wind.** In buddy mode, when an otherwise-lethal hit would kill a human, a stored
**medikit or stimpack is spent automatically to keep them standing** (`P_InventorySecondWind`,
hooked in `P_DamageMobj`): health is set to the item's heal value (25 / 10) instead of dying.
Never for the buddy itself (it has its own L4D down/revive).

**Buddy auto-heal.** The buddy spends its own held health artifacts when hurt
(`AICoop_AutoHeal`, value order medikit → flask → stimpack → urn → health bonus) before
hunting for a med-pack.

## 2. Heretic artifacts (`files/p_inv_heretic.c`)
The real Heretic artifacts, with sprites extracted into `run/ID0/hereticstuff.wad` by
`tools/extract_heretic_monsters.py`. Currently obtained via the
console `givearti <name>` (their real Heretic doomednums would collide with DOOM things, so
the pickup items are `doomednum = -1`); effects work without the wad, only the on-floor icon
needs it (gated by `HereticInv_Available`).

| Artifact | Effect |
|---|---|
| Quartz Flask | +25 HP |
| Mystic Urn | +100 HP |
| Tome of Power | Berserk (DOOM has no powered weapons — simplified) |
| Torch | infrared light, ~120 s |
| Time Bomb | spawns a fused bomb → `A_Explode` (radius 128) |
| Ring of Invincibility | `pw_invulnerability`, 30 s |
| Shadowsphere | `pw_invisibility` + MF_SHADOW, 60 s (true invisibility — see below) |
| Chaos Device | teleport to the level start, with fog |
| **Wings of Wrath** | flight (generic `pw_flight`) |
| **Morph Ovum** | fires an egg → morphs the hit monster into a chicken |

The use dispatcher in `P_UseArtifact` routes `h_arti_*` slots to `ApplyHereticArtifact`.

### Generic flight & morph (reusable by DOOM / Heretic / Hexen)
- **Flight** is a plain timed power `pw_flight` (`doomdef.h`, handled in `p_user.c`): float
  (MF_NOGRAVITY), jump to rise, climb/descend by the look pitch. Any inventory can grant it.
- **Morph** (`files/p_morph.c`): `P_MorphMonster(target, morphtype, tics)` turns a monster
  into a parameterised morph-creature (Heretic → chicken; Hexen → pig later) via a side-table
  (no `mobj_t`/savegame change); `P_MorphTicker` counts down and morphs it back (with a
  space-check), `P_MorphReset` clears it on level load.

### True invisibility
`pw_invisibility` (DOOM blursphere **and** the Heretic Shadowsphere) now makes the player/buddy
genuinely invisible to monsters: `P_LookForPlayers` won't acquire an invisible target and
`A_Chase` forgets one it isn't actively retaliating against — **only once you shoot a monster
does it come after you** (the shot sets its threshold in `P_DamageMobj`).

## 3. Hexen artifacts — planned
The generic flight + morph building blocks are in place (the Heretic Wings/Morph use them);
the Hexen item set (Flechette, Porkalator → pig morph, Disc of Repulsion, …) is future work.

## Console
`givearti <name>` gives any artifact for testing — DOOM overflow names
(stimpack/medikit/healthbonus/armorbonus/greenarmor/bluearmor/bullets/shells/rockets/cells)
and Heretic names (flask/urn/tome/torch/bomb/ring/shadow/chaos/wings/egg).

## Related docs
`BUDDY_PORTING.md` (the co-op buddy), `HERETIC_HEXEN.md` (the monster/asset ports),
`DIRECTOR_MODES.md` (the AI director), `CLAUDE.md` (architecture).
