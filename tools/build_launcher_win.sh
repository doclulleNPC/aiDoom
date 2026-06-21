#!/usr/bin/env bash
#
# build_launcher_win.sh -- cross-build the SDL3 launcher (launcher.exe) for
# Windows with MinGW-w64.  Point SDL3 at a MinGW SDL3 dev package (same as
# build_win.sh):
#
#     SDL3=/path/to/SDL3-devel-3.x.y-mingw/x86_64-w64-mingw32 ./tools/build_launcher_win.sh
#
set -eu

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${CC:=x86_64-w64-mingw32-gcc}"
: "${SDL3:?Set SDL3 to your MinGW SDL3 dir (contains include/ lib/ bin/)}"

command -v "$CC" >/dev/null 2>&1 || { echo "[build] $CC not found" >&2; exit 1; }
[ -f "$SDL3/include/SDL3/SDL.h" ] || { echo "[build] SDL3 headers not at $SDL3/include" >&2; exit 1; }
[ -f "$here/font_atlas.h" ] || python3 "$here/bake_font.py"

# -mwindows: GUI subsystem (no console window). SDL_MAIN_HANDLED: we own main().
"$CC" -O2 -DSDL_MAIN_HANDLED -I"$here" -I"$SDL3/include" \
    "$here/launcher.c" \
    -L"$SDL3/lib" -lSDL3 -mwindows -static-libgcc \
    -o "$here/launcher.exe"

mkdir -p "$here/../run"
cp -f "$here/launcher.exe" "$here/../run/launcher.exe"
[ -f "$SDL3/bin/SDL3.dll" ] && cp -f "$SDL3/bin/SDL3.dll" "$here/../run/SDL3.dll" || true
echo "built $here/launcher.exe (copied to run/launcher.exe)"