#!/usr/bin/env python3
# gen_id24.py -- generate files/id24_gen.h from the official ID24 data tables
# (docs/ID24 0.99.1/.../id24data.cpp).  Emits aiDoom-format sprite/sound/state/
# mobj tables with cross-references self-describingly ENCODED so the runtime
# installer (files/id24.c) can resolve them against the dynamically-grown tables.
#
# Encoding of a cross-reference to id24 content:
#   v = -1000000000 - (type*100000000 + rel)      type: 0=spr 1=state 2=mobj 3=sfx
# so every encoded ref is in [-1.4e9, -1e9].  Vanilla indices (small positive),
# 0/-1 (none), and literal fixed-point args (|v| < ~2e8) all pass through as-is.
import re, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
CPP  = os.path.join(HERE, "..", "docs", "ID24 0.99.1",
                    "ID24 data tables and supporting code 0.99.1", "id24data.cpp")
OUT  = os.path.join(HERE, "..", "files", "id24_gen.h")

ENC_BASE = 1000000000
def enc(t, rel): return -(ENC_BASE + t*100000000 + rel)
def is_hash(v):  return isinstance(v, int) and v <= -1000000000

text = open(CPP, "r", encoding="utf-8", errors="replace").read()

def table_region(name):
    m = re.search(r"id24%s\[\]\s*=\s*" % name, text)
    i = text.index("{", m.end())
    depth = 0
    for j in range(i, len(text)):
        if text[j] == '{': depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0: return text[i+1:j]
    raise RuntimeError("unterminated table " + name)

def strip_comments(s):
    s = re.sub(r"//[^\n]*", "", s)
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return s

def split_rows(region):
    """Top-level {...} groups -> list of inner strings."""
    s = strip_comments(region)
    rows, depth, start = [], 0, None
    for j, c in enumerate(s):
        if c == '{':
            if depth == 0: start = j+1
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: rows.append(s[start:j])
    return rows

def split_fields(row):
    """Split a row into fields by top-level commas (respect nested {} and strings)."""
    out, depth, cur, instr = [], 0, "", False
    for c in row:
        if instr:
            cur += c
            if c == '"': instr = False
            continue
        if c == '"': instr = True; cur += c
        elif c == '{': depth += 1; cur += c
        elif c == '}': depth -= 1; cur += c
        elif c == ',' and depth == 0:
            out.append(cur.strip()); cur = ""
        else: cur += c
    if cur.strip(): out.append(cur.strip())
    return out

def num(tok):
    tok = tok.strip().strip("{}").strip()
    if tok in ("features_id24", "features_boom", "features_mbf", "features_mbf21",
               "features_dehextra", "features_dsdhacked"):
        return 0
    if tok == "nullptr" or tok == "": return 0
    if tok == "true": return 1
    if tok == "false": return 0
    m = re.match(r"^-?\d+$", tok)
    if m: return int(tok)
    return tok  # a string literal or A_ name

def strlit(tok):
    m = re.search(r'"([^"]*)"', tok)
    return m.group(1) if m else ""

# ---- sprites: { id, features, "NAME", } ----
sprites, spr_map = [], {}
for r in split_rows(table_region("sprites")):
    f = split_fields(r)
    if len(f) < 3: continue
    sid = num(f[0]); name = strlit(f[2])
    spr_map[sid] = len(sprites); sprites.append(name)

# ---- sounds: { id, features, "NAME", sing, prio, link, pitch, vol, data } ----
sounds, sfx_map = [], {}
for r in split_rows(table_region("sounds")):
    f = split_fields(r)
    if len(f) < 5: continue
    sid = num(f[0]); name = strlit(f[2]); prio = num(f[4])
    sfx_map[sid] = len(sounds); sounds.append((name, prio if isinstance(prio,int) else 127))

# ---- states (assign rel idx first, then resolve) ----
state_rows = split_rows(table_region("states"))
state_map = {}
for i, r in enumerate(state_rows):
    f = split_fields(r)
    state_map[num(f[0])] = i

