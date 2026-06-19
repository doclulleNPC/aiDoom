# Doom Pathfinding

## What does the original 1993 Classic Doom engine actually use?

If you are talking about the original 1993 Classic Doom engine (id Tech 1), the answer is a bit of a trick question: the original engine doesn't actually have global pathfinding at all.

Unlike modern games, Classic Doom monsters don't use graphs or grids to plan a route from Room A to Room B across a map. Instead, they use a primitive local steering technique called Kinematic Steering (specifically via the A_Chase function in the source code).

When a monster wants to reach you, it looks at your current position, calculates which of the 8 compass directions brings it physically closest to you, and walks that way. If it hits a wall, it picks a random direction for a short duration (movecount), and then tries to walk toward you again. This is why Doom monsters constantly zig-zag and get stuck on corners unless they have a direct line of sight.

However, if you are developing a mod, writing a source port (like GZDoom), or building a custom AI bot to successfully navigate a complex Classic Doom map from one side to the other, you must map the engine's unique geometry to an algorithm. Here are the best ways to do it:

## 1. The Best Algorithm: A* (A-Star) with a Navigation Mesh

Because Doom maps are not made of grids but are instead divided into irregular, polygonal floor areas called Sectors, the industry standard for this is A* Search paired with a NavMesh.

Instead of searching individual pixels or a grid, you convert the Doom map's sectors into a network of nodes:

- **The Nodes:** The centers of convex sectors or walkable polygons.
- **The Edges:** The open borders/linedefs where two sectors meet.
- **The Heuristic:** The straight-line (Euclidean) distance from the monster's current room to the target room.

Why it's the best: A* will immediately calculate the exact sequence of rooms to walk through, ensuring the monster never gets stuck in a massive dead-end on the other side of the map. Modern Doom AI mods (like ZScript-based pathfinders) use exactly this.

## 2. The Engine-Native Approach: Chaining "Patrol Points"

If you want to keep the programming incredibly lightweight and closer to how 90s engines worked (similar to Quake or Half-Life), you use Dijkstra's Algorithm or BFS paired with Node Networks (Waypoint Graphs).

Instead of dynamically analyzing the architecture, map designers manually place invisible target markers (Waypoints/NavPoints) at every doorway, intersection, and corner of the map.

- You connect these points in a simple graph.
- You run Dijkstra's algorithm to calculate the shortest path of waypoints from Room A to Room B.
- The monster's AI is told to simply A_Chase the first waypoint. Once it touches it, its target switches to the second waypoint, and so on, until it reaches the final room.

## 3. Exploiting Doom's BSP Tree (The Clever Hack)

Doom maps are structurally organized using a Binary Space Partitioning (BSP) Tree to render walls efficiently. The engine splits the map into a hierarchy of sub-sectors.

Technically, you could write a pathfinding algorithm that traverses the map's existing BSP tree to find adjacent sub-sectors from point A to point B.

**The Catch:** While it requires zero extra setup or waypoints, BSP trees are optimized for rendering visibility, not physical walking. A path found purely via the BSP tree might think two rooms are connected because they share a partition line, even if there is a massive impassable window or a solid half-wall between them.

## Summary

- If you want a robust, modern AI bot that handles complex maps perfectly: Use A* on a Sector-based NavMesh.
- If you are coding within strict retro limitations: Manually place Waypoints and use Dijkstra's Algorithm to let monsters jump from door to door.

---

## Doom uses BSP trees — is that of any help?

Yes, absolutely! You hit on the most fascinating piece of Doom's architecture. Doom's BSP (Binary Space Partitioning) tree is a massive help, but maybe not in the way you'd think.

While you can use it for pathfinding, it requires a clever workaround because BSP trees were designed for eyesight (rendering), not footsteps (walking).

Here is how the BSP tree helps, how it hurts, and how you actually use it to find a path.

### The Help: Instant Location Tracking

In a massive map, how does the game engine instantly know which room Room A or Room B is in?

Without a BSP tree, the engine would have to check the coordinates of the player against every single polygon on the map. That would crash a 1993 PC.

Because Doom splits the map into a binary tree, the engine can do a lightning-fast search (taking O(log n) time) to find exactly which Sub-sector (the smallest leaf nodes of the BSP tree) points A and B reside in.

