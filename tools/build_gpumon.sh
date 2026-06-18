#!/usr/bin/env bash
# Build the SDL3 GPU monitor (gpumon) for Linux/macOS.
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
gcc -O2 -DSDL_MAIN_HANDLED -o "$here/gpumon" "$here/gpumon_sdl.c" \
    -I"$here" $(pkg-config --cflags --libs sdl3)
mkdir -p "$here/../run"
cp -f "$here/gpumon" "$here/../run/gpumon"
echo "built $here/gpumon (copied to run/gpumon)"
