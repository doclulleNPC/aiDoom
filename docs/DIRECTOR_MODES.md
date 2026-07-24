# Director: LLM vs. rule-based — what actually differs

## 1. Two director modes

BuddyDoom has two distinct director paths:

- `-director` — the offline/rule-based L4D-style director.
- `-aidirector [port]` — a TCP client drives pacing/spawn commands; the rule layer supplies fallback behavior.
- `-aidemo` — built-in director demo/stress tracking; it uses the same LLM-mode pacing semantics without requiring a remote client.

The monster tactic layer in `files/p_ai_llm.c` and the pacing/spawn layer in `files/p_ai_director.c` are related but not identical. A connected client can issue monster `act` or buddy commands without automatically taking over pacing.

## 2. LLM-mode semantics

The LLM path can issue:

- monster tactic directives (`act order=...`);
- buddy directives (`buddy order=...`);
- pacing spawns (`spawn type=... count=...`);
- item drops (`spawn item=medkit|ammo`);
- `director relax`.

The rule FSM watchdog is updated only by the pacing commands: monster spawn, item spawn and relax. It is **not** reset by ordinary `act` or `buddy` lines. After roughly `15*TICRATE` without a pacing command, the rule FSM is eligible to resume.

While the FSM is suppressed, the LLM path is not a fully autonomous pacing engine: the client must decide what to spawn, and the source contains a periodic horde safeguard that can still fire after its own gap and stress checks.

## 3. Rule mode

Pure `-director` always runs the rule FSM. It maintains a stress/intensity model and build-up → sustain/peak → fade/relax phases. It can:

- validate hidden spawn positions against survivors;
- bias pressure toward the exit route and objective rooms;
- seed and top up guards near exits/keys;
- wake non-objective monsters while leaving objective guards as ambush-style sleepers;
- apply focus/flank/fallback rule tactics;
- resurrect corpses under the implemented chance/rules;
- use DOOM1-safe remapping and overlay-aware Doom/FreeDoom/Heretic/Hexen pools;
- place Hexen stalkers only under the current liquid/placement constraints;
- cap live monsters and avoid spawning into walls/invalid sectors;
- provide emergency medkits/ammo;
- suppress hordes and special/boss pressure when the survivor is critically low on health or ammo;
- announce events on the HUD and through the separate director voice stream.

The rule tactics themselves assign only the current focus, left/right flank and fallback orders. Objective guards waiting near keys/exits are a separate mechanism; do not call that an `AIO_AMBUSH` command emitted by the rule tactic selector.

## 4. What is shared

Both paths use:

- the same tic-locked game simulation;
- the same hidden-spawn validation and checked actor spawning;
- the same actor pools and overlay detection;
- the same player stress/health/ammo safety gates;
- the same buddy navigation helper where a route to the exit/objective is required;
- the same monster directive side table and `A_Chase` integration for tactics;
- the same director HUD/voice announcements where the event path is active.

## 5. What is not shared

- LLM mode can receive remote orders and choose exact spawn/item commands.
- Rule mode chooses pacing, spawn type/count and phase transitions locally.
- LLM monster tactics do not, by themselves, suppress the rule spawn FSM.
- Rule tactics do not currently reproduce the complete LLM order vocabulary.
- There is one fixed buddy slot; no `-aicoop N` multi-buddy mode exists.

## 6. Voice timing

Director voice is not the same as buddy voice. Director ambient lines require a `16` second gap. Forced/event lines still obey a `6` second hard floor between any two director lines. Buddy voice has its own busy gate and does not self-preempt; see `docs/BUDDY_VOICE.md`.

## 7. Source map

- Stress/director state, spawns, safety gates and voice: `files/p_ai_director.c`, `files/p_ai_director.h`.
- Monster protocol and directive application: `files/p_ai_llm.c`, `files/p_ai_llm.h`.
- Monster chase hook: `files/p_enemy.c`.
- Buddy directive/reflex layer: `files/p_ai_coop.c`.
- Operational protocol: `docs/MONSTER_AGENT_GUIDE.md`.
