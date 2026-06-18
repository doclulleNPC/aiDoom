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
| `gpumon.py` | all | Live GPU monitor for the remote Ollama machine, terminal (see below). |
| `gpumon_sdl` / `gpumon_sdl.exe` | Linux / Windows | Same monitor as a small **SDL window** (bars for load/VRAM/temp/power). Build: Linux `tools/build_gpumon.sh`; Windows via CMake or `build_all_win.bat` (below). |
| `aidoom_config` / `aidoom_config.exe` | Linux / Windows | SDL3 settings editor; reads/writes `aidoom.cfg` here. Build: Linux `tools/build_config.sh`; Windows via CMake or `build_all_win.bat`. |
| `aidoom.cfg` | all | The single config file (game keys/video + Ollama), read by the game and all tools from this folder. |
| `aidoom.ico` | Windows | Source icon. The game **and** both tools embed it as their exe + live window/taskbar icon, so this file isn't needed at runtime. |

## What the launcher does

1. **Wait for Ollama** — polls `<ollama>/api/version` (up to ~30 s).
2. **Check the model** is pulled (`/api/tags`); warns (doesn't abort) if missing.
3. **Warm the model** into memory (`/api/generate` with a tiny prompt) so the
   first in-game planning round isn't slow. Skip with `--no-warm` / `-NoWarm`.
4. **Start aiDoom** with `-aidirector <port>` (and `-warp`, `-skill`, the AI co-op
   companion `-aicoop` by default, optional `-friendlyfire`).
5. **Start the Python director** (`ollama_director.py`), which loops
   observe → ask the LLM → issue squad orders. The game keeps running if Python
   or the model is unavailable. Closing the director (or Ctrl-C) stops the game.

## Requirements

- A built **aidoom** binary (the launcher looks in `run/`, then `../files/`).
  - Linux/macOS: run **`../build.sh`** — it compiles against system SDL3 and
    copies the `aidoom` binary into `run/`. (SDL3 is linked from the system, so
    there's no library to copy alongside it.)
  - Windows (MSVC + the SDL3 SDK): run **`..\build_all_win.bat`** — it builds
    `aidoom.exe` **and** both tools and copies them + `SDL3.dll` into `run/`.
    Equivalently with CMake (also stages everything into `run/`):
    `cmake -B build -DCMAKE_PREFIX_PATH=C:/path/to/SDL3 && cmake --build build`.
    (The legacy `tools/build_*_win.sh` are MinGW cross-builds needing a MinGW SDL3
    package; the MSVC path above is preferred on Windows.)
- A DOOM **IWAD**. The engine finds one via `-iwad <file>` → the `iwad` key in
  `aidoom.cfg` → an `iwads/` subfolder → `run/` (the binary's folder) → a Steam
  install. Pick which to use in the config app, or drop a `.wad` in `run/iwads/`.
  **Bring your own** — IWADs are not shipped.
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
| `--no-coop` | off | disable the AI co-op companion (player 2, on by default) |
| `--no-director` | off | launch only the game |
| `--no-warm` | off | skip model warm-up |

## Usage — Windows

Double-click **`start_aidoom.bat`**, or run the PowerShell script with params:

```powershell
.\start_aidoom.ps1
.\start_aidoom.ps1 -Model qwen2.5-coder:1.5b -Skill 4 -FriendlyFire
.\start_aidoom.ps1 -NoDirector
```

Params: `-Model -Port -Episode -Map -Skill -Ollama -FriendlyFire -NoCoop -NoDirector -NoWarm`.

The **AI co-op companion** (player 2) is launched **by default**; pass `--no-coop`
(`-NoCoop` on Windows) to play solo.

## Ollama server location

The server URL is configurable and defaults differ by platform:

- **`start_aidoom.sh`** defaults to **`http://192.168.2.114:11434`** (matches
  `OLLAMA_HOST` in `ollama_director.py`). Override with `--ollama`.
- **`start_aidoom.ps1`** defaults to `http://localhost:11434`. Override with `-Ollama`.

`ollama_director.py` itself takes `--host` / `--ollama-port`, or a full `--ollama`
URL (which the launchers pass through). Its built-in default host is
`OLLAMA_HOST = "192.168.2.114"`.

**`aidoom.cfg`** (in this folder, written by the SDL3 config app `aidoom_config`)
sets `ollama_host` / `ollama_port` / `ollama_model`, the chosen `iwad`, and the GPU
monitor's `gpu_host` / `gpu_user` / `gpu_ssh_port`. `ollama_director.py`,
`gpumon.py` and `start_aidoom.sh`/`.ps1` read it (next to themselves) on startup;
CLI flags still override. It's the same file the game uses for keys/video.

## GPU monitor (`gpumon.py`)

Small live monitor for the GPU on the (remote) Ollama machine — handy while the
AI Director is running. Two sources:

- **`nvidia-smi` over SSH** → true GPU load %, VRAM, temperature, power.
- **Ollama `/api/ps`** (`--ollama-only`) → only the VRAM of loaded models (no SSH).

```sh
python3 gpumon.py                 # host/user from aidoom.cfg (gpu_host/gpu_user), else 192.168.2.114 / lubee
python3 gpumon.py --once          # one snapshot instead of the live loop
python3 gpumon.py --ollama-only   # no SSH, model VRAM only
```

On startup it self-tests the SSH read; if that fails it automatically falls back
to the Ollama mode. Override with `--host` / `--user` / `--interval`. (The remote
SSH details are in the project memory; SSH is Windows-OpenSSH as user `lubee`.)

There's also a **graphical version**, `gpumon_sdl` (SDL3 window with live bars for
GPU load / VRAM / temperature / power). It reads the same `aidoom.cfg`
(`gpu_host` / `gpu_user` / `gpu_ssh_port`) and accepts `--host` / `--user` /
`--port`; build it with `tools/build_gpumon.sh` (Linux) or
`tools/build_gpumon_win.sh` (Windows/MinGW).

## Without the launcher (manual)

```sh
cd files && ./aidoom -warp 1 1 -skill 4 -aidirector 31666 &
python3 ../ollama_director.py --port 31666 --model mistral:7b-instruct
```

See `../AGENT_CONTROL.md` (§12–13) and `../MONSTER_AGENT_GUIDE.md` for the
director protocol and order vocabulary.
