# Driving BuddyDoom with an agent / LLM

> **Status note:** This document mixes historical design rationale with implementation notes. For the shipped player-agent API use `docs/AIPLAYER.md`; for monster/director control use `docs/MONSTER_AGENT_GUIDE.md`; for the buddy use `docs/BUDDY_PORTING.md`. The headless JSON/RL API proposed in later sections does not exist in the current engine.

## 0. Current shape

BuddyDoom has three separate external-AI surfaces:

1. `-aiplayer [port|demo]` — an external brain controls player 1 through `files/g_agent.c`.
2. `-aidirector [port]` / `-aidemo` — a director client controls monster tactics and pacing.
3. `-coop` / `-aicoop` — the second marine is run by the local buddy bot; `-aicoop` adds the director directive layer.

All gameplay still advances in the normal 35 Hz tic loop. External commands are high-level intent, not direct writes to arbitrary actor fields. Because external control changes the input stream, these modes should be considered single-player-only and not vanilla demo/netplay compatible.

## 1. Background: how player input works

The normal player path is `G_BuildTiccmd` in `files/g_game.c`. Keyboard/mouse input becomes a `ticcmd_t`, which the game consumes on the next simulation tic. The shipped player agent hooks that same boundary and fills the command from the reflex controller when `agent_active` is set.

The old design sketch below remains useful as architecture history, but the source names and option are now:

```c
// files/g_game.c
extern int agent_active;
void G_AgentBuildTiccmd(ticcmd_t *cmd);
```

Startup is in `files/d_main.c` (`G_AgentInit`). The option is `-aiplayer`, not the historical `-agent`.

## 2. Where to inject control

The stable hook is the ticcmd boundary, not the renderer or thinker list. A model should issue an intent such as “go to this point”, “attack that target”, or “use the door”, while the C reflex layer handles:

- fixed-point movement and turning;
- normal collision;
- weapon/tic timing;
- door passage and stuck recovery;
- buddy avoidance;
- target/line safety.

See `docs/AIPLAYER.md` for the actual protocol and steering order. The shared pathfinder is called `PF_AStar`, but it currently runs with `h=0`, so its search behaves as Dijkstra.

## 3. Observations

### 3a. Structured state (shipped)

The shipped agent/director paths serialize structured state from the engine:

- player position/angle/health/armor/weapon/region;
- live monsters and pickups as applicable;
- buddy state when active;
- distances, visibility, sounds/hearing and route/topology data;
- director stress/intensity data for the director observation.

The monster observation specifically includes regions and open/door/locked links. This supersedes the old claim that the external agent has no topology.

### 3b. Screen pixels

The engine can render normally, and an external system can capture the SDL window or framebuffer through tooling. There is no shipped vision-only/headless agent API in the core.

### 3c. Reward signal

There is no engine-side reward/done API. Reward calculation remains an external experiment/proposal.

## 4. Transport options: what exists versus what was proposed

| Transport | Current status |
|---|---|
| TCP newline protocol | **Shipped** for player agent and monster/director paths. |
| stdin/stdout JSON `OBS`/`ACT` | Proposal only. |
| Shared memory/semaphores | Not implemented. |
| MCP server | Not implemented in the game. A tool can wrap TCP externally. |
| gRPC | Not implemented. |

The current protocol is intentionally small and poll-based. Mutation lines do not receive a response envelope.

## 5. Monster/director path

The monster LLM hook is `A_LLMChase` through `files/p_ai_llm.c` / `files/p_enemy.c`. The client can issue one directive per monster, pacing spawns/items and buddy orders. `after=` delays a directive; it is not a command queue.

The rule director (`-director`) assigns focus/flank/fallback tactics and independently manages stress, objective guards, hidden spawns and safety gates. LLM `act` traffic does not itself reset the director pacing watchdog; only spawn/item/relax pacing commands do.

## 6. Buddy path

The buddy is not a silent slot:

- `-coop` runs the autonomous local bot;
- `-aicoop` runs the same bot plus high-level director overrides;
- the buddy is one fixed player-2 slot and is disabled in netgames.

Its navigation is a BSP-subsector graph search that behaves as Dijkstra. See `docs/BUDDY_PORTING.md` and `docs/Pathfinding.md`.

## 7. Historical proposal: headless stepping

A future headless API would need to be designed explicitly. It would likely add:

- `-headless` or a separate environment binary;
- a reset/step boundary around `TryRunTics`/game state;
- deterministic seed and episode management;
- server-side reward/done fields;
- optional frame skip;
- a transport wrapper.

None of those are current BuddyDoom command/API guarantees. Do not implement a client against the JSON sketches elsewhere in this document.

## 8. Compatibility and safety

- Keep model calls outside the game tic; deliver results through the existing line/socket path.
- Validate IDs and positions against the current observation.
- Bound spawn counts and route requests.
- Do not let a failed socket block the simulation.
- Do not store raw pointers in external protocol state.
- Keep source-side gameplay changes in fixed-point/tic-locked code.
- Treat savegame agent persistence conservatively: only a subset of intent/door state is archived, and the agent block is not independently versioned.

## References

- `docs/AIPLAYER.md` — actual player-agent protocol.
- `docs/MONSTER_AGENT_GUIDE.md` — actual monster/director protocol.
- `docs/DIRECTOR_MODES.md` — director semantics.
- `docs/BUDDY_PORTING.md` — buddy implementation.
- `docs/doom_agent_api_architecture.md` — future API proposal.
- `docs/doom_agent_api_vizdoom.md` — ViZDoom comparison/proposal.
