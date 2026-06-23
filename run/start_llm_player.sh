#!/bin/sh
# start_llm_player.sh -- FULL LLM mode: an Ollama LLM drives the marine (-aiplayer) while
# the rule director (-director) spawns monsters L4D-style.  Launches the game (you watch
# the window) and the llm_player.py brain that feeds it intents.
#
#   ./start_llm_player.sh [IWAD] [E M]      e.g.  ./start_llm_player.sh DOOM.WAD 1 1
#
# Env: AIPLAYER_PORT (31700), OLLAMA_URL (http://192.168.2.114:11434), OLLAMA_MODEL,
#      DECISION_SECS.  Add -aidirector + the director sidecar yourself for LLM monsters.
cd "$(dirname "$0")" || exit 1

IWAD="${1:-DOOM.WAD}"
EP="${2:-1}"
MAP="${3:-1}"
PORT="${AIPLAYER_PORT:-31700}"
export AIPLAYER_PORT="$PORT"

if [ ! -x ./aidoom ]; then echo "aidoom not found in run/ -- build it first (./build.sh)"; exit 1; fi

echo "=== aiDoom FULL LLM mode ==="
echo "  marine: LLM via -aiplayer $PORT   monsters: -director   map: E${EP}M${MAP}"
echo "  ollama: ${OLLAMA_URL:-http://192.168.2.114:11434}  (override with OLLAMA_URL/OLLAMA_MODEL)"

# Game in the background so this script can run the brain; you watch the game window.
./aidoom -iwad "$IWAD" -warp "$EP" "$MAP" -skill 3 -aiplayer "$PORT" -director &
GAME=$!

# Give the engine a moment to open the agent socket, then drive it.
sleep 2
python3 ./llm_player.py &
BRAIN=$!

# When the game exits, stop the brain (and vice-versa).
wait "$GAME"
kill "$BRAIN" 2>/dev/null
echo "=== LLM mode ended ==="
