#!/bin/sh
# start_llm_player.sh -- FULL LLM mode: an Ollama LLM drives EVERYTHING --
#   * the MARINE   via -aiplayer  + llm_player.py        (player socket, port 31700)
#   * the BUDDY + MONSTERS + pacing via -aidirector -aicoop + the `director` sidecar
#                                                          (director socket, port 31666)
# Launches the game (you watch the window) and both LLM clients, and starts the
# director sidecar if it's present.  Both clients talk to the same Ollama server.
#
#   ./start_llm_player.sh [IWAD] [E M]      e.g.  ./start_llm_player.sh DOOM.WAD 1 1
#
# Env: PLAYER_PORT (31700), DIRECTOR_PORT (31666), OLLAMA_URL/OLLAMA_MODEL (player brain),
#      DECISION_SECS.  The director sidecar reads its Ollama host/model from run/aidoom.cfg
#      (or its own defaults: localhost / ministral-3:8b).
cd "$(dirname "$0")" || exit 1

IWAD="${1:-DOOM.WAD}"
EP="${2:-1}"
MAP="${3:-1}"
PPORT="${PLAYER_PORT:-31700}"
DPORT="${DIRECTOR_PORT:-31666}"
export AIPLAYER_PORT="$PPORT"

[ -x ./aidoom ] || { echo "aidoom not found in run/ -- build it first (./build.sh)"; exit 1; }

# director sidecar (LLM monsters + buddy).  Optional: without it the engine still runs
# its built-in rule director (FSM) for monsters, but the LLM won't drive them or the buddy.
DIRBIN=./director; [ -x ./director.exe ] && DIRBIN=./director.exe
HAVE_DIR=0; [ -x "$DIRBIN" ] && HAVE_DIR=1

echo "=== aiDoom FULL LLM mode (E${EP}M${MAP}) ==="
echo "  marine : -aiplayer $PPORT  + llm_player.py   (ollama ${OLLAMA_URL:-http://localhost:11434})"
if [ "$HAVE_DIR" = 1 ]; then
  echo "  buddy + monsters : -aidirector $DPORT -aicoop + $DIRBIN"
else
  echo "  buddy + monsters : $DIRBIN missing -> built-in rule director only (no LLM monsters/buddy)"
fi

# The game: marine socket + (LLM) director socket + an AI buddy to command.
./aidoom -iwad "$IWAD" -warp "$EP" "$MAP" -skill 3 \
         -aiplayer "$PPORT" -aidirector "$DPORT" -aicoop -nofriendlyfire &
GAME=$!

# Let the engine open both sockets, then attach the two LLM brains.
sleep 2
PIDS=""
if [ "$HAVE_DIR" = 1 ]; then
  "$DIRBIN" --port "$DPORT" >/dev/null 2>&1 &
  PIDS="$PIDS $!"
fi
python3 ./llm_player.py &
PIDS="$PIDS $!"

# When the game window closes, stop the brains (and vice-versa).
wait "$GAME"
kill $PIDS 2>/dev/null
echo "=== LLM mode ended ==="
