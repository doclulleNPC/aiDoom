#!/usr/bin/env python3
"""
mark_internal_wad.py -- tag aiDoom's internal asset PWADs with a marker lump so the
launcher hides them from the user PWAD dropdown.

freedoomstuff / hereticstuff / hexenstuff / doom2stuff.wad are loaded by the game (or the
launcher's monster checkboxes), NOT picked as a user "-file".  The extract_*.py tools add
this marker on build; this script tags pre-existing wads in place (idempotent -- a second
run is a no-op).

The marker is a tiny lump named MARKER_LUMP placed first (before S_START), so it sits
OUTSIDE the sprite namespace and is harmless to the engine -- just one unused directory
entry the game never looks up.  files/launcher.c hides any wad that contains it.

    python3 tools/mark_internal_wad.py                 # tag the four ID0 stuff-wads
    python3 tools/mark_internal_wad.py path/to/x.wad   # tag specific wad(s)
"""

import struct
import sys
from pathlib import Path

MARKER_LUMP = "AISTUFF"
MARKER_DATA = b"aiDoom internal asset pack -- loaded by the game, not a user PWAD\n"


def add_marker(path):
    d = bytearray(Path(path).read_bytes())
    if len(d) < 12:
        return "skip (too small)"
    magic, numlumps, ofs = struct.unpack("<4sII", d[:12])
    if magic not in (b"PWAD", b"IWAD"):
        return f"skip (not a WAD: {magic!r})"
    if ofs + numlumps * 16 > len(d):
        return "skip (bad directory)"
    # read the existing directory; bail if already tagged
    ents = []
    for i in range(numlumps):
        e = ofs + i * 16
        fp, sz = struct.unpack("<II", d[e:e + 8])
        nm = bytes(d[e + 8:e + 16])
        if nm.split(b"\0")[0] == MARKER_LUMP.encode():
            return "already marked"
        ents.append((fp, sz, nm))
    # rebuild: [header + all lump data] + marker data + new directory (marker first).
    # (Tool-built wads store the directory at the end, so d[:ofs] is exactly the lump data.)
    out = bytearray(d[:ofs])
    mfp = len(out)
    out += MARKER_DATA
    new_ofs = len(out)
    ents.insert(0, (mfp, len(MARKER_DATA), MARKER_LUMP.encode().ljust(8, b"\0")[:8]))
    for fp, sz, nm in ents:
        out += struct.pack("<II", fp, sz) + nm.ljust(8, b"\0")[:8]
    out[:12] = struct.pack("<4sII", magic, numlumps + 1, new_ofs)
    Path(path).write_bytes(out)
    return f"marked ({numlumps} -> {numlumps + 1} lumps)"


def main():
    here = Path(__file__).resolve().parent.parent
    id0 = here / "run" / "ID0"
    targets = sys.argv[1:] or [str(id0 / w) for w in
        ("freedoomstuff.wad", "hereticstuff.wad", "hexenstuff.wad", "doom2stuff.wad")]
    for t in targets:
        p = Path(t)
        print(f"{p.name}: {add_marker(t) if p.exists() else 'not found (skip)'}")


if __name__ == "__main__":
    sys.exit(main())
