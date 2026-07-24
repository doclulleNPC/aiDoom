# UDMF Support — Implementation Plan

**Status:** proposal / not yet implemented.
**Spec:** `docs/udmf11.txt` (UDMF v1.1, Quasar 2009).
**Scope of this plan:** add read support for **UDMF maps in the `Doom` namespace**
to BuddyDoom's software renderer, reusing the existing binary-map runtime as much as
possible. Hexen/ZDoom/Strife namespaces (thing specials, `args[5]`, SPAC flags,
scripting) are called out as follow-on work but are **out of scope** for phase 1.

---

## 0. Current state (audit summary)

BuddyDoom has **no UDMF code at all**. `docs/udmf11.txt` is the only trace.

- `P_SetupLevel` (`files/p_setup.c:988`) is hard-wired to the 1993 binary format:
  it does `lumpnum = W_GetNumForName(label)` then reads geometry from **fixed lump
  offsets** (`lumpnum+ML_VERTEXES`, `+ML_SECTORS`, …) defined by the `ML_*` enum in
  `files/doomdata.h:43`.
- The only modern extension is **extended/compressed BSP nodes**:
  `P_LoadNodes_Extended` (`files/p_setup.c:287`) handles `XNOD` and zlib `ZNOD`
  in the `ML_NODES` lump. **GL node variants (`XGL*`/`ZGL*`) are explicitly not
  handled** — this matters a lot for UDMF (see §4).
- No Hexen binary format either (`mapthing_t` is the 10-byte vanilla struct;
  `line_t`/`sector_t` carry a single `special`+`tag`, no `id`, no `args[5]`).

A UDMF map fed to the current loader would not be detected: `label+ML_VERTEXES`
would index whatever lump sits 4 entries after the label (garbage or `ENDMAP`),
read as raw binary vertices → corrupt level or crash. **There is no format guard.**

So this is a **greenfield feature**, not a fix.

---

## 1. Architecture: where the new code goes

New files:

- **`files/p_udmf.c` / `files/p_udmf.h`** — the tokenizer + parser + a
  `P_LoadUDMF(int lumpnum)` entry point that populates the same global arrays the
  binary loaders populate (`vertexes/numvertexes`, `sectors/numsectors`,
  `sides/numsides`, `lines/numlines`) plus a things array.

Design principle: **the parser is the only new thing.** It fills the existing
runtime structs (`vertex_t`, `sector_t`, `side_t`, `line_t` in `r_defs.h`) and a
`mapthing_t[]` that we then feed to the existing `P_SpawnMapThing`. Everything
downstream (BSP render, collision, specials, savegames) stays untouched for the
Doom namespace. This is the smallest possible footprint.

### Dispatch in `P_SetupLevel`

Per spec §II.B, a UDMF map is `(HEADER) TEXTMAP … ENDMAP`. Detect it by testing the
lump **immediately after the label**:

```c
lumpnum = W_GetNumForName (lumpname);
leveltime = 0;

if (!strncasecmp (lumpinfo[lumpnum+1].name, "TEXTMAP", 8))   // UDMF path
{
    P_LoadUDMF (lumpnum + 1);          // parses TEXTMAP; fills verts/sectors/sides/lines/things
    P_LoadUDMFNodes (lumpnum);         // find ZNODES/XNOD lump between TEXTMAP and ENDMAP (§4)
    P_LoadBlockMap (-1);               // -1 => force the built-in generator (see §5)
    P_LoadReject   (-1);               // synthesize an all-visible reject (see §5)
}
else                                   // existing binary path, unchanged
{
    P_LoadVertexes (lumpnum+ML_VERTEXES);
    ...
}
```

Note `W_CheckNumForName`/`lumpinfo[]` name compare must be the 8-byte,
case-insensitive, NUL-tolerant compare (UDMF §I says identifiers/keywords are
case-insensitive; lump names follow the 8-byte rule in CLAUDE.md). `TEXTMAP` is
7 chars, so it is stored padded with one NUL — a plain `strncasecmp(...,8)` works.

---

## 2. The lexer / parser

The grammar (spec §I) is tiny — implement it directly, no generator:

```
translation_unit := global_expr*
global_expr      := block | assignment
block            := identifier '{' assignment* '}'
assignment       := identifier '=' value ';'
value            := int | float | quoted_string | keyword(true|false)
```

### Lexer rules to get right (each is a real compliance trap)

- **Comments:** `//` to EOL and `/* … */` (non-nestable). Strip before/while tokenizing.
- **Whitespace is insignificant** (§I).
- **Case-insensitive** identifiers and keywords (§I) — lowercase-fold field names
  and the `true`/`false` keywords on read.
