#!/usr/bin/env bash
# Build the SDL3 settings editor (buddydoom_config).
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
gcc -O2 -DSDL_MAIN_HANDLED -o "$here/buddydoom_config" "$here/buddydoom_config.c" \
    -I"$here" $(pkg-config --cflags --libs sdl3)
# place the binary next to the game in run/ (it reads buddydoom.cfg from there)
mkdir -p "$here/../run"
cp -f "$here/buddydoom_config" "$here/../run/buddydoom_config"
echo "built $here/buddydoom_config  (copied to run/buddydoom_config)"
