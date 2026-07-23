# Buddy AI: behavior and porting notes

## What the buddy is

The companion is player 2 (`coop_slot=1`) implemented in `files/p_ai_coop.c` / `.h`. It is a second marine using the normal player/mobj, weapon, pickup, damage and thinker systems.

- `-coop` enables the autonomous rule-based buddy.
- `-aicoop` enables the same buddy plus the LLM/director high-level directive layer.
- The buddy is disabled in netgames. Its simulation is tic-locked, but it is currently a single-player feature rather than a promise of demo/net compatibility.
- There is one fixed buddy slot; `-aicoop N` is not implemented.

## Decision order

The C bot builds a ticcmd every game tic. The broad behavior priorities are:

1. obey a temporary explicit order (`goto`, `hold`, `retreat`, `grab`, etc.);
2. recover/revive or seek support when downed/hurt and the current rule permits it;
3. acquire a visible target and fight;
4. avoid unsafe blast/projectile situations and crowding the human;
5. follow the player or move toward the current objective;
6. recover from a stuck/door state.

The exact decision path is source-driven and can change with the active mode. High-level commands do not bypass normal collision; they change intent consumed by the reflex controller.

## Targeting and combat

`AICoop_FindTarget` scans live shootable count-kill actors within the configured sight/range policy, checks direct sight and applies blacklist/priority rules. Weapon choice uses the buddy's own ammo and the normal Doom weapon state. Rocket/BFG splash safety, barrel avoidance, projectile dodging and friendly-fire protection are handled in the controller/interaction paths.

The buddy can yield when the human is very close, rather than continually trying to occupy the same space. It also contains support behavior for nearby dead marines and can auto-deploy the Security Drone under the implemented pressure conditions.

## Navigation

The pathfinder is in the same source file:

- graph nodes are BSP subsector centers;
- edges connect passable portal-like line transitions;
- door and damage-floor penalties affect costs;
- `PF_AStar` is the historical public name, but the heuristic is disabled (`h=0`), so the current search behaves as Dijkstra;
- `P_AICoop_NextWaypoint` returns a useful next waypoint and direct-reachability/string-pulling reduces corner scraping;
- `P_AICoop_NoteDamage` feeds the danger heatmap used by safe-route mode.

Door movement is not a blind USE spam loop. The bot recognizes a usable door ahead, applies a cooldown and approaches the opening using the route/door state. The source is authoritative for the supported special set; the old short list in this doc should not be treated as exhaustive.

Low steps can be jumped with the existing jump tic button where the local jumpability check permits it. The buddy path is not active in netgames.

## Objectives and support

The buddy follows the human by default and can be routed toward navigation/objective positions. It does not pocket keys: keycards/skulls remain available to the human.

When downed, the buddy remains in the level and can be revived by the human pressing USE nearby. `COOP_REVIVE_RANGE` is **64 map units**, not 96. The revive consumes one health item from the human inventory:

- stimpack first, reviving at 10 HP;
- otherwise medikit, reviving at 25 HP.

The buddy also has its own held health/artifact support path and displays its held artifact inventory in the HUD.

## Voice

Voice clips are pre-baked in the voice PWAD and routed through `files/i_voice.c`. The buddy gate is rate-limited and busy-aware; higher priority does not preempt an already-playing buddy clip. Director voice is a separate stream. See `docs/BUDDY_VOICE.md` for the exact tiers and cooldowns.

## HUD

The buddy HUD is top-right and includes a mugshot, animated status, three status lines, an artifact inventory line, downed text and a directional revive arrow. It is controlled by `show_buddy_hud`; the separate human selected-artifact overlay is controlled by `show_inventory_hud`. See `docs/BUDDY_HUD.md`.

## Console and config

Current buddy command aliases are implemented in `files/c_console.c`:

| Command | Effect |
|---|---|
| `where` / `buddy` / `comp` | Position/state report |
| `come` / `follow` | Follow order |
| `wait` / `stay` | Hold position |
| `attack` | Engage order |
| `report` / `status` | Stats report |
| `buddygod` | Invulnerability toggle |
| `buddyheal` / `buddyhp` | Heal/revive debug helper |
| `buddyarm` / `buddygive` | Give weapons/ammo/armor |
| `buddyhome` / `buddytp` | Return to map spawn |

Default keys are `,` (come), `.` (attack), `-` (stay), with `key_buddy_mode` unbound by default. `-buddyreact <tics>` is clamped to `0..70`.

## Current constants

| Constant | Value | Meaning |
|---|---:|---|
| `COOP_REVIVE_RANGE` | 64u | Human USE revive range |
| `COOP_HEAL_HP` | 50 | Buddy healing target/behavior threshold as used by source |
| `COOP_SAFE_HP` | 40 | Safe-route mode threshold |
| `COOP_BLAST_SAFE` | 176u | Rocket/BFG splash safety |
| `COOP_DODGE_RANGE` | 256u | Projectile dodge range |

Angles such as the movement turn constants are Doom's fixed angle-turn units; they are not degrees and should not be described as a full BAM value without qualification.

## Source map

- Buddy bot, pathfinder and directives: `files/p_ai_coop.c`, `files/p_ai_coop.h`.
- Player/mobj interactions and revive/item costs: `files/p_inter.c`, `files/p_mobj.c`, `files/p_invent.c`.
- Buddy HUD: `files/hu_buddy.c`, `files/hu_buddy.h`.
- Voice: `files/i_voice.c`.
- Console/config: `files/c_console.c`, `files/m_misc.c`.
- Agent/director integration: `files/p_ai_llm.c`, `files/p_ai_director.c`.
