#!/usr/bin/env python3
"""
extract_hexen.py -- build run/ID0/hexenstuff.wad: the Hexen MONSTERS and WEAPONS
(sprites palette-converted to the DOOM palette + their sounds) so they can later be
ported into DOOM by a files/hexen.c, exactly like extract_heretic_monsters.py +
files/heretic.c did for Heretic.

The engine MERGES sprite namespaces (R_InitSpriteLumps), so this WAD carries ONLY its
own Hexen sprites in one S_START..S_END -- they get ADDED to the IWAD's sprites.

Collisions: Hexen 4-char sprite codes (e.g. PLAY, FX12) would clash with DOOM / the
Heretic stuff, so every extracted code is RENAMED into an "X.." (heXen) namespace.
The rename is deterministic and collision-free; the full map is printed and written to
tools/hexen_sprite_map.txt so the C port (hexen.c) can use the SAME codes.

Sounds are copied VERBATIM (Hexen lump names are descriptive and up to 8 chars, so they
can't take a "DS" prefix like Heretic's did); naming them for the engine's `ds%s` lookup
is a porting-step decision.  The selected names are printed for reference.

Usage:
    python3 tools/extract_hexen.py                       # src ID0/hexen.wad, base = a DOOM IWAD in ID0
    python3 tools/extract_hexen.py --base ID0/DOOM.WAD --out ID0/hexenstuff.wad
"""

import argparse
import struct
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Hexen MONSTER sprite codes (body + gibs/variants + their projectiles).
# ---------------------------------------------------------------------------
MONSTER_SPRITES = [
    "ETTN", "ETTB",                                         # ettin (+ mace ball)
    "CENT", "CTXD", "CTFX", "CTDP",                         # centaur / slaughtaur (+ shield fx)
    "DEMN", "DEMA", "DEMB", "DEMC", "DEMD", "DEME", "DMFX", # chaos serpent (green) + fx
    "DEM2", "DMBA", "DMBB", "DMBC", "DMBD", "DMBE", "D2FX", # chaos serpent (brown) + fx
    "WRTH", "WRT2", "WRBL",                                 # reiver (wraith) + bone shot
    "MNTR", "FX12", "FX13", "MNSM",                         # minotaur (dark servant) + mace/floor-fire/smoke
    "SSPT", "SSDV", "SSXD", "SSFX",                         # stalker (+ dive/death/fx)
    "BISH", "BPFX",                                         # dark bishop + crozier shot
    "DRAG", "DRFX",                                         # death wyvern (dragon) + fx
    "FDMN", "FDMB",                                         # afrit (fire demon) + fireball
    "ICEY", "ICPR", "ICWS", "ICEC",                         # wendigo + ice shards + frozen-corpse shatter
    "SORC", "SBMP", "SBS1", "SBS2", "SBS3", "SBS4",         # heresiarch (sorcerer) + spell balls
    "SBMB", "SBMG", "SBFX",
    "KORX", "ABAT",                                         # korax (final boss) + bats
    "PIGY",                                                 # pig (morph)
    "FDTH",                                                 # generic burning death
]

# ---------------------------------------------------------------------------
# Hexen WEAPON sprite codes: per-class HUD weapons (W*), pickups/pieces (A*), projectiles.
# ---------------------------------------------------------------------------
WEAPON_SPRITES = [
    # Fighter: gauntlets, Timon's axe, hammer (+thrown), Quietus
    "FPCH", "WFAX", "FAXE", "FSFX", "WFHM", "FHMR", "FHFX", "FSRD",
    # Cleric: mace, serpent staff, firestorm, wraithverge (holy spirits)
    "CMCE", "WCSS", "CSSF", "WCFM", "CFLM", "CFFX", "CHLY", "SPIR",
    # Mage: wand, arc of death (lightning), bloodscourge, frost shards
    "MWND", "WMLG", "MLNG", "MLFX", "MLF2", "MSTF", "MSP1", "MSP2", "CONE", "SHEX",
    # Mage HUD weapon sprites
    "WFR1", "WFR2", "WFR3", "WCH1", "WCH2", "WCH3", "WMS1", "WMS2", "WMS3", "WPIG", "WMCS",
    # 4th-weapon assembly pieces (the Guardian artifacts)
    "AFWP", "ACWP", "AMWP", "AGER", "AGR2", "AGR3", "AGR4",
]

