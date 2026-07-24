# BUDDYDOOM_PARAMETERS.md — current CLI/config reference

**Source audit:** 2026-07-22. This file covers the engine options that are actually parsed in the current tree plus persistent keys consumed by the SDL3 tools. Old launcher options are not silently treated as supported.

**Naming note:** The project was previously known as `aiDoom` and has been renamed to **BuddyDoom**. The binary, persistent config filename and voice WAD still use the legacy `aidoom` token until the matching source/build rename lands: `./aidoom` (binary), `run/aidoom.cfg` (config), `aidoom.wad`/`aidoom_wad` (voice asset), `aidoom_config` (config tool), `AIDOOM_VERSION` (version macro, `files/aidoom_version.h`), `~/.aidoom/` (hypothetical user state). Each identifier is listed below with the same spelling the engine uses today.

## Important distinction

- A switch described as **implemented** is parsed or consumed by the current engine.
- A switch described as **tool-side** belongs to `tools/` and is not an engine gameplay option.
- Historical/proposed options are listed only when they are useful for recognizing old launch scripts; they are not presented as supported.

## Vanilla and compatibility switches

The engine still accepts the usual Doom startup parameters such as:

- `-iwad <file>` — select the IWAD.
- `-file <pwads...>` — load PWADs.
- `-deh <file>` — process an external DeHackEd/BEX text patch.
- `-warp <episode> <map>` or the supported map forms — start at a map.
- `-skill <1..5>` — choose skill.
- `-record <name>` / `-playdemo <name>` — record or play demos.
- `-timedemo <name>` — run a timed demo.
- `-complevel <level>` — select the compatibility level supported by the build.
- `-nomonsters`, `-respawn`, `-fast`, `-skill`, `-deathmatch`, `-altdeath` and the normal Doom networking/startup options where applicable.

The exact parser is `files/d_main.c`; this document deliberately does not claim support for options that are only found in old scripts or in another port.

## Network and demo compatibility

BuddyDoom contains both the original Doom-style networking code and a separate transport-side client implementation in `files/d_netcl.c`. The latter is not a finished replacement for the main game-loop netcode. Do not infer a complete multiplayer product from the presence of the transport module.

The AI companion is currently disabled in netgames (`files/p_ai_coop.c`). AI-controlled play and LLM director modes are therefore single-player features, even though their simulation work is tic-driven.

## AI and director modes

| Option | Meaning |
|---|---|
| `-coop` | Create the second marine and run the autonomous rule-based buddy. |
| `-aicoop` | Create the second marine, run the buddy bot, and allow the LLM/director tactic layer to override it. |
| `-buddyreact <tics>` | Set the buddy reaction delay; the value is clamped to `0..70`. |
| `-director` | Enable the offline/rule-based L4D-style director. |
| `-aidirector [port]` | Enable the TCP LLM director. The default port is `31666`. |
| `-aidemo` | Enable the built-in director demo/stress path without an external client. |
| `-aiplayer [port\|demo]` | Let the external agent or built-in demo brain control player 1. The default TCP port is `31700`. |

The monster/director wire protocol is documented in `docs/MONSTER_AGENT_GUIDE.md`. The as-built player-agent protocol is documented in `docs/AIPLAYER.md`.

These modes are not demo/netplay compatible: external commands change gameplay outside the vanilla deterministic input stream.

## Console

