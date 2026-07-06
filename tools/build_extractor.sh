#!/usr/bin/env bash
# Build the SDL3 asset extractor for aiDoom (tools/extractor.c).
# Replaces the extract_heretic_monsters.py / extract_hexen.py /
# extract_freedoom2.py scripts with a GUI: pick a source IWAD, hit Extract.
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
gcc -O2 -DSDL_MAIN_HANDLED -o "$here/extractor" "$here/extractor.c" \
    -I"$here" $(pkg-config --cflags --libs sdl3) -lm
# place the binary next to the game in run/
mkdir -p "$here/../run"
cp -f "$here/extractor" "$here/../run/extractor"
echo "built $here/extractor  (copied to run/extractor)"
