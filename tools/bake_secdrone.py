#!/usr/bin/env python3
# bake_secdrone.py -- add the Security Drone assets (from run/ID0/SecurityDrone.pk3)
# into run/ID0/buddydoom.wad, IN PLACE and idempotently:
#   * sprites  MNDR*/SHT1*/POW1*  (PNG in the pk3) -> DOOM patch_t, inserted into
#     buddydoom.wad's S_START..S_END sprite namespace so R_InitSprites picks them up.
#   * sounds   SECDRON1..4        (OGG in the pk3) -> DMX (11025 Hz, 8-bit mono),
#     added as lumps DSSECDR1..4 so the engine's getsfx() ("ds"+name) finds them.
#
# Re-run after any rebuild of buddydoom.wad.  Existing lumps are overwritten in place;
# new ones are inserted.
#
#   python3 tools/bake_secdrone.py
#
import os, struct, sys, io, subprocess
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PK3  = os.path.join(ROOT, "run", "ID0", "SecurityDrone.pk3")
WAD  = os.path.join(ROOT, "run", "ID0", "buddydoom.wad")

# (pk3 sprite lump, aidoom.wad lump).  DOOM sprite frames must be contiguous from
# frame 'A'; the pk3's POW1 impact frames are F..J, so remap them to A..E.
SPRITES = [(n, n) for n in
           ["MNDRA1","MNDRA2","MNDRA3","MNDRA4","MNDRA5","MNDRA6","MNDRA7","MNDRA8",
            "MNDRB1","MNDRB2","MNDRB3","MNDRB4","MNDRB5","MNDRB6","MNDRB7","MNDRB8",
            "MNDRC0","MNDRD0","MNDRE0","MNDRF0","MNDRG0","MNDRH0","SHT1A0","SHT1B0"]] + \
          [("POW1F0","POW1A0"),("POW1G0","POW1B0"),("POW1H0","POW1C0"),
           ("POW1I0","POW1D0"),("POW1J0","POW1E0")]
# (pk3 sound name, aidoom.wad lump name)  -- lump = "ds"+sfxname, sfxname<=6 chars.
SOUNDS = [("SECDRON1","DSSECDR1"), ("SECDRON2","DSSECDR2"),
          ("SECDRON3","DSSECDR3"), ("SECDRON4","DSSECDR4")]

# ---------------------------------------------------------------------------
def find_iwad():
    for p in ["run/ID0/DOOM.WAD","run/ID0/DOOM2.WAD","run/ID0/doom.wad",
              "run/ID0/doom2.wad","run/ID0/freedoom1.wad","run/ID0/freedoom2.wad"]:
        fp = os.path.join(ROOT, p)
        if os.path.exists(fp): return fp
    sys.exit("bake_secdrone: no IWAD under run/ID0/ for PLAYPAL")

def read_lumps(path):
    with open(path,"rb") as f: data=f.read()
    magic,num,off = struct.unpack("<4sii", data[:12])
    lumps=[]
    for i in range(num):
        lo,ls,nm = struct.unpack("<ii8s", data[off+i*16:off+i*16+16])
        lumps.append([nm.split(b"\x00")[0].decode("latin1"), data[lo:lo+ls]])
    return magic, lumps

def read_playpal(iwad):
    for name,d in read_lumps(iwad)[1]:
        if name.upper()=="PLAYPAL": return d[:768]
    sys.exit("bake_secdrone: PLAYPAL not found")

def grab_from_png(b):
    i=8
    while i<len(b):
        ln=struct.unpack(">I",b[i:i+4])[0]; typ=b[i+4:i+8]
        if typ==b"grAb": return struct.unpack(">ii",b[i+8:i+16])
        i+=12+ln
    return None

def build_nearest(playpal):
    pal=[(playpal[i*3],playpal[i*3+1],playpal[i*3+2]) for i in range(256)]
    cache={}
    def nearest(rgb):
        v=cache.get(rgb)
        if v is None:
            r,g,b=rgb; best=0; bd=1<<30
            for i,(pr,pg,pb) in enumerate(pal):
                d=(r-pr)**2+(g-pg)**2+(b-pb)**2
                if d<bd: bd=d; best=i;  # noqa
                if d==0: break
            v=cache[rgb]=best
        return v
    return nearest

