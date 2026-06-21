#!/usr/bin/env python3
"""
bake_buddy_voice.py -- pre-render the aiDoom AI co-op buddy's spoken lines
from ElevenLabs TTS, and pack them into a Doom PWAD (`buddy.wad`) so the
engine can play them offline with no network at runtime.

Why a WAD: the engine already has a single-file lump archive loader
(`files/w_wad.c`); shipping the voice lines as lumps is consistent with
every other asset the engine loads, and a single file is much nicer
to distribute than 37 loose .ogg files.  See CLAUDE.md / LEGACY_FIXES.md
("offline buddy voice via stb_vorbis + dedicated SDL audio stream").

Output:
    run/buddy.wad              (PWAD with ~130 DS* OGG/Vorbis lumps + a VOICEMAP
                                text lump carrying the lump<->phrase mapping)
    run/buddy_voice_manifest.txt  (same mapping, as a loose file)

Phrases: the original 37 (callouts/state/replies/status) PLUS event callouts
(kill/dodge/dry/barrel/crit/taunt/bigmon/edge/jump/door/stuck/lost/ff/plhurt/
pldown/healed/god/arm/lvlstart/lvlclear/idle/...), Joker-HL voice.

Pipeline: ElevenLabs returns MP3 -> ffmpeg transcodes to OGG/Vorbis -> packed into
the WAD.  API key from ~/.hermes/.env (or --key / $ELEVENLABS_API_KEY), NEVER aidoom.cfg.

ElevenLabs:
    - voice id        configurable; default Joker-HL (matches in-game default)
    - model           eleven_turbo_v2_5  (fast, callout-grade latency)
    - output_format   ogg_vorbis         (engine decodes with stb_vorbis)
    - voice_settings  stability 0.4 / similarity 0.85 / style 0.5 (per phrase)

Usage:
    export ELEVENLABS_API_KEY=sk_...
    python3 tools/bake_buddy_voice.py                       # full bake
    python3 tools/bake_buddy_voice.py --voice zmclHrhV...   # different voice
    python3 tools/bake_buddy_voice.py --out run/buddy.wad   # custom path
    python3 tools/bake_buddy_voice.py --dry-run             # print phrases, no API

Idempotent: skips phrases whose OGG already exists in a cache directory,
so re-runs only fetch what changed.  Use --force to redownload all.
"""

import argparse
import hashlib
import json
import os
import struct
import sys
import time
import urllib.request
import urllib.error
from pathlib import Path


# ---------- phrase table (37 entries, must match engine's runtime mapping) ----------

# Lump names are exactly 8 ASCII chars (Doom WAD limit); we use the same `DS` prefix
# as the engine's existing SFX lumps so the i_voice.c loader can range over `ds*`
# lumps in the buddy.wad namespace without colliding with game-SFX lumps (which
# live in the IWAD, not in buddy.wad).
def _lump(name: str) -> str:
    """Truncate/pad to exactly 8 chars (uppercase ASCII) per Doom WAD spec.
    Truncation silently merges lumps -- this raises instead so the bake
    script fails loudly on a typo rather than producing a WAD with collisions."""
    n = name.upper().replace("-", "").replace("'", "").replace(".", "")
    if len(n) > 8:
        raise ValueError(f"lump name '{name}' is {len(n)} chars (>8); "
                         f"shorten the prefix or the discriminator")
    return (n + "        ")[:8]


# Each entry: (lump_name_8char, phrase_to_speak)
# Auto-callouts (10, from p_ai_coop.c:714-716, rotated by rate limiter).
# Lump names are exactly 8 chars (Doom WAD spec): "DSCT" + "001".."004" for
# contact, "DSHR" + "01".."03" for hurt, "DSCL" + "01".."03" for clear.
CALLOUTS = [
    ("DSCT001", "Contact!"),
    ("DSCT002", "Tango, engaging!"),
    ("DSCT003", "I see one!"),
    ("DSCT004", "Got movement!"),
    ("DSHR01",  "I'm hit!"),
    ("DSHR02",  "Taking fire!"),
    ("DSHR03",  "I need health!"),
    ("DSCL01",  "Area clear."),
    ("DSCL02",  "All quiet."),
    ("DSCL03",  "Watch our six."),
]

