# aiDoom launchers (`run/`)

Double-click / one-command launchers that bring up **aiDoom + the LLM "AI
Director"** together: they wait for an Ollama server, (optionally) warm the
model, start the game with its `-aidirector` TCP server, then run the Python
director client that drives the monsters' tactics.

> Keep this README in sync when the launchers or `ollama_director.py` change.

## Contents

| File | Platform | Purpose |
|------|----------|---------|
| `start_aidoom.sh`  | Linux / macOS | Main launcher (bash). |
| `start_aidoom.bat` | Windows | Double-click shim → calls the PowerShell script. |
| `start_aidoom.ps1` | Windows | Main launcher (PowerShell). |
| `ollama_director.py` | all | The director client (talks to Ollama + the game). Mirror of the repo-root copy. |
| `gpumon.py` | all | Live GPU monitor for the remote Ollama machine (see below). |
| `aidoom.ico` | Windows | Icon for the launcher/shortcut. |

## What the launcher does

1. **Wait for Ollama** — polls `<ollama>/api/version` (up to ~30 s).
2. **Check the model** is pulled (`/api/tags`); warns (doesn't abort) if missing.
3. **Warm the model** into memory (`/api/generate` with a tiny prompt) so the
   first in-game planning round isn't slow. Skip with `--no-warm` / `-NoWarm`.
4. **Start aiDoom** with `-aidirector <port>` (and `-warp`, `-skill`, optional
   `-friendlyfire`).
5. **Start the Python director** (`ollama_director.py`), which loops
   observe → ask the LLM → issue squad orders. The game keeps running if Python
   or the model is unavailable. Closing the director (or Ctrl-C) stops the game.

## Requirements

- A built **aidoom** binary (the launcher looks in `run/`, then `../files/`).
  - Linux/macOS: run **`../build.sh`** — it compiles against system SDL3 and
    copies the `aidoom` binary into `run/`. (SDL3 is linked from the system, so
    there's no library to copy alongside it.)
  - Windows: build `files/Makefile.msvc`, then put `aidoom.exe` + `SDL3.dll` in `run/`.
- A DOOM **IWAD** (`doom1.wad` / `doom.wad` / `doom2.wad`) in `run/` or the
  binary's folder. The `.sh` passes it via `-iwad` if found; otherwise the engine
  auto-detects one in its working directory. **Bring your own** — IWADs are not shipped.
- An **Ollama** server reachable over HTTP, with the chosen model pulled
  (`ollama pull mistral:7b-instruct`). For a *remote* server it must listen on the
  network (`OLLAMA_HOST=0.0.0.0 ollama serve`), not just localhost.
- **Python 3** for the director (standard library only — the `.sh` prefers the
  `~/.doom-agent` venv, else `python3`).

## Usage — Linux / macOS

```sh
cd run
./start_aidoom.sh                                   # defaults
./start_aidoom.sh --model qwen3:8b --skill 4 --friendlyfire
./start_aidoom.sh --no-director                     # just the game, no LLM
./start_aidoom.sh --ollama http://localhost:11434   # override the server
```

Flags (unrecognized args are passed straight through to `aidoom`):

| Flag | Default | Meaning |
|------|---------|---------|
| `--model <name>` | `mistral:7b-instruct` | Ollama model for tactics |
| `--port <n>` | `31666` | director TCP port (`-aidirector`) |
| `--episode <n>` / `--map <n>` | `1` / `1` | `-warp` target |
| `--skill <n>` | `4` | difficulty (1–5) |
| `--ollama <url>` | `http://192.168.2.114:11434` | Ollama base URL |
| `--friendlyfire` | off | enable monster infighting (`-friendlyfire`) |
| `--no-director` | off | launch only the game |
| `--no-warm` | off | skip model warm-up |

## Usage — Windows

Double-click **`start_aidoom.bat`**, or run the PowerShell script with params:

```powershell
.\start_aidoom.ps1
.\start_aidoom.ps1 -Model qwen2.5-coder:1.5b -Skill 4 -FriendlyFire
.\start_aidoom.ps1 -NoDirector
```

Params: `-Model -Port -Episode -Map -Skill -Ollama -FriendlyFire -NoDirector -NoWarm`.

## Ollama server location

The server URL is configurable and defaults differ by platform:

- **`start_aidoom.sh`** defaults to **`http://192.168.2.114:11434`** (matches
  `OLLAMA_HOST` in `ollama_director.py`). Override with `--ollama`.
- **`start_aidoom.ps1`** defaults to `http://localhost:11434`. Override with `-Ollama`.

`ollama_director.py` itself takes `--host` / `--ollama-port`, or a full `--ollama`
URL (which the launchers pass through). Its built-in default host is
`OLLAMA_HOST = "192.168.2.114"`.

## GPU monitor (`gpumon.py`)

Small live monitor for the GPU on the (remote) Ollama machine — handy while the
AI Director is running. Two sources:

- **`nvidia-smi` over SSH** → true GPU load %, VRAM, temperature, power.
- **Ollama `/api/ps`** (`--ollama-only`) → only the VRAM of loaded models (no SSH).

```sh
python3 gpumon.py                 # defaults: host 192.168.2.114, user lubee (SSH)
python3 gpumon.py --once          # one snapshot instead of the live loop
python3 gpumon.py --ollama-only   # no SSH, model VRAM only
```

On startup it self-tests the SSH read; if that fails it automatically falls back
to the Ollama mode. Override with `--host` / `--user` / `--interval`. (The remote
SSH details are in the project memory; SSH is Windows-OpenSSH as user `lubee`.)

## Without the launcher (manual)

```sh
cd files && ./aidoom -warp 1 1 -skill 4 -aidirector 31666 &
python3 ../ollama_director.py --port 31666 --model mistral:7b-instruct
```

See `../AGENT_CONTROL.md` (§12–13) and `../MONSTER_AGENT_GUIDE.md` for the
director protocol and order vocabulary.
