#!/usr/bin/env bash
# Cross-build the SDL3 AI Director (director.exe) for Windows with MinGW-w64.
# The C replacement for ollama_director.py (no Python needed).
#   SDL3=/path/to/SDL3-devel-3.x.y-mingw/x86_64-w64-mingw32 ./tools/build_director_win.sh
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${CC:=x86_64-w64-mingw32-gcc}"
: "${SDL3:?Set SDL3 to your MinGW SDL3 dir (contains include/ lib/ bin/)}"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
"$CC" -O2 -DSDL_MAIN_HANDLED -I"$here" -I"$SDL3/include" \
    "$here/director.c" -L"$SDL3/lib" -lSDL3 -lws2_32 -mwindows -static-libgcc \
    -o "$here/director.exe"
mkdir -p "$here/../run"
cp -f "$here/director.exe" "$here/../run/director.exe"
echo "built $here/director.exe (copied to run/director.exe)"
