# API Architecture for an Agent to Play Classic Doom

To build an API for an AI agent to play classic Doom, modifying the source code from scratch is unnecessary. The optimal approach is to fork **`doomgeneric`** (the stripped-down, highly portable C implementation of the original source code) or build on top of **`ViZDoom`** (the established standard for RL research). 

The architecture must solve two fundamental engineering problems: exposing the game state to the agent and receiving actions back, all while decoupling the game’s internal tick rate from the API's execution speed.

---

## 1. Core Architecture & Synchronization

The classic Doom engine targets 35 frames per second (ticks). For an AI agent—especially one utilizing Deep Reinforcement Learning (DRL) or a Vision-Language Model (VLM)—running the engine in real-time is impossible. The system requires **virtual time**.

* **Execution Loop:** The engine must be modified to block and wait for an API command before advancing the game state. 
* **The "Step" Paradigm:** Instead of a traditional REST API with asynchronous CRUD endpoints, an RPC or IPC protocol is required to handle atomic state-action steps (similar to the OpenAI Gym / Farama Gymnasium interface).

```
[ Agent ]  ---( POST /step {actions} )--->  [ API Server / Wrapper ]
    ^                                                    | (Blocks Engine)
    |                                                    v
[ Returns: Frame Buffer + Game Vars ]  <---  [ Doom Engine Tick (+1) ]
```

---

## 2. API Schema Design

The API requires two primary endpoints: initialization and the state-action step loop.

### `POST /v1/game/init`
Configures the environment before execution begins.

```json
{
  "wad_path": "/data/doom1.wad",
  "map": "E1M1",
  "difficulty": 3,
  "render_modes": ["screen", "depth", "labels"]
}
```

### `POST /v1/game/step`
This is the core execution loop endpoint. It passes the agent's actions and forces the engine to calculate the next state.

#### Request Payload (Action Space)
Doom relies on binary inputs (buttons) and continuous inputs (turning axis).

```json
{
  "actions": {
    "MOVE_FORWARD": true,
    "MOVE_BACKWARD": false,
    "STRAFE_LEFT": false,
    "STRAFE_RIGHT": false,
    "TURN_LEFT": 0.0,
    "TURN_RIGHT": 15.5, 
    "ATTACK": true,
    "USE": false,
    "SELECT_WEAPON": 3
  },
  "ticks_to_advance": 4 
}
```

> **Design Note on Frame Skipping:** The `ticks_to_advance` parameter is critical. Human reaction time is slower than $1/35$th of a second. Allowing the engine to step forward 4 ticks per action mimics a human-like $\sim114\text{ms}$ reaction window and vastly accelerates training.

#### Response Payload (Observation Space)
The response delivers the raw sensory data, game variables, and metadata.

```json
{
  "game_variables": {
    "health": 100,
    "armor": 0,
    "ammo": [50, 0, 0, 0],
    "current_weapon": 2,
    "kill_count": 4,
    "secret_count": 0,
    "user_position": [1056.4, -2048.2, 0.0]
  },
  "buffers": {
    "screen": "iVBORw0KGgoAAAANSUhEUgAA...", 
    "depth": "Data stream or base64 representation...",
    "labels": [
      {"id": "imp_1", "class": "enemy", "bounding_box": [120, 45, 160, 90]}
    ]
  },
  "is_terminal": false,
  "reward": 10.0
}
```

---

## 3. Data Extraction & Interception (The C Layer)

To populate the response payload without relying on slow OCR or computer vision hacks, you must hook directly into Doom's C internals:

* **Visuals (`screen`):** Intercept the `I_FinishUpdate` function inside `i_video.c`. Instead of rendering the `dg_screen_buffer` array directly to an OS window, copy it directly into shared memory or a fast buffer.
* **Depth & Buffers:** If targeting modern vision-based agents, patch the software renderer's column/span drawers (`r_draw.c`) to extract distances to walls (`rw_distance`) and sprites. This yields a flawless grayscale depth-map.
* **Game State Variables:** Read directly from global variables defined in `d_player.h` (`plr->health`, `plr->readyweapon`, `plr->ammo[]`).
* **Object Labeling / Ground Truth:** Iterate over the `thinkers` linked list (`p_tick.c`) to pull active `mobj_t` structs (monsters, items, projectiles). Map their 3D coordinates relative to the player's view vector to instantly generate 2D bounding boxes.

---

## 4. Transport Protocol Selection

Depending on the targeted type of agent, the transport layer choice varies:

### Option A: gRPC / Protocol Buffers (Best for DRL Agents)
If training reinforcement learning models (e.g., PPO, DQN in PyTorch), HTTP/JSON introduces terrible serialization overhead. 
* **Why:** Protobuf handles raw byte arrays (the frame buffer) with virtually zero overhead. 
* **Performance:** Low latency, typed contracts, natively streams inputs/outputs over HTTP/2.

### Option B: MCP (Model Context Protocol) (Best for LLM/VLM Agents)
If the goal is to let a Multi-Modal LLM or coding assistant play Doom, exposing the API over the **Model Context Protocol (MCP)** via `stdio` or SSE is the modern standard.
* **Why:** LLM tools rely on specific JSON-RPC schemas.
* **Implementation:** The MCP server wraps the underlying game process, exposes `doom_start` and `doom_action` as standard tools, and translates the frame buffer/labels array into something text-based or multi-modal that the model can ingest natively.

---

## 5. Reward Shaping Engine (Server-Side)

If building this for RL, the API should allow server-side reward customization. Calculating rewards exclusively on the client side leads to sluggish iterations. The server should calculate the delta ($\Delta$) between frames:

$$\text{Reward} = \Delta\text{Kills} \times 100 + \Delta\text{Secrets} \times 50 + \Delta\text{Health} \times 1 - \text{TickPenalty}$$

Injecting this logic into the engine step ensures the environment is fully self-contained and behaves exactly like a traditional OpenAI Gym environment.