# ---- mobjs ----
mobj_rows = split_rows(table_region("mobjs"))
mobj_map = {}
for i, r in enumerate(mobj_rows):
    f = split_fields(r)
    mobj_map[num(f[0])] = i

def ref(v, kind):
    """kind: 'spr'|'state'|'mobj'|'sfx' -> encoded or literal."""
    if not is_hash(v): return v
    tbl = {'spr':(spr_map,0), 'state':(state_map,1), 'mobj':(mobj_map,2), 'sfx':(sfx_map,3)}[kind]
    m, t = tbl
    if v not in m:
        sys.stderr.write("WARN unresolved %s ref %d\n" % (kind, v)); return 0
    return enc(t, m[v])

# per-codepointer arg reference types (which arg index -> kind); misc1/misc2 = idx 0,1
ARG_REFS = {
    "A_PlaySound":        {"misc": {0:'sfx'}},
    "A_MonsterProjectile":{"args": {0:'mobj'}},
    "A_WeaponProjectile": {"args": {0:'mobj'}},
    "A_SpawnObject":      {"args": {0:'mobj'}},
    "A_MonsterMeleeAttack":{"args": {3:'sfx'}},
    "A_WeaponSound":      {"args": {0:'sfx'}},
    "A_WeaponMeleeAttack":{"args": {4:'sfx'}},
    "A_GunFlashTo":       {"args": {0:'state'}},
    "A_RefireTo":         {"args": {0:'state'}},
    "A_WeaponJump":       {"args": {0:'state'}},
    "A_RandomJump":       {"misc": {0:'state'}},   # vanilla-style: misc1=state, misc2=chance
    "A_CheckAmmo":        {"args": {0:'state'}},
    "A_JumpIfHealthBelow":{"args": {0:'state'}},
}

# state_t field order in id24data: id, feat, sprite, frame, tics, {action}, next,
# {misc1}, {misc2}, mbf21flags, {arg1..8}, tranmap
states = []
for r in state_rows:
    f = split_fields(r)
    sprite = ref(num(f[2]), 'spr')
    frame  = num(f[3]); tics = num(f[4])
    action = num(f[5])                       # A_name or 0
    action = action if isinstance(action, str) else "0"
    nxt    = ref(num(f[6]), 'state')
    misc1  = num(f[7]); misc2 = num(f[8])
    flags  = num(f[9])
    args   = [num(f[10+k]) for k in range(8)]
    # resolve any id24-hash misc/args per codepointer semantics; error if unmapped
    spec = ARG_REFS.get(action, {})
    for idx, val in enumerate([misc1, misc2]):
        if is_hash(val):
            k = spec.get("misc", {}).get(idx)
            if not k: sys.stderr.write("ERR %s misc%d hash unmapped\n"%(action,idx+1)); k='sfx'
            v = ref(val, k)
            if idx==0: misc1=v
            else: misc2=v
    for idx in range(8):
        if is_hash(args[idx]):
            k = spec.get("args", {}).get(idx)
            if not k: sys.stderr.write("ERR %s arg%d hash unmapped\n"%(action,idx+1)); k='mobj'
            args[idx] = ref(args[idx], k)
    states.append((sprite, frame, tics, action, nxt, misc1, misc2, flags, args))

