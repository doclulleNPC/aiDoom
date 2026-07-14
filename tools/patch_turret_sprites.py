#!/usr/bin/env python3
# patch_turret_sprites.py -- convert the turret sprite lumps (MTUR*) inside
# run/ID0/aidoom.wad from PNG to real DOOM patch_t format, IN PLACE.
#
# Actor sprites are drawn by the software sprite renderer, which reads patch_t
# column data directly (patch->width, patch->columnofs, posts).  The buddy HUD's
# BUF*/RARR* lumps can stay PNG because hu_buddy decodes them at runtime via
# V_CachePNG, but an *actor* sprite (MT_TURRET -> SPR_MTUR) must be patch_t.
#
# Re-run this after any rebuild of aidoom.wad that re-introduces the MTUR PNGs.
# It is idempotent: lumps already in patch format are left untouched.
#
#   python3 tools/patch_turret_sprites.py
#
import os, struct, sys, io
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WAD  = os.path.join(ROOT, "run", "ID0", "aidoom.wad")

def find_iwad():
    for p in ["run/ID0/DOOM.WAD", "run/ID0/DOOM2.WAD", "run/ID0/doom.wad",
              "run/ID0/doom2.wad", "run/ID0/freedoom1.wad", "run/ID0/freedoom2.wad"]:
        fp = os.path.join(ROOT, p)
        if os.path.exists(fp):
            return fp
    sys.exit("patch_turret_sprites: no IWAD under run/ID0/ for PLAYPAL")

def read_lumps(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, num, off = struct.unpack("<4sii", data[:12])
    lumps = []
    for i in range(num):
        lo, ls, nm = struct.unpack("<ii8s", data[off+i*16:off+i*16+16])
        name = nm.split(b"\x00")[0].decode("latin1")
        lumps.append([name, data[lo:lo+ls]])
    return magic, lumps

def read_playpal(iwad):
    _, lumps = read_lumps(iwad)
    for name, d in lumps:
        if name.upper() == "PLAYPAL":
            return d[:768]
    sys.exit("patch_turret_sprites: PLAYPAL not found")

def grab_from_png(b):
    i = 8
    while i < len(b):
        ln = struct.unpack(">I", b[i:i+4])[0]; typ = b[i+4:i+8]
        if typ == b"grAb":
            return struct.unpack(">ii", b[i+8:i+16])
        i += 12 + ln
    return None

def build_nearest(playpal):
    pal = [(playpal[i*3], playpal[i*3+1], playpal[i*3+2]) for i in range(256)]
    cache = {}
    def nearest(rgb):
        v = cache.get(rgb)
        if v is None:
            r, g, b = rgb; best = 0; bd = 1 << 30
            for i, (pr, pg, pb) in enumerate(pal):
                d = (r-pr)**2 + (g-pg)**2 + (b-pb)**2
                if d < bd:
                    bd = d; best = i
                    if d == 0: break
            v = cache[rgb] = best
        return v
    return nearest

def png_to_patch(b, nearest):
    img = Image.open(io.BytesIO(b))
    if img.mode != "P":
        img = img.convert("P")
    w, h = img.size
    pal = img.getpalette() or []
    tinfo = img.info.get("transparency", None)
    trans = set()
    if isinstance(tinfo, int):
        trans.add(tinfo)
    elif isinstance(tinfo, (bytes, bytearray)):
        trans = {i for i, a in enumerate(tinfo) if a == 0}
    px = img.load()

    remap = {}
    def mapidx(idx):
        if idx in trans:
            return None
        v = remap.get(idx)
        if v is None:
            v = remap[idx] = nearest((pal[idx*3], pal[idx*3+1], pal[idx*3+2]))
        return v

    grab = grab_from_png(b) or (w // 2, h)
    leftoffset, topoffset = grab

    header = struct.pack("<hhhh", w, h, leftoffset, topoffset)
    columns = bytearray()
    colofs = []
    base = len(header) + 4*w
    for x in range(w):
        colofs.append(base + len(columns))
        y = 0
        while y < h:
            while y < h and mapidx(px[x, y]) is None:
                y += 1
            if y >= h:
                break
            top = y
            run = bytearray()
            while y < h and mapidx(px[x, y]) is not None:
                run.append(mapidx(px[x, y])); y += 1
                if len(run) == 254:
                    break
            columns.append(top & 0xff)
            columns.append(len(run))
            columns.append(0)
            columns += run
            columns.append(0)
        columns.append(0xff)
    colbytes = b"".join(struct.pack("<i", o) for o in colofs)
    return header + colbytes + bytes(columns)

def write_wad(path, magic, lumps):
    with open(path, "wb") as f:
        f.write(magic)
        f.write(struct.pack("<i", len(lumps)))
        f.write(struct.pack("<i", 0))
        entries = []
        for name, data in lumps:
            off = f.tell(); f.write(data); entries.append((off, len(data), name))
        diroff = f.tell()
        for off, sz, name in entries:
            f.write(struct.pack("<ii", off, sz))
            f.write(name.encode("ascii")[:8].ljust(8, b"\x00"))
        f.seek(8); f.write(struct.pack("<i", diroff))

def is_png(b):
    return len(b) >= 4 and b[0] == 0x89 and b[1:4] == b"PNG"

def main():
    if not os.path.exists(WAD):
        sys.exit(f"patch_turret_sprites: {WAD} not found")
    nearest = build_nearest(read_playpal(find_iwad()))
    magic, lumps = read_lumps(WAD)

    changed = 0
    for lump in lumps:
        name, data = lump
        if name.upper().startswith("MTUR") and is_png(data):
            lump[1] = png_to_patch(data, nearest)
            print(f"  {name:9} {len(data):6}B PNG -> {len(lump[1]):6}B patch")
            changed += 1
        elif name.upper().startswith("MTUR"):
            print(f"  {name:9} already patch format -- left unchanged")

    if changed:
        write_wad(WAD, magic, lumps)
        print(f"patch_turret_sprites: converted {changed} MTUR lump(s) in {WAD}")
    else:
        print("patch_turret_sprites: nothing to do (no PNG MTUR lumps)")

if __name__ == "__main__":
    main()
