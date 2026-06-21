#!/bin/bash
#
# start_aibuddy.sh -- launch aiDoom with the AI/LLM co-op buddy.
#
# Starts BOTH halves: the game with -aicoop + the AI-director TCP server, and the
# native director (run/director) that drives the buddy's (and the monsters')
# tactics each cycle via a local Ollama model.  Closing the director window (or
# Ctrl-C) stops the game.
#
#   offline rule-based buddy instead?   ./start_buddy.sh        (no LLM)
#   full launcher (model warmup, etc.)? ./start_aidoom.sh --aicoop
#
# Ollama host/port/model come from aidoom.cfg; override with:
#   ./start_aibuddy.sh --ollama http://192.168.2.114:11434 --model qwen3:8b
# Any other args pass straight to the game (default warp: MAP01):
#   ./start_aibuddy.sh -warp 3 1 -skill 4
#
set -u
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

PORT=31666

# Ollama defaults from aidoom.cfg (next to this script), then built-in fallbacks.
HOST=$(awk '$1=="ollama_host"{print $2}'  aidoom.cfg 2>/dev/null | tail -1)
OPORT=$(awk '$1=="ollama_port"{print $2}' aidoom.cfg 2>/dev/null | tail -1)
MODEL=$(awk '$1=="ollama_model"{print $2}' aidoom.cfg 2>/dev/null | tail -1)
[ -n "$HOST" ]  || HOST=192.168.2.114
[ -n "$OPORT" ] || OPORT=11434
[ -n "$MODEL" ] || MODEL=mistral:7b-instruct
OLLAMA="http://$HOST:$OPORT"

# Parse a few overrides; everything else goes to the game.
GAME_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --ollama) OLLAMA="$2"; shift 2;;
        --model)  MODEL="$2";  shift 2;;
        --port)   PORT="$2";   shift 2;;
        *)        GAME_ARGS+=("$1"); shift;;
    esac
done

# Locate the binaries (run/ first, then the build dirs).
for c in ./aidoom ../files/aidoom; do [ -x "$c" ] && { AIDOOM="$c"; break; }; done
[ -n "${AIDOOM:-}" ] || { echo "[aibuddy] aidoom not found -- build it: ./build.sh" >&2; exit 1; }
DIRBIN=./director; [ -x "$DIRBIN" ] || DIRBIN=../tools/director
[ -x "$DIRBIN" ]   || { echo "[aibuddy] director not found -- build it: tools/build_director.sh" >&2; exit 1; }

# Default-warp to MAP01 and default to skill 4 unless the caller passed their own.
warp=0; skill=0
for a in ${GAME_ARGS[@]+"${GAME_ARGS[@]}"}; do
    [ "$a" = "-warp" ]  && warp=1
    [ "$a" = "-skill" ] && skill=1
done
[ "$skill" = 1 ] || GAME_ARGS=(-skill 4 ${GAME_ARGS[@]+"${GAME_ARGS[@]}"})
[ "$warp"  = 1 ] || GAME_ARGS=(-warp 1 1 ${GAME_ARGS[@]+"${GAME_ARGS[@]}"})

echo "[aibuddy] game:     $AIDOOM -aicoop -aidirector $PORT ${GAME_ARGS[*]}"
"$AIDOOM" -aicoop -aidirector "$PORT" "${GAME_ARGS[@]}" &
GAME_PID=$!
trap 'kill "$GAME_PID" 2>/dev/null' EXIT INT TERM

sleep 2   # let the game open the listening socket
echo "[aibuddy] director: $DIRBIN --port $PORT --model $MODEL --ollama $OLLAMA"
"$DIRBIN" --port "$PORT" --model "$MODEL" --ollama "$OLLAMA"
