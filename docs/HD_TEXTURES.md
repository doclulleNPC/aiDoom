# HD / true-color texture support — how the `../sdldoom-sdl3` sibling does it

A porting reference. The sibling SDL3 port (`../sdldoom-sdl3`) has a working
high-definition, true-color **texture / sprite / voxel replacement** system that
aiDoom does **not** (yet) have. This documents how it works so it can be ported.

> **Don't confuse the two "hires".** aiDoom already has `hires` — the *variable
> internal resolution* software renderer (draws the original 8-bit textures at a
> higher screen resolution). That is unrelated to what's below. The `hd_*` system
> is **true-color PNG replacement art**, a separate subsystem layered on top.

Relevant sibling files: `hd_texture.c/.h`, `hd_sprite.c/.h`, `hd_voxel.c`, plus
hooks in `r_data.c`, `r_draw.c`, `r_segs.c`, `r_plane.c`, `r_things.c`, `i_video.c`.

## TL;DR — the approach

A **hybrid parallel 32-bit path**, *not* a full true-color renderer and *not*
palette quantization. The vanilla 8-bit software renderer runs unchanged; when
true-color is armed, the column/span drawers **dual-write** — the normal 8-bit
pixel into `screens[0]` **and** a matching ARGB pixel into a parallel `screen32`
buffer. At present time `i_video.c` composites the two. HD is bolted on; the engine
is never converted to true-color.

This is the *lighter middle path* between the two truecolor tiers documented
elsewhere: instead of GZDoom's full dual-texture true-color renderer (which keeps a
32-bit copy of every texture and runs all lighting in RGB), it keeps the 8-bit
pipeline and only spends 32-bit work on the surfaces that actually have an HD
replacement.

## 1. Asset pipeline

- **Format: PNG**, decoded on demand with **stb_image** (`STB_IMAGE_IMPLEMENTATION`
  + `STBI_ONLY_PNG` in `hd_sprite.c`; `hd_texture.c` includes the header). Same
  single-header style aiDoom already uses for `stb_vorbis`.
- **Stored as ARGB8888** in `hdimage_t { int w, h; unsigned* rgba; }` (`hd_sprite.h`),
  packed straight from stb's RGBA bytes:
  ```c
  e->img.rgba[n] = ((unsigned)s[3]<<24) | ((unsigned)s[0]<<16)
                 | ((unsigned)s[1]<<8) | (unsigned)s[2];   // hd_texture.c
  ```
- **Side WADs, read directly (not through `w_wad`)** so PNG lumps never collide
  with the IWAD's 8-bit lumps of the same name:
  - `hdtextures.wad` — walls + flats. Kept open and `fseek`/`fread` per lump on
    demand (the file can be ~200 MB; it is *not* slurped).
  - `hdsprites.wad` — sprite frames (slurped fully into memory).
  - `voxels.wad` — raw KVX models + a `VOXELDEF` text manifest.
- **Matching to the vanilla asset: by 8-char lump name**, upper-cased / NUL-padded,
  linear-searched against the side-WAD directory (`HD_ResolveByName`), then the
  resolution is **cached per engine texture/flat index** (`texent[]`/`flatent[]`).
  Flats use the IWAD's own lump name (`lumpinfo[firstflat+flatnum].name`).
- **Memory management:** oversized PNGs are box-downscaled once to `<= HD_MAXDIM`
  (512); a ~256 MB decoded budget with **LRU eviction** (`HD_Evict`); **all caches
  freed at level load** (`HD_LevelReset` / `HD_SpriteReset` / `HD_VoxelReset`, called
  from `p_setup.c`). No mipmaps — single scale.

## 2. Rendering integration — the crux

`R_HDSetupWall(texnum, texcol)` / `R_HDSetupFlat(flatnum)` (in `r_data.c`, where
`texture_t` is private) bail unless `truecolor && mod_hdtextures`, look up the HD
image, and fill the `dc_hd*` / `ds_hd*` globals (HD width/height, the precomputed
HD column `dc_hu = c * hd->w / texW`, the original texture height/mask).

The 8-bit inner loops in `r_draw.c` then **branch on `truecolor && dc_hdsrc`** (walls)
/ `ds_hdsrc` (flats). HD wall column inner loop:

```c
int      tv = (frac>>FRACBITS) & dc_texhmask;          // original texture row
int      hv = (int)((long long)tv * dc_hdh / dc_texheight);  // -> HD row
unsigned px = dc_hdsrc[hv*dc_hdw + dc_hu];             // HD ARGB texel
unsigned r=(px>>16)&0xff, g=(px>>8)&0xff, b=px&0xff;
if (dim < 0.999) { r*=dim; g*=dim; b*=dim; }           // lighting (see below)
*dest  = dc_colormap[dc_source[(frac>>FRACBITS)&127]]; // 8-bit pixel (composite)
*dst32 = 0xff000000u | (r<<16) | (g<<8) | b;           // 32-bit pixel
```

Flats (`R_DrawSpan`) are the same shape, sub-sampling the HD flat over the 64-unit
tile.

### The key trick: cheap true-color lighting

Lighting on a true-color texel is **NOT a colormap lookup** — it's a precomputed
per-light-level **brightness scalar**. The drawer recovers the colormap *row*
(0..31 = distance/sector light) from the colormap pointer and indexes `fc_lightdim[]`:

```c
int row = (int)((dc_colormap - colormaps) >> 8);
if (row >= 0 && row < 32) dim = fc_lightdim[row];
```

