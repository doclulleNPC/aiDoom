#!/usr/bin/env python3
"""
Ollama AI Director for aiDoom.

Connects to aidoom's -aidirector TCP server, observes the game state, asks a
local Ollama model (mistral:7b-instruct) for tactical orders, and issues them.

Protocol (see AGENT_CONTROL.md 12-13 / p_ai_llm.c):
    observe\n  -> one JSON line {tic, player, monsters:[{id,type,pos,hp,see_player,order}], count}
    act order=<name> ids=1,2 for=<tics>\n
    wake\n / reset\n
Orders: chase hold fallback flank_left flank_right ambush focus_fire use_door

Usage:  python ollama_director.py [--port 31666] [--model mistral:7b-instruct]
"""
import socket, json, time, sys, argparse, urllib.request

ORDERS = {"chase", "hold", "fallback", "flank_left", "flank_right",
          "ambush", "focus_fire", "use_door"}

SYSTEM = (
    "You are the AI Director commanding the MONSTERS in a game of DOOM against "
    "the human player. You receive the live game state and assign each monster a "
    "tactical order. Think like a squad: pincer the player (some flank_left, some "
    "flank_right), have 1-2 strong monsters focus_fire, keep low-HP monsters back "
    "with fallback or hold, use ambush for monsters that can't see the player yet. "
    "Available orders: chase, hold, fallback, flank_left, flank_right, ambush, "
    "focus_fire, use_door. Respond ONLY with JSON of the form "
    '{"commands":[{"order":"flank_left","ids":[1,3]},{"order":"focus_fire","ids":[2]}]}. '
    "Every monster id must appear in exactly one command."
)


def ask_ollama(api, model, state):
    monsters = state.get("monsters", [])
    player = state.get("player", {})
    user = (f"Player: {json.dumps(player)}\n"
            f"Monsters ({len(monsters)}): {json.dumps(monsters)}\n"
            "Assign orders now as JSON.")
    body = json.dumps({
        "model": model,
        "stream": False,
        "format": "json",
        "options": {"temperature": 0.6},
        "messages": [
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": user},
        ],
    }).encode()
    req = urllib.request.Request(api, data=body,
                                 headers={"Content-Type": "application/json"})
    raw = urllib.request.urlopen(req, timeout=120).read()
    content = json.loads(raw)["message"]["content"]
    return json.loads(content)


def recv_line(sock):
    buf = bytearray()
    while not buf.endswith(b"\n"):
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf.decode("utf-8", "replace").strip()


def connect(port, tries=40):
    for i in range(tries):
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            if i == 0:
                print(f"[director] waiting for aidoom on 127.0.0.1:{port} ...")
            time.sleep(0.5)
    raise SystemExit(f"[director] could not connect to aidoom on port {port}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=31666)
    ap.add_argument("--model", default="mistral:7b-instruct")
    ap.add_argument("--ollama", default="http://localhost:11434/api/chat")
    ap.add_argument("--period", type=float, default=1.0,
                    help="seconds to wait between planning rounds")
    args = ap.parse_args()

    sock = connect(args.port)
    sock.settimeout(15)              # tolerate the game briefly not ticking
    print(f"[director] connected. model={args.model}")
    sock.sendall(b"wake\n")          # make the monsters engage the player
    try:
        recv_line(sock)              # "ok"
    except (TimeoutError, socket.timeout):
        pass

    round_no = 0
    while True:
        round_no += 1
        sock.sendall(b"observe\n")
        try:
            line = recv_line(sock)
        except (TimeoutError, socket.timeout):
            print(f"[round {round_no}] observe timed out (game paused/menu?), retrying")
            time.sleep(args.period)
            continue
        if not line:
            print("[director] connection closed by game.")
            break
        try:
            state = json.loads(line)
        except json.JSONDecodeError:
            time.sleep(args.period)
            continue

        valid = {m["id"] for m in state.get("monsters", [])}
        if not valid:
            print(f"[round {round_no}] no live monsters yet.")
            time.sleep(args.period)
            continue

        t0 = time.time()
        try:
            plan = ask_ollama(args.ollama, args.model, state)
        except Exception as e:
            print(f"[director] ollama error: {e}")
            time.sleep(args.period)
            continue
        dt = time.time() - t0

        issued = []
        for c in plan.get("commands", []):
            order = str(c.get("order", "")).strip().lower()
            ids = [str(i) for i in c.get("ids", [])
                   if isinstance(i, int) and i in valid]
            if order in ORDERS and ids:
                sock.sendall(f"act order={order} ids={','.join(ids)} for=105\n"
                             .encode())
                try:
                    recv_line(sock)  # "ok"
                except (TimeoutError, socket.timeout):
                    pass
                issued.append(f"{order}<-[{','.join(ids)}]")

        php = state.get("player", {}).get("health", "?")
        print(f"[round {round_no}] tic={state.get('tic')} "
              f"monsters={len(valid)} player_hp={php} "
              f"llm={dt:.1f}s -> {'  '.join(issued) if issued else '(no valid orders)'}")
        time.sleep(args.period)


if __name__ == "__main__":
    main()
