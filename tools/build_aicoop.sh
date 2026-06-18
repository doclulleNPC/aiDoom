#!/usr/bin/env bash
# Build the SDL3 co-op AI tuner (aicoop_config) for Linux/macOS.
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
gcc -O2 -DSDL_MAIN_HANDLED -o "$here/aicoop_config" "$here/aicoop_config.c" \
    -I"$here" $(pkg-config --cflags --libs sdl3)
mkdir -p "$here/../run"; cp -f "$here/aicoop_config" "$here/../run/aicoop_config"
echo "built $here/aicoop_config (copied to run/aicoop_config)"