# ---------------------------------------------------------------------------
# Hexen MONSTER sounds wired into files/hexen.c.  The engine looks a sound up as
# "ds<sfxname>" (i_sound.c), so each chosen Hexen DMX lump is copied under a NEW
# name "DS"+<short>, where <short> (<=6 chars, so DS+short <= the 8-byte lump cap)
# is the sfx tag in files/sounds.c / sounds.h (sfx_x_*).  Mapping below is:
#     short-sfx-name -> source Hexen lump (from hexen.wad SNDINFO / crispy-doom).
# Mirror these names exactly in files/sounds.{c,h} and files/hexen.c.
MONSTER_SOUNDS = {
    # Ettin (EttinSight/Active=cent2, Pain=cent1, Attack=ethit1, Death=cntdth1)
    "xetsit": "cent2",   "xetpai": "cent1",   "xetatk": "ethit1",  "xetdth": "cntdth1",
    # Centaur (taur1/taur2/taur4/centhit2/cntdth1)
    "xcesit": "taur1",   "xceact": "taur2",   "xcepai": "taur4",
    "xceatk": "centhit2","xcedth": "cntdth1",
    # Slaughtaur (centaur leader): leader attack = cntshld4
    "xslatk": "cntshld4",
    # Chaos Serpent / Demon (sbtsit5 sight+active, minact1 pain, dematk2 atk, sbtdth3 death)
    "xdesit": "sbtsit5", "xdepai": "minact1", "xdeatk": "dematk2", "xdedth": "sbtdth3",
    # Fire Demon / Afrit (active=fired5, pain=fired2, attack=spit6, death=fired3; FX hit=firedhit)
    "xfdact": "fired5",  "xfdpai": "fired2",  "xfdatk": "spit6",
    "xfddth": "fired3",  "xfdhit": "firedhit",
    # Wraith / Reiver (raith5a/raith3/raith4a/raith1b/rathdth2)
    "xwrsit": "raith5a", "xwract": "raith3",  "xwrpai": "raith4a",
    "xwratk": "raith1b", "xwrdth": "rathdth2",
    # Dark Bishop (sight=syab2d, active=stb1d, pain=bshpn1, attack=pop, death=bishdth1; FX=bshhit2)
    "xbisit": "syab2d",  "xbiact": "stb1d",   "xbipai": "bshpn1",
    "xbiatk": "pop",     "xbidth": "bishdth1","xbihit": "bshhit2",
    # Ice Guy / Wendigo (sight+active=frosty1, attack=frosty2; FX explode=shards1b; no pain/death sfx)
    "xicsit": "frosty1", "xicatk": "frosty2", "xichit": "shards1b",
    # Stalker / Serpent (sight=wtrcrt7, active=srfc3, pain=serppn1, attack=wtrswip, death=srpdth1; FX hit=glbhit4)
    "xstsit": "wtrcrt7", "xstact": "srfc3",   "xstpai": "serppn1",
    "xstatk": "wtrswip", "xstdth": "srpdth1", "xsthit": "glbhit4",
    # Death Wyvern / Dragon (sight+active=dragsit1, pain=dragpn2, attack=mage4, death=dragdie2; FX=mageball)
    "xdrsit": "dragsit1","xdrpai": "dragpn2", "xdratk": "mage4",
    "xdrdth": "dragdie2","xdrhit": "mageball",
}

# ---------------------------------------------------------------------------
# Hexen monster/weapon SOUND lump-name keywords (Hexen SFX names are descriptive,
# not <thing><action>).  A DMX lump whose name contains any of these is copied
# VERBATIM (kept for reference / future weapon work; the wired-up monster sounds
# go through MONSTER_SOUNDS above as DS* lumps).
# ---------------------------------------------------------------------------
SOUND_KEYWORDS = (
    # monsters
    "cent", "cnt", "eth", "taur", "minact", "mindth", "minpain", "minsit",
    "kor", "serp", "srp", "demat", "raith", "rath", "wrbl", "drag", "sor", "sbt",
    "bish", "bsh", "fired", "fdmn", "pig", "icedth", "icemv", "icebrk", "frosty",
    "icpr", "vamp", "bats", "glbh", "srfc", "mumpun", "squeal", "slurp", "shlurp",
    # weapons
    "axe", "ham", "hmhit", "punch", "sword", "holy", "spirt", "clhmm", "mageball",
    "wand", "blastr", "mage4", "cone3", "gnt", "wepele", "strike1", "strike3",
    # player-class pain / death / effort
    "fgt", "mgpain", "mgdth", "mggrunt", "mgxdth", "mgfall", "mghmm", "mgcdth",
    "clxdth", "plrdth", "plrpain", "plrburn", "plrcdth",
)


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


def make_rename(codes):
    """Deterministic, collision-free 4-char rename into the 'X' (heXen) namespace.
    XYZ stem = 'X'+code[:2]; 4th char tries code[2], code[3], then 2..9/A..Z."""
    used, ren = set(), {}
    for code in codes:
        stem = "X" + code[:2]
        for c in [code[2], code[3]] + list("23456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"):
            cand = (stem + c)[:4]
            if cand not in used:
                used.add(cand); ren[code] = cand; break
    return ren