# Where-state report (6, from p_ai_coop.c:368-369 `what[]`); HP/distance spoken
# by the screen HUD already, so the voice only carries the semantic state.
STATES = [
    ("DSWFOLLOW", "Following you."),
    ("DSWFIGHT",  "Fighting."),
    ("DSWHEAL",   "Getting health."),
    ("DSWHOLD",   "Holding position."),
    ("DSWCOME",   "Coming to you."),
    ("DSWGRAB",   "Grabbing an item."),
]

# Console replies (5, from c_console.c:262-264, p_ai_coop.c:412/428/432)
REPLIES = [
    ("DSSUMONOK", "On my way!"),
    ("DSWAITHD",  "Holding position."),
    ("DSWAITMV",  "Moving out."),
    ("DSATACK",   "Attacking!"),
    ("DSATNONE",  "No targets around."),
]

# Status report (16 = 9 weapons x {plain, with-ammo}); weapon names from
# p_ai_coop.c:438-439 `wn[NUMWEAPONS]`.  Numbers (HP/armor/ammo) appear on
# the HUD; the voice just announces the weapon.
#
# 4-letter weapon codes that are unique within the 8-char budget and don't
# collide with each other.  Plain = name only, Ammo = name + " loaded".
# Lump name = "DSST" + code + ("P"|"A")  ->  must be <= 8 chars.
# "DSST" (4) + code (3) + P/A (1) = 8 chars max -> use 3-char codes.
WEAPON_CODES = [
    ("FIS",  "fists",          "Fists."),
    ("PIS",  "pistol",         "Pistol."),
    ("SHT",  "shotgun",        "Shotgun."),
    ("CHG",  "chaingun",       "Chaingun."),
    ("RCK",  "rocketlauncher", "Rocket launcher."),
    ("PLS",  "plasma",         "Plasma rifle."),
    ("BFG",  "bfg",            "B. F. G."),
    ("CSW",  "chainsaw",       "Chainsaw."),
    ("SSH",  "supershotgun",   "Super shotgun."),
]
WEAPONS_WITH_AMMO = {"PIS", "SHT", "CHG", "RCK", "PLS", "BFG", "SSH"}

STATUS_LUMPS = []
for code, _wkey, wname in WEAPON_CODES:
    STATUS_LUMPS.append((_lump("DSST" + code + "P"), wname))
    if code in WEAPONS_WITH_AMMO:
        STATUS_LUMPS.append((_lump("DSST" + code + "A"), wname + " Loaded."))

