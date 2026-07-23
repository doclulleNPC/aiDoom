# MONSTER_AGENT_GUIDE.md — current TCP protocol

This is the operational reference for the line protocol implemented by `files/p_ai_llm.c`. It drives monster directives, the optional buddy directive layer, and LLM-director pacing commands. It is separate from the player-1 protocol in `docs/AIPLAYER.md`.
## 1. Connect

Start aiDoom with the director listener:

```sh
./run/aidoom -aidirector 31666 -warp 1 1
```

The port is optional; the default is `31666`. The protocol is plain newline-delimited text over TCP. Queries (`map`, `observe`) receive JSON; mutation commands (`act`, `spawn`, `director`, `buddy`, `reset`, `wake`) receive `ok` when a client is connected. The client polls observations and sends intents; the game loop continues at 35 Hz.

## 2. Observe

Send:

```text
observe
```

The reply is one JSON object. In a level it contains the current structured state. The exact serializer is `AI_Serialize` in `files/p_ai_llm.c`.

Representative shape:

```json
{
  "player": {
    "pos": [1024, -384],
    "angle": 90,
    "health": 87,
    "armor": 50,
    "weapon": 3,
    "region": 12
  },
  "buddy": {
    "pos": [940, -360],
    "health": 72,
    "armor": 20,
    "weapon": 2,
    "ammo": 44,
    "state": "engage",
    "region": 11,
    "d_player": 91,
    "route": [11, 12]
  },
  "monsters": [
    {
      "id": 1,
      "type": "imp",
      "pos": [1300, -500],
      "hp": 60,
      "region": 14,
      "see_player": 1,
      "see_buddy": 0,
      "d_player": 310,
      "d_buddy": 390,
      "order": "none"
    }
  ],
  "count": 1,
  "regions": [[11, 940, -360], [12, 1024, -384], [14, 1300, -500]],
  "links": [[11, 12, "open"], [12, 14, "door"]],
  "director": {
    "intensity": 42,
    "state": 1,
    "recent_dmg": 8,
    "ammo_pct": 67
  }
}
```

Fields can be absent when the buddy/director/level state is not available. Outside a level the response is:

```json
{"nolevel":true,"monsters":[]}
```

`AI_Serialize` caches its JSON snapshot for fewer than two game tics. The director state is numeric (`0` buildup, `1` sustain, `2` fade), and the buddy state/route fields are emitted only when `-aicoop` is active.

### IDs

Monster IDs are registry slot plus one. The registry is rebuilt during serialization; still-live objects usually retain side-table directives by `mobj_t *`, but a client must refresh IDs from every new observation. Do not carry an ID across roster/map changes and assume it still addresses the same actor.

## Current limitations

- one client is accepted per listener;
- `observe` caches the JSON snapshot for fewer than two tics;
- monster IDs are registry slot plus one and must be refreshed after every observation;
- `act` stores one directive per monster, so a later command overwrites an earlier one;
- the player-agent and monster/director protocols are separate even though both use TCP lines.

## 3. Monster directives

```text
act order=<order> ids=<id-list|all> [focus=<0|1>] [x=<map-x> y=<map-y>] [for=<tics>] [after=<tics>]
```

Examples:

```text
act order=focus_fire ids=1,2,3 focus=0 for=70
act order=flank_left ids=4 x=900 y=-200 for=105
act order=fallback ids=7 for=35
act order=hold ids=all for=20
act order=use_door ids=2 x=1200 y=-640 for=35
```

### Orders

The parser/executor supports the current order vocabulary in `p_ai_llm.c`, including:

- `none` / `chase` — normal Doom chase behavior;
- `hold` — hold/face/fire when possible;
- `fallback` — move away from the selected target;
- `flank_left`, `flank_right` — route to a flank destination; these are the exact parser names (not `flank_l`/`flank_r`);
- `focus_fire` — focus the requested survivor (`focus=0` player, `focus=1` buddy where available);
- `ambush` — wait/engage according to the implemented ambush policy;
- `use_door` — move/use near the requested door destination.

Use the exact aliases accepted by `AI_OrderFromName`; unknown names fall back according to the current parser behavior and should be treated as client errors.

### Timing is not a queue

Each monster has **one** directive slot. `for=<tics>` sets its active lifetime. `after=<tics>` delays that same stored directive. Sending a second directive to the same ID overwrites the first—it does not queue behind it.

For a real sequence, keep the first order active and send the follow-up later from the client after the next observation/timer.

## 4. Director pacing commands

The LLM director also accepts:

```text
spawn type=<monster-name> count=<1..8>
spawn item=<medkit|ammo>
director relax
```

These are the commands that update the director's LLM watchdog (`dir_llmlast`): monster spawn, item spawn and relax. Ordinary `act` and `buddy` commands do **not** reset that watchdog.

The offline rule FSM resumes after about 15 seconds without one of those pacing commands. While the FSM is suppressed, the rule director can still create periodic hordes under its own safety/cooldown rules.

Monster names are mapped by `P_Director_TypeByName` in `files/p_ai_director.c`. A client should use the names accepted there rather than inventing DECORATE-style class names.

## 5. Buddy directives

When `-aicoop` is active, send:

```text
buddy order=<engage|defend|hold|regroup|retreat|goto|grab> [focus=<id>] [x=<map-x> y=<map-y>] [for=<tics>]
```

The buddy's autonomous C controller remains the reflex/fallback layer. The directive changes high-level behavior; it does not directly move the actor.

## 6. Utility commands

The protocol also recognizes:

```text
reset
wake
```

Use `reset` to clear directive state according to the implemented handler. `wake` requests the supported monster-wakeup path. Their exact side effects are defined in `files/p_ai_llm.c` and should be rechecked when extending the protocol.

## 7. Control loop

A robust client should:

1. send `observe`;
2. discard stale IDs and rebuild its current actor model;
3. choose a small set of high-level orders;
4. send `act`/`buddy`/pacing lines;
5. wait a sensible interval before polling again;
6. explicitly send follow-up phases rather than pretending `after` is a queue.

Do not issue every monster a new order every tic. The C executor already runs every tic; the high-level model only needs to update intent.

## 8. Rule director interaction

The rule director assigns `flank_left`, `flank_right`, `focus_fire` and `fallback` to eligible awake monsters. Objective guards can remain idle as an ambush mechanism, but the rule selector does not assign every LLM order type.

The rule layer also wakes non-objective monsters, seeds/tops up objective guards, biases spawns along the exit route, suppresses dangerous hordes when survivors are critically stressed, and can drop emergency supplies.

## 9. Caveats

- External directives are single-player features and break vanilla demo/netplay semantics.
- Movement still uses normal Doom collision and the shared BSP-subsector navigation helper.
- The shared function is named `PF_AStar`, but its heuristic is disabled; current path search behaves as Dijkstra.
- The protocol is request/poll based. It is not a server-pushed event stream.

## Source map

- Protocol/serializer/directive side table: `files/p_ai_llm.c`, `files/p_ai_llm.h`.
- Rule/LLM pacing and spawn implementation: `files/p_ai_director.c`.
- Monster execution hook: `files/p_enemy.c`.
- Buddy directive/reflex layer: `files/p_ai_coop.c`.
- Design background: `docs/monster_llm_control.md`, `docs/AGENT_CONTROL.md`.
