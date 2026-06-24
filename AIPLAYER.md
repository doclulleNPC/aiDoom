# AIPLAYER.md — the `-aiplayer` agent (as built)

How the **human marine** is driven by code instead of the keyboard. This is the
*as-built* reference for `files/g_agent.c` + `run/llm_player.py`; the *design rationale*
(why it's shaped this way, the speed-gap, RL/headless ideas) lives in **`AGENT_CONTROL.md`
§1–11**, and the monster-side director in §12–13. For the buddy bot see **`BUDDY_*.md`**.

> A no-argument launch is still vanilla 1993 DOOM. `-aiplayer` is opt-in.

---

## 1. What it is

`-aiplayer` hands **player 1's per-tic `ticcmd`** to `g_agent.c` instead of reading the
keyboard. The split is deliberate:

- **An external BRAIN plans slowly** — an LLM (via `run/llm_player.py`), a script, or
  anything that speaks the line protocol — issuing *high-level intents* ("attack thing 3",
  "goto 1056 -3616").
- **A C REFLEX executes fast** — `G_AgentBuildTiccmd` turns the current intent into a
  concrete ticcmd **every tic (35 Hz)**: aim, step-toward, fire windows, door-use, kiting.

So the marine reacts at full game speed while the brain thinks at its own (much slower)
pace. The reflex is also a competent autonomous player on its own — see the demo brain.

### Two modes
```
-aiplayer 31700     LLM/socket mode: listen on 127.0.0.1:31700, a client drives it
-aiplayer demo      built-in scripted brain, no client (proves the hook + plays solo)
```
Parsed in `G_AgentInit` (called from `d_main.c`). The hook is in `g_game.c`
`G_BuildTiccmd`: `if (G_AgentActive()) { G_AgentBuildTiccmd(cmd); return; }`.

---

## 2. The wire protocol (TCP, one line each way)

Non-blocking single-client server (`Agent_OpenSocket`/`Agent_Poll`), same shape as
`p_ai_llm.c`. The client sends a command line; queries reply with one JSON line.

### Queries

`map` → **static level geometry, once** (`Agent_SendMap`):
```json
{"start":[x,y], "exit":[x,y]|null,
 "doors":[[x,y],...],
 "lines":[[x1,y1,x2,y2,special],...]}     // up to 1200 linedefs
```
`observe` → **live state** (`Agent_Observe`):
```json
{"tic":N,
 "player":{"x","y","z","angle","health","armor","weapon","ammo":[4],"kills","items"},
 "exit":[x,y]|null,
 "buddy":{"friend":true,"x","y","health"}|null,
 "things":[{"id","type","dist","rel","hp","vis","x","y","z"}, ...],
 "doors":[{"pos":[x,y],"seen":0|1}, ...],
 "lidar":[d0,d1,...,d7],
 "sounds":[{"name","dist","rel"}, ...],
 "waiting_at_door":true|false}
```
Notes: `angle` is degrees; `rel` is the bearing to a thing in degrees `[-180,180]`;
`vis` is line-of-sight; `id` indexes the per-observe **target registry** (`reg[]`) that
`target <id>` resolves through. Only live monsters (`MF_COUNTKILL`) and pickups
(`MF_SPECIAL`) appear — no barrels, no corpses. The **buddy is reported separately and is
never a thing/target** (it's a friend).

### Perception beyond `things` — sight, lidar, hearing
- **`things`** is *semantic* sight: every live monster/pickup with bearing + LoS, regardless
  of distance (the brain decides what's relevant).
- **`lidar`** is *geometric* sight: 8 ray distances at 45° around the marine
  (`Agent_TraceDistance`), each the walkable run before a wall, a >24u step-up, a >24u
  drop-off, or no head-room — out to 1024u. Index 0 is straight ahead, then CCW. Lets the
  brain see corridors/ledges/dead-ends without map geometry.
- **`sounds`** is *hearing*: every SFX the engine starts within 1600u of the marine is logged
  (`G_Agent_LogSound`, hooked into `S_StartSound`) with its lump name, distance and bearing,
  then drained each `observe`. So the brain hears gunfire / monster wake-ups / doors out of
  sight.
- **`waiting_at_door`** — the reflex is parked at a doorway waiting for it to rise (below).

### Intents (set state, no reply)
| command | effect |
|---|---|
| `goto <x> <y>` | walk to a map point (engine A*-paths there) |
| `face <x> <y>` | turn to look at a point |
| `turn <deg>` | turn by a relative amount |
| `attack <0\|1>` | hold fire on/off (auto-aims the nearest visible monster) |
| `target <id>` | attack a specific thing from the last `observe` |
| `weapon <1-8>` | switch weapon |
| `use` | press USE once (doors / switches) |
| `stop` | clear movement + attack + face intent |

---

## 3. The reflex controller (`G_AgentBuildTiccmd`, every tic)

The core. It **decouples AIM from MOVEMENT** so the marine can fire at a monster while
strafing toward a different goal. Order of operations:

1. **Not in a level?** (intermission / finale) → pulse `BT_ATTACK` every 16 tics to advance
   the tally screen, then return. (Without this the agent stalls on the end screen forever.)
2. **Demo mode** → run `Agent_Brain` to set the intents (below).
3. **Door bookkeeping** → mark any door within 160u as `seen`.
4. **Acquire target** → an explicit `target <id>`, else the nearest visible+shootable
   monster while `attack` is held (`Agent_NearestMonster`).
5. **Key fetch** → if a *locked* door is right ahead and we lack its key, find the matching
   key item in the map and detour to it (`Agent_LockedNeedAhead` + `Agent_FindKey`); clears
   on pickup / level change. The colour comes from the door's linedef special
   (26/32 blue, 27/34 yellow, 28/33 red).
6. **AIM target (`want`)** = the monster (visible + in `AGENT_FIRE_RANGE`), else an explicit
   `face`, else (filled in below) the direction we're walking.
7. **MOVEMENT goal (`gx,gy`)**, priority: explicit `goto` > key fetch > close in on the
   combat target > the level exit.
8. **Steering** (buddy-grade, `AICoop_*` from `p_ai_coop.c`) — turn the far goal into a
   reachable **steer point**:
   - a doorway on the way (`AICoop_FindDoorAhead`) → head straight at it (and if we're
     within 80u of it, set `waiting_at_door`);
   - else `AICoop_CanReach(goal)` clear → head straight at the goal;
   - else the A* portal waypoint (`P_AICoop_NextWaypoint`) if reachable → head at it;
   - else **corner-round**: `AICoop_ChaseDir` picks a walkable 8-dir, projected 96u ahead.
9. **Start nudge** (`force_straight`) → for the first ~3 s of a level with no goal/target,
   walk straight ahead at half speed, so the marine clears the spawn instead of dithering.
10. **Movement** = decompose the steer direction into **forward + strafe** relative to facing
    (`finecosine`/`finesine`), so the marine slides exactly along the route no matter which
    way it's looking. While not aiming a monster/face, `want` follows the move direction.
11. **At the exit switch** (within 64u) → face it, press `BT_USE`, stop — finishes the level.
12. **Turn** toward `want`, clamped to `AGENT_TURN`/tic.
13. **Fire** iff target visible **and** within `AGENT_FIRE_RANGE` **and** lined up within
    `AGENT_FACING` **and** the buddy is not in the line (`Agent_BuddyInLine`) **and** no
    explosive barrel is in the line (`Agent_BarrelInLine`).
14. **Kite (survival)** → a *visible* target closer than 128u (or 384u when `health < 40`)
    flips movement to **back away** while still firing — keeps a melee pinky off us.
15. **Weapon up** → if holding fist/pistol but we own a chaingun/shotgun, switch to it (one
    shot, so a foe dies before it reaches melee).
16. **Door open + WAIT** → `Agent_UseAhead` taps USE on a *shut* door (it skips ones already
    risen ≥56u) or the exit switch in the forward arc within 80u. Pressing USE on a doorway
    arms `door_wait_timer = 60`: for ~1.7 s the marine **faces the door and waits** for it to
    rise — it does not re-spam USE or wiggle (the fix for jittering against an opening door).
    `waiting_at_door` is reported in `observe`.
17. **Stuck recovery** → no progress for >5 tics while chasing (and not door-waiting) → sweep
    the view + strafe + USE to free up.

The agent's intent state (goal, attack/use flags, `door_wait_timer`) is **saved/restored
with the game** (`G_Agent_Archive`/`G_Agent_UnArchive`, called from `g_game.c`), so a
load resumes mid-plan instead of resetting.

### Tunables (top of `g_agent.c`)
`AGENT_TURN 1400` (max turn/tic ≈ 7.7°) · `AGENT_FACING 900` (fire cone ≈ 5°, ≈ auto-aim)
· `AGENT_FORWARD 50` (run speed) · `AGENT_FIRE_RANGE 1024u` · `AGENT_SIGHT 2048u` ·
`AGENT_REACH 64u` (goal-reached) · `AGENT_MAXDOORS 96` · `AGENT_MAXTHINGS 256`.

---

## 4. The built-in demo brain (`Agent_Brain`, `-aiplayer demo`)

A capable solo player, and the fallback when no client connects. Each pick:

1. **Kill first** — a foe in view → drop the wander goal, let the reflex engage (face +
   fire + kite). *Primary directive: kill monsters, stay alive.*
2. **Near the exit** (<512u) → beeline it; `Agent_UseAhead` then hits the switch.
3. **Door-directed exploration** — steer to the **nearest not-yet-`seen` doorway**
   (collected at level start). The key/exit route runs through doors, so this threads the
   map far more purposefully than a random walk.
4. **Random walk** fallback (±64° off facing, 512u hops) when all doors are done / none
   exist — covers open areas and the nukage-ledge starts that aren't gated by a door.
5. **Goal timeout** — abandon a goal chased >3 s without arriving; if it was a door, mark
   it done so we don't retry one we can't reach.

> Why a random walk at all: the very first `-aiplayer` shipped *only* a random walk, and
> that is what made it leave rooms and finish levels (it's why we needed the intermission
> skip). A fixed "beeline the exit" instead pins on the first wall/ledge. Door-targeting is
> the directed version; the random walk stays as the escape hatch.

Harness result (E1M3, demo): explores most of the level, ~9 kills, closes from 2000 to
~430 units of the exit. E1M1's nukage-ledge start (no door out) is the hard case.

---

## 5. The LLM client (`run/llm_player.py`)

The reference brain for socket mode. Talks to **Ollama** (default
`http://192.168.2.114:11434`, model `mistral:7b-instruct`).

- **Startup**: fetches `map` once (exit, spawn, all doors) as static context.
- **Event-triggered, not fixed-rate**: polls `observe` at ~10 Hz but only calls the LLM on
  a *meaningful* event — first state, a ≥3 s timeout, took damage, a new monster came into
  view, the current `target` died, or `goto` went `stuck` for >1.5 s. This keeps the slow
  LLM off the critical path while staying responsive.
- **Structured replies**: the model answers `THOUGHT: …` / `COMMAND: …`; a tolerant parser
  (`parse_reply`/`extract_cmd`) pulls one valid command out even from chatty 8B output.
- **Short-term memory**: the last ~5 thought→action pairs are fed back for continuity.

Env: `AIPLAYER_PORT` (31700), `OLLAMA_URL`, `OLLAMA_MODEL`.

### Full-LLM mode — `run/start_llm_player.sh`
One launch where an LLM drives **everything**: the marine (`-aiplayer 31700` +
`llm_player.py`) **and** the buddy + monsters + L4D pacing (`-aidirector 31666 -aicoop` +
the `run/director` sidecar). Two independent sockets, two brains, both to Ollama. Adds
`-nofriendlyfire` so a stray shot can't hurt the buddy. See `run/README.md`.

---

## 6. Safety / tactical helpers

- **`Agent_BuddyInLine`** — never fire when the AI buddy sits on the shot line (in front,
  closer than the target). The buddy is excluded from targeting entirely (it's a player
  mobj, not `MF_COUNTKILL`).
- **`Agent_BarrelInLine`** — never fire when an explosive barrel is on the shot line; the
  blast would hurt us as much as the foe.
- **Determinism caveat** — the reflex builds ordinary ticcmds (the buddy-safe channel), but
  socket + LLM timing is non-deterministic, so a recorded **demo of an `-aiplayer` game
  will not replay**. That's expected; the tic flow itself is untouched.

---

## 7. Status & known limits

Works: socket + demo control, decoupled aim/move, buddy-grade navigation (CanReach →
doorway → A* waypoint → ChaseDir), door-directed exploration, locked-door key fetch,
kiting + weapon-up survival, intermission skip, buddy/barrel fire safety.

Open: **surviving all the way to the exit** (the marine explores + fights well but can still
die late), and **the E1M1 nukage-ledge start** (no door gates the way out, so only the
random-walk fallback can thread it — stochastic). The navigation pieces are buddy-grade;
the remaining work is higher-level survival (hazard avoidance, low-HP retreat to health).

## 8. Files
- `files/g_agent.c` / `g_agent.h` — the agent (server, protocol, perception, reflex, demo
  brain, savegame archive).
- `files/g_game.c` — the `G_BuildTiccmd` hook. `files/d_main.c` — `G_AgentInit`.
- `files/p_ai_coop.c` — shared navigation (`P_AICoop_NextWaypoint`, `AICoop_CanReach`,
  `AICoop_FindDoorAhead`, `AICoop_ChaseDir`).
- `files/s_sound.c` — calls `G_Agent_LogSound` from `S_StartSound` (the `sounds` feed).
- `files/g_game.c` — also calls `G_Agent_Archive`/`G_Agent_UnArchive` (intent state in saves).
- `run/llm_player.py` — the Ollama brain. `run/start_llm_player.sh` — full-LLM launch.
- `AGENT_CONTROL.md` — the design rationale this implements.
