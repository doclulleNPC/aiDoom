# API architecture for an agent to play classic Doom

> **Design proposal, not an as-built BuddyDoom API.** The current engine ships TCP line protocols for `-aiplayer` and `-aidirector`; it does not ship the HTTP `/v1/game/init`/`step` server, headless mode, reward endpoint or MCP server described here. See `docs/AIPLAYER.md` and `docs/MONSTER_AGENT_GUIDE.md` for the implemented interfaces.

## 1. Core architecture and synchronization

BuddyDoom is a fork of the SDL Doom 1.10 line, not `doomgeneric`. The engine advances game state at 35 Hz through its normal tic loop. A future agent environment should preserve that boundary:

```text
agent client -> transport -> queued high-level action
                               |
                         game tic boundary
                               |
                ticcmd/reflex -> playsim -> observation
```

The current player agent already uses the safe part of this shape: `G_BuildTiccmd` delegates to `G_AgentBuildTiccmd` when active. A future HTTP environment would need a separate lifecycle/step wrapper rather than pretending the current TCP listener is an HTTP server.

## 2. Proposed API schema

The following is a proposed HTTP/RL shape only.

### `POST /v1/game/init`

```json
{
  "iwad": "DOOM2.WAD",
  "pwads": ["map.wad"],
  "map": "MAP01",
  "skill": 3,
  "seed": 1
}
```

A real implementation would need to define IWAD path policy, process lifetime, reset semantics, asset isolation and whether the engine is embedded or launched as a worker.

### `POST /v1/game/step`

#### Proposed request

```json
{
  "actions": {
    "forward": 1,
    "side": 0,
    "turn": -20,
    "fire": 1,
    "use": 0,
    "weapon": 0
  },
  "repeat": 1
}
```

The request must be converted to a `ticcmd_t` at a deterministic tic boundary. `repeat` is a proposal for frame skip, not an existing engine option.

#### Proposed response

```json
{
  "tick": 1234,
  "done": false,
  "reward": 0,
  "player": {},
  "entities": [],
  "events": []
}
```

The current engine does not emit these fields as a server contract. Reward and `done` semantics need an explicit design covering death, intermission, finale, map transitions and episode reset.

## 3. Data extraction/interception

The engine already has the data needed for a useful structured observation:

- player state in `player_t`/`files/d_player.h`;
- actors in the thinker/mobj system;
- map geometry, sectors, lines, subsectors and regions;
- sound events through the agent sound feed;
- director/buddy state through their existing serializers.

The current player and monster protocols should be reused as source material rather than adding a second conflicting representation. For reference, the player listener accepts `map`, `observe`, `goto`, `face`, `turn`, `attack`, `target`, `weapon`, `use` and `stop`; query replies are JSON, while mutation commands are silent. Any new API should define stable IDs, visibility semantics, fixed-point-to-integer conversion and map-transition behavior.

## 4. Transport choices

| Transport | Suitability | Status in BuddyDoom |
|---|---|---|
| TCP newline protocol | Simple remote control with low implementation cost | Shipped for player and monster/director agents |
| HTTP/JSON | Easy integration with RL wrappers and tools | Proposed only |
| Shared memory | High-throughput local training | Not implemented |
| stdin/stdout | Simple subprocess prototype | Not implemented as the current engine agent protocol |
| MCP | Tool-oriented LLM integration | External wrapper proposal only |

## 5. Determinism and reset

A real step server must own the simulation clock. It cannot merely sleep and sample the renderer. It should:

1. accept an action at a tic boundary;
2. build a `ticcmd_t` or equivalent input;
3. advance exactly the requested number of tics;
4. collect observations/events after simulation;
5. return reward/done and the next state;
6. reset all thinkers, RNG/seed policy, player state, map state and external AI state on episode reset.

The current `-aiplayer` TCP client does not provide this reset/step contract.

## 6. Security and process boundaries

If the proposal is implemented, do not let an HTTP client write arbitrary filesystem paths or run arbitrary console commands. Use an allowlisted IWAD/PWAD root, bounded action values and a worker/process boundary for untrusted clients.

## 7. Relationship to current docs

- Actual player agent: `docs/AIPLAYER.md`.
- Actual monster/director API: `docs/MONSTER_AGENT_GUIDE.md`.
- Design rationale for all AI control: `docs/AGENT_CONTROL.md`.
- ViZDoom comparison: `docs/doom_agent_api_vizdoom.md`.
