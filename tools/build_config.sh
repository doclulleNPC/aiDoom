#!/usr/bin/env bash
# Build the SDL3 settings editor (aidoom_config).
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
gcc -O2 -o "$here/aidoom_config" "$here/aidoom_config.c" \
    -I"$here" $(pkg-config --cflags --libs sdl3)
echo "built $here/aidoom_config"
