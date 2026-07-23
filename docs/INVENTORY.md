# INVENTORY.md — artifact and item inventory systems

This document separates the small Doom co-op overflow inventory from the Heretic artifact system. The two systems share UI/configuration helpers but are not the same inventory model.

## Current key defaults

The configuration key defaults are the current source defaults: `show_buddy_hud=1`, `show_inventory_hud=1`, `key_buddy_come=,`, `key_buddy_attack=.`, `key_buddy_stay=KEY_MINUS`, `key_buddy_mode=-1` (unbound), and arrow keys for the four inventory actions.

| Action | Default | Config key |
|---|---|---|
| Select previous item | Left Arrow | `key_inv_left` |
| Select next item | Right Arrow | `key_inv_right` |
| Use selected item | Down Arrow | `key_inv_use` |
| Drop selected item | Up Arrow | `key_inv_drop` |

The selection skips empty slots and wraps. Users can rebind all four actions in the config/menu system.

## 1. Doom overflow inventory — buddy-only

The overflow path is intentionally conservative: in solo play a health pickup at full health remains on the floor, as in vanilla Doom. With `-coop` or `-aicoop`, `P_StoreOverflow` can let the player/buddy path retain health and ammo pickups that would otherwise be wasted (`files/p_invent.c`, `files/p_inter.c`, `files/p_ai_coop.c`).

The buddy has a small held-artifact state used for automatic support behavior. It is not a general arbitrary-item bag and it does not replace Doom's normal weapon/ammo inventory.

### Second wind

`P_InventorySecondWind` is called from damage handling. When enabled by the companion inventory path, a medikit or stimpack can be consumed to prevent the supported marine from dropping all the way out:

- medikit: 25 HP;
- stimpack: 10 HP.

This is separate from the buddy's downed/revive state. The buddy itself is not revived through second wind; it uses the explicit co-op revive flow described in `docs/BUDDY_PORTING.md`.

## 2. Heretic artifacts

The Heretic artifact implementation lives in `files/p_invent.c`, `files/p_inv_heretic.c`, `files/p_morph.c`, and the player fields in `files/d_player.h`. The current artifact set is:

- Quartz Flask
- Mystic Urn
- Tome of Power
- Torch
- Time Bomb of the Ancients
- Ring of Invincibility
- Shadow Sphere
- Morph Ovum
- Wings of Wrath
- Chaos Device

The exact enum/field names are `h_arti_flask` through `h_arti_egg` in `files/d_player.h`.

### Implemented behavior

- Pickup, selection, use and drop are implemented through the inventory helpers.
- The Quartz Flask and Mystic Urn provide the current healing effects.
- Tome, Torch, Ring, Shadow Sphere, Chaos Device and Wings have their current ported effects where the corresponding player/game-mode hooks are active.
- Wings use the generic flight support.
- Morph Ovum calls the generic morph subsystem (`P_MorphMonster` in `files/p_morph.c`) and turns eligible monsters into the current supported morph form.
- True invisibility changes monster target acquisition/visibility behavior through the player/mobj and enemy paths.

This is additive content support, not a promise that aiDoom is a complete Heretic game mode. Full Corvus weapons, Heretic status-bar behavior, level progression and all original artifacts remain part of `docs/HERETIC_SUPPORT_PLAN.md`.

## 3. Hexen artifacts

The Hexen artifact set—Flechette, Porkalator, Disc of Repulsion and related class-specific behavior—is planned. The generic flight/morph building blocks exist, but the complete Hexen item and player system is not implemented.

## Console and HUD

The artifact inventory HUD is drawn by `HU_Inventory_Drawer` in `files/hu_buddy.c` and is controlled by `show_inventory_hud`. The selected artifact and held count appear in the bottom-center overlay when an artifact is selected/held.

The buddy's own artifact readout is a separate fourth line in the buddy HUD. See `docs/BUDDY_HUD.md`.

Buddy/admin console commands (`buddyheal`, `buddyarm`, `buddyhome`, etc.) are not artifact-selection commands; they are debug/control helpers implemented in `files/c_console.c`.

## Source map

- Generic inventory and overflow: `files/p_invent.c`, `files/p_inter.c`.
- Heretic artifacts: `files/p_inv_heretic.c`, `files/d_player.h`.
- Morphing: `files/p_morph.c`.
- Player input/config defaults: `files/m_misc.c`, `files/g_game.c`.
- HUD: `files/hu_buddy.c`, `files/hu_buddy.h`.
