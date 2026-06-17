<#
  start_aidoom.ps1 -- wait for Ollama, then launch aiDoom (+ the LLM director).

  Checks the local Ollama server is up and the model is available, optionally
  warms the model into memory, then starts aidoom.exe with the -aidirector TCP
  server and the Python director client that drives the monsters.

  Usage (or just double-click start_aidoom.bat):
    .\start_aidoom.ps1
    .\start_aidoom.ps1 -Model qwen2.5-coder:1.5b -Skill 4 -FriendlyFire
    .\start_aidoom.ps1 -NoDirector        # just the game, no LLM
#>
param(
    [string]$Model     = "mistral:7b-instruct",
    [int]   $Port      = 31666,
    [int]   $Episode   = 1,
    [int]   $Map       = 1,
    [int]   $Skill     = 4,
    [string]$Ollama    = "http://localhost:11434",
    [switch]$FriendlyFire,
    [switch]$NoDirector,
    [switch]$NoWarm
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here

# aidoom.cfg (next to this script, written by the SDL3 config app) sets the
# defaults for host/port/model; explicit -Ollama / -Model still win.
# Format: "key<whitespace>value".
$cfgFile = Join-Path $here "aidoom.cfg"
if (Test-Path $cfgFile) {
    $cfg = @{}
    foreach ($line in Get-Content $cfgFile) {
        $p = $line -split '\s+', 2 | Where-Object { $_ -ne '' }
        if ($p.Count -ge 2) { $cfg[$p[0]] = $p[1].Trim('"') }
    }
    if (-not $PSBoundParameters.ContainsKey('Ollama') -and $cfg.ContainsKey('ollama_host')) {
        $port = if ($cfg.ContainsKey('ollama_port')) { $cfg['ollama_port'] } else { '11434' }
        $Ollama = "http://$($cfg['ollama_host']):$port"
    }
    if (-not $PSBoundParameters.ContainsKey('Model') -and $cfg.ContainsKey('ollama_model')) {
        $Model = $cfg['ollama_model']
    }
}

function Info($m){ Write-Host "[start] $m" -ForegroundColor Cyan }
function Warn($m){ Write-Host "[start] $m" -ForegroundColor Yellow }
function Die ($m){ Write-Host "[start] $m" -ForegroundColor Red; Read-Host "Press Enter to exit"; exit 1 }

# --- 1. wait for the Ollama server ---
Info "waiting for Ollama at $Ollama ..."
$ready = $false
for ($i = 0; $i -lt 30; $i++) {
    try {
        $v = Invoke-RestMethod -Uri "$Ollama/api/version" -TimeoutSec 3
        Info "Ollama is up (version $($v.version))."
        $ready = $true; break
    } catch {
        Start-Sleep -Milliseconds 1000
    }
}
if (-not $ready) {
    Die "Ollama not reachable at $Ollama. Start it first:  ollama serve   (or launch the Ollama app)."
}

# --- 2. check the model is present ---
if (-not $NoDirector) {
    try {
        $tags = Invoke-RestMethod -Uri "$Ollama/api/tags" -TimeoutSec 5
        $have = @($tags.models | ForEach-Object { $_.name })
    } catch { $have = @() }
    if ($have -notcontains $Model) {
        Warn "model '$Model' is not pulled. Available: $($have -join ', ')"
        Warn "pull it with:  ollama pull $Model    (continuing; the director will fail until it exists)"
    } else {
        Info "model '$Model' is available."
        # --- 3. warm the model into memory so the first in-game round is fast ---
        if (-not $NoWarm) {
            Info "warming '$Model' (loading into memory) ..."
            try {
                $body = @{ model = $Model; prompt = "ok"; stream = $false } | ConvertTo-Json
                Invoke-RestMethod -Uri "$Ollama/api/generate" -Method Post -Body $body -ContentType "application/json" -TimeoutSec 120 | Out-Null
                Info "model warm."
            } catch { Warn "warm-up skipped: $($_.Exception.Message)" }
        }
    }
}

# --- 4. start aiDoom with the AI director TCP server ---
if (-not (Test-Path (Join-Path $here "SDL3.dll"))) { Die "SDL3.dll missing next to aidoom.exe in $here" }

# Find any known IWAD in this folder (commercial, shareware, or Freedoom).
$iwads = @("doom2.wad","doom.wad","doomu.wad","plutonia.wad","tnt.wad","doom1.wad",
           "freedoom2.wad","freedoom1.wad","freedm.wad")
$wad = $null
foreach ($w in $iwads) { if (Test-Path (Join-Path $here $w)) { $wad = $w; break } }
if (-not $wad) { Die "No IWAD found in $here (e.g. doom.wad / doom2.wad / doom1.wad / freedoom*.wad)" }
Info "using IWAD: $wad"

$gameArgs = @("-iwad",$wad,"-warp","$Episode","$Map","-skill","$Skill","-aidirector","$Port")
if ($FriendlyFire) { $gameArgs += "-friendlyfire" }
Info "launching aidoom.exe $($gameArgs -join ' ')"
Start-Process -FilePath (Join-Path $here "aidoom.exe") -ArgumentList $gameArgs -WorkingDirectory $here

if ($NoDirector) {
    Info "no director (just the game). done."
    exit 0
}

# --- 5. start the Python LLM director client ---
$py = $null
foreach ($cand in @("python","py")) {
    $c = Get-Command $cand -ErrorAction SilentlyContinue
    if ($c) { $py = $c.Source; break }
}
if (-not $py -and (Test-Path "C:\Python313\python.exe")) { $py = "C:\Python313\python.exe" }
if (-not $py) { Warn "Python not found -- game runs without the LLM director."; exit 0 }

$client = Join-Path $here "ollama_director.py"
if (-not (Test-Path $client)) { Warn "ollama_director.py not found in $here -- game runs without director."; exit 0 }

Start-Sleep -Seconds 2   # give the game a moment to open the listening socket
Info "starting LLM director: $Model -> 127.0.0.1:$Port"
& $py $client --port $Port --model $Model --ollama "$Ollama/api/chat"