def png_to_patch(b, nearest):
    img=Image.open(io.BytesIO(b))
    if img.mode!="P": img=img.convert("P")
    w,h=img.size; pal=img.getpalette() or []
    tinfo=img.info.get("transparency",None); trans=set()
    if isinstance(tinfo,int): trans.add(tinfo)
    elif isinstance(tinfo,(bytes,bytearray)): trans={i for i,a in enumerate(tinfo) if a==0}
    px=img.load(); remap={}
    def mp(idx):
        if idx in trans: return None
        v=remap.get(idx)
        if v is None: v=remap[idx]=nearest((pal[idx*3],pal[idx*3+1],pal[idx*3+2]))
        return v
    lo,to = grab_from_png(b) or (w//2,h)
    header=struct.pack("<hhhh",w,h,lo,to)
    cols=bytearray(); colofs=[]; base=len(header)+4*w
    for x in range(w):
        colofs.append(base+len(cols)); y=0
        while y<h:
            while y<h and mp(px[x,y]) is None: y+=1
            if y>=h: break
            top=y; run=bytearray()
            while y<h and mp(px[x,y]) is not None:
                run.append(mp(px[x,y])); y+=1
                if len(run)==254: break
            cols.append(top&0xff); cols.append(len(run)); cols.append(0); cols+=run; cols.append(0)
        cols.append(0xff)
    return header + b"".join(struct.pack("<i",o) for o in colofs) + bytes(cols)

def ogg_to_dmx(ogg_bytes):
    # decode OGG -> mono 11025 Hz unsigned-8 PCM via ffmpeg, wrap in a DMX header.
    p = subprocess.run(["ffmpeg","-hide_banner","-loglevel","error","-i","pipe:0",
                        "-ac","1","-ar","11025","-f","u8","-acodec","pcm_u8","pipe:1"],
                       input=ogg_bytes, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode!=0:
        sys.exit("bake_secdrone: ffmpeg failed: "+p.stderr.decode("latin1","ignore"))
    pcm=p.stdout
    return struct.pack("<HHI",3,11025,len(pcm)) + pcm

def zip_read(zippath, name):
    import zipfile
    with zipfile.ZipFile(zippath) as z:
        for n in z.namelist():
            if n.split("/")[-1]==name:
                return z.read(n)
    return None

def write_wad(path, magic, lumps):
    with open(path,"wb") as f:
        f.write(magic); f.write(struct.pack("<i",len(lumps))); f.write(struct.pack("<i",0))
        ent=[]
        for name,data in lumps:
            off=f.tell(); f.write(data); ent.append((off,len(data),name))
        diroff=f.tell()
        for off,sz,name in ent:
            f.write(struct.pack("<ii",off,sz)); f.write(name.encode("ascii")[:8].ljust(8,b"\x00"))
        f.seek(8); f.write(struct.pack("<i",diroff))

def main():
    if not os.path.exists(PK3): sys.exit(f"bake_secdrone: {PK3} not found")
    if not os.path.exists(WAD): sys.exit(f"bake_secdrone: {WAD} not found")
    nearest=build_nearest(read_playpal(find_iwad()))
    magic,lumps=read_lumps(WAD)
    names={n.upper():idx for idx,[n,_] in enumerate(lumps)}

    # Build the new sprite patches + sound DMX lumps.
    newspr=[]
    for src,dst in SPRITES:
        raw=zip_read(PK3, src)
        if raw is None: sys.exit(f"bake_secdrone: sprite {src} missing from pk3")
        newspr.append((dst, png_to_patch(raw, nearest)))
    newsnd=[]
    for src,dst in SOUNDS:
        raw=zip_read(PK3, src)
        if raw is None: sys.exit(f"bake_secdrone: sound {src} missing from pk3")
        newsnd.append((dst, ogg_to_dmx(raw)))

    # Overwrite existing lumps in place; collect the rest for insertion.
    def put_or_defer(name, data, defer):
        i=names.get(name.upper())
        if i is not None: lumps[i][1]=data
        else: defer.append([name,data])
    ins_spr=[]; ins_snd=[]
    for name,data in newspr: put_or_defer(name,data,ins_spr)
    for name,data in newsnd: put_or_defer(name,data,ins_snd)

    # Insert new sprites just before S_END (inside the sprite namespace).
    if ins_spr:
        send=None
        for idx,[n,_] in enumerate(lumps):
            if n.upper()=="S_END": send=idx
        if send is None: sys.exit("bake_secdrone: no S_END marker in buddydoom.wad")
        lumps[send:send]=ins_spr
    # New sounds: append at the very end (order doesn't matter for by-name lookup).
    if ins_snd:
        lumps.extend(ins_snd)

    write_wad(WAD, magic, lumps)
    print(f"bake_secdrone: {len(newspr)} sprites (+{len(ins_spr)} new), "
          f"{len(newsnd)} sounds (+{len(ins_snd)} new) -> {WAD}")

if __name__=="__main__":
    main()