# Extra event callouts (rotated by AICoop_Callout).  Tag = prefix + index (e.g.
# "kill:0"); lump <= 8 chars; mapping mirrored in files/i_voice.c.  Joker-HL voice.
EVENTS = [
    # -- combat
    ("DSKL01", "Got him!"), ("DSKL02", "Down!"), ("DSKL03", "Scratch one."), ("DSKL04", "Stay down."),
    # Duke-style monster-specific kill quips -- one per monster type (rare, see NoteKill).
    ("DSKI01", "I pimped that imp!"), ("DSKI02", "Imp? More like wimp."), ("DSKI03", "Hot-footed that imp."),
    ("DSKZM01", "Zombie down."),               # zombieman
    ("DSKSG01", "Thanks for the shells!"),      # shotgun guy
    ("DSKCG01", "Quit hoggin' the chaingun."),  # chaingunner
    ("DSKPK01", "Bad dog!"),                    # pinky / demon
    ("DSKSC01", "I see you, fuzzy."),           # spectre
    ("DSKSL01", "Lost soul, meet floor."),      # lost soul
    ("DSKCD01", "Eat dirt, meatball!"),         # cacodemon
    ("DSKPE01", "No more skull spam."),         # pain elemental
    ("DSKHK01", "Knight, night!"),              # hell knight
    ("DSKBN01", "Baron? Barely."),              # baron of hell
    ("DSKRV01", "Rest in pieces, bonehead!"),   # revenant
    ("DSKMC01", "Lay off the donuts."),         # mancubus
    ("DSKAR01", "Squashed that bug."),          # arachnotron
    ("DSKMM01", "Big spider, big splat."),      # spider mastermind
    ("DSKCY01", "Tim-ber!"),                    # cyberdemon
    ("DSKAV01", "Stay dead this time!"),        # arch-vile
    ("DSKNS01", "Wrong game, pal."),            # SS nazi
    ("DSKKN01", "Door's open!"),                # commander keen
    ("DSDG01", "Incoming!"), ("DSDG02", "Whoa!"), ("DSDG03", "Not today."),
    ("DSDRY01", "I'm dry!"), ("DSDRY02", "Out of ammo!"), ("DSDRY03", "Need a reload!"),
    ("DSBR01", "Not near that barrel!"), ("DSBR02", "Barrel -- hold up."), ("DSBR03", "That thing'll blow."),
    ("DSSP01", "On a roll!"), ("DSSP02", "Can't stop me!"), ("DSSP03", "They keep coming!"), ("DSSP04", "Damn, I'm good."),
    ("DSGB01", "Chunky."), ("DSGB02", "Boom!"), ("DSGB03", "Cleanup on aisle hell."),
    ("DSCR01", "I'm dying here!"), ("DSCR02", "Critical -- cover me!"), ("DSCR03", "Patch me up, now!"),
    ("DSFS01", "Just my fists now."), ("DSFS02", "Knuckle up."),
    ("DSTN01", "Come get some!"), ("DSTN02", "Is that all?"), ("DSTN03", "I do this for fun."), ("DSTN04", "Rest in pieces!"),
    ("DSBIG01", "Big one!"), ("DSBIG02", "Oh, that's a Cyberdemon..."), ("DSBIG03", "We're gonna need more ammo."),
    ("DSFL01", "Behind you!"), ("DSFL02", "They're flanking!"), ("DSFL03", "On your six!"),
    ("DSIF01", "Let 'em fight."), ("DSIF02", "They're killing each other!"),
    # -- navigation / movement
    ("DSED01", "Careful -- nukage."), ("DSED02", "Watch the edge."), ("DSED03", "Easy, long drop."),
    ("DSJP01", "Hup!"), ("DSJP02", "Up we go."),
    ("DSDO01", "Door!"), ("DSDO02", "Opening up."),
    ("DSSK01", "I'm stuck!"), ("DSSK02", "Gimme a sec."), ("DSSK03", "Snagged on something."),
    ("DSLS01", "Where'd you go?"), ("DSLS02", "Wait up!"), ("DSLS03", "I lost you!"),
    ("DSLK01", "Locked. Need a key."), ("DSLK02", "Can't open this one."),
    ("DSCU01", "Crusher!"), ("DSCU02", "Don't get squished."),
    # -- co-op / player
    ("DSPH01", "You okay?!"), ("DSPH02", "I got you covered!"), ("DSPH03", "Fall back, I'll hold!"),
    ("DSPD01", "No! ...I'll avenge you."), ("DSPD02", "Stay down, I got this."),
    ("DSFF01", "Hey! Watch it!"), ("DSFF02", "Friendly fire!"), ("DSFF03", "That's MY blood, pal."),
    ("DSFF04", "Friendly fire, jackass!"), ("DSFF05", "I'm on YOUR side, genius!"), ("DSFF06", "Shoot them, not me!"),
    ("DSNC01", "Nice shot!"), ("DSNC02", "Good kill."),
    # -- items / power-ups / status
    ("DSPU01", "Nice, ammo!"), ("DSPU02", "Health -- sweet."), ("DSPU03", "Don't mind if I do."),
    ("DSHL01", "Patched up."), ("DSHL02", "Better. Let's go."),
    ("DSBK01", "Berserk! Get over here!"), ("DSBK02", "Now I'm untouchable!"),
    ("DSGOD01", "Invincible!"), ("DSGOD02", "Can't touch me."),
    ("DSARM01", "Loaded for bear!"), ("DSARM02", "Now we're talking."),
    # -- progression / banter
    ("DSLV01", "Fresh hell."), ("DSLV02", "Here we go again."), ("DSLV03", "Let's clear it."),
    ("DSWN01", "All dead. Nice."), ("DSWN02", "Find the exit."), ("DSWN03", "Hail to the king."),
    ("DSSE01", "Secret!"), ("DSSE02", "Ooh, hidden stash."),
    ("DSID01", "Quiet... too quiet."), ("DSID02", "Anything?"), ("DSID03", "Still with me?"), ("DSID04", "I hate the waiting."),
]
for _n, _p in EVENTS:
    _lump(_n)   # enforce the 8-char Doom lump limit at import time

