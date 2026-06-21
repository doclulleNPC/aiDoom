#!/usr/bin/env bash
# Build the SDL3 launcher for aiDoom (tools/launcher.c).
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
gcc -O2 -DSDL_MAIN_HANDLED -o "$here/launcher" "$here/launcher.c" \
    -I"$here" $(pkg-config --cflags --libs sdl3)
# place the binary next to the game in run/
mkdir -p "$here/../run"
cp -f "$here/launcher" "$here/../run/launcher"
echo "built $here/launcher  (copied to run/launcher)"