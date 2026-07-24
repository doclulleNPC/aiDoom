# AIPLAYER.md — the `-aiplayer` agent (as built)

## Status

This is the source-backed description of the player-agent implementation in `files/g_agent.c` / `files/g_agent.h`. It is not the same thing as the older design proposal in `docs/AGENT_CONTROL.md`, and it is not a headless RL environment.

The agent drives player 1 in single-player mode. It is deterministic and tic-locked once an intent has been received, but an external client/LLM changes gameplay outside vanilla demo/netplay input semantics.

## 1. Startup and modes

The `-aiplayer` mode currently listens as `./buddydoom` (the legacy binary name; the project was renamed from `BuddyDoom` to **BuddyDoom** but the binary path was not changed in the same patch). The launch examples below will become `./buddydoom` once the matching source/build rename lands.

```sh
./buddydoom -aiplayer 31700
./buddydoom -aiplayer demo
```

The argument is optional: omitted/invalid port syntax uses the default TCP port `31700`; `demo` selects the built-in scripted brain. Startup is initialized from `files/d_main.c` through `G_AgentInit`.

The agent is built into the normal game binary and hooks `G_BuildTiccmd` in `files/g_game.c`. It does not add a separate headless binary, reward API or frame-step API.

## 2. Wire protocol

The listener is loopback-only (`127.0.0.1`) and accepts a single client. This avoids exposing a raw gameplay control socket to the LAN.

The non-blocking poll runs inside the game tick service, so a slow brain does not stall the playsim.

### Commands

The static `map` query sends the current player-start coordinates, an exit coordinate or `null`, door midpoints and up to 1200 line tuples `[x1,y1,x2,y2,special]`. The start coordinates are serialized directly by `Agent_SendMap`; clients should not assume they use the same conversion as dynamic `observe` coordinates.

Outside a level, `observe` returns the minimal player-agent response `{"player":null}`. The monster/director listener has a different no-level response (`{"nolevel":true,"monsters":[]}`).

### `observe` fields

The player agent's `observe` response contains `tic`, `player` (x/y/z, angle, health, armor, weapon, ammo, kills and items), `exit`, `buddy`, `things`, `doors`, eight-direction `lidar`, `sounds` and `waiting_at_door`. `things` excludes corpses and barrels; IDs are indexes into the current response.

### Intents

The command surface is: `map`, `observe`, `goto <x> <y>`, `face <x> <y>`, `turn <degrees>`, `attack <0|1>`, `target <id>`, `weapon <1..8>`, `use` and `stop`. `map`/`observe` are the only commands that receive JSON; mutation commands are silent.

The current implementation has no JSON action envelope, no generic `reset` response and no reward/done message. Those belong to the proposal docs, not this as-built protocol.

## 3. Reflex controller (`G_AgentBuildTiccmd`)

The external brain is intentionally slow. The C reflex layer runs every tic and owns the details that must remain immediate and collision-safe:

1. maintain the current target/goal and intent timers;
2. acquire/update the current navigation waypoint;
3. pass through a detected door in two phases;
4. try a direct goal when reachable;
5. otherwise follow the shared BSP-subsector route;
6. fall back to Doom's `ChaseDir`/turning behavior when no route is available;
7. avoid crowding the buddy when steering;
8. choose/use weapons and fire only when the geometry/target policy permits;
9. recover from stuck movement and clear completed door state.

The two-phase door behavior is important: the agent first reaches the door center, then continues roughly `96` map units beyond the opening before clearing the active passage state. The door bookkeeping uses the existing `160`-unit look-ahead/recognition behavior where applicable; these are not interchangeable constants.

The source uses `PF_AStar` as a historical function name, but its implementation is explicitly Dijkstra (`h=0`) and the door-special list used by the stuck-handler is intentionally narrow (`1`, `31`, `117`, `118`). The pathfinder also classifies a larger door set for graph/topology use.

## 4. Perception details

The agent receives structured world state rather than framebuffer pixels. `G_Agent_LogSound` is called from `S_StartSound` to populate the sounds/hearing feed. The observation path uses Doom's fixed-point positions and angles.

The barrel/line safety check in the reflex code is a geometric line-position approximation. Unlike a full buddy target check, it should not be documented as an unconditional `P_CheckSight` trace.

## 5. Persistence and savegames

`g_game.c` calls `G_Agent_Archive` and `G_Agent_UnArchive`. The serialized block covers a subset of agent state, including intents, exit/key state, door wait and door-seen flags.

It does not serialize the socket/client, live registry pointers, all active/beyond-door geometry, stuck history or every built-in demo-brain static. Do not promise that an arbitrary in-flight agent session resumes bit-for-bit after loading.

The current save block has no independently versioned/tagged agent section. Compatibility claims for old saves should therefore be treated conservatively and revalidated if the agent state grows.

## 6. Relationship to the other AI modes

- `-coop` / `-aicoop` control the second marine, not player 1.
- `-aidirector` controls pacing/spawns/tactics through the monster director protocol.
- `-director` enables the offline rule director.
- `docs/AGENT_CONTROL.md` contains historical design rationale and proposal material; it is not the API reference.

## 7. Source map

- `files/g_agent.c` — listener, parser, serializer, intent state and reflex controller. The exact player commands are `map`, `observe`, `goto <x> <y>`, `face <x> <y>`, `turn <degrees>`, `attack <0|1>`, `target <id>`, `weapon <1..8>`, `use` and `stop`; query replies are JSON and mutation commands are silent.
- `files/g_game.c` — `G_BuildTiccmd` hook and save hooks.
- `files/d_main.c` — startup initialization.
- `files/p_ai_coop.c` — shared buddy navigation helpers and avoidance.
- `files/p_sight.c` — line-of-sight implementation used by perception paths.
- `files/s_sound.c` — sound feed hook.
- `docs/AGENT_CONTROL.md` — design history/proposals.
