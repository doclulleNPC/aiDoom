#!/bin/bash
# Join the Chocolate/Crispy server as a 2-player co-op game.  No AI buddy --
# it is single-player only and ignored in netgames.  Run on every client.
cd "$(dirname "$0")" || exit 1
exec ./buddydoom -connect 192.168.2.10:2342 -netplayers 2 -warp 1 1 "$@"
