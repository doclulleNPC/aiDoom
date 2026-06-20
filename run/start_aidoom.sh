#!/usr/bin/env bash
#
# start_aidoom.sh -- launch aiDoom (optionally with the LLM director and/or
# the AI co-op companion).  Linux/macOS counterpart of start_aidoom.bat /
# start_aidoom.ps1.
#
# Modes (defaults: OFF for everything; plain `aidoom` with no extras):
#   default       : plain aiDoom -- no LLM director, no buddy, no Ollama check.
#                   Fastest startup; works fully offline.
#   --buddy       : enable the rule-based co-op companion ("buddy", player 2).
#                   Passes -coop to aidoom.  Local + deterministic, no LLM needed.
#                   (For buddy-only with NO director, use start_buddy.sh.)
#   --aicoop      : enable the AI/LLM-backed companion.  Passes -aicoop, which is a
#                   DISTINCT flag from -coop (the two are mutually exclusive); today
#                   it falls back to the rule-based behaviour until the AI companion
#                   layer ships (see AI_IMPROVEMENTS.md #1).
#   --director    : enable the LLM monster director.  Waits for Ollama, warms
#                   the model, launches the game + director client.  Implies
#                   Ollama host/port/model from aidoom.cfg or flags.
#   --director --buddy : both -- buddy + LLM-driven monsters.
#
# Usage:
#   ./start_aidoom.sh                          # plain aidoom (offline, fast)
#   ./start_aidoom.sh --buddy                  # + rule-based co-op buddy
#   ./start_aidoom.sh --aicoop                  # + AI/LLM co-op buddy (-aicoop)
#   ./start_aidoom.sh --director               # + LLM monster director
#   ./start_aidoom.sh --director --buddy       # + buddy + LLM director
#   ./start_aidoom.sh --director --model qwen3:8b --skill 4 --friendlyfire
#   ./start_aidoom.sh --ollama http://localhost:11434
#
# Requires: SDL3 installed (to run the binaries) and the aidoom binary built
# (see README).  For --director: the native director binary (tools/build_director.sh)
# and a reachable Ollama server.  No Python needed.

set -u

# --- defaults (Ollama IP; overridden by aidoom.cfg below) ---
MODEL="mistral:7b-instruct"
PORT=31666
EPISODE=1
MAP=1
SKILL=4
OLLAMA="http://192.168.2.114:11434"
FRIENDLYFIRE=0
NODIRECTOR=1		# default OFF -- plain aidoom, no LLM, no Ollama check
NOWARM=0
BUDDY=0			# default OFF -- plain aidoom, no co-op companion
AIBUDDY=0			# default OFF -- --aicoop enables the AI/LLM buddy (-aicoop)
DIRECTOR=0		# default OFF -- explicit --director enables the LLM monster director
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

# --- helper output functions (defined early so the arg parser can use them) ---
info(){ printf '\033[36m[start] %s\033[0m\n' "$*"; }
warn(){ printf '\033[33m[start] %s\033[0m\n' "$*"; }
die(){  printf '\033[31m[start] %s\033[0m\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --model)        MODEL="$2"; shift 2;;
        --port)         PORT="$2"; shift 2;;
        --episode)      EPISODE="$2"; shift 2;;
        --map)          MAP="$2"; shift 2;;
        --skill)        SKILL="$2"; shift 2;;
        --ollama)       OLLAMA="$2"; shift 2;;
        --friendlyfire) FRIENDLYFIRE=1; shift;;
        --director)     DIRECTOR=1; NODIRECTOR=0; shift;;
        --no-director)  NODIRECTOR=1; shift;;    # legacy alias
        --buddy)        BUDDY=1; shift;;
        --no-buddy)     BUDDY=0; shift;;
        --aicoop)       AIBUDDY=1; shift;;   # AI/LLM-backed buddy (-aicoop); falls back to rule-based until the AI layer ships
        --no-coop)      warn "--no-coop is deprecated, use --no-buddy"; BUDDY=0; shift;;
        --coop)         warn "--coop is deprecated, use --buddy"; BUDDY=1; shift;;
        --no-warm)      NOWARM=1; shift;;
        -h|--help)
            sed -n '3,30p' "$0"; exit 0;;
        *)              GAME_EXTRA+=("$1"); shift;;
    esac
done

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

# --- 1. wait for the Ollama server ---
# Only when the director is enabled.  Default (--director not given) skips
# Ollama entirely so the game launches immediately, even if Ollama is offline
# / slow / not running.  Pass --director to opt in.
if [ "$NODIRECTOR" = 1 ]; then
    info "no director -- skipping Ollama check and model warmup."
else
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

# --- 4. start aiDoom ---
# Default: nothing extra -- vanilla aiDoom, no -aidirector (no TCP server), no
# buddy.  Pass --director to add -aidirector + the native director (run/director);
# pass --buddy to add -coop (rule-based companion) or --aicoop (AI/LLM companion).
gameargs=( -warp "$EPISODE" "$MAP" -skill "$SKILL" )
[ "$NODIRECTOR" = 0 ] && gameargs+=( -aidirector "$PORT" )
[ "$BUDDY" = 1 ] && [ "$AIBUDDY" = 0 ] && gameargs+=( -coop )    # rule-based buddy
[ "$AIBUDDY" = 1 ]   && gameargs+=( -aicoop )   # AI/LLM buddy; -coop and -aicoop are mutually exclusive (aicoop wins)
[ "$FRIENDLYFIRE" = 1 ] && gameargs+=( -friendlyfire )
[ ${#GAME_EXTRA[@]} -gt 0 ] && gameargs+=( "${GAME_EXTRA[@]}" )

info "launching: $AIDOOM ${gameargs[*]}"
( cd "$GAMEDIR" && exec "$AIDOOM" "${gameargs[@]}" ) &
GAME_PID=$!
# stop the game if this launcher is interrupted or the director exits
trap 'kill "$GAME_PID" 2>/dev/null' EXIT INT TERM

if [ "$NODIRECTOR" = 1 ]; then
    info "no director -- buddy: $([ "$BUDDY" = 1 ] && echo ON || echo OFF), AI layer: $([ "$AIBUDDY" = 1 ] && echo stub || echo off). waiting for game to exit ..."
    trap - EXIT INT TERM
    wait "$GAME_PID"
    exit 0
fi

# --- 5. start the native SDL3 LLM director ---
sleep 2   # give the game a moment to open the listening socket
DIRBIN="$here/director"
[ -x "$DIRBIN" ] || DIRBIN="$here/../tools/director"
if [ ! -x "$DIRBIN" ]; then
    warn "director binary not found -- build it first:  tools/build_director.sh"
    warn "game runs without the LLM director."
    trap - EXIT INT TERM; wait "$GAME_PID"; exit 0
fi
info "starting director: $MODEL -> 127.0.0.1:$PORT (ollama $OLLAMA)"
"$DIRBIN" --port "$PORT" --model "$MODEL" --ollama "$OLLAMA/api/chat"
