#!/bin/bash
# Join the Chocolate/Crispy server on the GPU box as a 2-player co-op game with
# the AI buddy.  Run this on every client (the buddy needs -aicoop everywhere).
cd "$(dirname "$0")" || exit 1
exec ./aidoom -connect 192.168.2.114:2342 -aicoop -netplayers 2 -warp 1 1 "$@"
