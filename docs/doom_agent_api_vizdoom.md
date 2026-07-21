# API for an Agent to Play Classic Doom — ViZDoom / LLM-Agent Perspective

Companion to `doom_agent_api_architecture.md` in this directory, which describes
a low-level approach by hacking the C source of `sdldoom-1.10-mod` (hooking
`I_FinishUpdate`, walking `mobj_t` thinkers, etc.). This document takes the
opposite trade-off: keep the engine untouched, wrap it, expose a clean Python
API that any agent — script, RL policy, or LLM — can drive.

**TL;DR:** I'd use **ViZDoom** as the engine substrate (not `doomgeneric`, not
a custom port), wrap it in a 5-layer architecture, and design the policy
interface so an LLM can sit on top via a JSON observation/action bridge. The
actual reference implementation lives in `../doom_agent.py` and runs headless
on Debian 13 in about 5 s per episode.

---

## 0. The four real options

| Option                            | Effort    | Control | Best for                          |
|-----------------------------------|-----------|---------|-----------------------------------|
| **A. ViZDoom**                    | low       | high    | RL + LLM agents (95% of cases)    |
| B. Wrap `gzdoom` source port      | high      | max     | Modding / community compatibility  |
| C. Wrap PrBoom+ headless          | medium    | high    | Vanilla-netcode-faithful replay   |
| D. Pure-CV on a windowed GZDoom   | low       | lowest  | Last resort, very expensive       |

I'd pick **A**. ViZDoom already implements the gym-style step API, supports
depth + labels + automap buffers natively, runs headless on Linux, and is the
de-facto standard in the RL literature (so pretrained policies transfer).

---

## 1. Five layers, one per concern

```
┌─────────────────────────────────────┐
│  5. AGENT / POLICY                  │  ← LLM, RL, hand-written
├─────────────────────────────────────┤
│  4. TASK INTERFACE                  │  ← "clear room 3"
├─────────────────────────────────────┤
│  3. SCENE GRAPH / STATE             │  ← structured world
├─────────────────────────────────────┤
│  2. PERCEPTION                      │  ← buffers -> entities
├─────────────────────────────────────┤
│  1. ENGINE                          │  ← ViZDoom / gzdoom
└─────────────────────────────────────┘
```

Each layer has a narrow interface, is independently testable, and can be
replaced. The interesting work is in **layers 2 and 3**; the engine is a thin
wrapper, the policy is whatever you plug in.

---

## 2. Layer 1 — Engine

```python
class Engine(Protocol):
    def reset(self, map: str | None = None) -> None: ...
    def step(self, action: Action) -> None: ...
    def sense(self) -> EngineState: ...
    def is_done(self) -> bool: ...
    def close(self) -> None: ...
```

`Action` is **mixed discrete + continuous**. Doom is a first-person shooter;
mouse-look is essential, so a fully discrete action space loses too much
information. The minimum useful set:

  * Discrete (8): fwd, back, strafe-l, strafe-r, turn-l, turn-r, attack, use
  * Discrete (7): weapon slot 1–7
  * Continuous:  `turn_yaw_delta ∈ [-180°, +180°]`, `pitch_delta ∈ [-90°, +90°]`
  * Continuous:  `move_delta`, `strafe_delta` (signed, normalized)

In ViZDoom this maps to the `_DELTA` button variants (e.g.
`Button.TURN_LEFT_RIGHT_DELTA`) and discrete buttons for the rest.

### EngineState (returned by `sense()`)

```python
@dataclass
class EngineState:
    tick:       int
    health:     int
    armor:      int
    ammo:       dict[str, int]     # bullets, shells, rockets, cells
    weapons:    list[str]
    pos:        tuple[float, float, float]
    yaw:        float
    pitch:      float
    kills:      int
    frags:      int
    dead:       bool
    screen:     np.ndarray | None  # (H, W, 3) uint8
    depth:      np.ndarray | None  # (H, W)  float32
    labels:     np.ndarray | None  # (H, W)  uint8  semantic seg
    automap:    np.ndarray | None  # optional
```

All gameplay-relevant numbers come from the engine's `get_game_variable()`
API in ViZDoom 1.3 (the older `gs.position_x` / `gs.angle` attributes were
removed in the 1.x line — see *ViZDoom 1.3 footguns* at the end).

---

## 3. Layer 2 — Perception