- **Integers:** decimal, **octal `0NNN`**, and **hex `0xNN`** (spec grammar line 64).
  Use `strtol(tok, NULL, 0)` which handles all three.
- **Floats:** `strtod`. UDMF numbers are **double precision** (§II.A). We only use
  them at load time (see §3 determinism note).
- **Strings:** `"…"` with `\` escapes (`\"`, `\\`). Texture names are the main use;
  cap to 8 chars on store.
- **Unknown fields/blocks must be silently ignored**, not errored (§I lines 97–100).
  This is mandatory for forward-compat — parse the `identifier = value;` and drop it
  if we don't recognize it, and skip whole unknown blocks by brace-matching.

Suggested parser shape: a single forward pass over the lump text producing, per
block, a small `{ char key[64]; enum{INT,FLT,STR,BOOL} type; ... } field[]`
scratch list, then a `commit_<blocktype>()` that reads named fields with defaults.
Keep all scratch in a `PU_STATIC` zone buffer freed at the end of `P_LoadUDMF`
(no `malloc` — CLAUDE.md rule).

### Two-pass vs one-pass

Blocks are stored top-to-bottom and indexed by order (§II.A): the Nth `vertex`
block is vertex index N, etc. `linedef.v1/v2` and `sidedef.sector` are **indices**
into those arrays. So:

1. **Pass 1** — count blocks of each type (so we can `Z_Malloc` the arrays at the
   right size), OR append to growable temp arrays and copy into `PU_LEVEL` at the
   end. Counting-first is cleaner and matches how `P_Load*` size their allocs
   (`W_LumpLength / sizeof(record)`).
