#!/usr/bin/env python3
"""
extract_doom2.py -- build run/doom2stuff.wad from your own DOOM2 IWAD so the
DOOM2-exclusive monsters (revenant, mancubus, arachnotron, hell knight, pain
elemental, arch-vile, chaingunner, SS, keen) and the super shotgun work while
playing DOOM1 (doom.wad).

Why this is needed: doom.wad simply doesn't contain those sprites/sounds, so
spawning a DOOM2 monster in DOOM1 crashes the renderer ("missing rotations").
This tool copies the assets out of YOUR doom2.wad into a PWAD you load with
`-file doom2stuff.wad`.

What it puts in the WAD:
  * The ENTIRE sprite namespace (S_START..S_END) from doom2.wad.  This matters:
    the engine resolves the sprite range from the LAST S_START/S_END pair, so a
    PWAD that adds only the new sprites would shadow the IWAD's range and break
    every DOOM1 sprite.  doom2's sprite set is a superset of doom1's, so copying
    the whole namespace gives a complete, correct range (DOOM1 + DOOM2 sprites,
    incl. the SSG's SHT2/SGN2 frames).
  * Every DOOM2-exclusive sound (DS*/DP* present in doom2 but not in doom1) --
    the monster sounds + the SSG (DSDSHTGN/DSDBOPN/DSDBLOAD/DSDBCLS).  Sounds are
    global by name, so no markers are needed for them.

Usage:
    python3 tools/extract_doom2.py                  # auto-find run/doom2.wad + run/DOOM.WAD
    python3 tools/extract_doom2.py --doom2 X.wad --doom1 Y.wad --out run/doom2stuff.wad

Then play DOOM1 with it:
    ./aidoom -iwad DOOM.WAD -file doom2stuff.wad -aidirector 31666 ...
The engine auto-detects the DOOM2 sprites and lets the director spawn DOOM2
monsters (P_Director_SafeType).  `give supershotgun` (or the buddy loadout) hands
out the SSG.

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
        raise ValueError(f"{path}: not a WAD (magic {magic!r})")
    entries = []
    for i in range(n):
        fp, sz = struct.unpack("<II", data[off + i*16: off + i*16 + 8])
        nm = data[off + i*16 + 8: off + i*16 + 16].split(b"\x00")[0].decode("latin1")
        entries.append((nm, fp, sz))
    return data, entries


def find_index(entries, name):
    for i, (nm, _, _) in enumerate(entries):
        if nm == name:
            return i
    return -1


def write_wad(out_path, lumps):
    """lumps: list of (name, bytes).  Writes a PWAD."""
    header = 12
    cur = header
    dir_entries = []
    for name, data in lumps:
        dir_entries.append((cur, len(data), name))
        cur += len(data)
    with open(out_path, "wb") as f:
        f.write(b"PWAD")
        f.write(struct.pack("<II", len(lumps), cur))
        for _name, data in lumps:
            f.write(data)
        for fp, sz, name in dir_entries:
            f.write(struct.pack("<II", fp, sz))
            f.write(name.encode("ascii", "replace")[:8].ljust(8, b"\x00"))


def main():
    ap = argparse.ArgumentParser(description="Extract DOOM2 monsters/SSG into doom2stuff.wad")
    ap.add_argument("--doom2", default=None, help="path to doom2.wad (default: run/doom2.wad)")
    ap.add_argument("--doom1", default=None, help="path to a DOOM1 IWAD for the sound diff "
                                                  "(default: run/DOOM.WAD; optional)")
    ap.add_argument("--out",   default="run/doom2stuff.wad", help="output PWAD path")
    args = ap.parse_args()

    here = Path(__file__).resolve().parent.parent      # repo root
    d2 = Path(args.doom2) if args.doom2 else here / "run" / "doom2.wad"
    if not d2.exists():
        print(f"ERROR: DOOM2 IWAD not found at {d2}. Pass --doom2 <path>.", file=sys.stderr)
        return 2
    data2, e2 = read_wad(d2)
    names2 = [n for n, _, _ in e2]

    # --- sprites: the WHOLE S_START..S_END range (inclusive of the markers) ----
    s0 = find_index(e2, "S_START")
    s1 = find_index(e2, "S_END")
    if s0 < 0 or s1 < 0 or s1 < s0:
        print("ERROR: no S_START/S_END sprite namespace in the DOOM2 WAD.", file=sys.stderr)
        return 2
    sprite_lumps = [(e2[i][0], data2[e2[i][1]: e2[i][1] + e2[i][2]]) for i in range(s0, s1 + 1)]

    # --- sounds: DOOM2-exclusive DS*/DP* (not present in the DOOM1 IWAD) --------
    d1 = Path(args.doom1) if args.doom1 else here / "run" / "DOOM.WAD"
    doom1_names = set()
    if d1.exists():
        _, e1 = read_wad(d1)
        doom1_names = set(n for n, _, _ in e1)
    else:
        print(f"  (no DOOM1 IWAD at {d1}; taking ALL DOOM2 DS*/DP* sounds)")
    sound_lumps = []
    for nm, fp, sz in e2:
        if (nm.startswith("DS") or nm.startswith("DP")) and nm not in doom1_names:
            sound_lumps.append((nm, data2[fp: fp + sz]))

    lumps = sprite_lumps + sound_lumps
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    write_wad(out, lumps)
    total = sum(len(d) for _n, d in lumps)
    print(f"extract_doom2: wrote {out}")
    print(f"  sprites: {len(sprite_lumps)} lumps (full S_START..S_END namespace)")
    print(f"  sounds : {len(sound_lumps)} DOOM2-exclusive DS*/DP* lumps")
    print(f"  total  : {len(lumps)} lumps, {total/1024/1024:.1f} MB")
    print(f"  play:  ./aidoom -iwad DOOM.WAD -file {out.name} -aidirector 31666 ...")
    return 0


if __name__ == "__main__":
    sys.exit(main())
