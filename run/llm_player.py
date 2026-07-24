#!/usr/bin/env python3
"""
llm_player.py -- the brain for BuddyDoom's full-LLM player mode (-aiplayer).

Connects to the game's agent socket, polls observations at 10 Hz, triggers
the LLM only on key events (e.g. taking damage, new visible monsters, stuck,
or goal reached/timeout), and tracks recent history for stable, strategic planning.
"""
import json, os, socket, sys, time, urllib.request

PORT   = int(os.environ.get("AIPLAYER_PORT", "31700"))
OLLAMA = os.environ.get("OLLAMA_URL", "http://localhost:11434")
MODEL  = os.environ.get("OLLAMA_MODEL", "ministral-3:8b")

SYSTEM = """You are the brain of a DOOM marine. Each turn you receive the game state as
JSON (player status, exit coordinates, doors and their seen status, and "things": nearby monsters/items).
Your buddy is a FRIENDLY co-op marine.

Sensory Inputs:
- "lidar": 8 values (front, front-left, left, back-left, back, back-right, right, front-right) representing distances in map units to the nearest wall or obstacle. Use this for local navigation and avoiding walls.
- "sounds": a list of spatial sound events (e.g., shoot, item, door, growl) with distance and relative direction angle in degrees (positive is left, negative is right). Use this to locate hidden monsters or doors.
- "doors": list of doorways with their positions and "seen" state (1 if visited, 0 if unexplored).

Tactical guidelines:
- If a door is unopened (seen:0), you can explore it by sending "goto <x> <y>" with its coordinates.
- If you hear monsters (sound events) or see them (vis:1), target them or switch to a suitable weapon.
- Try to explore new rooms, collect items, open doors, and eliminate monsters.

Respond in this format:
THOUGHT: <brief reasoning about your current strategy, map exploration, and goals>
COMMAND: <exactly one command from the list below>

Commands:
  target <id>     attack that monster (auto-aims it); pick a visible (vis:1) monster
  attack 1        hold fire (auto-aims the nearest visible monster)
  attack 0        stop firing
  goto <x> <y>    walk to a map point (the engine paths there; e.g. toward the exit or an unseen door)
  face <x> <y>    turn to look at a point
  weapon <1-8>    switch weapon (1 fist, 2 pistol, 3 shotgun, 4 chaingun, 5 rocket, 6 plasma, 7 bfg)
  use             open a door / press a switch in front
  stop            stop moving and firing
"""

VERBS = ("target", "attack", "goto", "face", "weapon", "use", "stop")


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


def parse_reply(reply):
    """Extract THOUGHT and COMMAND out of the LLM response."""
    thought = ""
    cmd = ""
    for line in reply.splitlines():
        line_strip = line.strip()
        if line_strip.upper().startswith("THOUGHT:"):
            thought = line_strip[8:].strip()
        elif line_strip.upper().startswith("COMMAND:"):
            cmd = line_strip[8:].strip()
            
    if not cmd:
        # Fallback to general verb extractor
        cmd = extract_cmd(reply)
    if not thought:
        thought = "Engaged in combat/exploration."
    return thought, cmd


def ollama(prompt):
    body = json.dumps({
        "model": MODEL, "system": SYSTEM, "prompt": prompt, "stream": False,
        "options": {"temperature": 0.3, "num_predict": 64},
    }).encode()
    req = urllib.request.Request(OLLAMA + "/api/generate", data=body,
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())["response"].strip()


# Event Trigger State Variables
last_llm_time = 0
prev_state = None
current_cmd = "stop"
stuck_since = None
history = []  # list of {"thought": thought, "cmd": cmd}
MAX_HISTORY = 5
map_data = {}


