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

# --- (A) fork version: bump the patch (+0.0.1) on every recompile -------------
verf="$src/aidoom_version.h"
fork="$(sed -n 's/.*AIDOOM_VERSION[[:space:]]*"\([0-9.]*\)".*/\1/p' "$verf" 2>/dev/null || true)"
[ -n "$fork" ] || fork="0.2.0"
vmaj="${fork%%.*}"; vrest="${fork#*.}"; vmin="${vrest%%.*}"; vpat="${vrest##*.}"
vpat=$((vpat + 1)); fork="$vmaj.$vmin.$vpat"
cat > "$verf" <<EOF
// aiDoom fork version.  AUTO-MANAGED by build.sh -- the patch field is bumped
// +0.0.1 on every recompile.  Bump the major/minor by hand when you cut a release
// tag (and the patch keeps counting builds from there).
#ifndef __AIDOOM_VERSION__
#define __AIDOOM_VERSION__
#define AIDOOM_VERSION "$fork"
#endif
EOF

# --- (B) engine version: bump +0.01 when the savegame layout changes ----------
# The save file stamps VERSION_NUM in its header; if a saved struct's size changes
# the old saves become incompatible.  We fingerprint the sizes of every struct the
# savegame memcpy's (p_saveg.c) and, when that fingerprint changes, bump VERSION_NUM
# so stale saves are cleanly REJECTED ("bad version") instead of crashing on load.
sigf="$src/aidoom_saveg.sig"
probe="$(mktemp /tmp/aidoom_sgprobe.XXXXXX.c)"; probebin="${probe%.c}"
cat > "$probe" <<'EOF'
#include "i_system.h"
#include "z_zone.h"
#include "p_local.h"
#include "doomstat.h"
#include "r_state.h"
#include <stdio.h>
int main(void) {
    printf("%lu\n", (unsigned long)(
        sizeof(player_t)   + sizeof(mobj_t)      + sizeof(ceiling_t) +
        sizeof(vldoor_t)   + sizeof(floormove_t) + sizeof(plat_t)    +
        sizeof(lightflash_t) + sizeof(strobe_t)  + sizeof(glow_t)));
    return 0;
}
EOF
sig=""
if ( cd "$src" && gcc -fcommon -std=gnu11 $(pkg-config --cflags sdl3) \
       -I"$src" "$probe" -o "$probebin" ) 2>/dev/null; then
    sig="$("$probebin" 2>/dev/null || true)"
fi
rm -f "$probe" "$probebin"
if [ -n "$sig" ]; then
    old="$(cat "$sigf" 2>/dev/null || true)"
    if [ -n "$old" ] && [ "$sig" != "$old" ]; then
        ev="$(sed -n 's/.*VERSION_NUM[[:space:]]*=[[:space:]]*\([0-9]*\).*/\1/p' "$src/doomdef.h")"
        ev=$((ev + 1))
        sed -i "s/VERSION_NUM[[:space:]]*=[[:space:]]*[0-9]*/VERSION_NUM =  $ev/" "$src/doomdef.h"
        echo "[build] savegame layout changed -> engine version bumped to $((ev/100)).$(printf '%02d' $((ev%100))) (old saves now rejected, not crashed)"
    fi
    printf '%s\n' "$sig" > "$sigf"
else
    echo "[build] (warning: savegame-layout probe didn't compile; engine version unchanged)" >&2
fi

echo "[build] compiling aidoom $fork (SDL3 $(pkg-config --modversion sdl3)) ..."
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
