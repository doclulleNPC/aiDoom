# build_win.ps1 -- native Windows build of aiDoom + the config tool with MinGW gcc.
#
# Why this exists: files\Makefile.msvc needs nmake + cl (Visual Studio), and
# build.sh / tools\build_config.sh need pkg-config -- none of which are present
# on a plain MinGW install. This script builds both binaries with gcc, embeds
# the app icon via windres (so aidoom.exe / aidoom_config.exe show the icon in
# Explorer and the taskbar), and stages everything in run\ next to SDL3.dll.
#
# Usage (from a PowerShell prompt with MinGW gcc/windres on PATH):
#     .\build_win.ps1
#     .\build_win.ps1 -Sdl C:\path\to\SDL3        # override SDL3 SDK location
#
# Requires the SDL3 development SDK (headers + lib\<arch>\SDL3.dll). Defaults to
# C:\Source\SDL3, or set $env:SDL3_DIR.

param(
    [string]$Sdl = $(if ($env:SDL3_DIR) { $env:SDL3_DIR } else { "C:\Source\SDL3" })
)

# NOTE: keep this "Continue", not "Stop". gcc/windres emit warnings on stderr and
# Windows PowerShell wraps native stderr as ErrorRecords -- with "Stop" that aborts
# the build even though the tool exited 0. We gate on $LASTEXITCODE + throw instead.
$ErrorActionPreference = "Continue"
$root  = $PSScriptRoot
$files = Join-Path $root "files"
$tools = Join-Path $root "tools"
$run   = Join-Path $root "run"

foreach ($tool in @("gcc","windres")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "$tool not found on PATH (install MinGW / add it to PATH)."
    }
}

# Pick the SDL3 import DLL whose arch matches this gcc.
$mach = (& gcc -dumpmachine)
$arch = if     ($mach -like "x86_64*")  { "x64" }
        elseif ($mach -like "aarch64*" -or $mach -like "arm64*") { "arm64" }
        else   { "x86" }
$sdlinc = Join-Path $Sdl "include"
$sdldll = Join-Path $Sdl "lib\$arch\SDL3.dll"
foreach ($p in @($sdlinc, $sdldll)) {
    if (-not (Test-Path $p)) { throw "SDL3 SDK path missing: $p  (pass -Sdl <dir>)" }
}
Write-Host "[build] gcc=$mach  arch=$arch  SDL3=$Sdl"

# windres runs gcc as its preprocessor but (in older MinGW) doesn't quote the
# path -- which breaks when MinGW lives under "C:\Program Files\...". Hand it the
# 8.3 short path so the embedded command has no spaces.
$gccShort = (New-Object -ComObject Scripting.FileSystemObject).GetFile((Get-Command gcc).Source).ShortPath
$windresPP = @("--preprocessor", $gccShort, "--preprocessor-arg=-E",
               "--preprocessor-arg=-xc", "--preprocessor-arg=-DRC_INVOKED")

# The 1996 id source needs permissive flags for a modern compiler (see CLAUDE.md);
# -fno-strict-aliasing is required or -O2 miscompiles the type-punning engine.
$cflags = @(
    "-O2","-g","-fcommon","-fno-strict-aliasing","-std=gnu11",
    "-Wno-implicit-int","-Wno-implicit-function-declaration","-Wno-int-conversion",
    "-DSDL_MAIN_HANDLED","-I$sdlinc"
)

New-Item -ItemType Directory -Force -Path $run | Out-Null

# --- 1) game: aidoom.exe -------------------------------------------------------
Write-Host "[build] compiling aidoom.exe ..."
& windres (Join-Path $files "aidoom.rc") -O coff --include-dir $files `
    @windresPP -o (Join-Path $files "aidoom_res.o")
if ($LASTEXITCODE) { throw "windres (aidoom.rc) failed" }

$gameSrc = Get-ChildItem (Join-Path $files "*.c") | ForEach-Object { $_.FullName }
& gcc @cflags $gameSrc (Join-Path $files "aidoom_res.o") `
    -o (Join-Path $files "aidoom.exe") $sdldll -lm -lws2_32 -ldbghelp
if ($LASTEXITCODE) { throw "gcc (aidoom) failed" }

# --- 2) config tool: aidoom_config.exe ----------------------------------------
# Its .rc reuses files\aidoom.ico (via --include-dir), and the C file includes
# files\aidoom_icon.h for the live window icon, so add -I$files.
Write-Host "[build] compiling aidoom_config.exe ..."
if (-not (Test-Path (Join-Path $tools "font_atlas.h"))) {
    throw "tools\font_atlas.h missing -- run tools\bake_font.py first."
}
& windres (Join-Path $tools "aidoom_config.rc") -O coff --include-dir $files `
    @windresPP -o (Join-Path $tools "aidoom_config_res.o")
if ($LASTEXITCODE) { throw "windres (aidoom_config.rc) failed" }

& gcc @cflags "-I$tools" "-I$files" (Join-Path $tools "aidoom_config.c") `
    (Join-Path $tools "aidoom_config_res.o") `
    -o (Join-Path $tools "aidoom_config.exe") $sdldll -lm
if ($LASTEXITCODE) { throw "gcc (aidoom_config) failed" }

# --- 3) stage everything in run\ ----------------------------------------------
Copy-Item (Join-Path $files "aidoom.exe")        (Join-Path $run "aidoom.exe")        -Force
Copy-Item (Join-Path $tools "aidoom_config.exe") (Join-Path $run "aidoom_config.exe") -Force
Copy-Item $sdldll                                (Join-Path $run "SDL3.dll")          -Force
if (Test-Path (Join-Path $files "aidoom.ico")) {
    Copy-Item (Join-Path $files "aidoom.ico")    (Join-Path $run "aidoom.ico")        -Force
}

Write-Host "[build] done -> run\aidoom.exe, run\aidoom_config.exe, run\SDL3.dll"
