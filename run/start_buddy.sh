#!/bin/bash
#
# start_buddy.sh -- launch aiDoom OFFLINE (no LLM / no AI director) with the
# rule-based co-op buddy.  Just the companion, no Ollama, no network.
#
#   -coop    = rule-based buddy (offline, no LLM)   <- this script
#   -aicoop  = AI/LLM-backed buddy (needs the director; use start_aidoom.sh)
#
# By default jumps straight to MAP01 so the buddy is visible immediately
# (the title screen has no buddy -- you only see Player 1 in the title demo).
# Pass -warp yourself to override (e.g. `./start_buddy.sh -warp 3 4 -skill 4`).
#
set -u
cd "$(dirname "${BASH_SOURCE[0]}")" || exit 1

# Find the binary (run/ first, then the files/ build dir).
for c in ./aidoom ./aidoom.exe ../files/aidoom; do
    [ -x "$c" ] && { AIDOOM="$c"; break; }
done
[ -n "${AIDOOM:-}" ] || { echo "[start_buddy] aidoom binary not found -- build it first (./build.sh)." >&2; exit 1; }

# Default warp to MAP01 unless the user overrides with their own -warp.
if ! printf '%s\n' "$@" | grep -q -- '-warp'; then
    set -- -warp 1 1 "$@"
fi

echo "[start_buddy] offline (no AI director) + rule-based buddy: $AIDOOM -coop $*"
exec "$AIDOOM" -coop "$@"
