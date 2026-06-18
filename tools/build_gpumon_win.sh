#!/usr/bin/env bash
# Cross-build the SDL3 GPU monitor (gpumon_sdl.exe) for Windows with MinGW-w64.
#   SDL3=/path/to/SDL3-devel-3.x.y-mingw/x86_64-w64-mingw32 ./tools/build_gpumon_win.sh
set -eu
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${CC:=x86_64-w64-mingw32-gcc}"
: "${SDL3:?Set SDL3 to your MinGW SDL3 dir (contains include/ lib/ bin/)}"
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"
"$CC" -O2 -DSDL_MAIN_HANDLED -I"$here" -I"$SDL3/include" \
    "$here/gpumon_sdl.c" -L"$SDL3/lib" -lSDL3 -mwindows -static-libgcc \
    -o "$here/gpumon_sdl.exe"
mkdir -p "$here/../run"
cp -f "$here/gpumon_sdl.exe" "$here/../run/gpumon_sdl.exe"
echo "built $here/gpumon_sdl.exe (copied to run/gpumon_sdl.exe)"