### The Problem: Walls Look Like Doors to a BSP

The BSP tree splits the map using invisible lines called dividers. It doesn't care if a line is a doorway, an open field, or a 10-foot-thick solid concrete wall. It just cares about dividing the space for the renderer.

If you tried to do a standard pathfinding algorithm (like A*) directly on the raw BSP leaves, the algorithm might see that Sub-sector 4 is right next to Sub-sector 5 in the tree, decide to walk straight from 4 to 5 — but in reality there is a solid, impassable wall between them.

The BSP tree alone tells you what spaces exist, but it doesn't inherently tell you if a monster can walk between them.

### How to Actually Use the BSP for Pathfinding

To use the BSP tree to get from Room A to Room B, you have to look at the Linedefs (the walls/lines) that make up the edges of those BSP sub-sectors. This is how modern source ports and advanced bots do it:

1. **Find the Leaves.** You pass the coordinates of Room A and Room B down the BSP tree to find their respective starting and ending sub-sectors.
2. **Build an Adjacency Graph via "Two-Sided Lines".** In Doom, lines that separate two walkable areas (like a doorway or an invisible border between two rooms) are marked as Two-Sided Linedefs. Lines that are solid walls are One-Sided Linedefs. You can filter the BSP data to build a navigation graph:
   - **Nodes:** The walkable Sub-sectors found in the BSP tree.
   - **Edges (Connections):** Only the Two-Sided Linedefs where the ceiling height is high enough and the floor drop is low enough for a monster to step over.
3. **Run A* on the Resulting Graph.** Now that you've used the BSP tree to identify the rooms and used the Linedef data to filter out the solid walls, you have a perfect, lightweight graph. You can run A* or Dijkstra's across these sub-sectors to find the exact path from A to B.

## Summary

The BSP tree is the perfect index. It instantly tells your algorithm where Room A and Room B are in the data structure. You just have to cross-reference the tree with Doom's wall data (Linedefs) to make sure your pathfinding algorithm doesn't try to walk through a solid wall!

---

## How about Dijkstra's and BSP?

Combining Dijkstra's Algorithm with Doom's BSP tree is actually the exact foundation used by some of the most famous automated Doom bots in history (like the legendary RealBot or CajunBot from the 90s/2000s, and modern source-port AI).

When you combine them, the BSP tree acts as the Map Generator, and Dijkstra acts as the Navigator.

Here is exactly how Dijkstra's algorithm operates on a Doom BSP tree to find the perfect path from Room A to Room B.

### Step 1: Turning the BSP Tree into a Dijkstra Graph

Dijkstra's algorithm requires a network of points (nodes) connected by paths (edges), where each path has a "weight" (the physical distance between them).

You can extract this graph directly from Doom's engine data structures:

