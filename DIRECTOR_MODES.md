# Director: LLM vs rule-based — what differs, and rule-based tactics

## 1. Does behaviour differ with vs without the LLM?

Yes, but it's narrower than it looks, and there's a fallback gotcha.

### Different WITH the LLM (`-aidirector`, or `-aicoop` + a connected director)

| Aspect | With LLM | Without (rule `-director`) |
|---|---|---|
| **Monster tactics** | Squad orders — flank / ambush / focus-fire / fall-back — issued over TCP (`p_ai_llm.c`: `act order=…`); `A_Chase` diverts to `A_LLMChase` for directed monsters. | **None.** The rule director only *spawns/paces*; monsters use vanilla `A_Chase` (or pack-hunt with `monster_pack 1`). |
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

## 2. Can we build coordinated monster tactics for L4D mode WITHOUT the LLM?

**Yes — and most of it already exists.** The tactical *execution* layer is completely
LLM-agnostic; the LLM only *decides which monster gets which order*. Replace that one
decision step with C heuristics and you have rule-based squad tactics.

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

### Recommended first slice
Flank + focus-fire only, gated behind `-director` (or a `monster_tactics 1` config):
group monsters that can see the player, send ~⅓ to flank left/right and the rest to
focus the player; re-plan every 35 tics. Build on `monster_pack`'s ally-awareness for
the grouping. Add ambush/fall-back once that feels good.
