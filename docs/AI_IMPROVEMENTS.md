# AI/LLM improvements for aiDoom

This file is a capability matrix plus a deliberately short backlog. Several items in the old version were already shipped but were still written as future work—the documentation equivalent of a zombie state that forgot to die.

## Shipped today

| Capability | Source / status |
|---|---|
| Rule-based buddy | `files/p_ai_coop.c`; enabled with `-coop` or `-aicoop`. |
| LLM buddy directive layer | `files/p_ai_llm.c` parses `buddy order=...` and applies high-level state through the buddy controller. |
| Monster LLM tactics | `act order=...` side-table directives, executed from the monster chase path. |
| Director pacing | `files/p_ai_director.c`: stress model, phases, checked spawns, exit/objective pressure, safety gates and supplies. |
| Structured monster observation | Player, buddy, monsters, visibility/distances, regions, links and director stress. |
| Two voice streams | `files/i_voice.c`: buddy `DS*` and Director `DD*` streams. |
| Shared navigation | BSP-subsector graph and `P_AICoop_NextWaypoint`; current `PF_AStar` search behaves as Dijkstra because `h=0`. |
| Pack hunt | Current monster pack configuration/path where enabled. |
| Agent player control | `-aiplayer [port|demo]`, separate protocol/reflex controller in `files/g_agent.c`. |

The buddy is one fixed slot. There is no `-aicoop N` multi-buddy mode.

## 1. Better LLM buddy coordination

Already implemented at a basic high-level layer: the director can send `buddy order=engage|defend|hold|regroup|retreat|goto|grab` and the buddy keeps its autonomous C reflex behavior underneath.

Useful future work would be:

- richer order arguments with explicit target/route expiry;
- acknowledgements or an observation field confirming the active directive;
- clearer conflict rules between human keyboard orders, local bot policy and LLM orders;
- separate combat/support directives rather than one overloaded state.

Do not describe this as an unimplemented “LLM buddy layer” anymore.

## 2. Richer monster tactics

The current rule selector assigns focus, left/right flank and fallback. The LLM can request more orders, but the executor still has a small tactical vocabulary and one directive slot per monster.

Potential improvements:

- per-monster role/formation state rather than overwrite-only directives;
- squad membership and explicit handoff/expiry;
- better door/ambush execution;
- projectile timing and cover evaluation;
- a safe, bounded sequence primitive instead of pretending `after` queues commands.

## 3. Director map-state coordination

Already implemented beyond the original proposal. The observation includes:

- monster positions/types/HP/regions;
- player and optional buddy state;
- visibility and distances;
- region centroids and open/door/locked links;
- director intensity, recent damage and ammo percentage.

The rule director also uses objective/exit detection and shared navigation to bias pressure. Keep future additions focused on information quality and cost, not on re-implementing a broadcast that already exists.

## 4. Persistent learning

Not implemented. A file such as `~/.aidoom/ai_memory.dat` remains a proposal and is not part of the current runtime.

If added later, keep learned state out of deterministic gameplay unless it is explicitly versioned/seeded. Prefer a tool-side analytics file first.

## 5. RL/headless hybrid

Not implemented. There is no shipped `-headless` mode, server-side reward, `done` flag, fixed step API or frame-skip wrapper in the AI modules. The proposal/comparison docs may describe these as architecture options, but they must be labelled as such.

A future implementation would need explicit ownership of:

- video/audio initialization;
- tic stepping and action repeat;
- reset/save lifecycle;
- reward/done semantics;
- deterministic seed and episode boundaries.

## 6. Voice and coordination

The two streams are already shipped. Remaining improvements are catalogue/UX work:

- expose voice-gate drops in a debug console rather than silently losing every rejected line;
- add localization/alternate persona bundles;
- make the Director's 16 s ambient and 6 s forced-line floor configurable only if there is a real use case;
- keep WAD tag names byte-checked.

## 7. Multi-agent coordination

Not implemented beyond one buddy plus the separate monster director. A real multi-agent design would need stable registry identities, ownership/arbitration and savegame semantics before adding more actors.

## 8. Recommended order

1. Improve observability/acknowledgement without changing simulation.
2. Make the one-slot directive semantics explicit in clients.
3. Add targeted tactical executors (door/cover/formation) behind flags.
4. Only then evaluate persistent learning or multi-agent state.
5. Treat headless RL as a separate environment project, not a small console option.

## Related docs

- `docs/MONSTER_AGENT_GUIDE.md` — shipped monster/director protocol.
- `docs/AIPLAYER.md` — shipped player-agent protocol.
- `docs/DIRECTOR_MODES.md` — rule versus LLM pacing.
- `docs/BUDDY_PORTING.md` — buddy behavior.
- `docs/BUDDY_VOICE.md` — voice gate and assets.
- `docs/doom_agent_api_architecture.md` and `docs/doom_agent_api_vizdoom.md` — proposals/comparisons only.
