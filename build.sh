#!/usr/bin/env bash
#
# build.sh -- build aiDoom (Linux/macOS, SDL3) and copy the artifact into run/.
#
# The 1996 id source needs permissive flags for a modern compiler; SDL3 comes
# from the system (pkg-config). On Windows use files/Makefile.msvc instead (it
# produces aidoom.exe and copies SDL3.dll next to it).
#
# Usage:  ./build.sh
set -eu

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="$here/files"
run="$here/run"

command -v gcc >/dev/null 2>&1 || { echo "[build] gcc not found" >&2; exit 1; }
pkg-config --exists sdl3 || { echo "[build] SDL3 dev package not found (pkg-config sdl3)" >&2; exit 1; }

echo "[build] compiling aidoom (SDL3 $(pkg-config --modversion sdl3)) ..."
( cd "$src" && gcc -O2 -g -fcommon -fno-strict-aliasing -std=gnu11 \
    -Wno-implicit-int -Wno-implicit-function-declaration \
    -Wno-int-conversion -Wno-return-mismatch \
    -DSDL_MAIN_HANDLED $(pkg-config --cflags sdl3) \
    *.c -o aidoom $(pkg-config --libs sdl3) -lm )

# Always copy the artifact(s) into run/ so the launcher finds them there.
# (Linux/macOS link SDL3 from the system, so there is no DLL to copy here;
#  on Windows the MSVC build copies SDL3.dll next to aidoom.exe.)
mkdir -p "$run"
cp -f "$src/aidoom" "$run/aidoom"
echo "[build] done -> $src/aidoom  (copied to $run/aidoom)"
