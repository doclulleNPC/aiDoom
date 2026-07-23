# Monster LLM control — design rationale and shipped boundary

## Scope

This document explains why an LLM can be useful for high-level monster tactics, then states the boundary of what is actually shipped. The operational protocol is `docs/MONSTER_AGENT_GUIDE.md`; the player-agent protocol is `docs/AIPLAYER.md`.

## 1. Keep Doom's simulation

The engine's actor state machine, thinker list, fixed-point geometry, blockmap collision and `P_CheckSight` remain authoritative. An external model should not directly rewrite arbitrary `mobj_t` fields or run off-tic wall-clock logic.

The model supplies intent at a slower cadence. The C layer stores a directive and executes it on the normal 35 Hz tic flow. This preserves the existing collision, attack and animation code while allowing tactics such as focus, flank or fallback.

## 2. Three control granularities

1. **Per-tic motor control:** too large and too latency-sensitive for a remote LLM.
2. **State-machine replacement:** powerful but destroys vanilla behavior and compatibility.
3. **High-level tactical intent:** the shipped compromise. The LLM chooses an order/destination; `A_LLMChase` and the existing movement/attack states execute it.

The current director also accepts pacing commands (`spawn`, item spawn and `director relax`) separately from monster `act` tactics.

## 3. Where the hook lives

The implemented path is:

- `files/p_ai_llm.c` / `.h` — TCP line parser, observations and directive side table;
- `files/p_enemy.c` — monster chase hook diverts directed monsters to `A_LLMChase`;
- `files/p_tick.c` / game ticker path — AI network service and per-tic integration;
- `files/p_ai_director.c` — LLM/rule pacing, spawns, stress and announcements;
- `files/p_ai_coop.c` — shared waypoint navigation and optional buddy directives.

The side table is keyed by `mobj_t *`; no monster directive fields were added to savegame structs.

## 4. Actual observation

The current `observe` reply is JSON, but the transport is a plain newline command protocol. It includes, where available:

- player position, angle, health, armor, weapon and region;
- optional buddy position/health/armor/weapon/ammo/state/region, distances and route;
- live monsters with ID, type, position, HP, region, visibility to player/buddy, distances and current order;
- region centroids and topology links labelled open/door/locked;
- director intensity/state/recent damage/ammo percentage;
- a no-level response with `nolevel=true`.

The server is polled by the client. It is not an event-driven push API.

## 5. Actual action surface

Monster tactics:

```text
act order=<none|chase|hold|fallback|flank_left|flank_right|focus_fire|ambush|use_door> ids=<id-list|all> [focus=0|1] [x=... y=...] [for=tics] [after=tics]
```

Pacing:

```text
spawn type=<name> count=<1..8>
spawn item=<medkit|ammo>
director relax
```

Buddy layer:

```text
buddy order=<engage|defend|hold|regroup|retreat|goto|grab> ...
```

Utility:

```text
reset
wake
```

There is one directive slot per monster. `after` delays that directive; it is not a queue. A subsequent `act` overwrites the stored order.

## 6. What is not shipped

The following remain design proposals rather than current engine APIs:

- gRPC/MCP as a native transport;
- stdin/stdout JSON `OBS`/`ACT` framing;
- a `-headless` no-video/no-audio stepped environment;
- server-side reward/done/reset semantics for RL;
- a teleporter-specific order;
- arbitrary encounter IDs or named monster objects independent of the current registry.

Use `docs/doom_agent_api_architecture.md` and `docs/doom_agent_api_vizdoom.md` as proposals/comparisons, not as implementation references.

## 7. Determinism and compatibility

The C execution is tic-locked, but an external LLM changes the command stream and can be nondeterministic. Do not promise vanilla demo or netplay compatibility. The rule director is also gameplay code, not a proof of network synchronization.

The current navigation helper is historically named `PF_AStar`, but the heuristic is disabled and the search behaves as Dijkstra. Door use is approximate high-level behavior, not a fully modelled teleporter/door planner.

## 8. Source map

- `files/p_ai_llm.c`, `files/p_ai_llm.h` — protocol/serializer/directives.
- `files/p_ai_director.c`, `files/p_ai_director.h` — pacing and rule/LLM director.
- `files/p_enemy.c` — chase integration.
- `files/p_ai_coop.c` — buddy and navigation.
- `docs/MONSTER_AGENT_GUIDE.md` — operator guide.
- `docs/AGENT_CONTROL.md` — wider design history.
