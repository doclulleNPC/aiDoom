#!/usr/bin/env bash
# Build the SDL3 GPU monitor (gpumon_sdl) for Linux/macOS.
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
gcc -O2 -DSDL_MAIN_HANDLED -o "$here/gpumon_sdl" "$here/gpumon_sdl.c" \
    -I"$here" $(pkg-config --cflags --libs sdl3)
mkdir -p "$here/../run"
cp -f "$here/gpumon_sdl" "$here/../run/gpumon_sdl"
echo "built $here/gpumon_sdl (copied to run/gpumon_sdl)"
