# Visibility cache for the BuddyDoom buddy

## Decision

A persistent visibility cache is **not implemented** and is currently not justified for the buddy's target-acquisition path. The current code performs a direct `P_CheckSight` in `AICoop_FindTarget` and uses the result immediately.

That conclusion is scoped. `P_CheckSight` has many other callers across the engine—enemy AI, director spawn validation, dead-marine revival, security drones, turrets, the player agent and other gameplay paths. This document does not claim that a global visibility cache would be useless for all of them.

## What a cache would need to solve

A cache would store a relation such as:

```text
(observer mobj, target mobj, map/geometry generation) -> visible / blocked
```

The result would need a short lifetime because doors move, actors move, lifts change sector heights and map transitions invalidate every relation. A negative result is especially dangerous to keep too long: a door opening or a target stepping into view changes it immediately.

## Candidate design

If profiling ever justifies it, the least invasive version would be a small, per-level, bounded table owned by the buddy/AI subsystem:

- key by observer/target registry slot rather than raw long-lived pointers;
- invalidate on `P_SetupLevel` and major geometry changes;
- use a very short tic TTL;
- include distance and target movement in the replacement policy;
- never bypass a required gameplay-critical recheck;
- keep the cache out of savegame state unless a stable serialized format is designed.

A negative result should expire at least as aggressively as a positive one. The engine's deterministic tic flow is preferable to wall-clock expiration.

## Current hook points

- Buddy target acquisition: `AICoop_FindTarget` in `files/p_ai_coop.c`.
- Sight implementation: `P_CheckSight` in `files/p_sight.c`.
- Director hidden-spawn validation and survivor visibility: `files/p_ai_director.c`.
- Agent perception: `files/g_agent.c`.
- Other gameplay uses: `files/p_enemy.c`, `files/p_secdrone.c`, `files/p_turret.c`, `files/revmarine.c` and related collision/interaction paths.

The existing `m_misc.c` configuration system has no shipped visibility-cache toggle. Any proposed `ai_viscache` key should therefore remain a proposal until the runtime and config code exist.

## Profiling conclusion

The historical measurement in this document is a buddy-specific measurement, not a whole-engine count. It supports keeping the simple direct sight check for now. Revisit the decision only if a future client polls large visibility snapshots every tic or a profiler shows sight tracing as a material frame/tic cost.

## Related work

- `docs/BUDDY_PORTING.md` — current target and movement behavior.
- `docs/YAPB_ARCHITECTURE.md` — external bot architecture comparison.
- `files/p_sight.c` — authoritative line-of-sight implementation.