Three sources fused, in order of reliability:

  1. **Engine variables** — 100 % reliable, zero cost: health, ammo, position,
     facing, killcount, frags.
  2. **Automap + depth buffers** — engine-provided, perfect: which walls are
     behind corners, distances to occluded geometry. *Don't* run monocular
     depth estimation on the screen image when you can read it directly.
  3. **Vision model on screen** — for *what* is visible: monster type, item
     type, door state. YOLO-World or GroundingDINO works well; prompt the
     model with the Doom-specific class list so it doesn't try to label
     things that aren't there.

```python
class Perception:
    def update(self, raw: EngineState) -> WorldState: ...

    # Internal: extract entities of a given label class.
    def _extract(self, label_id: int, kind: str, raw: EngineState) -> list[Entity]: ...
```

### Tracking

A monster that disappears behind a corner in frame 12 and re-appears in frame
240 must keep its identity. **Hungarian matching** on (centroid, area, tiny
appearance embedding) per frame is plenty for Doom. ByteTrack / OcSort
work too if you want a third-party component.

### Damage tracking

A 35-frame rolling window on `Δhealth` gives you "damage per second" cheaply.
No need to instrument the engine — just diff the variable.

---

## 4. Layer 3 — Scene graph / WorldState

This is what the policy *thinks about*. Not pixels, not buffers — a
structured world.

```python
@dataclass
class Entity:
    eid:    int                # stable across frames
    kind:   str                # "imp", "medkit", "door"
    pos:    tuple[float, float]
    distance: float
    bearing_deg: float         # in agent's local frame
    visible: bool
    state:  str                # "idle", "attacked", "door:locked"
    health: float | None
    facing: float | None

@dataclass
class WorldState:
    agent:        AgentState
    visible:      list[Entity]
    known_map:    TopologicalMap        # optional, built from automap buffer
    threats:      list[Entity]
    resources:    list[Entity]
    objectives:   list[Objective]
    game_vars:    dict                  # health, ammo, armor, weapon
    dmg_recent:   int                   # 1-second window
```

---

## 5. Layer 4 — Task interface (three granularities)

```python
# Low-level: raw action vector.
agent.act(state) -> np.ndarray         # length 12 in basic config

# Mid-level: skills.
class Skills:
    def navigate_to(self, target: Vec3) -> np.ndarray: ...
    def engage(self, enemy_id: int)     -> np.ndarray: ...
    def pickup(self, item_id: int)      -> np.ndarray: ...
    def open_door(self, door_id: int)   -> np.ndarray: ...
    def retreat_to_cover(self)          -> np.ndarray: ...

# High-level: tasks.
class Task:
    def clear_room(self, room_id: int)  -> np.ndarray: ...
    def grab_red_key(self)              -> np.ndarray: ...
    def reach_exit(self)                -> np.ndarray: ...
```

LLM-agents perform much better on the **skill** or **task** level than on
raw action vectors. Pure pixel→action is RL territory and eats GPUs
indiscriminately.

---

## 6. Layer 5 — LLM bridge (the actual point of this exercise)

If the agent is an LLM (you, me, another one), the bridge is a JSON-RPC
observation/action protocol. Observation is text+optional image, action is
a JSON dict.

### Observation

```json
{
  "tick": 4521,
  "health": 67, "armor": 50,
  "ammo": {"bullets": 35, "shells": 8, "rockets": 2, "cells": 0},
  "weapons": ["fist", "pistol", "shotgun", "chaingun"],
  "pos": [1234.0, -567.0, 0.0],
  "yaw": 89.0, "pitch": 0.0,
  "kills": {"done": 5, "total": 8},
  "visible_entities": [
    {"id": 42, "type": "imp", "distance": 12.3,
     "bearing_deg": 45, "state": "alerted"},
    {"id": 43, "type": "medkit", "distance": 3.1, "bearing_deg": -110}
  ],
  "known_rooms": [
    {"id": 1, "name": "start", "cleared": true, "exits": ["n", "e"]},
    {"id": 2, "name": "hallway", "cleared": false, "exits": ["s"]}
  ],
  "last_damage": {"from_id": 42, "amount": 8, "tick": 4519}
}
```

### Action

```json
{
  "type": "combo",
  "move": 0.6,            // forward
  "strafe": 0.0,
  "turn": -0.42,          // radians/frame
  "pitch": 0.0,
  "buttons": ["attack"],
  "weapon_slot": 3,
  "duration_ticks": 6,
  "intent": "engage the imp that just shot me, then strafe right"
}
```

`intent` is the agent's free-form reasoning — useful for chain-of-thought
debug, memory, and learning from human feedback, but ignored by the engine.

---

