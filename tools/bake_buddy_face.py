#!/usr/bin/env python3
"""
bake_buddy_face.py -- pack the AI co-op buddy's mugshot graphics into aidoom.wad.

The buddy STBAR shows its own face (a distinct mugshot set from the player's), so
the lumps are named BUF* (BUFST00.. / BUFDEAD0 / BUFEVL* / ...) instead of the
IWAD's STF* -- different names so they don't collide with the player face and so
W_CacheLumpName resolves them straight out of aidoom.wad.

Source: run/buddyface/BUF*.lmp (already in Doom patch_t format -- raw lumps, copied
verbatim).  This script merges them into the existing run/aidoom.wad (which also holds
the DS* voice lumps) and is idempotent: re-running overwrites the BUF* lumps in place
and leaves every other lump untouched, so it can run after bake_buddy_voice.py.

Usage:
    python3 tools/bake_buddy_face.py
    python3 tools/bake_buddy_face.py --wad run/aidoom.wad --faces run/buddyface
"""

import argparse
import struct
import sys
from pathlib import Path


def read_wad(path: Path):
    """Return (is_pwad, [(name, data), ...]) preserving lump order."""
    if not path.exists():
        return True, []
    d = path.read_bytes()
    sig, n, off = struct.unpack("<4sII", d[:12])
    lumps = []
    for i in range(n):
        lo, ls, nm = struct.unpack("<II8s", d[off + i * 16: off + i * 16 + 16])
        name = nm.rstrip(b"\0").decode("latin1")
        lumps.append((name, d[lo:lo + ls]))
    return sig == b"PWAD", lumps


def write_wad(path: Path, lumps):
    """Write a PWAD: header, lump data blob, then the directory."""
    out = bytearray(b"PWAD")
    out += struct.pack("<II", len(lumps), 0)        # numlumps, (diroff patched below)
    dir_entries = []
    for name, data in lumps:
        if len(name) > 8:
            sys.exit(f"lump name too long (>8): {name!r}")
        dir_entries.append((len(out), len(data), name))
        out += data
    diroff = len(out)
    for filepos, size, name in dir_entries:
        out += struct.pack("<II8s", filepos, size, name.encode("latin1").ljust(8, b"\0"))
    struct.pack_into("<I", out, 8, diroff)          # patch diroff in the header
    path.write_bytes(out)


def main():
    here = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser()
    ap.add_argument("--wad", default=str(here / "run" / "aidoom.wad"))
    ap.add_argument("--faces", default=str(here / "tools" / "buddyface"))
    args = ap.parse_args()

    wad = Path(args.wad)
    faces_dir = Path(args.faces)

    face_files = sorted(faces_dir.glob("BUF*.lmp"))
    if not face_files:
        sys.exit(f"no BUF*.lmp found in {faces_dir} (rename STF* -> BUF* first)")

    is_pwad, lumps = read_wad(wad)
    if not is_pwad and lumps:
        sys.exit(f"{wad} is an IWAD; refusing to modify")

    faces = {f.stem.upper(): f.read_bytes() for f in face_files}

    # Overwrite existing BUF* lumps in place; append the rest in sorted order.
    by_name = {n: i for i, (n, _) in enumerate(lumps)}
    appended = []
    for name in sorted(faces):
        if name in by_name:
            lumps[by_name[name]] = (name, faces[name])
        else:
            appended.append((name, faces[name]))
    lumps.extend(appended)

    write_wad(wad, lumps)
    print(f"wrote {wad}: {len(lumps)} lumps total ({len(faces)} BUF* faces, "
          f"{len(appended)} newly added)")


if __name__ == "__main__":
    main()