PHRASES = CALLOUTS + STATES + REPLIES + STATUS_LUMPS + EVENTS
_allnames = [n for n, _ in PHRASES]
assert len(_allnames) == len(set(_allnames)), "duplicate lump name in PHRASES"


# ---------- ElevenLabs API ----------

# Buddy voice id ("Joker-HL").  Stored HERE in tools/ -- it's only needed for the
# offline bake; the game ships pre-baked OGGs (buddy.wad) and never does live TTS.
DEFAULT_VOICE = "wJmFT75XSkFKaBF1R0rX"
DEFAULT_MODEL = "eleven_turbo_v2_5"
# ElevenLabs does NOT support ogg_vorbis (their docs say so; we tried).
# We fetch MP3 (mp3_44100_128) and transcode to OGG/Vorbis with ffmpeg below.
API_URL = "https://api.elevenlabs.io/v1/text-to-speech/{voice}" \
          "?output_format=mp3_44100_128&optimize_streaming_latency=0"


def cfg_value(cfg_path, key):
    """Read a 'key value' / 'key \"value\"' line from aidoom.cfg."""
    try:
        with open(cfg_path) as f:
            for line in f:
                parts = line.strip().split(None, 1)
                if len(parts) == 2 and parts[0] == key:
                    return parts[1].strip().strip('"')
    except OSError:
        pass
    return None


def load_face_lumps(out_path):
    """Buddy HUD mugshots (run/buddyface/BUF*.lmp).  ALWAYS packed into buddy.wad so a
    voice re-bake never drops the BUF* faces the HUD needs (hu_buddy.c).  Same lumps as
    tools/bake_buddy_face.py -- voice bake includes them so you don't have to run that too."""
    faces_dir = Path(out_path).parent / "buddyface"
    lumps = [(f.stem.upper(), f.read_bytes()) for f in sorted(faces_dir.glob("BUF*.lmp"))]
    if not lumps:
        print(f"WARNING: no BUF*.lmp in {faces_dir} -- buddy HUD faces will be MISSING!",
              file=sys.stderr)
    else:
        print(f"  + {len(lumps)} BUF* HUD face lumps")
    return lumps


def env_file_value(path, *names):
    """Read KEY=VALUE from a dotenv file (e.g. ~/.hermes/.env), trying each name.
    Secrets (the ElevenLabs key) live HERE, never in aidoom.cfg."""
    try:
        with open(os.path.expanduser(path)) as f:
            env = {}
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                k = k.strip()
                if k.startswith("export "):
                    k = k[len("export "):].strip()
                env[k] = v.strip().strip('"').strip("'")
        for n in names:
            if env.get(n):
                return env[n]
    except OSError:
        pass
    return None


