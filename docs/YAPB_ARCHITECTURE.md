# yapb architecture for aiDoom

This document is a porting/reference note: yapb is a Half-Life bot architecture, while aiDoom has a much smaller native companion stack. The useful output is the separation of concerns, not a claim that the two engines share APIs.

## yapb's layers

The reference bot separates roughly into:

- engine hooks and lifecycle;
- bot state/blackboard;
- combat and aim;
- navigation and route following;
- waypoint graph generation;
- communication and debug tooling.

That separation is useful in aiDoom because the game simulation is deterministic and tic-locked. Perception, decision-making and ticcmd generation must not leak wall-clock behavior into the playsim.

## aiDoom mapping

| yapb concern | aiDoom location | Current status |
|---|---|---|
| Bot lifecycle | `files/p_ai_coop.c` | Implemented for one fixed buddy slot (`coop_slot=1`). |
| Combat/aim | `files/p_ai_coop.c`, `files/p_enemy.c` | Rule-based buddy combat and shared Doom weapon/projectile logic. |
| Navigation | `files/p_ai_coop.c` | BSP-subsector portal graph, danger costs and waypoint following. |
| Visibility | `files/p_sight.c` and direct callers | Direct checks; the proposed persistent cache was rejected for now. |
| Voice/UI | `files/i_voice.c`, `files/hu_buddy.c` | Separate buddy and director voice streams plus HUD. |
| External high-level control | `files/p_ai_llm.c`, `files/g_agent.c` | Monster/director and player-agent TCP line protocols. |

## Navigation: current terminology matters

The public function is `PF_AStar`, but its heuristic is intentionally disabled (`h = 0`) in the current implementation. The search therefore behaves as Dijkstra. It is not the same as yapb's fully heuristic A* presentation.

The graph is built from BSP subsectors and passable portal connections. Edge costs can include closed-door and damage-floor penalties. `P_AICoop_NextWaypoint` is the shared route-query surface used by the buddy and other AI paths.

## Combat and safety

The buddy controller contains target selection, weapon selection, projectile/blast safety, movement avoidance and recovery. It is not a generic behavior-tree or planner framework. High-level orders are translated into the existing ticcmd/reflex path rather than directly mutating actor state.

A danger heatmap is fed by damage observations (`P_AICoop_NoteDamage`). The safe-route mode is used when the buddy is low on health. The visibility-cache proposal was explicitly rejected because the current target path did not justify its invalidation complexity.

## External control and voice

`files/p_ai_llm.c` can serialize structured monster/buddy observations, topology and director stress, and can apply one directive slot per monster. `after=` is a delayed start on that one stored directive, not a multi-command queue.

Buddy audio is pre-baked and gated by `AICoop_VoiceGate`; the buddy stream does not preempt itself when a higher-priority line arrives. Director audio uses its own stream and cooldowns. See `docs/BUDDY_VOICE.md`.

## What is not implemented

- multiple independent buddy slots or an `-aicoop N` mode;
- a general yapb-compatible waypoint asset format;
- a persistent visibility cache;
- a generic behavior-tree editor;
- a headless RL step/reward API;
- a proof that the current agent/buddy behavior is netgame-compatible. The buddy is disabled in netgames.

## Practical lessons to carry over

1. Keep lifecycle, navigation, combat and presentation separate.
2. Keep route search deterministic and bounded.
3. Treat visibility and door state as volatile inputs.
4. Make the reflex layer own movement/collision consequences; high-level controllers should issue intent.
5. Document actual source names even when historical names such as `PF_AStar` are misleading.

## References

- `files/p_ai_coop.c` / `files/p_ai_coop.h` — native buddy and pathfinder.
- `docs/BUDDY_PORTING.md` — as-built buddy behavior.
- `docs/VISIBILITY_CACHE.md` — cache decision.
- `docs/AIPLAYER.md` — player-agent reflex controller.
- `docs/AI_IMPROVEMENTS.md` — shipped capabilities versus future proposals.