## 7. The two real hard problems

These are the science, not the engineering.

### 7.1 Reward shaping

Doom gives you almost no reward signal. `episode_done` is useless for
training. You need shaped rewards that combine:

```
reward = Δkills * 100
       + Δsecrets * 50
       + Δhealth * 1.0
       - 0.01                 # tick penalty (encourages speed)
       + 0.1 * (1 - health/100)  # mild survival pressure
```

R&D, ICM, and curiosity-driven exploration help when maps are large and
sparse-reward.

### 7.2 Partial observability

Doom maps are hidden. The agent sees only a slice of *now*. Without explicit
memory the agent walks in circles in the start room forever. Solutions:

  * **Topological map** built from the automap buffer (which you already
    have for free)
  * **Surprise-driven memory** (Random Network Distillation, Intrinsic
    Curiosity Module)
  * **Episodic memory** of "what was in the last room I cleared"

---

## 8. Reference implementation

`../doom_agent.py` is the working end-to-end version of everything above:

  * Layer 1: `Engine` class, 1.3-clean API
  * Layer 2: `Perception` class, label-buffer extraction + damage window
  * Layer 3: `WorldState` dataclass
  * Layer 4: `ReactiveImpPolicy` (a 30-line rule-based bot)
  * Layer 5: `Policy` protocol (drop in an LLM policy by replacing the
    function — same signature, JSON in, action vector out)

Run it:

```sh
source ~/.doom-agent/bin/activate
python ../doom_agent.py --episodes 3 --no-window    # headless
python ../doom_agent.py --episodes 1                # with GZDoom window
```

The shipped bot is intentionally dumb (~0 % win rate against a single imp) —
the point is the *pipeline*. The `ReactiveImpPolicy` body is a
proof-of-concept; an LLM- or RL-driven policy plugs in at the same seam.

---

## 9. ViZDoom 1.3 footguns (lessons from the first run)

If you take the reference implementation and adapt it, these are the API
gotchas I hit on the way to a clean compile:

  * **Button enum renamed.** The old `Button.MOVE_FORWARD_BACKWARD` is now
    `Button.MOVE_FORWARD_BACKWARD_DELTA`. Same for the strafe and turn
    axes. The discrete-only variants (`MOVE_FORWARD`, `TURN_LEFT`,
    `SELECT_WEAPON1`...) still exist for classic Doom digital input.
  * **`GameState.position_x/y/z/angle` removed.** Everything position-
    related lives in `get_game_variable(GameVariable.POSITION_X)` etc.
    Same for `HEALTH`, `ARMOR`, `AMMO0..3`, `KILLCOUNT`, `FRAGCOUNT`.
  * **`set_automode(Mode.PLAYER)` → `set_mode(Mode.PLAYER)`.** Spectator
    is the default.
  * **Buffer ordering.** `screen_buffer` now comes back as `(H, W, C)` —
    no transpose needed. Older examples on the web still do
    `np.transpose(gs.screen_buffer, (1, 2, 0))` which is wrong on 1.3.
  * **Game variables as int arithmetic.** `GameVariable.USER1 + i` raises
    `TypeError` — it's an enum, not an int. Iterate by name:
    `getattr(vzd.GameVariable, f"USER{i+1}")`.
  * **`set_actors_info_enabled` does not exist.** Use
    `set_sectors_info_enabled` if you need that; for per-actor info
    (3D positions, types), the `labels_buffer` plus a YOLO pass is the
    standard substitute.

These are exactly the kind of breakage that makes the
`doom_agent_api_architecture.md` C-source approach attractive — once you've
patched a 1996 C engine, *you* control the API surface and it never moves.
Trade-off: weeks of work instead of an afternoon.

---

## 10. What I'd build next, given a free week

  1. **LLM-policy version of `../doom_agent.py`**: serialize `WorldState` to
     a compact prompt (~500 tokens), call a model, parse the JSON action,
     step. Compare against the rule-based bot on a held-out map set.
  2. **ByteTrack-based entity tracker**: real IDs across frames instead
     of per-frame ones.
  3. **Topological map builder** from the automap buffer: split the
     navigable space into rooms + doorways, give the LLM an actual
     "where am I" instead of just coordinates.
  4. **Frame skip wrapper**: action-repeating at the env level (4 ticks per
     action) for ~4× throughput on RL training, matching human-ish reaction
     time.
  5. **MCP server** exposing `doom_start` and `doom_action` as tools — so
     any MCP-compatible agent (Claude Code, etc.) can sit down and play.