# mobjinfo field order (id24): type,feat, doomednum,spawnstate,spawnhealth,seestate,
# seesound,reactiontime,attacksound,painstate,painchance,painsound,meleestate,
# missilestate,deathstate,xdeathstate,deathsound,speed,radius,height,mass,damage,
# activesound,flags,raisestate, fastspeed,meleerange,infightinggroup,projectilegroup,
# splashgroup,mbf21flags,ripsound, id24flags,minrespawntics,respawndice,dropthing,...
mobjs = []
for r in mobj_rows:
    f = split_fields(r)
    g = [num(x) for x in f]
    ded = g[2]
    if isinstance(ded, int) and ded < 0: ded = ded & 0xFFFF   # id24 ednum -> unsigned16
    m = dict(
        doomednum=ded, spawnstate=ref(g[3],'state'), spawnhealth=g[4],
        seestate=ref(g[5],'state'), seesound=ref(g[6],'sfx'), reactiontime=g[7],
        attacksound=ref(g[8],'sfx'), painstate=ref(g[9],'state'), painchance=g[10],
        painsound=ref(g[11],'sfx'), meleestate=ref(g[12],'state'),
        missilestate=ref(g[13],'state'), deathstate=ref(g[14],'state'),
        xdeathstate=ref(g[15],'state'), deathsound=ref(g[16],'sfx'), speed=g[17],
        radius=g[18], height=g[19], mass=g[20], damage=g[21],
        activesound=ref(g[22],'sfx'), flags=g[23], raisestate=ref(g[24],'state'),
        fastspeed=g[25], meleerange=g[26], infight=g[27], projgrp=g[28],
        splashgrp=g[29], mbf21flags=g[30],
        dropthing=(ref(g[35],'mobj') if is_hash(g[35]) else g[35]))
    mobjs.append(m)

# ---------------------------------------------------------------- emit
def cnum(v): return str(v)
o = []
o.append("// GENERATED by tools/gen_id24.py from the official ID24 0.99.1 data tables.")
o.append("// Do not edit by hand -- re-run the generator.  See files/id24.c for install.")
o.append("#ifndef __ID24_GEN__\n#define __ID24_GEN__\n")
o.append("#define ID24_NSPR   %d" % len(sprites))
o.append("#define ID24_NSFX   %d" % len(sounds))
o.append("#define ID24_NSTATE %d" % len(states))
o.append("#define ID24_NMOBJ  %d\n" % len(mobjs))

o.append("static const char* const id24_sprnames[ID24_NSPR] = {")
o.append("  " + ", ".join('"%s"' % s for s in sprites))
o.append("};\n")
o.append("static const struct { const char* name; int prio; } id24_sfx[ID24_NSFX] = {")
o.append("  " + ", ".join('{"%s",%d}' % (n,p) for (n,p) in sounds))
o.append("};\n")

o.append("// {sprite, frame, tics, \"action\", nextstate, misc1, misc2, flags, arg0..7}")
o.append("static const struct { int sprite,frame,tics; const char* action; int next,misc1,misc2,flags,args[8]; } id24_st[ID24_NSTATE] = {")
for (sp,fr,ti,ac,nx,m1,m2,fl,ar) in states:
    o.append("  {%d,%d,%d,\"%s\",%d,%d,%d,%d,{%s}}," % (sp,fr,ti,ac,nx,m1,m2,fl, ",".join(cnum(a) for a in ar)))
o.append("};\n")

o.append("static const struct { int doomednum,spawnstate,spawnhealth,seestate,seesound,reactiontime,attacksound,painstate,painchance,painsound,meleestate,missilestate,deathstate,xdeathstate,deathsound,speed,radius,height,mass,damage,activesound,flags,raisestate,fastspeed,meleerange,infight,projgrp,splashgrp,mbf21flags,dropthing; } id24_mobj[ID24_NMOBJ] = {")
for m in mobjs:
    o.append("  {%(doomednum)d,%(spawnstate)d,%(spawnhealth)d,%(seestate)d,%(seesound)d,%(reactiontime)d,%(attacksound)d,%(painstate)d,%(painchance)d,%(painsound)d,%(meleestate)d,%(missilestate)d,%(deathstate)d,%(xdeathstate)d,%(deathsound)d,%(speed)d,%(radius)d,%(height)d,%(mass)d,%(damage)d,%(activesound)d,%(flags)d,%(raisestate)d,%(fastspeed)d,%(meleerange)d,%(infight)d,%(projgrp)d,%(splashgrp)d,%(mbf21flags)d,%(dropthing)d}," % m)
o.append("};\n#endif")

open(OUT, "w", encoding="utf-8").write("\n".join(o) + "\n")
sys.stderr.write("gen_id24: %d sprites, %d sounds, %d states, %d mobjs -> %s\n"
                 % (len(sprites), len(sounds), len(states), len(mobjs), OUT))
