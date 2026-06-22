# Director: LLM vs rule-based — what differs, and rule-based tactics

## 1. Does behaviour differ with vs without the LLM?

Yes, but it's narrower than it looks, and there's a fallback gotcha.

### Different WITH the LLM (`-aidirector`, or `-aicoop` + a connected director)

| Aspect | With LLM | Without (rule `-director`) |
|---|---|---|
| **Monster tactics** | Squad orders — flank / ambush / focus-fire / fall-back — issued over TCP (`p_ai_llm.c`: `act order=…`); `A_Chase` diverts to `A_LLMChase` for directed monsters. | **Same order set, rule-assigned** (`P_AI_RuleTactics`): flank / focus / ambush / fall-back by geometry + LOS. The LLM *replaces* this when actively issuing orders. |
| **Spawn policy** | The model decides what / when / how many from the live state (`spawn type=… count=…`, `director relax`). | Fixed stress→spawn FSM curves (BUILDUP→SUSTAIN→FADE). |
| **Buddy orders** | LLM gives the buddy high-level orders (engage/defend/regroup/goto/…). | Buddy runs purely on its built-in bot. |
| **Director voice** | `dir:flank/ambush/focus/fallback` lines fire (from tactics). | Only phase/spawn/relax/item lines fire. |

### The SAME in both modes
Intensity/stress tracking, objective seeding + idle guards, periodic objective top-up,
emergency medkit/ammo drops, exit-proximity pressure, the `doom2stuff` overlay, and the
buddy's core bot (acquire/fire/follow/heal).

### Fallback gotcha
`-aidirector` *layers on top of* the rule director; it doesn't replace it. The FSM is
suppressed only while the LLM is **actively** issuing commands
(`runfsm = gametic - dir_llmlast > 15 s`). So:
- LLM connected & busy → LLM tactics + LLM spawns (+ periodic hordes).
- **No director client connected / it crashed / the watchdog killed it** → `dir_llmlast`
  never updates → `runfsm` always true → **`-aidirector` behaves like `-director`**
  (rule FSM only, no tactics). If the `director` sidecar window isn't showing live
  "round N … orders=K", the tactical layer isn't firing.

## 2. Rule-based coordinated tactics — DONE (`P_AI_RuleTactics`)

**Implemented and at parity with the LLM's `act` order set.** The tactical *execution*
layer (`A_LLMChase`, the `aient[]` side-table) is LLM-agnostic; the LLM only *decides
which monster gets which order*. `P_AI_RuleTactics` (`p_ai_llm.c`) now makes those same
decisions by C heuristics every ~1 s:

- **focus-fire** (`AIO_FOCUS`) — any monster with LOS to the player presses the attack.
- **flank** (`AIO_FLANK_L/R`) — when ≥4 are visible, ~⅓ peel off (alternating L/R) for a
  pincer (skips point-blank ones).
- **ambush** (`AIO_AMBUSH`) — a hidden monster already camped on the player's *nearest
  objective* (exit/key, passed in from the director) lies in wait there.
- **fall-back** (`AIO_FALLBACK`) — a badly-wounded monster (≤¼ HP) with LOS kites/retreats
  instead of suiciding.

Orders auto-expire (`for_tics`) back to vanilla; unassigned monsters stay on `A_Chase`.
It's **deterministic** (geometry only — demo/netplay-safe), unlike the LLM path. Called
from the director ticker whenever the rule layer is in charge (pure `-director`, or the
LLM has gone quiet), and **the LLM replaces it** while actively issuing orders (`runfsm`
false → the ticker returns before the rule call, so they never fight).

### How it was built (kept for reference)
The tactical *execution* layer is completely LLM-agnostic; the LLM only *decides which
monster gets which order*. Replacing that one decision step with C heuristics gave
rule-based squad tactics.

### What's already there (shared, reusable)
- **Order verbs** (`p_ai_llm.c`): `AIO_FLANK_L/R`, `AIO_AMBUSH`, `AIO_FOCUS`,
  `AIO_FALLBACK`, `AIO_HOLD`, `AIO_USEDOOR`, `AIO_CHASE`.
- **Directive side-table** keyed by `mobj_t*` (`aient[]`) + `AI_BuildRegistry()` (ids) +
  `AI_Apply(order, ids, …)` (assign) — no struct/savegame change.
- **Executor** `A_LLMChase(actor)` — runs the assigned order each tic (`A_Chase` diverts
  to it for directed monsters). This is what actually makes a monster flank/hold/etc.
- **Proof it works without an LLM**: `AI_DemoDirector()` (`-aidemo`) already assigns
  flank/fallback orders algorithmically every ~3 s — a crude rule-based tactician.

### So the gap is just a smarter rule-based "tactician"
A per-N-tic function (call it `P_Director_Tactics`, runs under `-director`) that:
1. `AI_BuildRegistry()` → the live directable monsters + the player/buddy positions.
2. Classify each monster by geometry/LOS/distance and assign an order:
   - **Flank**: a monster with LOS to the player but off to a side → `AIO_FLANK_L/R`
     (the side away from where the player is facing / where allies already are).
   - **Focus-fire**: when several monsters can see the player, point the pack at the
     player (or the most-wounded survivor) → `AIO_FOCUS`.
   - **Ambush**: freshly-spawned monsters near the player's *path ahead* (toward the
     exit/key — we already track objectives) → `AIO_AMBUSH` until the player is close.
   - **Fall-back / regroup**: a lone monster far from allies → `AIO_FALLBACK` so the
     pack gathers before assaulting (mirrors L4D "build the horde, then commit").
3. Re-evaluate every ~1 s; orders auto-expire back to vanilla (`for_tics`), exactly as
   the LLM path does.

This reuses `AI_Apply` + `A_LLMChase` unchanged — only the *assignment heuristics* are
new. Effort: ~moderate (the heuristic pass; the hard part — execution — is done). It's
essentially porting the LLM's "decide orders" step into deterministic C, which also keeps
demos/netplay in sync (the LLM path is non-deterministic and stays outside the tic lock).

### Remaining LLM functions vs the rule director (parity status)
- **Monster tactics (`act`)** — ✅ at parity (`P_AI_RuleTactics`, above).
- **Spawning (`spawn type/count`)** — ✅ the rule FSM spawns richly (buildup/sustain,
  hordes, behind-player, objective seeding/guards, exit pressure).
- **Items (`spawn item`)** — ✅ rule FSM drops (emergency medkit/ammo, relax gifts).
- **Pacing (`director relax`)** — ✅ rule FSM (BUILDUP→SUSTAIN→FADE).
- **Buddy orders (`buddy order=…`)** — ⚠️ the buddy runs on its own autonomous bot in
  both modes (engage/defend/follow/heal), so it's *functionally* covered, but the rule
  director does not yet *issue* explicit buddy directives the way the LLM can. Lowest-
  value gap (the bot already self-manages); a small follow-up if desired.