2. **Pass 2** — fill `vertexes[]`, then `sectors[]`, then `sides[]`, then `lines[]`
   (lines reference sides+vertices+sectors, so those must exist first — same
   ordering constraint the binary path documents at `p_setup.c:992` "most of this
   ordering is important"), then collect `things[]`.

---

## 3. Field → struct mapping (Doom namespace)

### `namespace` (global assignment, §II.C)

Read it. For phase 1 accept `Doom` (and treat `Heretic`, `Boom`, `MBF`, `ZDoom`
loosely as Doom-compatible for geometry). For unknown namespaces: **load anyway**
but `printf` a warning — the spec says a port *may* ignore namespace. Do **not**
hard-fail (we still render geometry fine); just don't promise special semantics.

### vertex → `vertex_t` (`r_defs.h:73`)

```c
li->x = (fixed_t)(x_double * FRACUNIT);   // round-to-fixed at load
li->y = (fixed_t)(y_double * FRACUNIT);
```

**Determinism note (CLAUDE.md):** floats are fine *here* — this is load-time
conversion, not playsim. The stored value is `fixed_t`; the playsim never sees the
double again. Use a consistent rounding (`lround`) so the same map always yields
the same fixed coords across platforms.

### sector → `sector_t` (`r_defs.h:103`) — mirror `P_LoadSectors` (`p_setup.c:235`)

| UDMF field | struct | note |
|---|---|---|
| `heightfloor` (def 0) | `floorheight = v<<FRACBITS` | |
| `heightceiling` (def 0) | `ceilingheight = v<<FRACBITS` | |
| `texturefloor` | `floorpic = R_FlatNumForName(name)` | no valid default → required |
| `textureceiling` | `ceilingpic = R_FlatNumForName(name)` | |
| `lightlevel` (**def 160**) | `lightlevel` | UDMF default is 160, not 0 |
| `special` (def 0) | `special` | |
| `id` (def 0) | `tag` | Doom-namespace sector tag = `id` |

Also replicate the Boom init the binary path does (`heightsec=-1`,
`floorlightsec=ceilinglightsec=-1`, `sky=0`) — `p_setup.c:259`.

### sidedef → `side_t` (`r_defs.h:165`) — mirror `P_LoadSideDefs`

| UDMF field | struct |
|---|---|
| `offsetx` (def 0) | `textureoffset = v<<FRACBITS` |
| `offsety` (def 0) | `rowoffset = v<<FRACBITS` |
| `texturetop` (def "-") | `toptexture = R_TextureNumForName` |
| `texturebottom` (def "-") | `bottomtexture` |
| `texturemiddle` (def "-") | `midtexture` |
| `sector` (no default) | `sector = &sectors[idx]` |

Keep the raw top/bottom names if we want ID24 music-change lines to keep working
(the binary path stashes `sd_raw_top/bot`, `p_setup.c:478`) — optional for phase 1.

### linedef → `line_t` (`r_defs.h:200`) — mirror `P_LoadLineDefs` (`p_setup.c:484`)

Flags: OR together the `ML_*` bits (`doomdata.h:101`) from the booleans —

| UDMF bool | `ML_*` |
|---|---|
| `blocking` | `ML_BLOCKING` (1) |
| `blockmonsters` | `ML_BLOCKMONSTERS` (2) |
| `twosided` | `ML_TWOSIDED` (4) |
| `dontpegtop` | `ML_DONTPEGTOP` (8) |
| `dontpegbottom` | `ML_DONTPEGBOTTOM` (16) |
| `secret` | `ML_SECRET` (32) |
| `blocksound` | `ML_SOUNDBLOCK` (64) |
| `dontdraw` | `ML_DONTDRAW` (128) |
| `mapped` | `ML_MAPPED` (256) |
| `passuse` | `ML_PASSUSE` (512) — Boom, honored in Doom namespace |

`special` → `ld->special`. **Tag/id:** per spec §III "*** Tag / ID Behavior"
(lines 418–438): in Doom namespace the tag is stored as **both** `id` and `arg0`.
We only have one `tag` slot, so set `ld->tag = arg0` (the special's target); if
`arg0==0` fall back to `id`. Set `sidenum[0]=sidefront`, `sidenum[1]=sideback`
(def −1). Then recompute `dx/dy`, `slopetype`, `bbox` exactly as `p_setup.c:507–542`
and resolve front/back sectors (`p_setup.c:554–565`, including the missing-right-side
dummy). Reuse the **unsigned** sidenum handling for >32767 sidedefs.

### thing → `mapthing_t` (`doomdata.h:212`), then `P_SpawnMapThing`

`mapthing_t` is `short x,y,angle,type,options`. Fill from UDMF and call the existing
`P_SpawnMapThing` (already invoked by the binary `P_LoadThings`, `p_setup.c:419`) so
player starts / DM starts / skill filtering all keep working.

- `x = lround(x_double)`, `y = lround(y_double)`, `angle`, `type` direct.
- **`options` flags** — recompose the vanilla `MTF_*` bits from the UDMF booleans:
  - `skill1||skill2` → easy bit, `skill3` → normal bit, `skill4||skill5` → hard bit
    (vanilla only has 3 skill bits; map the 5 UDMF skills onto them).
  - `ambush` → `MTF_AMBUSH`.
  - `!single` → `MTF_NOTSINGLE`; `!dm` → `MTF_NOTDM`; `!coop` → `MTF_NOTCOOP`
    (note the **inversion** — vanilla stores "not present in", UDMF stores "present in").
- **`height`** (Z offset), **Hexen `special`+`args`**, thing `id`, class/dormant/
  Strife flags: **dropped in phase 1** (vanilla `mapthing_t` has nowhere to put them;
  Doom things spawn on the floor). Log if a nonzero thing special is seen so we know
  a map wanted Hexen semantics.

---

## 4. Nodes — the hard dependency ⚠️

**This is the biggest risk and the reason UDMF is non-trivial for a software
renderer.** UDMF's `TEXTMAP` contains **no BSP** (no SEGS/SSECTORS/NODES). The
renderer (`r_bsp.c`) and sight/collision need a BSP tree. Two options:

**Option A (recommended for phase 1): require prebuilt nodes.**
UDMF maps almost always ship a nodes lump between `TEXTMAP` and `ENDMAP`, named
**`ZNODES`** (occasionally the raw lump). BUT modern node builders (ZDBSP) emit
**GL nodes** in one of the `XGLN` / `XGL2` / `XGL3` (and zlib `ZGLN`/`ZGL2`/`ZGL3`)
formats — which BuddyDoom's `P_LoadNodes_Extended` **does not parse** (it only does
non-GL `XNOD`/`ZNOD`, and even says so at `p_setup.c:273`). So phase 1 needs:

1. Locate the node lump: scan lumps from `lumpnum+1` to `ENDMAP` for `ZNODES`
   (or accept an `XNOD`/`ZNOD` payload).
2. Extend the extended-node reader to handle the **GL** layouts. The XGL3 seg
   record differs (per-subsector seg lists, partner segs, v1 + partner + linedef,
   32-bit everything, `0xFFFFFFFF` = minisegs with no linedef). This is the real
   work — budget most of phase-1 effort here. Minisegs (segs with no linedef) must
   be handled: they get `linedef = NULL`; `r_segs.c`/`r_bsp.c` must tolerate that
   (vanilla assumes every seg has a linedef — verify and guard).

**Option B: build nodes at runtime** (port a BSP builder, e.g. a trimmed ZDBSP/
BSP-nodes). Larger, slower per level load, but removes the external-nodes
requirement. Defer — only if maps without a node lump must be supported.

**Recommendation:** phase 1 = Option A restricted to what we can validate. If the
node lump is a format we can't parse, **reject the map cleanly** with a message
(spec §II.A: don't attempt to load a map that exceeds engine capability) rather
than crash.

---

## 5. Blockmap & reject

- **Blockmap:** UDMF need not carry one. BuddyDoom already has a **from-scratch
  blockmap generator** (`P_CreateBlockMap`, invoked by `P_LoadBlockMap` when the
  lump is absent/empty — see `p_setup.c:776`). Wire the UDMF path to force
  generation (pass a sentinel or call the generator directly) instead of reading a
  `ML_BLOCKMAP` lump.
- **Reject:** synthesize an all-zero (all-visible) reject matrix sized
  `(numsectors² + 7)/8` in `PU_LEVEL` when no REJECT lump is present. All-visible
  is always safe (reject is only an optimization).

---

## 6. Data-model extension (phase 2, Hexen/ZDoom namespace)

Only needed once we want line/thing **specials with parameters** or ACS:

- Add `int id;` and `int args[5];` to `line_t`; `int special; int args[5]; int id;`
  and a Z-height to the thing spawn path; `int id;` distinct from `tag` on
  `sector_t`.
- **Savegame impact (CLAUDE.md auto-versioning):** `line_t`/`sector_t` are **not**
  in the `p_saveg.c` memcpy signature list, so changing them does **not** bump
  `VERSION_NUM`. But if the Hexen work touches `mobj_t` (e.g. to store thing
  special/args on the actor), that **will** re-fingerprint `files/aidoom_saveg.sig`
  and auto-bump the engine version — expected, not a bug.
- Phase 1 changes **no** saved struct, so **no savegame break**.

---

## 7. Build-system wiring (don't repeat the LNK2019)

`p_udmf.c` is picked up automatically by:
- **`build.sh`** — compiles `files/*.c` (glob). ✅ nothing to do.
- **CMake** — check whether `CMakeLists.txt` globs or lists sources; add if listed.

But **MSVC lists objects by hand** (`files/Makefile.msvc:26` `OBJS = …`). The
recent commit `daf8abc` fixed exactly this class of bug (`w_inflate.obj` missing →
LNK2019). **Add `p_udmf.obj`** to the `OBJS` list (e.g. next to `p_setup.obj` on
line 34) or the Windows build will link-fail.

Also: header edits (`r_defs.h`, `doomdata.h`) need `make clean` — the Makefile has
no header deps (CLAUDE.md "IMPORTANT build gotcha").

---

## 8. Testing (no test suite exists — verify by running)

1. **Minimal hand-written UDMF map** — one square room, one namespace line, 4
   verts / 4 lines / 4 sides / 1 sector / 1 player-1 start. Pack into a PWAD as
   `MAP01`/`TEXTMAP`/`ZNODES`/`ENDMAP`, load with `-file`. Confirm it spawns and
   renders.
2. **A real ZDoom/UDMF-Doom-namespace map** (with ZDBSP GL nodes) — the node-format
   coverage from §4 is the make-or-break test.
3. **Regression:** load a stock binary IWAD map — the `TEXTMAP` guard must be inert
   (dispatch only triggers when lump `+1` is literally `TEXTMAP`).
4. **Malformed input:** truncated block, unknown field, unknown namespace, missing
   node lump — must warn/reject, never crash (spec §II.A).

---

## 9. Phasing summary

| Phase | Deliverable | Touches |
|---|---|---|
| 1a | Lexer/parser + `P_LoadUDMF`, geometry + things (Doom ns) | new `p_udmf.c/.h`, `p_setup.c` dispatch |
| 1b | GL-node reader (`XGL3`/`ZGL*`) + miniseg tolerance | `p_setup.c` (extend `P_LoadNodes_Extended`), maybe `r_bsp.c`/`r_segs.c` |
| 1c | Blockmap/reject synthesis wiring | `p_setup.c` |
| 1d | Build wiring (`Makefile.msvc` OBJS, CMake), tests | build files |
| 2 | Hexen/ZDoom: `args[5]`, line/thing `id`, thing specials/Z, ACS stub | `r_defs.h`, `doomdata.h`, spawn path, savegame sig bump |

**Riskiest item:** §4 (GL nodes). Everything else is mechanical field-copying that
parallels the existing `P_Load*` functions almost line-for-line.