def fetch_mp3(phrase, voice, model, key, retries=3):
    """POST to ElevenLabs TTS, return raw MP3 bytes (mp3_44100_128)."""
    body = json.dumps({
        "text": phrase,
        "model_id": model,
        "voice_settings": {
            "stability": 0.4, "similarity_boost": 0.85, "style": 0.5,
        },
    }).encode()
    req = urllib.request.Request(
        API_URL.format(voice=voice),
        data=body,
        headers={"xi-api-key": key, "Content-Type": "application/json",
                 "Accept": "audio/mpeg"},
    )
    last = None
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            last = e
            if e.code in (429, 500, 502, 503, 504):
                time.sleep(2 ** attempt)
                continue
            raise
    raise last


def _find_ffmpeg():
    """Locate ffmpeg on PATH (we need it for MP3->OGG transcoding)."""
    from shutil import which
    for f in ("ffmpeg", "ffmpeg.exe"):
        if which(f):
            return f
    return None


def transcode_mp3_to_ogg(mp3_bytes, ffmpeg, trim_silence=True):
    """Transcode MP3 bytes to OGG/Vorbis bytes using ffmpeg (in-memory).
    With trim_silence=True (default), leading and trailing silence are
    stripped via the silenceremove filter -- ElevenLabs TTS often pads
    short phrases to ~30-42 seconds; without trim, "Contact!" would play
    for almost a minute."""
    import subprocess, tempfile
    with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as tmpi:
        tmpi.write(mp3_bytes); tmp_in = tmpi.name
    tmp_out = tmp_in[:-4] + ".ogg"
    try:
        # silenceremove: strip silence >0.05s below -35dB at start/end.
        # start_periods=1 means we keep audio up to the first non-silent
        # stretch and drop everything before; same for end.
        af = ("silenceremove=start_periods=1:start_duration=0.05:"
              "start_threshold=-35dB:stop_periods=-1:stop_duration=0.05:"
              "stop_threshold=-35dB") if trim_silence else "anull"
        cmd = [ffmpeg, "-y", "-loglevel", "error",
               "-i", tmp_in,
               "-af", af,
               "-c:a", "libvorbis", "-q:a", "5",  # ~96 kbps, good for speech
               tmp_out]
        subprocess.run(cmd, check=True, capture_output=True)
        with open(tmp_out, "rb") as f:
            return f.read()
    finally:
        for p in (tmp_in, tmp_out):
            try: os.unlink(p)
            except OSError: pass


def fetch_ogg(phrase, voice, model, key, ffmpeg, trim_silence=True):
    """End-to-end: fetch MP3 from ElevenLabs, return trimmed OGG/Vorbis bytes."""
    mp3 = fetch_mp3(phrase, voice, model, key)
    return transcode_mp3_to_ogg(mp3, ffmpeg, trim_silence=trim_silence)


# ---------- Doom PWAD writer ----------

# PWAD on-disk layout (little-endian), per Doom spec:
#   header:      "PWAD" (4) + numlumps (4) + diroffset (4)              = 12 bytes
#   lump data:   all lump bytes concatenated in directory order
#   directory:   per-lump entry = filepos (4) + size (4) + name (8)    = 16 bytes
# Directory is at the END of the file, not after the header.  Many loaders
# accept both, but the Doom spec is unambiguous and `W_CheckNumForName` in
# aiDoom's w_wad.c reads the directory from the file end (it was written
# that way originally because files were streamed in order).
def write_wad(out_path, lumps):
    """`lumps` is a list of (8-char-name, bytes); writes a Doom PWAD."""
    header_size = 12
    data_off = header_size
    # Lump data offsets are absolute within the file; compute them sequentially.
    entries = []
    cur = data_off
    for name, data in lumps:
        entries.append((cur, len(data), name))
        cur += len(data)
    dir_off = cur  # directory sits right after the lump data
    # Header
    with open(out_path, "wb") as f:
        f.write(b"PWAD")
        f.write(struct.pack("<II", len(lumps), dir_off))
        # Lump data (no directory in between)
        for _name, data in lumps:
            f.write(data)
        # Directory at end
        for filepos, size, name in entries:
            f.write(struct.pack("<II", filepos, size))
            f.write(name.encode("ascii", "replace")[:8].ljust(8, b"\x00"))