`fc_lightdim[row]` is derived **once** (`i_video.c`) from the 8-bit colormap as the
average luminance ratio of mapped-vs-base palette across all 256 entries:

```c
fc_lightdim[row] = den>0 ? num/den : 1.0;   // num=Σ lum(mapped), den=Σ lum(base)
```

So HD shading = original-palette texel × (smooth approximation of that colormap
row's dimming). Special colormaps (invuln/light-amp, rows ≥ 32) are not approximated
— they fall back to palette-snapped color. **This `fc_lightdim` luminance-ratio table
is the single piece that makes true-color lighting cheap** (one float multiply per
channel, no per-texel colormap math), and it's the part most worth copying.

## 3. Hooks into the vanilla renderer

| Surface | Hook | Notes |
|---|---|---|
| Walls | `r_segs.c` `R_RenderSegLoop` calls `R_HDSetupWall(...)` before each tier's `colfunc()`, then **clears `dc_hdsrc = 0` immediately after** | the clear-after means non-replaced columns / special colfuncs (fuzz) fall through to vanilla safely |
| Flats | `r_plane.c` `R_DrawPlanes` calls `R_HDSetupFlat(flattranslation[pl->picnum])` before a plane's spans | sky uses `colfunc()` directly, no HD setup |
| Sprites | `r_things.c` `R_DrawVisSprite` tries voxel → HD sprite → 8-bit | see §5 |

There is **no per-column HD flag** — the drawers just branch on `dc_hdsrc`/`ds_hdsrc`
being non-NULL, which only happens when setup succeeded. Global enables: `truecolor`
(armed per-frame at the top of `R_RenderPlayerView` from `mod_fullcolor`) plus the
persistent `mod_hdtextures` / `mod_hdsprites` / `mod_voxels` toggles (`doomstat.h`,
menu in `m_menu.c`).

## 4. Framebuffer / present

Both buffers always exist: 8-bit `screens[0]` (always written) and
`screen32 = malloc(MAXWIDTH*MAXHEIGHT*4)` (the parallel ARGB view buffer). HD does
**not** switch to a pure true-color renderer — it arms the dual-write.

`I_FinishUpdate` fills the SDL streaming texture by expanding `screens[0]` through
`palette[]`, **except** inside the 3D-view rect where, if a true-color view was
captured this frame, it prefers `screen32` — but only where the current 8-bit pixel
still equals a snapshot taken right after the 3D pass:

```c
if (x >= vx0 && x < vx1 && src[x] == snap[x]) dst[x] = s32[x];      // HD pixel
else                                          dst[x] = palette[src[x]]; // palette
```

The snapshot (`view8snap`, captured before HUD/menu draw) means any 2D overdraw
(status bar, menus, messages) reverts that pixel to palette expansion automatically.
`view_truecolor` is one-shot per frame. The final SDL texture is always 32-bit;
"HD on" only changes which pixels are HD-sourced vs palette-expanded.

## 5. Sprites & voxels

- **HD sprites (`hd_sprite.c`)** — straight 2D PNG replacement. `R_BlitHDSprite`
  rasterizes the ARGB frame into `screen32` at the vissprite's exact screen
  footprint, scaling source U/V to `hd->w/hd->h`, clipping per column with
  `mfloorclip`/`mceilingclip`, applying the same `fc_lightdim` dim, and
  **alpha-blending** (alpha 0 skip, 255 opaque, else `(r*a + dr*na)/255`). Writes
  only `screen32`.
- **Voxels (`hd_voxel.c`)** — actual **KVX 3D models**, not 2D sprites. `voxels.wad`
  carries raw KVX lumps + a `VOXELDEF` manifest binding sprite-frame keys → KVX lump
  + yaw fixup + scale. `VOX_Decode` reads mip-0 surface voxels and converts the KVX
  6-bit palette to opaque ARGB. `HD_DrawVoxel` projects each surface voxel to a
  screen-aligned square splat from the vissprite's world pos/angle/scale,
  **depth-sorts back-to-front** (painter's algorithm), dims by `fc_lightdim`, paints
  into `screen32` clipped per column. Takes precedence over HD/8-bit sprites.

## Porting notes for aiDoom

aiDoom already has the prerequisites that make this tractable:

- An SDL3 streaming texture present path (`i_video.c`) — extend it with the
  `screen32` + snapshot composite.
- `MAXWIDTH*MAXHEIGHT` static sizing for renderer tables.
- The `stb_*` single-header vendoring pattern (add `stb_image.h`).

What to port, roughly in order:
1. `screen32` buffer + the `I_FinishUpdate` composite (snapshot vs HD pixel).
2. `fc_lightdim[32]` table built once from `colormaps`/`palette` (the cheap-lighting
   trick).
3. `hd_texture.c` loader (side `hdtextures.wad`, name match, ARGB decode, LRU).
4. `R_HDSetupWall/Flat` + the `dc_hd*`/`ds_hd*` branch in `R_DrawColumn`/`R_DrawSpan`,
   and the `r_segs.c`/`r_plane.c` setup+clear hooks.
5. (optional) `hd_sprite.c` then `hd_voxel.c`.
6. `mod_fullcolor`/`mod_hdtextures`/... config flags + Video-menu toggles.

Caveats inherited from the design: special colormaps (invuln) aren't true-color;
2D overdraw is palette-expanded by construction; HD is gated behind `truecolor`
(arm per frame); memory is bounded by LRU + per-level reset (HD art is large).