Toggle the Quake-style console with **F12** or backquote (`` ` ``). Commands are implemented in `files/c_console.c`.

General commands include:

- `help`, `clear`, `echo`, `quit`
- `god`, `noclip`, `notarget`
- `give`, `map` / `warp`
- `summon <type>` where the current actor tables provide the requested type
- `director [on|off|demo]` — toggle the rule/LLM director path; aliases include `ai` and `llm`.

Buddy commands and aliases include:

| Command | Alias / effect |
|---|---|
| `where` | Also `buddy`, `comp`; report the buddy position/state. |
| `come` | Also `follow`; order the buddy to follow. |
| `wait` | Also `stay`; hold position. |
| `attack` | Order the buddy to engage. |
| `report` | Also `status`; report HP, armor, weapon and ammo. |
| `buddygod` | Toggle buddy invulnerability. |
| `buddyheal` | Also `buddyhp`; heal/revive the buddy. |
| `buddyarm` | Also `buddygive`; give weapons, ammo and armor. |
| `buddyhome` | Also `buddytp`; teleport the buddy to its map spawn. |

The `llm` console alias currently toggles the director; it is not a general chat-to-buddy command.

## Gameplay flags

Important implemented flags also include:

- `-infight` — allow same-species infighting behavior.
- `-nofriendlyfire` / `-noff` — protect player and buddy from each other.
- `-infinitetall` — restore vanilla infinitely-tall actor collision; normal BuddyDoom behavior permits over/under object movement.
- `-autoaim` — restore vanilla vertical aim assist; the default player behavior uses free-look pitch.
The engine currently exposes `monster_pack` and `monster_pack_range` as persistent configuration keys (`files/p_enemy.c`, `files/m_misc.c`); there is no separate `-monsterpack` command-line switch in the current parser.

Always check `files/d_main.c` and the relevant subsystem before copying a flag into a launcher: old launch scripts in `tools/scripts/` are historical and may contain options no longer parsed.

## Persistent configuration

The game reads and writes `run/aidoom.cfg` through `files/m_misc.c`. Relevant current keys include:

### Buddy and HUD

- `show_buddy_hud` — buddy top-right HUD; default `1`.
- `show_inventory_hud` — selected artifact inventory HUD; default `1`.
- `key_buddy_come` — default `,`.
- `key_buddy_attack` — default `.`.
- `key_buddy_stay` — default `-`.
- `key_buddy_mode` — default unbound (`-1`).

### Artifact inventory

The current defaults are arrow keys, not the historical bracket/Enter/`d` table:

| Action | Default | Config key |
|---|---|---|
| Select previous artifact | Left Arrow | `key_inv_left` |
| Select next artifact | Right Arrow | `key_inv_right` |
| Use selected artifact | Down Arrow | `key_inv_use` |
| Drop selected artifact | Up Arrow | `key_inv_drop` |

### Monster and voice settings

- `monster_pack` — pack-hunt toggle; default `0`.
- `monster_pack_range` — pack range; default `2048`.
- `aidoom_wad` — voice/asset WAD path; the legacy `buddy_wad` key is accepted for compatibility. Default is `aidoom.wad`, resolved through `I_Voice_ResolveWad` and the ID0 lookup.

### Tool-side Ollama/GPU settings

The following are read and written by SDL3 tools such as `tools/aidoom_config.c`, `tools/director.c`, and `tools/gpumon_sdl.c`; the engine preserves them when saving the config but does not use them as gameplay settings:

- `ollama_host` — config-app default `localhost`.
- `ollama_port` — default `11434`.
- `ollama_model` — config-app default `ministral-3:8b`.
- `gpu_host` — default `localhost`.
- `gpu_user` — gpumon's built-in fallback is `lubee`; `aidoom_config` may write an empty value for the user to fill in. Do not assume the two tools have the same default.
- `gpu_ssh_port` — SSH port used by gpumon.

## Historical or unsupported options

Some old notes mention `-fastdemo`, `-cdrom`, `-server`, or `-headless`. They are not part of the current shipped engine interface described above. `docs/AGENT_CONTROL.md`, `docs/doom_agent_api_architecture.md`, and `docs/doom_agent_api_vizdoom.md` contain design material for a future stepped/headless API; that API is not implemented merely because the proposal exists.

## Source of truth

- Startup parsing and mode initialization: `files/d_main.c`.
- Configuration defaults: `files/m_misc.c`.
- Console commands: `files/c_console.c`.
- Player-agent protocol: `files/g_agent.c`, `files/g_agent.h`, `docs/AIPLAYER.md`.
- Monster/director protocol: `files/p_ai_llm.c`, `files/p_ai_director.c`, `docs/MONSTER_AGENT_GUIDE.md`.
- Buddy behavior and commands: `files/p_ai_coop.c`, `files/hu_buddy.c`, `docs/BUDDY_PORTING.md`.
