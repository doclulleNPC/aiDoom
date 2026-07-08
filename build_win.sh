#!/usr/bin/env bash
#
# build_win.sh -- build aidoom.exe for Windows with MinGW-w64, with the app icon
# embedded in the binary (via windres). Run it on Linux (cross-compile) or inside
# MSYS2/MinGW. For the Visual Studio build use files/Makefile.msvc instead (it
# already links the icon resource the same way).
#
# You must point SDL3 at a MinGW SDL3 *development* package (the
# "SDL3-devel-<ver>-mingw" release), specifically its x86_64-w64-mingw32 subdir
# that contains include/, lib/ and bin/:
#
#     SDL3=/path/to/SDL3-devel-3.x.y-mingw/x86_64-w64-mingw32 ./build_win.sh
#
set -eu

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/files"
run="$here/run"

: "${CC:=x86_64-w64-mingw32-gcc}"
: "${WINDRES:=x86_64-w64-mingw32-windres}"
: "${SDL3:?Set SDL3 to your MinGW SDL3 dir (contains include/ lib/ bin/), e.g. SDL3=/opt/SDL3-mingw/x86_64-w64-mingw32}"

command -v "$CC"      >/dev/null 2>&1 || { echo "[build] $CC not found"      >&2; exit 1; }
command -v "$WINDRES" >/dev/null 2>&1 || { echo "[build] $WINDRES not found" >&2; exit 1; }
[ -f "$SDL3/include/SDL3/SDL.h" ] || { echo "[build] SDL3 headers not at $SDL3/include/SDL3/SDL.h" >&2; exit 1; }

echo "[build] compiling app icon resource (windres) ..."
"$WINDRES" "$src/aidoom.rc" -O coff -o "$src/aidoom_res.o"

echo "[build] compiling aidoom.exe (MinGW) ..."
( cd "$src" && "$CC" -O2 -fcommon -fno-strict-aliasing -std=gnu11 \
    -Wno-implicit-int -Wno-implicit-function-declaration \
    -Wno-int-conversion -Wno-return-mismatch \
    -Wno-incompatible-pointer-types \
    -DSDL_MAIN_HANDLED -DWIN32 -I"$SDL3/include" \
    *.c aidoom_res.o \
    -L"$SDL3/lib" -lSDL3 -lws2_32 -lm -ldbghelp -mconsole \
    -static-libgcc \
    -o aidoom.exe )

mkdir -p "$run"
cp -f "$src/aidoom.exe" "$run/aidoom.exe"
[ -f "$SDL3/bin/SDL3.dll" ] && cp -f "$SDL3/bin/SDL3.dll" "$run/SDL3.dll" && echo "[build] copied SDL3.dll"
echo "[build] done -> $src/aidoom.exe  (icon embedded; copied to $run/aidoom.exe)"
