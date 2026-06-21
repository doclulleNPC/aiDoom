# Driving aiDoom with an Agent / LLM

A design note: how to rebuild this engine so an external **agent** (a scripted
bot, an RL policy, or an **LLM** with tool-use) can perceive the world and control
the player. It is written against *this* codebase (functions/files are real), and
ordered from the smallest useful change to a full "Doom-as-an-environment" setup.

---

## 0. TL;DR — the recommended shape

```
            observe (JSON state  ± screenshot)
   ┌──────────────────────────────────────────────┐
   │                                               ▼
┌────────┐   high-level intent        ┌─────────────────────┐
│  LLM   │ ──────────────────────────▶│  reflex controller  │
│ planner│   "goto (x,y)", "kill 3",  │  (C, runs every tic)│
│        │   "press use", "weapon 4"  │  intent → ticcmd_t  │
└────────┘                            └─────────┬───────────┘
   ▲  (one call every ~0.5–5 s)                 │ per-tic (35 Hz)
   └────────────────────────────────────────────┘
                                                 ▼
                                       G_BuildTiccmd hook → playsim
```

The single most important idea: **do not make the 35 Hz game loop wait on the
LLM.** Doom advances the world in fixed 1/35 s *tics*; an LLM answers in
hundreds of milliseconds to seconds. So split control into two layers:

- **Planner (LLM):** sees a compact observation, emits a *high-level intent* via
  tool calls. Runs asynchronously, a few times per second at most.
- **Reflex controller (C):** every tic, turns the current intent into a concrete
  `ticcmd_t`. Handles aiming, step-toward-target, stuck detection, firing windows.

For pure RL / training you can instead run **stepped** (the engine blocks for one
action per tic) — see §6.

---

## 1. Background: how player input already works

All player control in Doom funnels through one struct, built once per tic:

