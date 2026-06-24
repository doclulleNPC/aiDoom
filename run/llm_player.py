#!/usr/bin/env python3
"""
llm_player.py -- the brain for aiDoom's full-LLM player mode (-aiplayer).

Connects to the game's agent socket, asks it for `observe` (one JSON line of game
state), hands that to an Ollama LLM, and sends back ONE high-level intent command.
The game's C reflex controller (files/g_agent.c) turns each intent into the per-tic
ticcmds (aim, step-toward, fire windows), so the marine reacts at 35 Hz while the LLM
plans at its own (slower) pace.

Env:
  AIPLAYER_PORT  game agent port           (default 31700)
  OLLAMA_URL     Ollama server             (default http://192.168.2.114:11434)
  OLLAMA_MODEL   model name                (default llama3.1:8b)
  DECISION_SECS  seconds between decisions (default 1.0)
"""
import json, os, socket, sys, time, urllib.request

PORT   = int(os.environ.get("AIPLAYER_PORT", "31700"))
OLLAMA = os.environ.get("OLLAMA_URL", "http://192.168.2.114:11434")
MODEL  = os.environ.get("OLLAMA_MODEL", "llama3.1:8b")
GAP    = float(os.environ.get("DECISION_SECS", "1.0"))

SYSTEM = """You are the brain of a DOOM marine. Each turn you receive the game state as
JSON (player x/y/angle/health/ammo, and "things": nearby monsters/items with id, type,
dist in map units, rel = angle to them in degrees [-180..180], hp, vis=in-line-of-sight).
A "buddy" field (if not null) is your FRIENDLY co-op marine -- an ally, never a target;
the engine already holds your fire when the buddy is in your line of fire.
Reply with exactly ONE command line, nothing else. Commands:
  target <id>     attack that thing (auto-aims it); pick a visible (vis:1) monster
  attack 1        hold fire (auto-aims the nearest visible monster)
  attack 0        stop firing
  goto <x> <y>    walk to a map point (the engine paths there)
  face <x> <y>    turn to look at a point
  weapon <1-8>    switch weapon (1 fist,2 pistol,3 shotgun,4 chaingun,5 rocket,6 plasma,7 bfg)
  use             open a door / press a switch in front
  stop            stop moving and firing
Strategy: if a monster is visible, target it; otherwise explore to find the exit, and
`use` doors/switches. Reply with ONLY the command."""

VERBS = ("target", "attack", "goto", "face", "weapon", "use", "stop", "turn")


def extract_cmd(reply):
    """Pull a known command out of a possibly-chatty LLM reply (8B models add prose)."""
    low = reply.lower().replace("`", " ").replace("*", " ")
    for seg in low.replace(",", "\n").splitlines():
        seg = seg.strip()
        for v in VERBS:
            if seg.startswith(v):
                return seg
    for v in VERBS:                      # else: first verb appearing anywhere
        i = low.find(v)
        if i >= 0:
            return low[i:].splitlines()[0].strip()
    return ""


def ollama(prompt):
    body = json.dumps({
        "model": MODEL, "system": SYSTEM, "prompt": prompt, "stream": False,
        "options": {"temperature": 0.3, "num_predict": 24},
    }).encode()
    req = urllib.request.Request(OLLAMA + "/api/generate", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())["response"].strip()


def main():
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    except OSError as e:
        print(f"[llm_player] cannot connect to 127.0.0.1:{PORT} ({e}) -- is the game up "
              f"with -aiplayer {PORT}?", file=sys.stderr)
        return 1
    f = s.makefile("rb")
    print(f"[llm_player] connected :{PORT}  ollama={OLLAMA}  model={MODEL}", flush=True)
    while True:
        try:
            s.sendall(b"observe\n")
            line = f.readline()
        except OSError:
            print("[llm_player] game closed the socket -- exiting")
            return 0
        if not line:
            return 0
        try:
            state = json.loads(line)
        except ValueError:
            continue
        if not state.get("player"):
            time.sleep(GAP); continue
        try:
            reply = ollama("STATE: " + line.decode().strip() + "\nYour single command:")
        except Exception as e:
            print(f"[llm_player] ollama error: {e}", file=sys.stderr)
            time.sleep(2); continue
        cmd = extract_cmd(reply)
        if cmd:
            print("[llm_player] >", cmd, flush=True)
            try:
                s.sendall((cmd + "\n").encode())
            except OSError:
                return 0
        else:
            print("[llm_player] (no command in reply:", repr(reply[:50]), ")", flush=True)
        time.sleep(GAP)


if __name__ == "__main__":
    sys.exit(main())
