#!/usr/bin/env bash
# Build the native SDL3 AI Director (director) for Linux/macOS -- the C
# native LLM monster director (no Python needed).  -> run/director
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
gcc -O2 -DSDL_MAIN_HANDLED -o "$here/director" "$here/director.c" \
    -I"$here" $(pkg-config --cflags --libs sdl3)
mkdir -p "$here/../run"
cp -f "$here/director" "$here/../run/director"
echo "built $here/director (copied to run/director)"
