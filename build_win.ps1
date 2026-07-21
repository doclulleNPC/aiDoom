# build_win.ps1 -- native Windows build of aiDoom + all tools with MinGW gcc.
# (game + aidoom_config, gpumon, launcher, director, extractor -- the MinGW analog
#  of build_all_win.bat, which needs MSVC.)
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

# --- 2) tools: SDL3 GUI apps (config / gpumon / launcher / director / extractor) ---
# Each owns main() (SDL_MAIN_HANDLED), has an icon .rc reusing files\aidoom.ico (via
# --include-dir), and includes files\aidoom_icon.h for the live window icon (so add
# -I$files). They are GUI apps -> -mwindows (WINDOWS subsystem, no console window),
# matching tools\Makefile.msvc's /SUBSYSTEM:WINDOWS. director also links psapi (process
# stats) + ws2_32 (TCP to Ollama).  Same as the game, they link the SDL3 import DLL.
if (-not (Test-Path (Join-Path $tools "font_atlas.h"))) {
    throw "tools\font_atlas.h missing -- run tools\bake_font.py first."
}
$toolTargets = @(
    @{ name = "aidoom_config"; src = "aidoom_config.c"; libs = @() },
    @{ name = "gpumon";        src = "gpumon_sdl.c";    libs = @() },
    @{ name = "launcher";      src = "launcher.c";      libs = @() },
    @{ name = "director";      src = "director.c";      libs = @("-lpsapi","-lws2_32") },
    @{ name = "extractor";     src = "extractor.c";     libs = @() }
)
foreach ($t in $toolTargets) {
    Write-Host "[build] compiling $($t.name).exe ..."
    $res = Join-Path $tools "$($t.name)_res.o"
    & windres (Join-Path $tools "$($t.name).rc") -O coff --include-dir $files `
        @windresPP -o $res
    if ($LASTEXITCODE) { throw "windres ($($t.name).rc) failed" }
    & gcc @cflags "-mwindows" "-I$tools" "-I$files" (Join-Path $tools $t.src) $res `
        -o (Join-Path $tools "$($t.name).exe") $sdldll -lm $t.libs
    if ($LASTEXITCODE) { throw "gcc ($($t.name)) failed" }
}

# --- 3) stage everything in run\ ----------------------------------------------
Copy-Item (Join-Path $files "aidoom.exe") (Join-Path $run "aidoom.exe") -Force
foreach ($t in $toolTargets) {
    Copy-Item (Join-Path $tools "$($t.name).exe") (Join-Path $run "$($t.name).exe") -Force
}
Copy-Item $sdldll (Join-Path $run "SDL3.dll") -Force
if (Test-Path (Join-Path $files "aidoom.ico")) {
    Copy-Item (Join-Path $files "aidoom.ico") (Join-Path $run "aidoom.ico") -Force
}

Write-Host "[build] done -> run\ : aidoom.exe + $(($toolTargets | ForEach-Object { "$($_.name).exe" }) -join ' + ') + SDL3.dll"
