# HD / true-color texture support — porting reference

> **Not implemented in BuddyDoom.** This document describes how the sibling `../sdldoom-sdl3` port approaches true-color textures, sprites and voxels. BuddyDoom currently uses its palette/software renderer and has no `screen32`, `hd_*` loader or `R_HDSetupWall` implementation.

## TL;DR

The sibling port keeps the classic indexed renderer for normal work and adds a parallel true-color path for replacement art:

1. load PNG/true-color assets into a cache;
2. render true-color walls/sprites/voxels into a 32-bit buffer alongside the indexed path;
3. apply lighting approximately at draw time;
4. composite/present through SDL.

This is a porting reference, not a feature list for this repository.

## 1. Asset pipeline in the sibling

The sibling's asset pipeline resolves replacement texture/sprite/voxel names, decodes image data and maintains per-level/cache lifetimes. It must handle missing replacements, dimensions, transparency, animation and WAD/PWAD precedence.

Before porting any part here, define the BuddyDoom-side naming policy and memory budget. Classic WAD patch names are eight bytes; PNG replacements need a separate lookup convention.

## 2. Rendering integration

The difficult part is not PNG decoding. The difficult part is making every renderer primitive agree on color, lighting, transparency and clipping.

The sibling uses a parallel ARGB/true-color write path and an approximate light-dimming calculation (`fc_lightdim` in the reference implementation). A future BuddyDoom port would need equivalent hooks in:

- wall columns (`files/r_segs.c`, `files/r_draw.c`);
- planes (`files/r_plane.c`);
- sprites/masked columns (`files/r_things.c`);
- framebuffer/present (`files/i_video.c`).

The current `dc_texheight` code in BuddyDoom is tall-column sampling support, not HD texture support.

## 3. What a future BuddyDoom port would own

A minimal design would likely add:

- a true-color framebuffer sized to the current internal resolution;
- PNG decode/cache code (the repository already contains image helper code, but no HD replacement subsystem);
- texture replacement lookup and per-level invalidation;
- wall/plane/sprite true-color draw hooks;
- palette/colormap-to-light mapping;
- SDL texture upload/composite path;
- config/menu toggles and a bounded memory policy.

Do not add the names from the sibling (`screen32`, `dc_hd*`, `R_HDSetupWall`, `hd_texture.c`, `hd_sprite.c`, `hd_voxel.c`) to BuddyDoom documentation until those files and symbols exist here.

## 4. Lighting and special effects

True-color lighting is an approximation unless the complete palette/colormap semantics are reproduced. In particular:

- invulnerability/special colormaps need explicit handling;
- translucent overdraw must define source/destination blending;
- 2D HUD assets remain palette-authored unless a parallel UI path is added;
- skies, flats and sprites may need different sampling/clipping rules.

## 5. Sprites and voxels

The sibling's voxel/replacement approach is optional future work. BuddyDoom's current `r_things.c` still renders Doom sprite patches through the indexed renderer. A true-color sprite path must preserve vertical clipping, fuzz/invisibility behavior, tall patches and actor scaling.

## 6. Recommended porting order

1. Add a read-only asset probe/cache with no gameplay changes.
2. Add a 32-bit framebuffer and present it for a single test surface.
3. Port wall columns.
4. Port planes and masked/sprite columns.
5. Port HUD/2D and special colormaps.
6. Add replacements, voxels, menu/config and memory eviction only after the renderer path is stable.

Each step needs a real game run at multiple internal resolutions; a successful compile is not enough for renderer work.

## Source boundaries

- Current BuddyDoom indexed renderer: `files/r_draw.c`, `files/r_segs.c`, `files/r_plane.c`, `files/r_things.c`, `files/i_video.c`.
- Current PNG/image helpers: `files/v_png.c`, `files/stb_image.h`.
- Reference implementation: sibling project `../sdldoom-sdl3`.
