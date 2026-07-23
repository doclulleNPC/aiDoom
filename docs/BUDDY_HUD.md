# BUDDY_HUD.md — companion and artifact HUDs

**Source audit:** 2026-07-22. The implementation described here is the current `hu_buddy.c` path; earlier TTF/strip experiments are historical only.

The active implementation is `files/hu_buddy.c` / `files/hu_buddy.h`. It no longer uses the older centered TTF strip described in historical notes. The current buddy HUD is rendered with Doom patch/font assets in the game overlay.

## Buddy HUD

`HU_Buddy_Drawer` draws the companion panel when:

- the game is in the appropriate level/draw state;
- `show_buddy_hud` is enabled;
- the buddy slot exists and `playeringame[slot]` is active.

It works for both `-coop` and `-aicoop`, because both modes run the buddy. The downed state is drawn rather than being suppressed.

### Layout

The panel is anchored at the **top-right** and contains:

- an animated `BUF*` buddy mugshot;
- three status lines for the current health/armor/weapon/ammo/behavior display;
- a fourth line for the buddy's own held artifact inventory icons/counts;
- `BUDDY DOWN` / revive guidance when downed;
- an eight-direction `RARR*` arrow from the human toward the downed buddy.

Text uses the message-font helpers in `hu_buddy.c`. The source function set—not historical helper names—is authoritative for rendering behavior.

## Human artifact HUD

`HU_Inventory_Drawer` is a separate bottom-center overlay controlled by `show_inventory_hud`. It shows the human player's currently selected artifact and held count when an artifact is selected/available.

Do not confuse these two displays:

- buddy top-right line: the buddy's own artifact inventory;
- human bottom-center line: the player's selected artifact.

## Configuration

`files/m_misc.c` persists:

- `show_buddy_hud` — default `1`;
- `show_inventory_hud` — default `1`.

Both can be changed through the current menu/config path. They are independent toggles.

## Assets

The HUD uses the current WAD/image assets loaded through the normal patch system:

- `BUF*` buddy face animation frames;
- `RARR*` directional arrows;
- the Doom HUD/message font and inventory icon assets where available.

If an asset is missing, the drawer should fail gracefully according to its lookup guards. A visually missing element should be debugged by checking the WAD lump name and the draw gate, not by reintroducing the deleted TTF-atlas implementation.

## Draw integration

The HUD is initialized/drawn from the normal engine overlay path. The relevant integrations are:

- initialization in startup/HU setup;
- per-frame draw from the HUD/video path after the 3D view is available;
- resolution-aware patch drawing through the existing `V_*` routines;
- live player/buddy state from `P_AICoop_Slot()` and `players[]`.

The 2D routines use the engine's 320×200-authored coordinate convention and scale through the current high-resolution video layer.

## Debugging checklist

1. Confirm `-coop` or `-aicoop` created the buddy slot.
2. Confirm `show_buddy_hud 1` / `show_inventory_hud 1` in the active config.
3. Confirm the game is in `GS_LEVEL` and the expected player slot is active.
4. Confirm the required face/arrow/icon lumps exist in the loaded WADs.
5. Confirm the overlay draw happens after the 3D view and before final presentation.
6. Use a known visible PLAYPAL index when drawing temporary debug pixels; palette index `255` is not guaranteed white.
7. Treat Xvfb-only rendering oddities as a separate verification concern and still test in the real game.

## Historical note

Earlier iterations experimented with a centered strip, TTF atlas rendering, background boxes and half-scale patch helpers. Those functions are not the current implementation and have been removed from this document to avoid turning design archaeology into fake API documentation. Git history remains the correct place to inspect those experiments.

## Source map

- `files/hu_buddy.c`, `files/hu_buddy.h` — current drawers, text helpers, face/arrow assets and toggles.
- `files/p_ai_coop.c` — buddy slot/state/artifact data.
- `files/m_misc.c` — persistent config defaults.
- `files/hu_stuff.c`, `files/i_video.c`, `files/v_video.c` — HUD/video integration.
- `docs/INVENTORY.md` — artifact behavior and current key defaults.
