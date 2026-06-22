#!/usr/bin/env python3
"""
extract_heretic_monsters.py -- build run/ID0/hereticstuff.wad: the Heretic ENEMIES
(sprites palette-converted to the DOOM palette + their sounds) so they can be
spawned in DOOM by files/heretic.c.

Single-range sprite engine note: a PWAD's own S_START/S_END *replaces* the active
sprite range, so a Heretic-only sprite WAD would shadow the DOOM sprites. We
therefore build a SUPERSET, exactly like doom2stuff.wad: the base IWAD's full
sprite namespace + the (renamed) Heretic monster sprites, all in one S_START..S_END.

Collisions: a Heretic monster sprite whose 4-char code clashes with a DOOM sprite
(e.g. HEAD = cacodemon vs ironlich) is RENAMED via SPRITE_RENAME below. files/heretic.c
must use the SAME renamed codes (kept in sync here + there).

Usage:
    python3 tools/extract_heretic_monsters.py                 # base ID0/doom2.wad, src ID0/heretic.wad
    python3 tools/extract_heretic_monsters.py --base ID0/DOOM.WAD
"""

import argparse
import struct
import sys
from pathlib import Path

# Heretic monster + projectile sprite codes -> DOOM-side code used by heretic.c.
# Renamed to an "H.."/"F.." space that doesn't collide with DOOM/DOOM2 sprite codes.
SPRITE_RENAME = {
    "IMPX": "HIMP",   # gargoyle (+ leader, chunks, fireball share IMPX)
    "MUMM": "HMUM",   # mummy / golem (+ ghost/leader/soul)
    "FX15": "HMUF",   # mummy leader fireball
    "KNIG": "HKNI",   # undead warrior (knight)
    "SPAX": "HKAX",   # knight axe
    "RAXE": "HKRX",   # knight red axe
    "BEAS": "HBEA",   # weredragon (beast)
    "FRB1": "HBEB",   # weredragon fireball
    "CLNK": "HCLK",   # sabreclaw (clink)
    "WZRD": "HWIZ",   # disciple of d'sparil (wizard)
    "FX11": "HWIB",   # wizard fireball
    "SNKE": "HSNK",   # ophidian (snake)
    "SNFX": "HSNB",   # snake projectile
    "HEAD": "HIRO",   # iron lich (COLLIDES with DOOM cacodemon)
    "FX05": "HIRB",   # ironlich fx
    "FX06": "HIRW",   # ironlich whirlwind fx
    "FX07": "HIRX",   # ironlich fx
    "MNTR": "HMIN",   # maulotaur (minotaur)
    "FX12": "HMNA",   # minotaur fx
    "FX13": "HMNB",
    "FX14": "HMNC",
    "SRCR": "HSR1",   # d'sparil (on bird)
    "SOR2": "HSR2",   # d'sparil (on foot)
    "FX16": "HSRB",   # sorcerer fx
    "CHKN": "HCHK",   # chicken (morph target)
}


def read_wad(path):
    data = path.read_bytes()
    magic, n, off = struct.unpack("<4sII", data[:12])
    ent = []
    for i in range(n):
        fp, sz = struct.unpack("<II", data[off+i*16: off+i*16+8])
        nm = data[off+i*16+8: off+i*16+16].split(b"\x00")[0].decode("latin1")
        ent.append((nm, fp, sz))
    return data, ent


def lump(data, ent, name):
    for nm, fp, sz in ent:
        if nm == name:
            return data[fp:fp+sz]
    return None


def pal(data, ent):
    p = lump(data, ent, "PLAYPAL")
    return [(p[i*3], p[i*3+1], p[i*3+2]) for i in range(256)]


def build_xlat(sp, dp):
    xl = bytearray(256)
    for i, (r, g, b) in enumerate(sp):
        best, bd = 0, 1 << 30
        for j, (R, G, B) in enumerate(dp):
            d = (r-R)**2+(g-G)**2+(b-B)**2
            if d < bd:
                bd, best = d, j
                if not d:
                    break
        xl[i] = best
    return xl


def remap_patch(raw, xl):
    b = bytearray(raw)
    if len(b) < 8:
        return bytes(b)
    w = struct.unpack("<h", b[0:2])[0]
    if w <= 0 or 8+4*w > len(b):
        return bytes(b)
    for o in struct.unpack(f"<{w}I", b[8:8+4*w]):
        if o <= 0 or o >= len(b):
            continue
        while o < len(b) and b[o] != 0xff:
            length = b[o+1]
            for k in range(length):
                p = o+3+k
                if p < len(b):
                    b[p] = xl[b[p]]
            o += length+4
    return bytes(b)


def is_dmx(raw):
    return len(raw) >= 8 and raw[0] == 3 and raw[1] == 0


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


def main():
    here = Path(__file__).resolve().parent.parent
    id0 = here / "run" / "ID0"
    ap = argparse.ArgumentParser()
    ap.add_argument("--src",  default=str(id0/"heretic.wad"))
    ap.add_argument("--base", default=str(id0/"doom2.wad"), help="IWAD for the base sprite namespace + target palette")
    ap.add_argument("--out",  default=str(id0/"hereticstuff.wad"))
    a = ap.parse_args()
    sp, sb, op = Path(a.src), Path(a.base), Path(a.out)
    for p in (sp, sb):
        if not p.exists():
            print(f"ERROR: {p} not found", file=sys.stderr); return 2

    hdata, hent = read_wad(sp)
    bdata, bent = read_wad(sb)
    xl = build_xlat(pal(hdata, hent), pal(bdata, bent))   # heretic -> doom palette
    bn = [n for n, _, _ in bent]

    # base = the whole DOOM2 S_START..S_END (so the range stays a superset)
    bs0, bs1 = bn.index("S_START"), bn.index("S_END")
    out = [(bent[i][0], bdata[bent[i][1]: bent[i][1]+bent[i][2]]) for i in range(bs0, bs1)]  # incl S_START, excl S_END

    # + heretic monster sprites: every lump whose 4-char code is in SPRITE_RENAME,
    #   palette-converted and renamed (FOO + suffix -> NEWCODE + suffix).
    n_spr = 0
    for nm, fp, sz in hent:
        code = nm[:4]
        if code in SPRITE_RENAME and len(nm) > 4:
            new = SPRITE_RENAME[code] + nm[4:]
            out.append((new[:8], remap_patch(hdata[fp:fp+sz], xl)))
            n_spr += 1
    out.append(("S_END", b""))

    # + heretic monster sounds (DMX, verbatim).  Names kept; heretic.c references them.
    n_snd = 0
    want = ("imp", "mum", "bst", "clk", "snk", "kgt", "wiz", "hed", "minsit",
            "minat", "mindth", "minact", "minpai", "sbtsit", "sorzap")
    for nm, fp, sz in hent:
        raw = hdata[fp:fp+sz]
        if is_dmx(raw) and any(k in nm.lower() for k in want):
            out.append((nm, raw)); n_snd += 1

    op.parent.mkdir(parents=True, exist_ok=True)
    write_wad(op, out)
    total = sum(len(d) for _n, d in out)
    print(f"extract_heretic_monsters: wrote {op}")
    print(f"  base sprites (DOOM2 namespace): {bs1-bs0}")
    print(f"  heretic monster sprites (converted+renamed): {n_spr}")
    print(f"  heretic monster sounds: {n_snd}")
    print(f"  total: {len(out)} lumps, {total/1024/1024:.1f} MB")
    print(f"  renamed codes (mirror in files/heretic.c): " +
          ", ".join(f"{k}->{v}" for k, v in SPRITE_RENAME.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
