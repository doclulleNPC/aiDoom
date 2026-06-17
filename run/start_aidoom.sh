#!/usr/bin/env bash
#
# start_aidoom.sh -- wait for Ollama, then launch aiDoom (+ the LLM director).
# Linux/macOS counterpart of start_aidoom.bat / start_aidoom.ps1.
#
# Checks the Ollama server is up and the model is available, optionally warms it
# into memory, starts aidoom with the -aidirector TCP server, then runs the
# Python director client that drives the monsters.
#
# Usage:
#   ./start_aidoom.sh
#   ./start_aidoom.sh --model qwen3:8b --skill 4 --friendlyfire
#   ./start_aidoom.sh --no-director            # just the game, no LLM
#   ./start_aidoom.sh --no-coop                # disable the AI co-op companion
#   ./start_aidoom.sh --ollama http://localhost:11434
# The AI co-op companion (player 2) is ON by default; disable it with --no-coop.
# Unrecognized args are passed straight through to aidoom.
#
# Requires: SDL3 installed (to run the binary) and the aidoom binary built
# (see README; e.g. in files/). Python 3 (stdlib only) for the director.

set -u

# --- defaults (Ollama IP matches ollama_director.py's OLLAMA_HOST) ---
MODEL="mistral:7b-instruct"
PORT=31666
EPISODE=1
MAP=1
SKILL=4
OLLAMA="http://192.168.2.114:11434"
FRIENDLYFIRE=0
NODIRECTOR=0
NOWARM=0
COOP=1			# AI co-op companion (player 2) on by default; --no-coop to disable
GAME_EXTRA=()

# aidoom.cfg (next to this script, written by the SDL3 config app) overrides the
# built-in defaults; explicit CLI flags below still win. Format: "key<ws>value".
_here_pre="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "$_here_pre/aidoom.cfg" ]; then
    _h=$(awk '$1=="ollama_host"{print $2}'  "$_here_pre/aidoom.cfg" | tail -1)
    _p=$(awk '$1=="ollama_port"{print $2}'  "$_here_pre/aidoom.cfg" | tail -1)
    _m=$(awk '$1=="ollama_model"{print $2}' "$_here_pre/aidoom.cfg" | tail -1)
    [ -n "$_h" ] && OLLAMA="http://${_h}:${_p:-11434}"
    [ -n "$_m" ] && MODEL="$_m"
fi

while [ $# -gt 0 ]; do
    case "$1" in
        --model)        MODEL="$2"; shift 2;;
        --port)         PORT="$2"; shift 2;;
        --episode)      EPISODE="$2"; shift 2;;
        --map)          MAP="$2"; shift 2;;
        --skill)        SKILL="$2"; shift 2;;
        --ollama)       OLLAMA="$2"; shift 2;;
        --friendlyfire) FRIENDLYFIRE=1; shift;;
        --no-director)  NODIRECTOR=1; shift;;
        --no-warm)      NOWARM=1; shift;;
        --no-coop)      COOP=0; shift;;
        --coop)         COOP=1; shift;;
        -h|--help)
            sed -n '3,20p' "$0"; exit 0;;
        *)              GAME_EXTRA+=("$1"); shift;;
    esac
done

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
info(){ printf '\033[36m[start] %s\033[0m\n' "$*"; }
warn(){ printf '\033[33m[start] %s\033[0m\n' "$*"; }
die(){  printf '\033[31m[start] %s\033[0m\n' "$*" >&2; exit 1; }

command -v curl >/dev/null 2>&1 || die "curl is required."

# --- locate the aidoom binary (run dir, or the files/ build dir) ---
AIDOOM=""
for c in "$here/aidoom" "$here/../files/aidoom" "$here/aidoom.exe"; do
    [ -x "$c" ] && { AIDOOM="$c"; break; }
done
[ -n "$AIDOOM" ] || die "aidoom binary not found. Build it first (see README), e.g. in files/."
GAMEDIR="$(cd "$(dirname "$AIDOOM")" && pwd)"

# IWAD selection is handled by the engine: -iwad / aidoom.cfg "iwad" / iwads\/ /
# the game folder / Steam (see IdentifyVersion). The game runs from GAMEDIR below.

# --- locate python (the director uses only the stdlib) ---
PY=""
for c in "$HOME/.doom-agent/bin/python" python3 python; do
    if [ -x "$c" ] || command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done

# --- 1. wait for the Ollama server ---
info "waiting for Ollama at $OLLAMA ..."
ready=0
for i in $(seq 1 30); do
    if curl -s -m 3 "$OLLAMA/api/version" >/dev/null 2>&1; then
        info "Ollama is up ($(curl -s -m 3 "$OLLAMA/api/version"))."
        ready=1; break
    fi
    sleep 1
done
[ "$ready" = 1 ] || die "Ollama not reachable at $OLLAMA. Start it: 'ollama serve' (set OLLAMA_HOST=0.0.0.0 for remote access)."

# --- 2/3. check the model is present, then warm it into memory ---
if [ "$NODIRECTOR" = 0 ]; then
    if curl -s -m 5 "$OLLAMA/api/tags" 2>/dev/null | grep -q "\"$MODEL\""; then
        info "model '$MODEL' is available."
        if [ "$NOWARM" = 0 ]; then
            info "warming '$MODEL' (loading into memory) ..."
            if curl -s -m 120 "$OLLAMA/api/generate" \
                 -d "{\"model\":\"$MODEL\",\"prompt\":\"ok\",\"stream\":false}" >/dev/null 2>&1; then
                info "model warm."
            else
                warn "warm-up skipped."
            fi
        fi
    else
        warn "model '$MODEL' is not pulled. Pull it:  ollama pull $MODEL"
        warn "(continuing; the director will fail until the model exists)"
    fi
fi

# --- 4. start aiDoom with the AI director TCP server ---
gameargs=( -warp "$EPISODE" "$MAP" -skill "$SKILL" -aidirector "$PORT" )
[ "$COOP" = 1 ]        && gameargs+=( -aicoop )
[ "$FRIENDLYFIRE" = 1 ] && gameargs+=( -friendlyfire )
[ ${#GAME_EXTRA[@]} -gt 0 ] && gameargs+=( "${GAME_EXTRA[@]}" )

info "launching: $AIDOOM ${gameargs[*]}"
( cd "$GAMEDIR" && exec "$AIDOOM" "${gameargs[@]}" ) &
GAME_PID=$!
# stop the game if this launcher is interrupted or the director exits
trap 'kill "$GAME_PID" 2>/dev/null' EXIT INT TERM

if [ "$NODIRECTOR" = 1 ]; then
    info "no director (just the game)."
    trap - EXIT INT TERM
    wait "$GAME_PID"
    exit 0
fi

# --- 5. start the Python LLM director client ---
CLIENT="$here/ollama_director.py"
[ -f "$CLIENT" ] || CLIENT="$here/../ollama_director.py"
if [ ! -f "$CLIENT" ]; then
    warn "ollama_director.py not found -- game runs without the director."
    trap - EXIT INT TERM; wait "$GAME_PID"; exit 0
fi
if [ -z "$PY" ]; then
    warn "python not found -- game runs without the director."
    trap - EXIT INT TERM; wait "$GAME_PID"; exit 0
fi

sleep 2   # give the game a moment to open the listening socket
info "starting LLM director: $MODEL -> 127.0.0.1:$PORT (ollama $OLLAMA)"
"$PY" "$CLIENT" --port "$PORT" --model "$MODEL" --ollama "$OLLAMA/api/chat"
