#!/usr/bin/env python3
"""
extract_game_assets.py -- pull sprites + sounds out of another game's IWAD and
PALETTE-CONVERT them into the DOOM palette, so they can be shown/played by this
Doom engine.  Works for Heretic, Hexen (different palettes -> real conversion)
and Freedoom2 (Doom palette -> identity copy).

This is the ASSET half of bringing Heretic/Hexen content in.  Using the assets
in-game still needs C actor definitions (sprite/state/mobjinfo/sound tables) --
see HERETIC_HEXEN.md and the gzdoom ZScript reference; this tool just produces a
palette-correct WAD of their graphics + sounds.

How it works:
  * Reads the source IWAD's PLAYPAL and a reference DOOM PLAYPAL (the target).
  * Builds a 256->256 nearest-colour translation (source index -> Doom index).
  * Re-colours every sprite (S_START..S_END) by remapping ONLY the pixel bytes in
    each column post -- the patch structure/offsets are unchanged (1 byte in, 1
    byte out), so this is exact and handles tall patches.
  * Copies every DMX sound lump (format-3 header) verbatim -- audio is palette-
    independent.  (Lump names keep the source game's names; the C port maps them.)

Usage:
    python3 tools/extract_game_assets.py --src ID0/heretic.wad --out ID0/heretic_assets.wad
    python3 tools/extract_game_assets.py --src ID0/hexen.wad   --out ID0/hexen_assets.wad
    python3 tools/extract_game_assets.py --src ID0/freedoom2.wad --out ID0/freedoom2_assets.wad
    # --refpal defaults to ID0/DOOM.WAD's PLAYPAL.

Reads only YOUR own WADs locally; nothing is redistributed.
"""

import argparse
import struct
import sys
from pathlib import Path


def read_wad(path):
    data = path.read_bytes()
    magic, n, off = struct.unpack("<4sII", data[:12])
    if magic not in (b"IWAD", b"PWAD"):
        raise ValueError(f"{path}: not a WAD ({magic!r})")
    ent = []
    for i in range(n):
        fp, sz = struct.unpack("<II", data[off+i*16: off+i*16+8])
        nm = data[off+i*16+8: off+i*16+16].split(b"\x00")[0].decode("latin1")
        ent.append((nm, fp, sz))
    return data, ent


def lump_bytes(data, ent, name):
    for nm, fp, sz in ent:
        if nm == name:
            return data[fp: fp+sz]
    return None


def palette(data, ent):
    p = lump_bytes(data, ent, "PLAYPAL")
    return [(p[i*3], p[i*3+1], p[i*3+2]) for i in range(256)]


def build_xlat(src_pal, dst_pal):
    """src index -> nearest dst index."""
    xl = bytearray(256)
    for i, (r, g, b) in enumerate(src_pal):
        best, bd = 0, 1 << 30
        for j, (R, G, B) in enumerate(dst_pal):
            d = (r-R)**2 + (g-G)**2 + (b-B)**2
            if d < bd:
                bd, best = d, j
                if d == 0:
                    break
        xl[i] = best
    return xl


def remap_patch(raw, xl):
    """Remap a Doom patch's pixels through xl; structure/offsets unchanged."""
    b = bytearray(raw)
    if len(b) < 8:
        return bytes(b)
    w = struct.unpack("<h", b[0:2])[0]
    if w <= 0 or 8 + 4*w > len(b):
        return bytes(b)                       # not a patch -> leave alone
    colofs = struct.unpack(f"<{w}I", b[8: 8+4*w])
    for o in colofs:
        if o <= 0 or o >= len(b):
            continue
        while o < len(b) and b[o] != 0xff:
            length = b[o+1]
            for k in range(length):           # pixels at o+3 .. o+3+length-1
                p = o + 3 + k
                if p < len(b):
                    b[p] = xl[b[p]]
            o += length + 4
    return bytes(b)


def is_dmx_sound(raw):
    return len(raw) >= 8 and raw[0] == 0x03 and raw[1] == 0x00


def write_wad(out, lumps):
    cur = 12
    dirent = []
    for nm, d in lumps:
        dirent.append((cur, len(d), nm))
        cur += len(d)
    with open(out, "wb") as f:
        f.write(b"PWAD")
        f.write(struct.pack("<II", len(lumps), cur))
        for _nm, d in lumps:
            f.write(d)
        for fp, sz, nm in dirent:
            f.write(struct.pack("<II", fp, sz))
            f.write(nm.encode("ascii", "replace")[:8].ljust(8, b"\x00"))


def main():
    here = Path(__file__).resolve().parent.parent
    id0 = here / "run" / "ID0"
    ap = argparse.ArgumentParser(description="Palette-convert another game's sprites/sounds to DOOM")
    ap.add_argument("--src", required=True, help="source IWAD (heretic/hexen/freedoom2)")
    ap.add_argument("--out", required=True, help="output PWAD")
    ap.add_argument("--refpal", default=None, help="DOOM IWAD for the target palette (default ID0/DOOM.WAD)")
    args = ap.parse_args()

    src = Path(args.src);  src = src if src.is_absolute() or src.exists() else id0 / src.name
    ref = Path(args.refpal) if args.refpal else id0 / "DOOM.WAD"
    if not src.exists(): print(f"ERROR: source {src} not found", file=sys.stderr); return 2
    if not ref.exists(): print(f"ERROR: palette ref {ref} not found", file=sys.stderr); return 2

    sdata, sent = read_wad(src)
    rdata, rent = read_wad(ref)
    src_pal, dst_pal = palette(sdata, sent), palette(rdata, rent)
    identity = src_pal == dst_pal
    xl = build_xlat(src_pal, dst_pal)
    names = [n for n, _, _ in sent]

    # sprites: the whole S_START..S_END namespace (palette-converted)
    s0 = names.index("S_START") if "S_START" in names else -1
    s1 = names.index("S_END")   if "S_END"   in names else -1
    sprites, sounds = [], []
    if 0 <= s0 < s1:
        for i in range(s0, s1+1):
            nm, fp, sz = sent[i]
            raw = sdata[fp: fp+sz]
            if nm in ("S_START", "S_END") or sz == 0:
                sprites.append((nm, raw))                 # markers / empty pass through
            else:
                sprites.append((nm, raw if identity else remap_patch(raw, xl)))
    # sounds: every DMX lump, verbatim
    for nm, fp, sz in sent:
        raw = sdata[fp: fp+sz]
        if is_dmx_sound(raw):
            sounds.append((nm, raw))

    out = Path(args.out); out = out if out.is_absolute() else here / out
    out.parent.mkdir(parents=True, exist_ok=True)
    write_wad(out, sprites + sounds)
    total = sum(len(d) for _n, d in sprites + sounds)
    print(f"extract_game_assets: {src.name} -> {out}")
    print(f"  palette: {'identity (same as DOOM)' if identity else 'CONVERTED to DOOM palette'}")
    print(f"  sprites: {len(sprites)} lumps (S_START..S_END)")
    print(f"  sounds : {len(sounds)} DMX lumps")
    print(f"  total  : {len(sprites)+len(sounds)} lumps, {total/1024/1024:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
