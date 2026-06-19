#!/usr/bin/env python3
# buddy_voice.py -- speaks the aiDoom AI co-op buddy's lines with ElevenLabs TTS.
#
# The game (files/p_ai_coop.c) appends each spoken line to buddy_say.txt in its
# working dir (run/).  This helper tails that file and, for every new line, calls
# the ElevenLabs text-to-speech API with the "DoomSlayer" voice and plays the audio.
# It never touches the game -- the engine stays deterministic and never blocks on
# the network.  Run it alongside the game:
#
#     cd run && python3 ../tools/buddy_voice.py        # (game writes buddy_say.txt here)
#
# API key (keep it secret -- never commit it):
#     export ELEVENLABS_API_KEY=sk_...                 # or
#     put 'elevenlabs_api_key  sk_...' in aidoom.cfg   # (gitignored)
#
# Stdlib only -- no pip install needed.

import os, sys, time, json, tempfile, subprocess, urllib.request, urllib.error, argparse

DEFAULT_VOICE = "zmclHrhV7VYdPfzybfK6"      # ElevenLabs "DoomSlayer"
DEFAULT_MODEL = "eleven_turbo_v2_5"         # fast, low latency for callouts
API = "https://api.elevenlabs.io/v1/text-to-speech/{vid}"


def cfg_value(cfg_path, key):
    """Read a 'key value' / 'key "value"' line from an aidoom.cfg-style file."""
    try:
        with open(cfg_path) as f:
            for line in f:
                parts = line.strip().split(None, 1)
                if len(parts) == 2 and parts[0] == key:
                    return parts[1].strip().strip('"')
    except OSError:
        pass
    return None


def find_player():
    """First available audio player for an mp3; returns an argv prefix or None."""
    for argv in (["ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet"],
                 ["mpg123", "-q"], ["mpv", "--no-video", "--really-quiet"],
                 ["cvlc", "--play-and-exit", "--intf", "dummy"], ["afplay"]):
        from shutil import which
        if which(argv[0]):
            return argv
    return None


def tts(text, key, voice, model):
    body = json.dumps({
        "text": text,
        "model_id": model,
        "voice_settings": {"stability": 0.4, "similarity_boost": 0.85, "style": 0.5},
    }).encode()
    req = urllib.request.Request(
        API.format(vid=voice), data=body,
        headers={"xi-api-key": key, "Content-Type": "application/json",
                 "Accept": "audio/mpeg"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return r.read()


def main():
    ap = argparse.ArgumentParser(description="Speak aiDoom buddy lines via ElevenLabs.")
    ap.add_argument("--file", default="buddy_say.txt", help="line file the game writes")
    ap.add_argument("--cfg", default="aidoom.cfg", help="config to read the API key from")
    ap.add_argument("--voice", default=DEFAULT_VOICE)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--key", default=None, help="ElevenLabs API key (else env/cfg)")
    args = ap.parse_args()

    key = args.key or os.environ.get("ELEVENLABS_API_KEY") or cfg_value(args.cfg, "elevenlabs_api_key")
    player = find_player()
    if not key:
        print("buddy_voice: no API key (set ELEVENLABS_API_KEY or elevenlabs_api_key in "
              "aidoom.cfg) -- printing lines only.", file=sys.stderr)
    if not player:
        print("buddy_voice: no audio player found (install ffmpeg/ffplay or mpg123).", file=sys.stderr)

    print(f"buddy_voice: watching {args.file} (voice {args.voice}, model {args.model})")
    # Wait for the file, then follow it from the end (skip backlog).
    while not os.path.exists(args.file):
        time.sleep(0.5)
    with open(args.file) as f:
        f.seek(0, os.SEEK_END)
        while True:
            line = f.readline()
            if not line:
                time.sleep(0.2)
                continue
            line = line.strip()
            if not line:
                continue
            print(f"[Buddy] {line}", flush=True)
            if not key:
                continue
            try:
                audio = tts(line, key, args.voice, args.model)
            except urllib.error.HTTPError as e:
                print(f"buddy_voice: ElevenLabs HTTP {e.code}: {e.read()[:200]!r}", file=sys.stderr)
                continue
            except Exception as e:
                print(f"buddy_voice: TTS error: {e}", file=sys.stderr)
                continue
            if not player:
                continue
            with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as tf:
                tf.write(audio)
                path = tf.name
            try:
                subprocess.run(player + [path], check=False)
            finally:
                try: os.unlink(path)
                except OSError: pass


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
