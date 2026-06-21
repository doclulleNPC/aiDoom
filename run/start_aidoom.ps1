<#
  start_aidoom.ps1 -- wait for Ollama, then launch aiDoom with FULL LLM support.

  DEFAULT = the AI co-op buddy (-aicoop) PLUS the LLM director (-aidirector +
  director.exe): the model drives the monsters, the L4D stress/spawn pacing, and the
  buddy's tactics in one loop.  Checks Ollama is up, warms the model, then launches
  the game + the native director.exe (no Python).

  Usage (or just double-click start_aidoom.bat):
    .\start_aidoom.ps1                    # FULL LLM: AI buddy + director (default)
    .\start_aidoom.ps1 -RuleCoop          # rule-based companion instead of the AI buddy
    .\start_aidoom.ps1 -NoCoop            # no companion at all
    .\start_aidoom.ps1 -NoDirector        # just the game, no LLM director
    .\start_aidoom.ps1 -Model qwen2.5-coder:1.5b -Skill 4 -FriendlyFire
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
    [switch]$NoWarm,
    [switch]$NoCoop,         # no co-op companion at all
    [switch]$RuleCoop        # rule-based companion (-coop) instead of the default AI buddy (-aicoop)
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

# IWAD selection is handled by the engine itself, in this order:
#   -iwad <file>  >  aidoom.cfg "iwad"  >  iwads\  >  this folder  >  Steam.
$gameArgs = @("-warp","$Episode","$Map","-skill","$Skill","-aidirector","$Port")
if     ($NoCoop)   { }                        # no companion
elseif ($RuleCoop) { $gameArgs += "-coop" }   # rule-based companion instead of the AI buddy
else               { $gameArgs += "-aicoop" } # default: AI/LLM co-op buddy (full LLM)
if ($FriendlyFire) { $gameArgs += "-friendlyfire" }
Info "launching aidoom.exe $($gameArgs -join ' ')"
Start-Process -FilePath (Join-Path $here "aidoom.exe") -ArgumentList $gameArgs -WorkingDirectory $here

if ($NoDirector) {
    Info "no director (just the game). done."
    exit 0
}

# --- 5. start the native SDL3 LLM director ---
Start-Sleep -Seconds 2   # give the game a moment to open the listening socket
$dirbin = Join-Path $here "director.exe"
if (-not (Test-Path $dirbin)) {
    Warn "director.exe not found -- build it first:  tools/build_director_win.sh"
    Warn "game runs without the LLM director."
    exit 0
}
Info "starting director: $Model -> 127.0.0.1:$Port"
& $dirbin --port $Port --model $Model --ollama "$Ollama/api/chat"
