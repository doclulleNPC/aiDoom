#!/usr/bin/env bash
# Cross-build the SDL3 co-op AI tuner (aicoop_config.exe) with MinGW-w64.
#   SDL3=/path/to/SDL3-mingw/x86_64-w64-mingw32 ./tools/build_aicoop_win.sh
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${CC:=x86_64-w64-mingw32-gcc}"
: "${SDL3:?Set SDL3 to your MinGW SDL3 dir (contains include/ lib/ bin/)}"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
"$CC" -O2 -DSDL_MAIN_HANDLED -I"$here" -I"$SDL3/include" \
    "$here/aicoop_config.c" -L"$SDL3/lib" -lSDL3 -mwindows -static-libgcc \
    -o "$here/aicoop_config.exe"
mkdir -p "$here/../run"; cp -f "$here/aicoop_config.exe" "$here/../run/aicoop_config.exe"
echo "built $here/aicoop_config.exe (copied to run/aicoop_config.exe)"
