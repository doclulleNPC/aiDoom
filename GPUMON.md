# gpumon — GPU monitor for the Ollama machine

A small live monitor for the GPU that runs your Ollama models — handy while the
**LLM AI Director** is driving monsters/the buddy. It's a native **SDL3 window**
(`run/gpumon` / `run\gpumon.exe`, source `tools/gpumon_sdl.c`) with live bars and a
**Reconnect** button. No Python.

It shows **GPU load %, VRAM, temperature and power** via `nvidia-smi`, reading its
target from `aidoom.cfg`.

## Where the data comes from

- **`nvidia-smi` over SSH** → true GPU utilisation, VRAM, temp, power.
- **Ollama `/api/ps` over HTTP** (direct to the Ollama host, no SSH) → the name of the
  model currently loaded on the GPU, shown under the card name (or "(none loaded)").
- **localhost** → `nvidia-smi` is run **directly, without SSH** (no key needed) when
  the host is empty / `localhost` / `127.0.0.1`.
- **WSL hosts:** if the SSH session lands in a WSL shell (where `nvidia-smi` isn't on
  PATH), it falls back to `nvidia-smi.exe`, then `/mnt/c/Windows/System32/nvidia-smi.exe`,
  so it works whether SSH lands in cmd/PowerShell or WSL.

## Configuration

Reads **`aidoom.cfg`** (written by the `aidoom_config` settings app, in the `run/`
folder). Keys used:

| Key | Meaning | Default |
|-----|---------|---------|
| `gpu_host` | host running the GPU (falls back to `ollama_host`) | `192.168.2.114` |
| `gpu_user` | SSH user on that host | `lubee` |
| `gpu_ssh_port` | SSH port | `22` |

Set `gpu_host` to **`localhost`** to monitor the local GPU with no SSH at all.
Override at launch with `--host` / `--user` / `--port`.

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

## Run

Run `run/gpumon` (Linux) / `run\gpumon.exe` (Windows). It polls every ~2 s and draws
load / VRAM / temp / power bars. **On a connection error it stops** (no silent
auto-retry) and shows a **Reconnect** button — click it to try again. `Esc` quits.

## Build

```sh
cmake -B build && cmake --build build      # any platform — also builds the game + config app
./build_all_win.bat                        # Windows one-shot (MSVC); stages into run/
tools/build_gpumon.sh                       # Linux only (gcc + pkg-config sdl3)
```

(Source is `tools/gpumon_sdl.c`; the output binary is named `gpumon`.)