def check_trigger(curr_state):
    global last_llm_time, prev_state, current_cmd, stuck_since
    now = time.time()
    
    if not prev_state:
        return True, "initial_state"
        
    # If waiting at door, only trigger on damage or near visible monsters (dist < 400)
    if curr_state.get("waiting_at_door") == True:
        stuck_since = None  # Reset stuck timer while waiting
        prev_hp = prev_state.get("player", {}).get("health", 100)
        curr_hp = curr_state.get("player", {}).get("health", 100)
        if curr_hp < prev_hp:
            return True, "player_damaged"
            
        curr_vis_monsters = [t for t in curr_state.get("things", []) if t.get("vis") == 1 and t.get("type") != "item" and t.get("dist") < 400]
        if curr_vis_monsters:
            return True, "monster_near"
            
        return False, None
        
    # 1. Timeout backup (at least run once every 3.0 seconds to keep updated)
    if now - last_llm_time > 3.0:
        return True, "timeout"
        
    # 2. Player took damage
    prev_hp = prev_state.get("player", {}).get("health", 100)
    curr_hp = curr_state.get("player", {}).get("health", 100)
    if curr_hp < prev_hp:
        return True, "player_damaged"
        
    # 3. Sighted a new visible threat
    prev_vis = {t["id"] for t in prev_state.get("things", []) if t.get("vis") == 1}
    curr_vis = {t["id"] for t in curr_state.get("things", []) if t.get("vis") == 1}
    if not curr_vis.issubset(prev_vis):
        return True, "new_monster_sighted"
        
    # 4. Target monster is dead/gone
    if current_cmd and current_cmd.startswith("target"):
        try:
            target_id = int(current_cmd.split()[1])
            curr_monsters = {t["id"] for t in curr_state.get("things", [])}
            if target_id not in curr_monsters:
                return True, "target_eliminated"
        except (ValueError, IndexError):
            pass
            
    # 5. Stuck detection (moving to goto, but coordinates are stable)
    if current_cmd and current_cmd.startswith("goto"):
        curr_pos = [curr_state.get("player", {}).get("x", 0), curr_state.get("player", {}).get("y", 0)]
        prev_pos = [prev_state.get("player", {}).get("x", 0), prev_state.get("player", {}).get("y", 0)]
        dist_moved = ((curr_pos[0] - prev_pos[0])**2 + (curr_pos[1] - prev_pos[1])**2)**0.5
        
        # 10 Hz check: if we moved less than 5 units in 100ms
        if dist_moved < 5.0:
            if stuck_since is None:
                stuck_since = now
            elif now - stuck_since > 1.5:
                stuck_since = None
                return True, "stuck_detected"
        else:
            stuck_since = None
            
    return False, None


def main():
    global last_llm_time, prev_state, current_cmd, history, map_data
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    except OSError as e:
        print(f"[llm_player] cannot connect to 127.0.0.1:{PORT} ({e}) -- is the game up "
              f"with -aiplayer {PORT}?", file=sys.stderr)
        return 1
        
    f = s.makefile("rb")
    print(f"[llm_player] connected :{PORT}  ollama={OLLAMA}  model={MODEL}", flush=True)

    # Fetch global static map information at startup
    try:
        s.sendall(b"map\n")
        map_line = f.readline()
        map_data = json.loads(map_line)
        print(f"[llm_player] Map loaded. Exit: {map_data.get('exit')}, Spawn: {map_data.get('start')}, Total doors: {len(map_data.get('doors', []))}", flush=True)
    except Exception as e:
        print(f"[llm_player] failed to load map: {e}", file=sys.stderr)
        map_data = {}
    
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
            time.sleep(0.1)
            continue
            
        if not state.get("player"):
            time.sleep(0.1)
            continue
            
        # Check event trigger conditions
        triggered, reason = check_trigger(state)
        
        if triggered:
            # Format short term memory
            hist_str = ""
            for idx, entry in enumerate(history):
                hist_str += f"{idx+1}. Thought: {entry['thought']} -> Action: {entry['cmd']}\n"
            if not hist_str:
                hist_str = "None (No previous actions)\n"
            
            # Format static map data context
            exit_coords = map_data.get('exit')
            spawn_coords = map_data.get('start')
            doors_list = map_data.get('doors', [])
            map_context = (
                f"LEVEL INFO:\n"
                f"  Player Spawn Location: {spawn_coords}\n"
                f"  Exit Switch Location: {exit_coords}\n"
                f"  Doors on Map: {doors_list}\n"
            )
                
            prompt = (
                f"{map_context}\n"
                f"RECENT ACTIONS MEMORY:\n{hist_str}\n"
                f"TRIGGER: {reason.upper()}\n"
                f"STATE: {line.decode().strip()}\n"
                f"Your single command:"
            )
            
            try:
                reply = ollama(prompt)
                last_llm_time = time.time()
            except Exception as e:
                print(f"[llm_player] ollama error: {e}", file=sys.stderr)
                time.sleep(1)
                continue
                
            thought, cmd = parse_reply(reply)
            
            if cmd:
                print(f"[llm_player] Trigger: {reason} | Thought: {thought} | Cmd: {cmd}", flush=True)
                try:
                    s.sendall((cmd + "\n").encode())
                    current_cmd = cmd
                    
                    # Update history
                    history.append({"thought": thought, "cmd": cmd})
                    if len(history) > MAX_HISTORY:
                        history.pop(0)
                except OSError:
                    return 0
            else:
                print(f"[llm_player] (no command in reply: {repr(reply[:50])})", flush=True)
                
        prev_state = state
        time.sleep(0.1)  # State polling frequency (10 Hz)


if __name__ == "__main__":
    sys.exit(main())