- **The Nodes:** Each walkable Sub-sector (the individual floor polygons split up by the BSP tree). To make it easy for Dijkstra, you calculate the center point (centroid) of each sub-sector.
- **The Edges:** Any Two-Sided Linedef (a boundary line between two sub-sectors that isn't a solid wall).
- **The Weights:** The actual physical distance (in Doom map units) from the center of Sub-sector 1 to the center of Sub-sector 2.

### Step 2: Running Dijkstra on the BSP Data

Once the map is loaded and the BSP sub-sectors are linked into a graph, Dijkstra takes over to find the absolute shortest path from Room A to Room B:

1. **Locate Start and End.** The engine passes the coordinates of Room A and Room B down the BSP tree. Within microseconds, the tree tells you: "Room A is in Sub-sector #42, Room B is in Sub-sector #501."
2. **Initialize Dijkstra.** Dijkstra sets the "cost" to reach Sub-sector #42 to 0, and the cost to reach every other sub-sector on the map to infinity (∞).
3. **Flood Fill Exploration.** Dijkstra looks at Sub-sector #42 and examines all adjacent sub-sectors connected by two-sided lines. It calculates the distance to them and updates their cost.
4. **Prioritize the Shortest Path.** It continuously expands outward, always choosing the next closest sub-sector.
5. **Stop at the Destination.** The moment Dijkstra reaches Sub-sector #501 (Room B), it stops. By tracing its steps backward, it gives you a perfect list of sub-sectors to walk through.

### The Pro: Handling Doors, Lifts, and Teleporters

The absolute best reason to use Dijkstra over standard A* in Doom is how Dijkstra handles dynamic map elements like doors, lifts, and teleporters.

In Doom, the BSP data structure includes sector heights. If a door is closed, the ceiling height of that sector drops to the floor height.

Because Dijkstra calculates the true "cost" of a path, you can dynamically alter the edge weights in real-time based on the map state:

- **Open Door:** Weight = 50 units (normal walking distance).
- **Closed Door:** Weight = 50 units + a 200-unit "penalty" (because the monster has to stop and wait for it to open).
- **Acid/Lava Floor:** Weight = 1000-unit penalty (the bot can walk through it if it's the only way, but will actively try to find a dry route).
- **Teleporter:** Weight = 1 unit (instantly teleports across the map). Dijkstra handles teleporters beautifully because it doesn't care about physical direction; it just sees two nodes connected by a very "cheap" edge.

### The Con: The "Zig-Zag" Problem

While Dijkstra on a BSP tree is mathematically perfect, it can look a bit silly in-game.

Because BSP trees split rooms into arbitrary triangles and polygons for rendering, the center points of these sub-sectors might not form a straight line. If a monster blindly walks from the exact center of one sub-sector to the next, it might zig-zag rhythmically down a perfectly straight hallway.

To fix this, developers use a post-processing trick called **String Pulling** (or the Funnel Algorithm). Once Dijkstra finds the list of BSP sub-sectors, a smoothing algorithm "tightens" the path into a clean, natural straight line from Room A to Room B.

---

## Bot / AI Companion Pathfinding Problems in Classic Doom

> A bot or AI companion runs into the wall instead of finding the transition to the neighboring room, because it wants to take the shortest path to the player.

The problem you describe hits the nail on the head. Classic Doom's original AI is extremely simple and has no real pathfinding in the modern sense — no A* algorithm, no graph search.

If you try to write a bot or AI companion for a game like this that just blindly runs at the player, it fails at every L-shaped wall.

Here is the explanation of why the original Doom AI acts so "dumb", how it perceives rooms, and how the problem needs to be solved.

### 1. Why the Doom AI runs blindly into walls

The Classic Doom engine was optimized for monsters that attack the player, not for companions that intelligently follow them through a labyrinth.

- **The Line-of-Sight focus:** Monsters in Doom only move when they have a direct line of sight to the player, or when they are alarmed by noise.
- **Pure vector running:** As soon as a monster is chasing the player, it tries in the vast majority of cases to move along a direct, linear path (like a vector) toward the player's coordinates.
- **The "reaction collision":** When a monster hits a wall, it only notices after the collision has already happened (via the blockmap mentioned earlier). The code then reacts in a simple way: the monster changes direction aimlessly (often at a 90° or 45° angle), walks a bit, and then immediately tries to head straight for the player again along the vector.

For an enemy charging at you in open rooms, that is perfectly sufficient. For a bot companion that has to follow you from Room A through a door into Room B, it is fatal: it sees your coordinates behind the wall, walks straight into the wall, and stays stuck in that loop.

### 2. The problem with "sectors" and transitions

In Doom the world is divided into sectors (rooms, corridors, doors). A sector theoretically knows which sectors are adjacent to it. In the original game, however, the AI barely uses this information for movement at all.

Two decisive things are missing from the original engine for smart pathfinding:

- **No room graph:** The AI does not understand: "To get into Room B, I have to go through Door C first." It only knows its own position and the player's.
- **No look-ahead path planning:** The bot does not "see" the wall in advance on a map — it only "feels" it when it bumps into it.

### 3. How to fix the problem (a modern bot routing system)

When modders or source port developers (such as ZDoom, or specialized bot mods like Cajun Bot) want to ship a working companion, they have to bolt a new system onto the engine. There are two classical approaches.

#### Approach A: The Waypoint system (classic)

The developer (or a background algorithm) lays an invisible net of nodes (waypoints) over the map.

- Every door, every corner, and every room transition gets a waypoint.
- The waypoints are linked to each other (a graph).
- **Bot flow:** the bot does not search for the direct path to the player, but for the shortest path along the waypoint net. It runs to waypoint 1 (door), then waypoint 2 (corridor), and only then to the player.

#### Approach B: Navigation Meshes (NavMeshes)

This is the more modern variant. The walkable sectors of the Doom level are decomposed into simple polygons (mostly triangles or quads).

- The bot then knows exactly: "I am on polygon 5. The player is on polygon 12. To get there I have to cross polygons 6, 7, and 8."
- The bot then deliberately steers for the edge transitions (doors / passages) between those polygons, instead of blindly picking the straight-line distance.

### The "Quick & Dirty" fix in the Doom code

If you want to change the original system as little as possible, developers often use a trick: when the bot has not seen the player directly for X seconds (line of sight broken) and its own coordinates stop changing (it is stuck), a fallback logic kicks in. The bot "remembers" the last visible coordinates of the player (often the doorframe) and walks there first, instead of heading straight for the player's current position.

---

## Navigation Meshes look a lot like BSP — why can't I just use BSP for Approach B without the downsides?

That is a damn sharp question! You are absolutely right: both a BSP tree and a navigation mesh decompose the level into polygonal, walkable areas. Visually, a 2D slice through a BSP tree often looks strikingly similar to a NavMesh.

You can theoretically use a BSP tree for pathfinding (games like Quake 3 actually did something similar for their bots via "AAS" technology), but in practice it has massive, fundamental disadvantages compared to a real NavMesh.

The main reasons why BSP is unsuitable for pathfinding without adaptation can be boiled down to three core problems.

### 1. Topology: the BSP tree lacks "neighborhood" (connectivity)

To feed a pathfinding algorithm like A* (A-Star), you need a graph. The algorithm has to be able to ask a cell: "Who are your direct neighbors, and how big are the doors to them?"

The NavMesh is built exactly for that. It is a network of connected polygons. Every polygon knows exactly which other polygons touch its edges.

The BSP tree, on the other hand, is a hierarchical nesting (a family tree). A leaf in the BSP tree knows who its "parent" or "child" in the tree is, but it has no idea who its spatial neighbor is.

*Metaphor:* The NavMesh is like a street map (you can see which streets are connected). The BSP tree is like a postal code system (it keeps splitting the country into halves and quarters, but doesn't directly tell you whether you can walk from postal code A to postal code B without crossing a river).

To navigate from one room to the next inside a BSP tree, the AI would have to do extremely expensive geometric tests to figure out which walls of the leaves are actually adjacent. That eats performance instantly.

### 2. Geometry hell: unnecessary fragments (sliver polygons)

A BSP tree is generated by chopping hyperplanes across the entire level to sort the geometry.

If a wall sits at an angle in the room, the BSP compiler often mentally extends that line through half the level. That causes otherwise simple, flat rooms to be chopped up inside the BSP tree into countless tiny, elongated triangles (slivers).

For the renderer this is fine — the GPU doesn't care about the shape of the triangles.

For pathfinding this is a disaster. The A* algorithm would have to compute its way through hundreds of tiny micro-cells, even though the bot is just walking straight through an empty room. A NavMesh, on the other hand, fuses that empty room into a single, large, clean polygon.

### 3. Dimension and size conflicts (the bot's "body")

A BSP tree partitions space regardless of who is moving inside it. A NavMesh, on the other hand, is baked specifically for the agent's size.

A fat Cyberdemon needs a different NavMesh than a small Imp, because the Cyberdemon cannot fit through narrow doors. With a NavMesh, the geometry around the monster's radius is "shrunk" during baking. The bot can then rely on: "As long as my center stays on this NavMesh, I won't get stuck anywhere."

A BSP tree is rigid. It only knows where the real wall is. If you use it for pathfinding, the bot would have to mathematically check at every step whether its shoulder or hitbox is scraping the wall on the left or right (which brings us right back to the problem of bumping into edges).

## Summary

| Feature | BSP Tree | Navigation Mesh |
| --- | --- | --- |
| Main purpose | Visibility checks & render sorting | Path planning & routing |
| Structure | Hierarchical tree (A splits into B and C) | Connected graph (cell 1 is adjacent to cell 2) |
| Polygon shape | Often fragmented, many unnecessary slivers | Optimized, as large an area as possible |
| Size awareness | Ignores the bot's thickness | Baked to fit the bot's size |

You use the NavMesh because it is an abstraction of the world optimized for one single thing: making life as easy and computationally cheap as possible for the bot — while the BSP tree is a slave to the visual geometry.