# ---------- main ----------

def main():
    ap = argparse.ArgumentParser(description="Bake aiDoom buddy voice into buddy.wad")
    ap.add_argument("--voice", default=None,
                    help="ElevenLabs voice id override (default: DEFAULT_VOICE in this "
                         "tool -- Joker-HL)")
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--key",   default=None)
    ap.add_argument("--cfg",   default="aidoom.cfg")
    ap.add_argument("--out",   default="run/buddy.wad",
                    help="output PWAD path (default: run/buddy.wad)")
    ap.add_argument("--cache", default=".buddy_voice_cache",
                    help="dir for raw OGGs (skips re-download on rerun)")
    ap.add_argument("--force", action="store_true",
                    help="redownload even if cached")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the phrase table, do not call ElevenLabs")
    ap.add_argument("--offline-test", action="store_true",
                    help="skip ElevenLabs; emit 0.3s sine-tone OGGs via ffmpeg "
                         "for smoke-testing the engine pipeline without API quota")
    ap.add_argument("--no-trim", action="store_true",
                    help="keep leading/trailing silence in OGGs "
                         "(default: trim via ffmpeg silenceremove)")
    args = ap.parse_args()

    # Voice id lives in tools/ (DEFAULT_VOICE below) -- it's only used here, offline,
    # when baking buddy.wad; the game just plays the pre-baked OGGs, never live TTS.
    # NOT in aidoom.cfg.  --voice can override for a one-off bake.
    args.voice = args.voice or DEFAULT_VOICE

    out_path = Path(args.out).resolve()
    cache_dir = Path(args.cache).resolve()

    if args.dry_run:
        print(f"voice={args.voice} model={args.model}")
        print(f"phrases ({len(PHRASES)}):")
        for name, phrase in PHRASES:
            print(f"  {name}  <-  {phrase!r}")
        print(f"would write -> {out_path}")
        return 0

    # Offline-test mode: synthesise sine-tone OGGs with ffmpeg.  Useful for
    # smoke-testing the engine's OGG-decode + audio-stream pipeline without
    # burning ElevenLabs quota.  No key required.
    if args.offline_test:
        import subprocess
        for f in ("ffmpeg", "ffmpeg.exe"):
            from shutil import which as _which
            if _which(f):
                FFMPEG = f
                break
        else:
            print("ERROR: --offline-test needs ffmpeg on PATH.", file=sys.stderr)
            return 2
        out_path.parent.mkdir(parents=True, exist_ok=True)
        cache_dir.mkdir(parents=True, exist_ok=True)
        lumps = []
        manifest_lines = []
        print(f"bake_buddy_voice: OFFLINE-TEST mode (sine-tone OGGs) -> {out_path}")
        for i, (name, phrase) in enumerate(PHRASES, 1):
            cached = cache_dir / f"{name}_offline.ogg"
            # 220 Hz base + 11 Hz per index so each phrase is a distinct tone
            freq = 220 + i * 11
            # No trim in offline-test (sine has no silence to remove).
            af = "anull"
            cmd = [FFMPEG, "-y", "-loglevel", "error",
                   "-f", "lavfi",
                   "-i", f"sine=frequency={freq}:duration=0.3",
                   "-af", af,
                   "-c:a", "libvorbis", "-q:a", "0",
                   str(Path(cached).resolve())]
            subprocess.run (cmd, check=True, capture_output=True)
            data = cached.read_bytes()
            lumps.append((name, data))
            manifest_lines.append(f"{name}\t{phrase}\toffline-sine({freq}Hz)\t{len(data)}\n")
            print(f"  [{i:2d}/{len(PHRASES)}] {name}  '{phrase}' -> {freq} Hz")
        lumps += load_face_lumps(out_path)	# buddy HUD mugshots -- always included
        manifest_txt = (f"OFFLINE-TEST MODE (sine tones)\n"
                        f"generated={time.strftime('%Y-%m-%d %H:%M:%S')}\n\n"
                        + "".join(manifest_lines))
        lumps.append(("VOICEMAP", manifest_txt.encode("utf-8")))
        write_wad(out_path, lumps)
        (out_path.parent / "buddy_voice_manifest.txt").write_text(manifest_txt)
        total = sum(len(d) for _n, d in lumps)
        print(f"\nbake_buddy_voice: wrote {out_path}  ({len(lumps)} lumps, {total} bytes)")
        return 0

    # Key precedence: --key > env var > ~/.hermes/.env.  NEVER aidoom.cfg -- secrets
    # don't belong in the game config (which can be shared / committed by accident).
    key = args.key or os.environ.get("ELEVENLABS_API_KEY") \
          or env_file_value("~/.hermes/.env",
                            "ELEVENLABS_API_KEY", "ELEVEN_API_KEY", "XI_API_KEY")
    if not key:
        print("ERROR: no ElevenLabs API key.  Put 'ELEVENLABS_API_KEY=sk_...' in "
              "~/.hermes/.env (preferred), export ELEVENLABS_API_KEY, or pass --key.",
              file=sys.stderr)
        return 2

    out_path.parent.mkdir(parents=True, exist_ok=True)
    cache_dir.mkdir(parents=True, exist_ok=True)

    print(f"bake_buddy_voice: voice={args.voice} model={args.model}")
    print(f"  cache={cache_dir}\n  out={out_path}")
    print(f"  {len(PHRASES)} phrases\n")

    lumps = []
    manifest_lines = []
    ffmpeg = _find_ffmpeg()
    if not ffmpeg:
        print("ERROR: ffmpeg not found on PATH (needed for MP3->OGG transcode).",
              file=sys.stderr)
        return 2
    for i, (name, phrase) in enumerate(PHRASES, 1):
        # Cache key is content-addressed on phrase+voice+model so different
        # voices land in separate files.
        h = hashlib.sha1(f"{args.voice}|{args.model}|{phrase}".encode()).hexdigest()[:16]
        cached = cache_dir / f"{name}_{h}.ogg"
        if cached.exists() and not args.force:
            data = cached.read_bytes()
            src = "cached"
        else:
            print(f"  [{i:2d}/{len(PHRASES)}] {name}  '{phrase}' ...", end="", flush=True)
            try:
                data = fetch_ogg(phrase, args.voice, args.model, key, ffmpeg,
                                  trim_silence=not args.no_trim)
            except Exception as e:
                print(f" FAILED: {e}", file=sys.stderr)
                return 3
            cached.write_bytes(data)
            print(f" {len(data)} bytes")
            src = "fetched"
            # Be polite to the API
            time.sleep(0.3)
        lumps.append((name, data))
        manifest_lines.append(f"{name}\t{phrase}\t{src}\t{len(data)}\n")

    lumps += load_face_lumps(out_path)		# buddy HUD mugshots -- always included

    # Pack the lump<->phrase mapping INTO the WAD as a text lump ("VOICEMAP") so the
    # WAD is self-documenting -- no external file needed to know what each lump says.
    manifest_txt = (f"voice={args.voice}\nmodel={args.model}\n"
                    f"generated={time.strftime('%Y-%m-%d %H:%M:%S')}\n\n"
                    + "".join(manifest_lines))
    lumps.append(("VOICEMAP", manifest_txt.encode("utf-8")))

    write_wad(out_path, lumps)

    # ... and a copy next to the WAD for reproducibility/grep.
    (out_path.parent / "buddy_voice_manifest.txt").write_text(manifest_txt)

    total = sum(len(d) for _n, d in lumps)
    print(f"\nbake_buddy_voice: wrote {out_path}  ({len(lumps)} lumps, {total} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())