- `ticcmd_t` (`d_ticcmd.h`) — the *only* channel into the playsim:
  - `forwardmove` (signed: +forward / −back)
  - `sidemove` (signed: strafe right/left)
  - `angleturn` (turn, applied to the player's view angle)
  - `buttons` (bitfield: `BT_ATTACK`, `BT_USE`, and weapon-change bits
    `BT_CHANGE`/`BT_WEAPONMASK`)
  - `consistancy` (net/demo sync — leave the engine's value)
- `G_BuildTiccmd(ticcmd_t* cmd)` (`g_game.c`) reads the keyboard/mouse/joystick
  and fills `cmd`. This is the **one function** that maps human input to a tic.
- `D_DoomLoop` → `TryRunTics` → `G_Ticker` applies the tics to the simulation.

Because everything goes through `ticcmd_t`, an agent that produces `ticcmd_t`s is
indistinguishable from a player to the rest of the engine — it stays
deterministic and demo/net-compatible. **This is the injection point.**

---

## 2. Where to inject control

Add a hook at the top of `G_BuildTiccmd`:

```c
// g_game.c
extern int   agent_active;            // set by -agent / menu
void G_AgentBuildTiccmd (ticcmd_t* cmd);   // new, in g_agent.c

void G_BuildTiccmd (ticcmd_t* cmd)
{
    if (agent_active) { G_AgentBuildTiccmd(cmd); return; }
    ... existing human input ...
}
```

New module **`g_agent.c`** owns: the IPC, the current intent, the reflex
controller, and observation gathering. Wire it into `doom_SOURCES` in the
Makefile (and remember: this Makefile has **no header dependency tracking**, so
`make clean` after touching shared headers — see `CLAUDE.md`).

A coarser alternative (good for a first prototype) is to *post synthetic events*
via `D_PostEvent(&event)` the way `i_video.c` does for the keyboard. It's simpler
(no new code paths) but less precise than writing `ticcmd_t` directly, and it
can't express analog move/turn amounts cleanly. Prefer the `ticcmd_t` hook.

---

## 3. Observations — the "sensors"

The agent needs to perceive the world. Three tiers, combinable:

### 3a. Structured state (recommended for LLMs — token-cheap, precise)
All of this is already in memory; gather it into a JSON object once per decision:

- **Player** (`players[consoleplayer]`, `player->mo`):
  - position `mo->x, mo->y, mo->z` (16.16 fixed → divide by `FRACUNIT`)
  - facing `mo->angle` (BAM → degrees)
  - `health`, `armorpoints`, `readyweapon`, `ammo[]`, `weaponowned[]`
  - `killcount`, `itemcount`, `secretcount`, on-ground / momentum
- **Nearby things** — walk the thinker list and keep mobjs in range:
  ```c
  for (th = thinkercap.next; th != &thinkercap; th = th->next)
      if (th->function.acp1 == (actionf_p1)P_MobjThinker) {
          mobj_t* m = (mobj_t*)th;
          // classify by m->type (MT_POSSESSED, MT_IMP, MT_HEALTH, ...)
          // record relative angle/distance: P_AproxDistance, R_PointToAngle2
          // visible? P_CheckSight(player->mo, m)
      }
  ```
  Emit a short list: `{type:"imp", dist:320, rel_angle:-15, visible:true, hp:60}`.
- **Geometry / navigation**: current `sector` (floor/ceiling height, special,
  damaging?), the `line`s of the subsector, locked doors, switches. For real
  navigation, precompute a reachability graph from the BSP/blockmap offline.

This is dramatically cheaper (and more reliable) for an LLM than pixels: a few
hundred tokens of JSON vs. an image, and no hallucinated geometry.

### 3b. Screen pixels (for a vision model, or debugging)
`I_ReadScreen(buf)` already copies the 8-bit framebuffer (`screens[0]`,
`SCREENWIDTH*SCREENHEIGHT`). Expand through the palette to RGB and PNG-encode
(downscale to e.g. 320×200 or smaller to save tokens), then base64 it for a
vision LLM. Slow and lossy; use as a *supplement* to 3a, not the main channel.

### 3c. Reward signal (only if you do RL)
Derive per-step from state deltas: `+kill`, `+item/secret`, `+level exit`,
`−health lost`, `−death`, small `−time` to discourage idling. Expose alongside
the observation.

---

## 4. The agent interface (IPC)

`g_agent.c` talks to the external process. Pick by use-case:

| Transport | Good for | Notes |
|---|---|---|
| **stdin/stdout, line-delimited JSON** | quick prototype, single agent | trivial; engine prints `OBS {...}`, reads `ACT {...}` |
| **Unix/TCP socket** | clean separation, remote agent | recommended default; one socket, request/response |
| **Shared memory + semaphores** | RL throughput (thousands of steps/s) | what ViZDoom does; lowest latency |
| **MCP server** | plugging into Claude/agent tooling | wrap the socket as an MCP server exposing `observe`/`act` tools |

Minimal protocol (newline JSON over a socket):

```
→  {"cmd":"observe"}
←  {"player":{...}, "things":[...], "tic":1234, "reward":0.0, "done":false}
→  {"cmd":"act", "intent":{"goto":[1024,-256]}}          // high-level
   {"cmd":"act", "ticcmd":{"forward":50,"turn":-256,"attack":true}}  // low-level
←  {"ok":true}
→  {"cmd":"reset"}    // (training) restart map via G_InitNew
```

For an **LLM via tool-use** (e.g. Claude tool calling), expose the verbs as
tools: `observe()`, `move_to(x,y)`, `turn_to(angle)`, `attack()`, `use()`,
`select_weapon(n)`, `wait(ticks)`. The model reads the `observe()` result, calls a
tool, repeats. The reflex layer (§5) executes the tool's *intent* over many tics.

---

## 5. Bridging the speed gap (the crux for LLMs)

35 tics/s vs. an LLM that answers every ~1 s = ~35 tics per decision. Handle it
**without stalling the loop**:

- **Latched intent**: `g_agent.c` keeps the last intent and re-derives a fresh
  `ticcmd_t` *every* tic from it. While the LLM is "thinking", the bot keeps
  executing the previous intent (e.g. keeps walking toward the goal, keeps
  tracking the target). The socket read is non-blocking.
- **Reflex behaviours** the C layer implements so the LLM needn't micro-manage:
  - *goto(x,y)*: turn toward target, `forwardmove` forward, back off on wall
    contact (momentum/`P_TryMove` feedback), basic unstuck.
  - *aim+fire*: rotate `angleturn` toward the chosen enemy, set `BT_ATTACK` only
    inside an aim tolerance and respect weapon refire timing.
  - *use*: pulse `BT_USE` for one tic (it's edge-triggered).
  - *weapon switch*: set `BT_CHANGE | (n<<BT_WEAPONSHIFT)`.
- **Decision cadence**: trigger a new LLM call on events (enemy appeared, took
  damage, reached goal, intent stale > N tics), not on a fixed clock — saves
  tokens and reacts faster when it matters.

A practical tiering with Claude: a cheaper/faster model (e.g. Haiku) for frequent
reflex-level judgement calls, a stronger model (Opus/Sonnet) for occasional
planning ("which room next, which key to find"). The C reflex layer remains the
real-time controller either way.

---

## 6. Headless & stepped mode (training / batch)

For RL or fast LLM iteration you usually want **no window, no audio, no
real-time throttle, and lockstep stepping**:

- Add `-headless`: skip `I_InitGraphics`/`I_InitSound` (or stub `I_FinishUpdate`,
  `I_UpdateSound`). The playsim doesn't need a framebuffer — it renders into
  `screens[0]` only if you ask for pixels (§3b).
- Replace the real-time clock: `I_GetTime` gates how many tics run per wall-clock
  second. In stepped mode, advance **exactly one tic per `act`** (engine blocks
  on the socket read), so the env is synchronous and reproducible — the classic
  `obs, reward, done = env.step(action)` gym contract.
- Determinism is already a property of the engine (`m_random` table RNG, 16.16
  fixed-point). Seed via the level/skill and keep actions reproducible → demos
  and training runs replay bit-exactly. **Don't** introduce floats into the
  playsim or this breaks.
- Fast-forward: in stepped mode just don't sleep; you'll get thousands of
  tics/s headless, bounded by the sim and observation cost.

---

## 7. Action space (what the agent can emit)

Map directly to `ticcmd_t`. A compact discrete set covers Doom well:

| Action | ticcmd effect |
|---|---|
| forward / back | `forwardmove = ±forwardmove[run]` |
| strafe L / R | `sidemove = ±sidemove[run]` |
| turn L / R (analog ok) | `angleturn += ±step` |
| run modifier | pick the fast row of the move tables |
| attack | `buttons |= BT_ATTACK` |
| use (open/switch) | `buttons |= BT_USE` (one-tic pulse) |
| weapon 1..7 | `buttons |= BT_CHANGE | (n<<BT_WEAPONSHIFT)` |

LLMs do better with the **high-level** verbs of §4/§5; RL policies usually take
this raw discrete/continuous space directly.

---

## 8. Concrete change list (smallest → fullest)

1. **`g_game.c`**: `agent_active` flag + hook in `G_BuildTiccmd`; parse `-agent`.
2. **`g_agent.c`/`.h`** (new): socket I/O, intent latch, reflex controller
   (`G_AgentBuildTiccmd`), observation serializer. Add to Makefile `doom_SOURCES`.
3. **Observation**: helpers reading `players[]`, `player->mo`, the thinker list,
   sectors/lines; fixed→float and BAM→degree conversions.
4. **`i_system.c`**: make `I_GetTime` honor a `-timedemo`-style "free-running" or
   "stepped" mode so the loop can be driven by the agent.
5. **`i_video.c`/`i_sound.c`**: `-headless` guards so it runs with no display/audio.
6. **(optional) `d_main.c`/`g_game.c`**: a `reset` path (`G_InitNew`) for episodic
   training; expose `killcount`/exit as `done`.
7. **(optional) MCP wrapper**: a tiny server translating MCP tool calls to the
   socket protocol, so Claude (or any agent runtime) can drive it via tools.

Keep all of it behind `agent_active`/`-agent`/`-headless` so normal play is
untouched.

---

## 9. Prior art worth copying

- **ViZDoom** is exactly this idea, productionised: a Doom engine (ZDoom-based)
  exposing the screen buffer + structured game variables over shared memory with
  a Python `step()/reset()` API; it's a standard RL research platform. If the
  goal is RL training, consider building on ViZDoom rather than re-deriving the
  loop here. This document is the "do it inside aiDoom" path, which is the better
  choice if you specifically want an **LLM tool-use agent** wired into *this*
  engine, or to keep the SDL build/menu/hi-res work you already have.

---

## 10. Suggested first milestone

A vertical slice that proves the whole pipe end-to-end:

1. `-agent` + `-headless`, stepped mode, one TCP socket.
2. `observe` returns player pose + health + a list of visible monsters.
3. `act` accepts two intents: `turn_to(angle)` and `attack()`.
4. Drive it from a 30-line script (or an LLM tool loop): observe → if a monster
   is visible, `turn_to` its angle, then `attack()`.
5. Confirm it kills the first imp in E1M1, headless, deterministically.

From there, add `goto`, weapon switching, and door/switch use, then hand the same
tools to an LLM planner.

---

## 11. Points worth stealing from the companion design docs

This repo already has two related notes — `doom_agent_api_architecture.md`
(C-hooking) and `doom_agent_api_vizdoom.md` (wrap ViZDoom). They overlap with the
above but raise several ideas this document under-covered. The genuinely additive
ones, and how they change the recommendation:

- **The software renderer makes the "sensors" nearly free — this is the real
  reason to hook *this* engine.** A modern GPU port has to fake these; aiDoom
  already computes them:
  - **Depth buffer:** the wall rasteriser already has `rw_distance` (`r_segs.c`)
    and per-column scale; tapping the column/span drawers (`r_draw.c`) yields a
    *flawless* grayscale depth map — no monocular-depth guessing.
  - **Ground-truth labels / bounding boxes / semantic segmentation:** project each
    visible `mobj_t`'s world coords to screen via `R_PointToAngle2` + the view
    transform to get exact 2D boxes and a per-pixel label buffer. This is *perfect*
    object detection with zero CV — the thing vision pipelines spend the most
    effort approximating. Strong argument for the C path over CV-on-pixels.
  This upgrades §3: prefer engine-extracted depth+labels over screenshots for any
  vision-flavoured agent.

- **Frame-skip as a first-class step parameter (`ticks_to_advance`, default ~4).**
  One agent action repeats for N tics (~4 tics ≈ 114 ms, a human-ish reaction
  window) → ~4× training/decision throughput and steadier behaviour. Make it an
  explicit arg of `act`/`step`, not just an event cadence (§5). Cheap, high-value.

- **Partial observability is THE hard problem, and §3 ignored it.** Doom maps are
  hidden; with only "now", an agent loops in the start room forever. Give it
  memory: a **topological map** (rooms + doorways) built from the **automap
  buffer** or the BSP — far better for an LLM than raw `(x,y)`; plus, for RL,
  curiosity/exploration bonuses (RND / ICM) on sparse maps, and episodic memory of
  cleared rooms. "Where am I, what have I seen" belongs in the observation.

- **Entity tracking for stable IDs across occlusion.** My observation hands out
  per-tic lists; a monster that ducks behind a corner and returns needs the *same*
  id. Hungarian matching on (centroid, distance, type) — or ByteTrack — keeps
  identity, which the planner needs for "the imp that just shot me".

- **Transport, by agent type:** gRPC/Protobuf for RL (zero-copy frame buffers, no
  JSON-serialisation tax) vs. MCP/JSON-RPC for LLM/VLM agents. (§4 lists both; the
  perf reason for gRPC on the RL path is the point.)

- **Build-vs-wrap, stated plainly.** Wrapping **ViZDoom** (or forking the portable
  **`doomgeneric`**) is an afternoon and gym-standard, but the API moves under you
  (the companion ViZDoom note logs a page of 1.3 breaking-change "footguns").
  Hooking aiDoom is weeks of work but gives an API surface *you* own forever —
  plus the free depth/label buffers above. Pick ViZDoom for RL-research throughput
  and pretrained-policy transfer; pick the in-engine path (this doc) for a bespoke
  **LLM tool-use agent** wired into the SDL/hi-res/menu build we already have.
  (The ViZDoom reference `../doom_agent.py` is real and **verified working**:
  ViZDoom 1.3.0 in the `~/.doom-agent` venv, ViZDoom cloned at `../ViZDoom`; runs
  headless at ~250 tics/s — `~/.doom-agent/bin/python ../doom_agent.py --episodes
  2 --no-window`. The shipped `ReactiveImpPolicy` is intentionally dumb (0 wins);
  it proves the pipeline, and an LLM/RL policy drops in at the `Policy` seam.)

Net: the C-hook path I described is *more* attractive than first framed, because
the perception layer that's normally the expensive part is essentially already
written inside the software renderer.

---

## 12. Letting an LLM control the *monsters* (AI Director)

Everything above drives the **player**. You can also drive the **enemies** — and
it's arguably a more natural fit, because monster "thinking" in Doom is already
centralised in a handful of functions. ViZDoom can't do this (its API exposes
players, not monster AI), so this is an **in-engine C-hook** feature (or GZDoom
ZScript) — another reason to own the engine. (A short companion note,
`monster_llm_control.md`, covers the same idea with extra emphasis on the order
vocabulary, directive scheduling, and model choice — folded in below.) A working
engine-side implementation now ships in `files/p_ai_llm.c` — see §13.

### Where monster AI lives
Every monster is a `mobj_t` whose thinker is `P_MobjThinker`, driven by the
**state machine** in `info.c`. The actual decisions sit in a few action functions
in **`p_enemy.c`**:

- `A_Look` — wake up, acquire `target` (sight/sound radius).
- `A_Chase` — the core loop: move toward target (`P_NewChaseDir`/`P_Move`, 8
  `movedir` directions), decide melee vs missile, retarget, make noise.
- `A_FaceTarget` + the per-type attacks (`A_PosAttack`, `A_TroopAttack`,
  `A_SargAttack`, `A_CPosAttack`, …) — fire, gated by the monster's
  `missilestate`/`meleestate` in the state machine.

`A_Chase` is the monster equivalent of the player's `G_BuildTiccmd`: the one
decision point to hook.

### The key idea: direct at the squad level, not per-monster-per-tic
A level has dozens of monsters, each thinking every few tics — far too many
agents to drive individually with an LLM. Three granularities:

| Approach | Scales? | Good for |
|---|---|---|
| (a) per-monster micro (LLM picks `movedir`/attack each) | no — too many agents/tokens | a single boss only |
| **(b) Director / squad commander** | yes | the sweet spot |
| (c) selective: one "smart" boss on the LLM, rest vanilla | yes, cheap | most impact per effort |

With **(b)** the LLM issues infrequent high-level orders per group — "imps: flank
left", "group A: fall back and regroup", "ambush at door X", "focus-fire the
wounded player" — and the existing C action functions execute them every tic
(pathing via `P_NewChaseDir`, aiming via `A_FaceTarget`, firing via the
state-machine attack transition). You reuse the engine's reflexes and replace
only the *tactics*.

### What you gain
Vanilla `A_Chase` is primitive: greedily lurch at the target, no coordination,
the infamous "infighting". An LLM director can do what the original AI never
could — flanking, baiting, retreat-and-regroup, using doors/teleporters
tactically, surrounding the player, target prioritisation. In effect a tactical
**"AI Director"** (cf. Left 4 Dead), but per-encounter and LLM-driven.

### Observation (cheap)
Per encounter, a compact JSON: the monsters (type, hp, pos, current state, can it
see the player via `P_CheckSight`), player pos/hp/weapon, coarse room geometry.
Update on **events** (player enters room, monster hit/killed, sightline
gained/lost), not per tic.

### Keep the order vocabulary tiny
The C layer only needs to understand a small fixed set of orders — anything
finer is micromanagement the LLM is bad at:

`chase` · `hold` · `fallback` · `flank_left` · `flank_right` ·
`ambush_at(x,y)` · `focus_fire(target)` · `use_door` · `use_teleporter`

Each directive carries scheduling so the loop **never waits on the model**:
`for_tics` (how long this order stays valid) and `after_tics` (delay before it
activates — lets the LLM queue a follow-up, e.g. "flank for 70 tics, *then*
focus-fire"). While the model thinks, monsters keep executing the latched order;
when `for_tics` expires with no fresh directive, fall back to vanilla `A_Chase`.

### Model choice: latency beats intelligence
Use the **fastest model that still reasons**, not the strongest — a fast tier
(e.g. Claude Haiku) answers in a few hundred ms, which is plenty for
"flank / fall back / focus-fire", whereas a 3-second "perfect plan" is 3 seconds
of monsters stuck in a wall. Reserve a stronger model for boss design or a single
selective (c) boss. (Cost is negligible: a few encounter-calls per second on a
small model is cents/hour.)

### Concrete hooks in aiDoom
- New module **`p_ai_llm.c`**: gather encounter state, talk to the LLM
  (async/latched), write per-monster **directives**.
- A custom **`A_LLMChase`** that reads the directive and realises it via
  `P_TryWalk`/`A_FaceTarget`/attack-state transition.
- Branch in `A_Chase` (`p_enemy.c`):
  `if (actor->llm_directive) { A_LLMChase(actor); return; }`.
- **Store directives in a side table keyed by the `mobj_t*`**, not in `mobj_t`
  itself — then you don't extend the struct or touch the savegame format
  (`p_saveg.c` memcpy's `mobj_t` wholesale; see the 64-bit notes above).

### Same two caveats as the player bot
- **Latency** → latch directives: a monster keeps executing its last order while
  the LLM thinks (the loop never blocks on the model).
- **Determinism**: monster AI leans hard on the `P_Random` table (move dir,
  attack/pain chance); LLM intervention breaks demo/net sync for those monsters.
  Fine for single-player encounters; don't expect demo compatibility.

### Tooling note
**ViZDoom does not expose monster AI** — it controls players/player-bots. Real
LLM-driven monsters need the in-engine C hook here, or GZDoom **ZScript/ACS**. So
the `doom_agent.py`/ViZDoom setup (§8–9) does *not* help for this; it's squarely
the engine-hook path's territory.

### A nice hybrid
Run the **player on ViZDoom** (or the §2 player hook) *and* the **monsters on the
in-engine director** at once: an LLM-vs-LLM Doom, or an LLM dungeon-master tuning
difficulty live against a human player.

---

## 13. The AI-Director implementation (shipped)

§12 is now implemented in **`files/p_ai_llm.c` / `p_ai_llm.h`** and verified
(builds clean, monsters drive correctly, no crash). It is fully behind flags —
normal play is untouched.

### Files & hooks
- **`p_ai_llm.c/.h`** — the module: side-table registry, directive executor
  `A_LLMChase`, observation serializer, non-blocking TCP server, demo director.
  Added to `aidoom_SOURCES`/`_OBJECTS` in `Makefile` + `Makefile.in`.
- **`p_enemy.c` `A_Chase`** — one divert at the top:
  `if (P_AI_Active(actor)) { A_LLMChase(actor); return; }`.
- **`p_tick.c` `P_Ticker`** — `P_AI_Ticker()` each gameplay tic (poll socket, age
  timers, run demo director).
- **`p_setup.c` `P_SetupLevel`** — `P_AI_Reset()` drops directives on level load.

### Run it
```sh
./aidoom -aidemo -warp 1 1          # built-in director: monsters flank/fall back
./aidoom -aidirector 31666 -warp 1 1   # open a control socket on 127.0.0.1:31666
```
Drive the socket (line protocol, one client):
```
observe\n                                  -> one JSON line: player + monsters[]
act order=flank_left ids=1,2 for=140\n     -> ok
act order=fallback ids=3 for=70 after=35\n -> ok   (delayed start)
reset\n                                    -> ok
wake\n                                     -> ok   (testing aid: wake all monsters)
```
An operator's manual for an agent driving this socket is in
`MONSTER_AGENT_GUIDE.md`.

**Verified behaviourally** (player stationary, monster awake): under `order=none`
the monster charges the player (distance 1681→1438); the instant a `fallback`
order lands it reverses and flees (1438→1759, then plateaus at a wall). So
`A_LLMChase` really does drive live monsters — charge under vanilla, retreat under
the directive.
Observation (real output, abridged):
```json
{"tic":58,"player":{"pos":[1056,-3616,0],"angle":90,"health":100,"armor":0,"weapon":1},
 "monsters":[{"id":1,"type":"imp","pos":[3440,-3472],"hp":60,"see_player":false,"order":"none"},
             {"id":2,"type":"zombieman","pos":[2912,-2816],"hp":20,...}],"count":6}
```
Orders: `chase hold fallback flank_left flank_right ambush(x,y) focus_fire(focus=id)
use_door`. Each carries `for=<tics>` (validity) and `after=<tics>` (delay) so the
loop never waits on the director.

### How it works (and the design choices that matter)
- **Side-table, not `mobj_t`.** Directives live in `aient[]` keyed by `mobj_t*` —
  the struct and the savegame format are untouched (see the `p_saveg.c` 64-bit
  notes). Each `observe` rebuilds the id registry but **preserves** still-valid
  directives for monsters that are still alive.
- **`A_LLMChase` reuses the engine's primitives** — `P_Move`/`P_NewChaseDir` for
  pathing, `A_FaceTarget` for aim, and the stock melee/missile state transitions
  for firing. It only swaps the *movement* per order (e.g. fallback = move in the
  direction opposite the target; flank = ±90° of it). So directed monsters still
  acquire targets and shoot normally.
- **Latched + gated.** `P_AI_Active` only diverts for an active, non-`chase` order
  (`after<=0`, `for>0`); when `for` expires the monster reverts to vanilla
  `A_Chase`. While the director "thinks", monsters keep executing the last order.

### Caveats / what's deliberately partial
- A monster only obeys once it's **awake** — `A_Chase` (hence the hook) runs only
  for alerted monsters; asleep ones still need to see/hear the player first.
- **Determinism**: directed monsters break demo/net sync (they bypass the
  `P_Random`-driven path). Single-player only — same caveat as §12.
- `use_door`/`use_teleporter` are approximated as "move toward target" (real
  door/teleport routing is future work); `focus_fire` defaults to the player.
- The observation has no room/topology yet (§11 "partial observability") — add a
  topological map from the BSP/automap for real navigation orders.
- Implementation note: `p_ai_llm.c` does **not** include `p_local.h` — that pulls
  in `p_spec.h`, whose enum constants `open`/`close` collide with `<unistd.h>`;
  the two engine functions it needs are declared by hand instead.

## 14. Letting the LLM control the *buddy* (-aicoop)

The same director transport also commands the co-op companion. `-aicoop` (instead
of `-coop`) starts the buddy in AI mode: it opens the AI socket (on the
`-aidirector` port or the default 31666), adds the buddy to the `observe` stream,
and accepts `buddy` orders.  `start_aidoom.sh --aicoop` launches game + director.

### Protocol additions
- **Observation** gains a `"buddy"` object when `-aicoop` is active:
  `"buddy":{"pos":[x,y],"health":h,"armor":a,"weapon":w,"ammo":n,"state":"...","region":r,"route":[[x,y],...]}`
  — `route` is a downsampled list of **reachable** waypoints along the buddy→player
  path (engine-computed via the portal graph), giving the director real spatial
  context + valid coordinates to `goto`.
- **Map topology** (so the LLM can reason about walls / flanking / doors): every
  entity (player, buddy, monster) carries a `"region"` (sector id); the state adds
  `"regions":[[id,x,y],...]` (room centres, occupied rooms + their one-hop
  connectors) and `"links":[[a,b,"open"|"door"|"locked"],...]` (how those rooms
  connect).  Each monster also gives `"see_buddy"` and distances `"d_player"` /
  `"d_buddy"` (was only `see_player`).
- **Command:** `buddy order=<tactic> [focus=<monster id>] [x=<n> y=<n>] [for=<tics>]\n`
  → `ok\n`.  Tactics: `engage` (focus a specific monster, else nearest), `defend`,
  `hold`, `regroup`, `retreat`, `goto` (x,y), `grab`.

### How it executes
Unlike the monsters (which divert `A_Chase`), the buddy LLM order maps onto the
buddy's existing **rule-based overrides** in `p_ai_coop.c`
(`P_AICoop_SetDirective`): `engage`→forced target, `hold`/`regroup`/`retreat`→the
stay/come timers, `goto`→a move-to-point timer.  The proven rule-based BuildCmd
then executes it per tic.  Orders **expire** after `for` tics, so the buddy
reverts to autonomous behaviour the moment the director stops talking.

The director (`tools/director.c`) drives **monsters and/or buddy** from one LLM
call: it asks for `{"commands":[...monsters...],"buddy":{"order","focus"}}` and
issues whatever the live state contains (monsters from `-aidirector`, buddy from
`-aicoop`).  Same single-player/determinism caveats as §12–13.
