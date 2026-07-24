# API for an agent to play classic Doom — ViZDoom / LLM perspective

> **Proposal/comparison document.** This is not the shipped BuddyDoom API. Current as-built references are `docs/AIPLAYER.md` (player agent), `docs/MONSTER_AGENT_GUIDE.md` (monster/director) and `docs/BUDDY_PORTING.md` (buddy). The HTTP/headless/RL/MCP interfaces below are design options.

## 0. The real options

There are four practical ways to let an agent play Doom:

1. use an existing environment such as ViZDoom;
2. wrap the current BuddyDoom TCP player-agent listener;
3. add a native headless/step API to BuddyDoom;
4. build a separate tool process that captures the SDL window and sends input.

The current tree implements option 2 for high-level player control (`-aiplayer [port|demo]`). It does not implement a ViZDoom-compatible HTTP or Gym server.

## 1. Five layers

A robust environment separates:

1. engine/simulation;
2. action adapter;
3. perception/observation;
4. reward/episode logic;
5. transport and agent orchestration.

The agent protocol is client-driven polling. `map` and `observe` receive JSON; the intent commands mutate controller state without emitting an acknowledgement. The game continues at 35 Hz.

## 2. Layer 1 — engine

The source engine provides:

- fixed-point 35 Hz simulation;
- player/mobj/thinker state;
- WAD/map loading;
- software renderer and SDL3 presentation;
- sound events and normal Doom interaction.

The safest integration point is the ticcmd boundary. A future environment should not directly mutate actors from an HTTP handler.

## 3. Layer 2 — perception

### Structured observation

The current agent/director serializers can expose positions, health, armor, weapons, ammo, monsters, visibility/distances, sounds, buddy state, map regions/links and director stress. This is generally cheaper and more deterministic than screenshot-only control.

### Pixels

A vision agent can capture the SDL window or use a future framebuffer endpoint. The current core does not expose a headless pixel observation contract.

### Tracking

A future API must define stable entity IDs. Current monster IDs are registry-derived and should be refreshed from every observation; they are not guaranteed to survive roster/map changes.

### Damage tracking

A future reward layer can derive damage/kills/secrets from state deltas or explicit engine events, but this is not currently returned by BuddyDoom's TCP API.

## 4. Layer 3 — action space

### Current TCP player-agent action concept

The shipped agent accepts high-level intent commands and converts them in C to movement/turn/fire/use/weapon ticcmds. This is intentionally more semantic than a raw screenshot-to-keypress loop.

### Proposed Gym/HTTP action

A future wrapper could accept:

```json
{
  "forward": -1,
  "side": 1,
  "turn": 32,
  "fire": true,
  "use": false,
  "weapon": 3
}
```

The wrapper must clamp values, convert them to fixed-point/engine units and define whether an action lasts one tic or a repeated batch.

## 5. Layer 4 — reward and episode state

A future reward function might combine:

```text
reward = 100 * delta_kills
       + 50 * delta_secrets
       + 1 * delta_health
       - tick_penalty
```

That is a proposal, not a source behavior. Decide explicitly how to handle weapon pickups, damage, death, intermission, finale, secrets, map exits and timeouts. Reward should be computed at the simulation boundary if the goal is a self-contained environment.

## 6. Layer 5 — orchestration

Possible wrappers include:

- a Python client around the current TCP listener;
- an HTTP server around a dedicated worker process;
- a Gymnasium adapter;
- an MCP server exposing start/observe/act tools;
- a vision capture/input process.

These are external/tooling projects until code lands in the repository. Do not document them as installed BuddyDoom binaries.

## 7. ViZDoom comparison

| Concern | Current BuddyDoom tree | ViZDoom-style environment |
|---|---|---|
| Simulation | Native SDL3 Doom fork | Dedicated environment wrapper |
| Raw screen API | SDL window/tool capture | First-class observation API |
| Structured state | Existing agent/director serializers | Scenario-defined game variables |
| Action timing | Normal 35 Hz game loop | Environment step contract |
| Reward/done | Not shipped | Environment-defined |
| Reset | Normal game/save lifecycle | First-class episode reset |
| Remote protocol | TCP line protocol | Usually in-process/Python API |

## 8. Recommended path

For an LLM or low-throughput agent, start with the shipped TCP protocol and structured observations. For RL, build a separate deterministic step wrapper with explicit reset/reward/seed semantics. Do not pretend that adding JSON around the current socket automatically creates a ViZDoom replacement.

## References

- `docs/AIPLAYER.md` — current player-agent implementation.
- `docs/MONSTER_AGENT_GUIDE.md` — current monster/director protocol.
- `docs/doom_agent_api_architecture.md` — proposed HTTP/RL architecture.
- `docs/AGENT_CONTROL.md` — design rationale and boundaries.
