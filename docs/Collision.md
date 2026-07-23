# Doom collision: background reading

This is a short background note about the collision architecture inherited from classic Doom. It is not a complete description of every aiDoom collision extension; for implementation details, read `files/p_map.c`, `files/p_maputl.c`, `files/p_sight.c` and the relevant docs under `docs/`.

## 1. The two spatial systems in classic Doom

### Blockmap

The blockmap divides the map into fixed-size square cells. Each cell stores references to lines and things that overlap it. During movement or hitscan, Doom visits the cells crossed by the query and tests only the nearby candidates.

In aiDoom, the blockmap remains the main broad-phase collision/index structure. The code walks blockmap lines/things through the `PIT_*` and `P_BlockThingsIterator` paths in `files/p_map.c` and `files/p_maputl.c`.

### BSP tree

The BSP tree partitions the map into subsectors for rendering and spatial lookup. It is excellent for deciding which sectors/segments are visible and for locating the subsector containing an actor. It is not, by itself, a full swept-volume collision solver.

aiDoom also uses the BSP subsector graph for buddy/agent navigation (`files/p_ai_coop.c`), but that is path planning, not a replacement for physical collision.

## 2. Why Doom used the blockmap

The blockmap was cheap on 1993 hardware and easy to use for both line and thing queries. A moving actor usually needs to test a small neighborhood, not the entire map. Fixed-point arithmetic and the 35 Hz tic simulation make the result deterministic.

The price is that the blockmap is a coarse spatial index. The final collision checks still have to handle actor radii, line openings, step height, floors/ceilings and the special rules of Doom's movement model.

## 3. What a Quake-style approach changes

Later engines made more extensive use of BSP/clip-node or similar convex-volume queries for collision. That can give cleaner geometric separation, but it requires a different map representation and a substantially different movement solver. It is not a drop-in optimization for classic Doom.

Therefore the useful engineering conclusion is not "replace Doom's blockmap with a BSP tree". It is:

- keep the blockmap broad phase for compatibility;
- use BSP/subsector data where it is a good index (rendering, navigation, location);
- improve specific collision predicates only when the gameplay contract requires it.

## 4. aiDoom-specific caveats

The current port adds over/under actor movement by default and keeps a compatibility flag, `-infinitetall`, for vanilla infinitely-tall actor collision. `PIT_CheckThing` in `files/p_map.c` is the relevant path.

Other collision-related features include checked monster spawning (`P_SpawnMonsterChecked` in `files/p_mobj.c`), projectile/thing blocking, and the buddy's pathfinder/door handling. These are separate layers and should not be conflated with the original blockmap design.
