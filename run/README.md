# aiDoom run folder (`run/`)

**The `launcher` GUI is the way to start aiDoom.** Run `run/launcher` (or
`launcher.exe`): pick the IWAD, optionally an extra **PWAD** (the dropdown below
IWAD lists every other wad here — freedoomstuff/hereticstuff/hexenstuff/… and any
PWAD you drop in, minus the IWADs and aidoom.wad — and adds it as `-file`), the
Buddy mode (off / `-coop` / `-aicoop`), the Monster mode (vanilla / L4D /
`-aidirector`), and the Skill, then hit **Launch** — it starts the game with the
right flags and auto-starts the native `director` sidecar when an AI mode is chosen.

> **Monster modes:** **vanilla** = stock 1993 AI; **L4D** = the rule-based `-director`
> (stress-driven extra spawns + the spoken game-master "voice of god"); **`-aidirector`** =
> the LLM monster director (Ollama). A selected **SIGIL / SIGIL II** PWAD is checksum-verified
> (name **and** MD5) and warped to its own episode — `SIGIL_V1_23` → **E5M1**,
> `SIGIL_COMPAT` → **E3M1**, `SIGIL II` → **E6M1**; a SIGIL-named file with the wrong checksum
> is ignored (not loaded).

> **Game WADs live in `run/ID0/`** (DOOM.WAD, doom2.wad, aidoom.wad,
> doom2stuff.wad, heretic.wad, …). The engine, launcher and tools search there
> automatically, so bare names (`-iwad DOOM.WAD`, `-file doom2stuff.wad`) resolve
> without a path. **Savegames** are written to `run/ID0/` too.

> The old one-shot **`start_*` shell/PowerShell scripts are obsolete** (the
> launcher replaces them) and have been moved to **`tools/scripts/`** as a backup
> — they still run from there if you want a scripted/headless launch.

> Keep this README in sync when the launcher or the native `director` change.

## Contents

| File | Platform | Purpose |
|------|----------|---------|
| `ID0/` | all | **Game WADs + savegames** (IWADs, aidoom.wad, doom2stuff.wad, doomsav*.dsg). |
| `director` / `director.exe` | Linux / Windows | **Native SDL3 AI director** (C) — the no-Python LLM monster director: a small window showing live status + a scrolling log, talks to Ollama + the game. Used by the launchers for `--director`. Build: `tools/build_director.sh` (Linux) / `tools/build_director_win.sh` (Windows). |
| (`../tools/buddy_voice.py`) | all | AI co-op **buddy voice**: tails `buddy_say.txt` (written here by `-aicoop`) and speaks lines via ElevenLabs (Joker-HL). Run from here: `python3 ../tools/buddy_voice.py`. Needs `ELEVENLABS_API_KEY` (env or `elevenlabs_api_key` in `aidoom.cfg`) + ffplay/mpg123. |
| `gpumon` / `gpumon.exe` | Linux / Windows | GPU monitor as a small **SDL window** (bars for load/VRAM/temp/power). On error it stops and shows a **Reconnect** button (no auto-retry). Build: Linux `tools/build_gpumon.sh`; Windows via CMake or `build_all_win.bat`. See **`../GPUMON.md`**. |
| `aidoom_config` / `aidoom_config.exe` | Linux / Windows | SDL3 settings editor; reads/writes `aidoom.cfg` here. Build: Linux `tools/build_config.sh`; Windows via CMake or `build_all_win.bat`. |
| `launcher` / `launcher.exe` | Linux / Windows | SDL3 **launcher GUI**: scans this folder (+ `~/.doom`) for IWADs, lets you pick the IWAD and the Buddy (off / `-coop` / `-aicoop`) and Monster (vanilla / L4D / `-aidirector`) modes, then launches the game with the right flags (auto-starting the native `director` sidecar when AI monsters are picked). Build: Linux `tools/build_launcher.sh`; Windows via CMake or `build_all_win.bat`. |
| `aidoom.cfg` | all | The single config file (game keys/video + Ollama), read by the game and all tools from this folder. |

(The source `aidoom.ico` lives one level up, in the repo root.)

## What the launcher does

