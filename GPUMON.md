# gpumon — GPU monitor for the Ollama machine

A small live monitor for the GPU that runs your Ollama models — handy while the
**LLM AI Director** is driving monsters. It comes in two flavours:

| Tool | What | Where |
|------|------|-------|
| **`gpumon.py`** | terminal monitor (ANSI bars), Python, no build | repo root |
| **`gpumon` / `gpumon.exe`** | small **SDL3 window** with live bars + a **Reconnect** button | built into `run/` |

Both show **GPU load %, VRAM, temperature and power** via `nvidia-smi`, and both
read their target from the same `aidoom.cfg`.

## Where the data comes from

- **`nvidia-smi` over SSH** → true GPU utilisation, VRAM, temp, power (the good one).
- **localhost** → `nvidia-smi` is run **directly, without SSH** (no key needed) when
  the host is empty / `localhost` / `127.0.0.1`.
- **Ollama `/api/ps`** (`gpumon.py --ollama-only`) → only the VRAM of currently
  loaded models, no SSH. A fallback when you can't SSH.

## Configuration

Both tools read **`aidoom.cfg`** (written by the `aidoom_config` settings app, in the
`run/` folder). Keys used:

| Key | Meaning | Default |
|-----|---------|---------|
| `gpu_host` | host running the GPU (falls back to `ollama_host`) | `192.168.2.114` |
| `gpu_user` | SSH user on that host | `lubee` |
| `gpu_ssh_port` | SSH port | `22` |

Set `gpu_host` to **`localhost`** to monitor the local GPU with no SSH at all.

## SSH setup (remote host)

For a remote host the monitor SSHes in, so it needs your public key installed there
(passwordless). Easiest: open **`aidoom_config`** and click **“Copy SSH key”** (it
generates a key if needed and installs it on `gpu_user@gpu_host`). Or manually:

```sh
ssh-copy-id -p 22 lubee@192.168.2.114      # Linux/macOS
```

> **Windows note:** `gpumon.exe` is a 32-bit program, so it locates `nvidia-smi`
> through the `Sysnative` alias (a 32-bit process otherwise sees `System32`
> redirected to `SysWOW64`, where `nvidia-smi` isn't). It also runs all commands via
> `CreateProcess`/`CREATE_NO_WINDOW`, so no console window flashes on each poll.

## `gpumon.py` (terminal)

```sh
python3 gpumon.py                 # host/user from aidoom.cfg, else 192.168.2.114 / lubee
python3 gpumon.py --host localhost --user ""   # local GPU, no SSH
python3 gpumon.py --once          # one snapshot instead of the live loop
python3 gpumon.py --ollama-only   # no SSH, model VRAM only (Ollama /api/ps)
```

Options: `--host --user --ssh-port --key --ollama-port --ollama-only --once
--interval`. On startup it self-tests the SSH read and falls back to Ollama mode if
that fails, so it won't error on every frame.

## `gpumon` (SDL3 window)

Run `run/gpumon` (Linux) / `run\gpumon.exe` (Windows). It polls every ~2 s and draws
load / VRAM / temp / power bars. **On a connection error it stops** (no silent
auto-retry) and shows a **Reconnect** button — click it to try again. `Esc` quits.
Accepts `--host` / `--user` / `--port` to override the config.

## Build (the SDL window version)

`gpumon.py` needs no build. The SDL `gpumon`:

```sh
cmake -B build && cmake --build build      # any platform — also builds the game + config app
./build_all_win.bat                        # Windows one-shot (MSVC); stages into run/
tools/build_gpumon.sh                       # Linux only (gcc + pkg-config sdl3)
```

(Source is `tools/gpumon_sdl.c`; the output binary is named `gpumon`.)
