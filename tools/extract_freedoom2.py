#!/usr/bin/env python3
"""
extract_freedoom2.py -- build run/ID0/freedoom2stuff.wad: the DOOM2-exclusive monsters
(revenant, mancubus, arch-vile, arachnotron, hell knight, pain elemental, chaingunner,
the Wolfenstein SS, the bosses, ...) and their sounds, taken from a FREE freedoom2.wad
so they can be spawned in DOOM1.  Mirrors tools/extract_doom2.py but with free art.

freedoom2 uses the DOOM palette, so sprites are copied verbatim (no remap).  It shares
DOOM2's lump NAMES, so we diff against a DOOM1 IWAD and keep only what DOOM1 lacks --
i.e. the extra monsters -- as an additive S_START..S_END PWAD.  The engine MERGES sprite
namespaces (R_InitSpriteLumps), so this WAD ADDS those sprites to DOOM1's set without
shadowing them; DOOM1's own monsters keep their art.  No full-superset copy needed.

Usage:
    python3 tools/extract_freedoom2.py                         # ID0/freedoom2.wad + ID0/DOOM.WAD
    python3 tools/extract_freedoom2.py --doom1 ID0/freedoom1.wad   # diff against freedoom1 instead
    ./aidoom -iwad DOOM.WAD -file freedoom2stuff.wad -director ...
"""

import argparse
import struct
import sys
from pathlib import Path


def read_wad(path):
    data = path.read_bytes()
    magic, n, off = struct.unpack("<4sII", data[:12])
    ent = []
    for i in range(n):
        fp, sz = struct.unpack("<II", data[off+i*16: off+i*16+8])
        nm = data[off+i*16+8: off+i*16+16].split(b"\x00")[0].decode("latin1")
        ent.append((nm, fp, sz))
    return data, ent


def is_dmx(raw):
    return len(raw) >= 8 and raw[0] == 3 and raw[1] == 0	# DMX sound: format 3, lo byte 0


def write_wad(out, lumps):
    cur = 12
    de = []
    for nm, d in lumps:
        de.append((cur, len(d), nm)); cur += len(d)
    with open(out, "wb") as f:
        f.write(b"PWAD"); f.write(struct.pack("<II", len(lumps), cur))
        for _n, d in lumps:
            f.write(d)
        for fp, sz, nm in de:
            f.write(struct.pack("<II", fp, sz))
            f.write(nm.encode("ascii", "replace")[:8].ljust(8, b"\x00"))


def sprite_range(ent):
    names = [n for n, _, _ in ent]
    return names.index("S_START"), names.index("S_END")


def main():
    here = Path(__file__).resolve().parent.parent
    id0 = here / "run" / "ID0"
    ap = argparse.ArgumentParser(description="Extract DOOM2-exclusive monsters from freedoom2 (free art)")
    ap.add_argument("--src",   default=str(id0/"freedoom2.wad"), help="freedoom2 IWAD")
    ap.add_argument("--doom1", default=str(id0/"DOOM.WAD"),      help="DOOM1 IWAD to diff against")
    ap.add_argument("--out",   default=str(id0/"freedoom2stuff.wad"))
    a = ap.parse_args()
    src, d1, outp = Path(a.src), Path(a.doom1), Path(a.out)
    for p in (src, d1):
        if not p.exists():
            print(f"ERROR: {p} not found", file=sys.stderr); return 2

    fdata, fent = read_wad(src)
    _ddata, dent = read_wad(d1)

    # DOOM1's sprite 4-char codes + sound lump names (what it already has).
    try:
        ds0, ds1 = sprite_range(dent)
    except ValueError:
        print("ERROR: DOOM1 IWAD has no S_START/S_END", file=sys.stderr); return 2
    d1_codes = set(dent[i][0][:4] for i in range(ds0+1, ds1))
    d1_sounds = set(n for n, _, _ in dent)

    try:
        fs0, fs1 = sprite_range(fent)
    except ValueError:
        print("ERROR: freedoom2 has no S_START/S_END", file=sys.stderr); return 2

    # + sprites whose 4-char code is NOT in DOOM1 (the extra DOOM2 monsters), verbatim.
    out = [("S_START", b"")]
    n_spr = 0
    codes = set()
    for nm, fp, sz in fent[fs0+1: fs1]:
        if nm[:4] not in d1_codes:
            out.append((nm, fdata[fp:fp+sz])); n_spr += 1; codes.add(nm[:4])
    out.append(("S_END", b""))

    # + DMX sounds present in freedoom2 but not in DOOM1 (the extra monsters' sounds).
    n_snd = 0
    for nm, fp, sz in fent:
        if nm in d1_sounds:
            continue
        raw = fdata[fp:fp+sz]
        if (nm.startswith("DS") or nm.startswith("DP")) and is_dmx(raw):
            out.append((nm, raw)); n_snd += 1

    outp.parent.mkdir(parents=True, exist_ok=True)
    write_wad(outp, out)
    total = sum(len(d) for _n, d in out)
    print(f"extract_freedoom2: wrote {outp} (DOOM2-exclusive monsters, free art)")
    print(f"  sprites added: {n_spr}  ({len(codes)} actors: {', '.join(sorted(codes))})")
    print(f"  sounds added:  {n_snd}")
    print(f"  total: {len(out)} lumps, {total/1024/1024:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