1. **Wait for Ollama** — polls `<ollama>/api/version` (up to ~30 s).
2. **Check the model** is pulled (`/api/tags`); warns (doesn't abort) if missing.
3. **Warm the model** into memory (`/api/generate` with a tiny prompt) so the
   first in-game planning round isn't slow. Skip with `--no-warm` / `-NoWarm`.
4. **Start aiDoom** with `-aidirector <port>` (and `-warp`, `-skill`, the AI co-op
   companion `-aicoop` by default, optional `-friendlyfire`).
5. **Start the native director** (`run/director`, a small SDL3 window), which loops
   observe → ask the LLM → issue squad orders and shows live status + a log. The
   game keeps running if the model is unavailable. Closing the director window (or
   Ctrl-C) stops the game.

## Requirements

- A built **aidoom** binary (the launcher looks in `run/`, then `../files/`).
  - Linux/macOS: run **`../build.sh`** — it compiles against system SDL3 and
    copies the `aidoom` binary into `run/`. (SDL3 is linked from the system, so
    there's no library to copy alongside it.)
  - Windows (MSVC + the SDL3 SDK): run **`..\build_all_win.bat`** — it builds
    `aidoom.exe` **and** both tools and copies them + `SDL3.dll` into `run/`.
    Equivalently with CMake (finds a sibling `../SDL3` SDK automatically, also
    stages everything into `run/`): `cmake -B build && cmake --build build`.
    (The legacy `tools/build_*_win.sh` are MinGW cross-builds needing a MinGW SDL3
    package; the MSVC path above is preferred on Windows.)
- A DOOM **IWAD**, placed in **`run/ID0/`**. The engine finds one via `-iwad <file>`
  → the `iwad` key in `aidoom.cfg` → **`run/ID0/`** → `iwads/` → `run/` → `$DOOMWADDIR`
  → a Steam install. Bare names resolve under `ID0/` automatically. Pick which to use
  in the launcher/config app. **Bring your own** — IWADs are not shipped.
- An **Ollama** server reachable over HTTP, with the chosen model pulled
  (`ollama pull mistral:7b-instruct`). For a *remote* server it must listen on the
  network (`OLLAMA_HOST=0.0.0.0 ollama serve`), not just localhost.
- The **native director** binary for `--director`: build it once with
  `tools/build_director.sh` (Linux) / `tools/build_director_win.sh` (MinGW). No
  Python required.

## Usage — the launcher (preferred)

```sh
cd run
./launcher          # pick IWAD / buddy / monster / skill, then Launch
```

## Usage — backup scripts (Linux / macOS)

The `start_*` scripts now live in **`tools/scripts/`** (the launcher replaces them).
Run them from there if you want a scripted launch — they still work:

```sh
cd run
../tools/scripts/start_aidoom.sh                    # FULL LLM: AI buddy (-aicoop) + director (default)
../tools/scripts/start_aidoom.sh --buddy                           # rule-based buddy instead of the AI buddy
../tools/scripts/start_aidoom.sh --offline                         # plain aidoom, no LLM/Ollama
../tools/scripts/start_aidoom.sh --model qwen3:8b --skill 4 --friendlyfire
../tools/scripts/start_aidoom.sh --ollama http://localhost:11434   # override the server
```

Flags (unrecognized args are passed straight through to `aidoom`):

| Flag | Default | Meaning |
|------|---------|---------|
| `--director` | off | LLM **monster** director (`-aidirector` + run the director) |
| `--buddy` | off | rule-based co-op buddy, player 2 (`-coop`, no LLM) |
| `--aicoop` | off | **AI buddy**: `-aicoop` + turns the director on, so the LLM directs the buddy's tactics (engage/defend/regroup/…) in the same loop as the monsters |
| `--model <name>` | `mistral:7b-instruct` | Ollama model for tactics |
| `--port <n>` | `31666` | director TCP port (`-aidirector`) |
| `--episode <n>` / `--map <n>` | `1` / `1` | `-warp` target |
| `--skill <n>` | `4` | difficulty (1–5) |
| `--ollama <url>` | `http://192.168.2.114:11434` | Ollama base URL |
| `--friendlyfire` | off | enable monster infighting (`-friendlyfire`) |
| `--no-warm` | off | skip model warm-up |

`--director` (monsters) and `--aicoop` (buddy) compose: pass both and one LLM loop
directs the enemy squad *and* the ally buddy from the same observation.

## Usage — Windows

Prefer **`launcher.exe`**. The backup scripts now live in **`tools\scripts\`**:
double-click **`tools\scripts\start_aidoom.bat`**, or run the PowerShell script:

```powershell
tools\scripts\start_aidoom.ps1      # FULL LLM: AI buddy (-aicoop) + director (default)
.\start_aidoom.ps1 -RuleCoop        # rule-based buddy instead of the AI buddy
.\start_aidoom.ps1 -NoDirector      # just the game, no LLM director
.\start_aidoom.ps1 -Model qwen2.5-coder:1.5b -Skill 4 -FriendlyFire
```

Params: `-Model -Port -Episode -Map -Skill -Ollama -FriendlyFire -NoCoop -NoDirector -NoWarm`.

The **AI co-op companion** (player 2) is launched **by default**; pass `--no-coop`
(`-NoCoop` on Windows) to play solo.

## Ollama server location

The server URL is configurable and defaults differ by platform:

- **`start_aidoom.sh`** defaults to **`http://192.168.2.114:11434`** (from
  `aidoom.cfg`). Override with `--ollama`.
- **`start_aidoom.ps1`** defaults to `http://localhost:11434`. Override with `-Ollama`.

The **`director`** binary itself takes `--host` / `--ollama-port`, or a full
`--ollama` URL (which the launchers pass through), and reads `aidoom.cfg` next to it
on startup.

**`aidoom.cfg`** (in this folder, written by the SDL3 config app `aidoom_config`)
sets `ollama_host` / `ollama_port` / `ollama_model`, the chosen `iwad`, and the GPU
monitor's `gpu_host` / `gpu_user` / `gpu_ssh_port`. The `director` and
`start_aidoom.sh`/`.ps1` read it (next to themselves) on startup; CLI flags still
override. It's the same file the game uses for keys/video.

## GPU monitor

While the AI Director runs, watch the Ollama machine's GPU with **`gpumon`** (an SDL3
window, built into this folder).
It reads `gpu_host` / `gpu_user` / `gpu_ssh_port` from `aidoom.cfg` here and uses
`nvidia-smi` over SSH (or directly for `localhost`); the SDL one has a **Reconnect**
button. Full docs: **`../GPUMON.md`**.

