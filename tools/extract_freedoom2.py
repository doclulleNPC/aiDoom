#!/usr/bin/env python3
"""
extract_freedoom2.py -- build run/ID0/freedoomstuff.wad: the DOOM2-exclusive monsters
(revenant, mancubus, arch-vile, arachnotron, chaingunner, hell knight, pain elemental,
the Wolfenstein SS, commander keen) + their projectiles + sounds, taken from a FREE
freedoom2.wad so they can be spawned in DOOM1.

Treated like the Heretic port: the monster/projectile sprites are RENAMED with an
`F*` prefix (SKEL->FSKE, FATT->FFAT, ...), so they NEVER collide with / override the
vanilla DOOM sprites or a loaded doom2stuff.wad.  files/freedoom.c then clones each
DOOM2 actor into a new MT_FD_* slot whose states use the renamed sprites (the DOOM2
states/mobjinfo/A_* funcs already live in the engine).  Sounds keep their original
DS* names -- the cloned mobjinfo reference the same sfx indices, and same-name lumps
just dedupe harmlessly if doom2stuff is also loaded.

freedoom2 uses the DOOM palette, so sprite pixels are copied verbatim (no remap).

Usage:
    python3 tools/extract_freedoom2.py                 # ID0/freedoom2.wad
    ./aidoom -iwad DOOM.WAD -file freedoomstuff.wad -director ...
"""

import argparse
import struct
import sys
from pathlib import Path

# DOOM2 monster/projectile sprite 4-char code -> Freedoom renamed code (must match the
# SPR_F* / sprnames[] additions in files/info.h + info.c, and freedoom.c's sprmap[]).
RENAME = {
    "SKEL": "FSKE", "FATT": "FFAT", "VILE": "FVIL", "BSPI": "FBSP", "CPOS": "FCPO",
    "BOS2": "FBO2", "PAIN": "FPAI", "SSWV": "FSSW", "KEEN": "FKEE",   # monsters
    "FATB": "FFAB", "FBXP": "FFBX", "MANF": "FMAN", "FIRE": "FFIR",   # projectiles
    "APLS": "FAPL", "APBX": "FAPB",
}


def read_wad(path):
    data = path.read_bytes()
    _magic, n, off = struct.unpack("<4sII", data[:12])
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
    ap = argparse.ArgumentParser(description="Extract DOOM2-exclusive monsters from freedoom2 (renamed, free art)")
    ap.add_argument("--src",   default=str(id0/"freedoom2.wad"), help="freedoom2 IWAD")
    ap.add_argument("--doom1", default=str(id0/"DOOM.WAD"),      help="DOOM1 IWAD (sound diff)")
    ap.add_argument("--out",   default=str(id0/"freedoomstuff.wad"))
    a = ap.parse_args()
    src, d1, outp = Path(a.src), Path(a.doom1), Path(a.out)
    if not src.exists():
        print(f"ERROR: {src} not found", file=sys.stderr); return 2

    fdata, fent = read_wad(src)
    d1_sounds = set()
    if d1.exists():
        _dd, dent = read_wad(d1)
        d1_sounds = set(n for n, _, _ in dent)

    try:
        fs0, fs1 = sprite_range(fent)
    except ValueError:
        print("ERROR: freedoom2 has no S_START/S_END", file=sys.stderr); return 2

    # Sprites: only the monster/projectile codes in RENAME, with the 4-char prefix
    # swapped to its F* code (frame/rotation suffix kept verbatim).
    out = [("S_START", b"")]
    n_spr = 0
    seen = set()
    for nm, fp, sz in fent[fs0+1: fs1]:
        new = RENAME.get(nm[:4])
        if new:
            out.append((new + nm[4:], fdata[fp:fp+sz])); n_spr += 1; seen.add(nm[:4])
    out.append(("S_END", b""))

    # Sounds: DOOM2-exclusive DMX lumps (not in DOOM1), kept under their original names.
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
    missing = sorted(set(RENAME) - seen)
    print(f"extract_freedoom2: wrote {outp}")
    print(f"  renamed sprites: {n_spr}  ({len(seen)}/{len(RENAME)} codes: {', '.join(sorted(seen))})")
    if missing:
        print(f"  WARNING: codes not found in freedoom2: {', '.join(missing)}")
    print(f"  sounds (orig names): {n_snd}")
    print(f"  total: {len(out)} lumps, {total/1024/1024:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