def find_base(id0, given):
    """Resolve the IWAD whose PLAYPAL is the target palette."""
    if given:
        p = Path(given)
        return p if p.is_absolute() else (id0 / given)
    for name in ("doom2.wad", "DOOM2.WAD", "doom.wad", "DOOM.WAD", "plutonia.wad", "tnt.wad"):
        if (id0 / name).exists():
            return id0 / name
    return id0 / "DOOM.WAD"


def main():
    here = Path(__file__).resolve().parent.parent
    id0 = here / "run" / "ID0"
    ap = argparse.ArgumentParser()
    ap.add_argument("--src",  default=str(id0/"hexen.wad"))
    ap.add_argument("--base", default=None, help="IWAD for the target palette (auto-detected in ID0 if omitted)")
    ap.add_argument("--out",  default=str(id0/"hexenstuff.wad"))
    a = ap.parse_args()
    sp, sb, op = Path(a.src), find_base(id0, a.base), Path(a.out)
    for p in (sp, sb):
        if not p.exists():
            print(f"ERROR: {p} not found", file=sys.stderr); return 2

    hdata, hent = read_wad(sp)
    bdata, bent = read_wad(sb)
    xl = build_xlat(pal(hdata, hent), pal(bdata, bent))   # hexen -> doom palette (base = target)

    # which selected sprite codes actually exist in hexen.wad's sprite namespace?
    present = set(nm[:4] for nm, fp, sz in hent if len(nm) > 4)
    wanted = [c for c in (MONSTER_SPRITES + WEAPON_SPRITES) if c in present]
    missing = [c for c in (MONSTER_SPRITES + WEAPON_SPRITES) if c not in present]
    ren = make_rename(wanted)

    out = [("S_START", b"")]
    n_spr = 0
    for nm, fp, sz in hent:
        code = nm[:4]
        if code in ren and len(nm) > 4:
            new = ren[code] + nm[4:]
            out.append((new[:8], remap_patch(hdata[fp:fp+sz], xl)))
            n_spr += 1
    out.append(("S_END", b""))

    # WIRED-UP monster sounds: copy each chosen Hexen DMX lump under "DS"+<short>
    # so the engine's "ds%s" lookup (i_sound.c) finds it for sfx_x_* (files/sounds.c).
    by_name = {nm.upper(): (fp, sz) for nm, fp, sz in hent}
    n_ds = 0
    ds_missing = []
    for short, srclump in MONSTER_SOUNDS.items():
        key = srclump.upper()
        if key not in by_name:
            ds_missing.append(srclump); continue
        fp, sz = by_name[key]
        raw = hdata[fp:fp+sz]
        if not is_dmx(raw):
            ds_missing.append(srclump + "(not-dmx)"); continue
        out.append(("DS" + short.upper()[:6], raw)); n_ds += 1

    # monster / weapon sounds (DMX), copied verbatim (reference, not wired up).
    n_snd = 0
    snd_names = []
    seen = set()
    for nm, fp, sz in hent:
        raw = hdata[fp:fp+sz]
        if nm not in seen and is_dmx(raw) and any(k in nm.lower() for k in SOUND_KEYWORDS):
            out.append((nm[:8], raw)); n_snd += 1; snd_names.append(nm); seen.add(nm)

    # tag as an aiDoom-internal asset pack so the launcher hides it from the user PWAD list
    out.insert(0, ("AISTUFF", b"aiDoom internal asset pack -- loaded by the game, not a user PWAD\n"))
    op.parent.mkdir(parents=True, exist_ok=True)
    write_wad(op, out)

    # sidecar rename map for the future hexen.c port
    mapfile = here / "tools" / "hexen_sprite_map.txt"
    with open(mapfile, "w") as f:
        f.write("# Hexen sprite code -> hexenstuff.wad code (use these in files/hexen.c)\n")
        cat = {c: "monster" for c in MONSTER_SPRITES}
        cat.update({c: "weapon" for c in WEAPON_SPRITES})
        for c in wanted:
            f.write(f"{c} -> {ren[c]}   ({cat.get(c,'?')})\n")
        if missing:
            f.write("\n# not present in this hexen.wad (skipped): " + ", ".join(missing) + "\n")

    total = sum(len(d) for _n, d in out)
    print(f"extract_hexen: wrote {op}")
    print(f"  base palette: {sb.name}")
    print(f"  monster/weapon sprites (converted+renamed): {n_spr}  ({len(wanted)} codes)")
    print(f"  WIRED monster sounds (DS* lumps for sfx_x_*): {n_ds}")
    if ds_missing:
        print(f"    WARNING: missing source lumps for DS sounds: {', '.join(ds_missing)}")
    print(f"  monster/weapon sounds (verbatim, reference): {n_snd}")
    print(f"  total: {len(out)} lumps, {total/1024/1024:.1f} MB")
    print(f"  sprite rename map -> {mapfile.relative_to(here)}")
    if missing:
        print(f"  NOTE: {len(missing)} selected codes absent in this IWAD: {', '.join(missing)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