## Without the launcher (manual)

```sh
cd run && ./aidoom -warp 1 1 -skill 4 -aidirector 31666 &
./director --port 31666 --model mistral:7b-instruct
```

See `../AGENT_CONTROL.md` (§12–13) and `../MONSTER_AGENT_GUIDE.md` for the
director protocol and order vocabulary.

## Full LLM mode (marine + buddy + monsters)

`./start_llm_player.sh [IWAD] [E M]` runs aiDoom **fully LLM-driven** -- the LLM controls
the marine, the AI buddy AND the monsters:

- **marine** -- `-aiplayer <port>` (`files/g_agent.c`) exposes an `observe` JSON state and
  accepts high-level intents (`goto`/`target`/`attack`/`use`/`weapon`/`face`/`stop`) over a
  TCP socket; `llm_player.py` is the brain (observe -> Ollama -> one command), and a C
  reflex controller turns each intent into per-tic ticcmds (aim/step/fire, line-of-sight
  gated). `-aiplayer demo` = built-in scripted brain (no LLM), to prove the hook.
- **buddy + monsters + pacing** -- `-aidirector <port> -aicoop` + the `director` sidecar
  (`run/director`): one Ollama brain that already issues monster tactics, buddy orders and
  L4D spawn pacing. The script starts it if present; without it the engine uses its
  built-in rule director (no LLM monsters/buddy).

Two independent sockets (player 31700, director 31666), each with its own client; both
talk to Ollama. Env: `PLAYER_PORT`, `DIRECTOR_PORT`, `OLLAMA_URL`, `OLLAMA_MODEL` (player
brain, default `llama3.1:8b`), `DECISION_SECS`.
