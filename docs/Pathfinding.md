# Doom pathfinding

This document contains the background theory and the current aiDoom implementation boundary. The original Doom renderer's BSP tree is useful to navigation, but classic Doom does not ship with a general-purpose A* navigation mesh.

## Current aiDoom pathfinder

The implemented buddy/agent pathfinder is in `files/p_ai_coop.c`. It builds a graph over BSP subsectors and portal-like connections through walkable two-sided lines. Edge costs include distance and penalties for doors or dangerous floor sectors.

The public helper is named `PF_AStar`, but the current implementation deliberately disables the heuristic (`h = 0`) to avoid oscillation caused by heuristic inconsistency. In practice it is Dijkstra over the subsector graph, followed by direct-reachability/string-pulling behavior when choosing the next waypoint. `P_AICoop_NextWaypoint` exposes the shared next-waypoint operation to the buddy, player agent and director.

This naming is historical source/API debt: do not describe the current route search as heuristic A* merely because the function is called `PF_AStar`.

## 1. A* and navigation meshes as general techniques

A* is appropriate when a navigation graph has a useful admissible heuristic. A navigation mesh is appropriate when the walkable surface can be represented as connected regions that already account for actor size and movement constraints.

For classic Doom, both approaches require an additional graph extraction step. The WAD provides map geometry, sectors, lines, subsectors and a blockmap; it does not provide a modern navmesh authored for a bot.

## 2. Patrol points and authored routes

A chain of patrol points is cheap and predictable, but it does not cover a dynamically loaded arbitrary PWAD without extra authoring. It can still be useful for scripted encounters, objective rooms or director spawn logic.

## 3. Why the BSP helps

The BSP gives a fast spatial partition and stable subsector identity. A subsector graph can therefore be constructed at level setup and reused for route queries. It is a practical engine-native compromise:

- no external navmesh asset is required;
- the graph follows the map's actual partition;
- door and hazard penalties can be added to edges;
- the same helper can serve the buddy, the player agent and the director.

It is not a collision solver. Movement still goes through the normal Doom blockmap/collision code; the pathfinder only proposes a waypoint.

## 4. Costs, danger and doors

`p_ai_coop.c` maintains route-related state and can apply a danger/safe-route mode when the buddy is hurt. Damage observations feed the danger heatmap through `P_AICoop_NoteDamage`. Door edges are classified so the caller can approach and use doors instead of blindly steering into walls.

The LLM player agent uses this shared navigation machinery, but its reflex controller remains responsible for turning a waypoint into ticcmds, aiming, use actions, weapon choice and stuck recovery. See `docs/AIPLAYER.md` and `docs/BUDDY_PORTING.md`.

## 5. Algorithm comparison

| Approach | Strength | Limitation in classic Doom |
|---|---|---|
| Blockmap lookup | Fast local collision candidate lookup | Coarse grid; not a route planner |
| BSP subsector graph | Uses data already loaded by the engine | Graph quality depends on portals/subsector layout |
| Dijkstra (`PF_AStar` with `h=0`) | Stable and deterministic; no heuristic inconsistency | Expands more nodes than a good A* heuristic |
| A* | Efficient with a valid heuristic | Current heuristic was disabled in this implementation |
| Patrol points | Simple authored behavior | Requires map-specific data |
| Modern navmesh | Good size-aware route planning | Requires a new bake/loader/runtime subsystem |

## Summary

The source-backed statement is: aiDoom has a deterministic BSP-subsector graph pathfinder whose current search behaves as Dijkstra, not a fully heuristic A* implementation. It supplies waypoints; vanilla collision and tic simulation remain authoritative.
