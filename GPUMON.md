# gpumon — GPU monitor for the Ollama machine

A small live monitor for the GPU that runs your Ollama models — handy while the
**LLM AI Director** is driving monsters/the buddy. It's a native **SDL3 window**
(`run/gpumon` / `run\gpumon.exe`, source `tools/gpumon_sdl.c`) with live bars, the
currently-loaded Ollama model, and a **Reconnect** button. No Python.

It shows **GPU load %, memory, temperature and power**, reading its target from
`aidoom.cfg`. The monitoring tool is **auto-detected**, so the same binary works
against an NVIDIA box or an Apple-Silicon Mac.

---

## Supported devices

| GPU / host | Tool used | How it's read | Needs sudo? |
|---|---|---|---|
| **NVIDIA** (Linux, Windows, WSL) | `nvidia-smi` | CSV query | no |
| **Apple Silicon** (M1–M4, incl. Pro/Max/Ultra) | [`macmon`](https://github.com/vladkens/macmon) | `macmon pipe` JSON (IOReport) | **no** |
| AMD / Intel GPUs | — | not supported yet | — |

gpumon **probes the target once** (over SSH, or locally) and picks the tool:

1. `nvidia-smi` present → **NVIDIA** mode.
2. else `macmon` present → **Apple Silicon** mode.
3. else host is macOS but no `macmon` → shows *"macmon not installed — run: brew install macmon"*.
4. else → *"no GPU tool found (need nvidia-smi or macmon)"*.

The detected tool is cached; clicking **Reconnect** re-probes (so you can install
`macmon`, then reconnect, without restarting gpumon).

### What each bar means per device

| Bar | NVIDIA (`nvidia-smi`) | Apple Silicon (`macmon`) |
|---|---|---|
| **GPU load** | `utilization.gpu` % | `gpu_usage` ratio ×100 |
| **VRAM / RAM** | dedicated VRAM used / total | **unified memory** used / total (labelled "RAM") |
| **Temp** | `temperature.gpu` °C | `gpu_temp_avg` °C — *may read 0 on some Apple chips* |
| **Power** | `power.draw` W (full-scale 350 W) | `gpu_power` W (full-scale 60 W) |

> **Apple notes:** Apple Silicon has **unified memory** (no separate VRAM), so the
> memory bar shows total system RAM usage — the closest proxy for "GPU memory".
> GPU temperature isn't exposed by every M-series chip; if it reads 0, that's the
> platform, not a bug. `macmon` uses Apple's IOReport, so it needs **no `sudo`** —
> unlike `powermetrics`.

---

## Where the data comes from

- **GPU stats over SSH** (`nvidia-smi` or `macmon pipe`) → load, memory, temp, power.
- **Ollama `/api/ps` over HTTP** (direct to the Ollama host, no SSH) → the name of the
  model currently loaded on the GPU, shown under the card name (or "(none loaded)").
- **localhost** → the tool runs **directly, without SSH** (no key needed) when the host
  is empty / `localhost` / `127.0.0.1`.
- **WSL hosts:** if the SSH session lands in a WSL shell (where `nvidia-smi` isn't on
  PATH), it falls back to `nvidia-smi.exe`, then `/mnt/c/Windows/System32/nvidia-smi.exe`.

---

## Configuration

Reads **`aidoom.cfg`** (written by the `aidoom_config` settings app, in the `run/`
folder). Keys used:

| Key | Meaning | Default |
|-----|---------|---------|
| `gpu_host` | host running the GPU (falls back to `ollama_host`) | `192.168.2.114` |
| `gpu_user` | SSH user on that host | `lubee` |
| `gpu_ssh_port` | SSH port | `22` |
| `ollama_host` | host serving Ollama (for the `/api/ps` model readout) | = `gpu_host` |

Set `gpu_host` to **`localhost`** to monitor the local GPU with no SSH at all.
Override at launch with `--host` / `--user` / `--port`.

Example for a Mac M4 running Ollama on the LAN:

```ini
gpu_host        192.168.2.102
gpu_user        yourmacuser
gpu_ssh_port    22
ollama_host     192.168.2.102
```

---

## Installing the monitoring tool on the target

### NVIDIA host
`nvidia-smi` ships with the NVIDIA driver — nothing to install. Just make sure it's
on `PATH` for the SSH user (`ssh user@host nvidia-smi` should print a table).

### Apple Silicon (Mac) host
Install **macmon** (Rust, tiny, no sudo at runtime):

```sh
brew install macmon
# verify it emits JSON:
macmon pipe -s 1
```

gpumon automatically prepends Homebrew's bin (`/opt/homebrew/bin` on Apple Silicon,
`/usr/local/bin` on Intel) to the remote `PATH`, so you do **not** need macmon on the
non-interactive SSH `PATH` — a plain `brew install macmon` is enough.

Also enable **Remote Login** so gpumon can SSH in:
*System Settings → General → Sharing → Remote Login → On* (note the user it lists).

---

## SSH setup (remote host)

For a remote host gpumon SSHes in, so it needs your **public key installed there**
(passwordless — gpumon uses `BatchMode=yes` and never prompts for a password).

Easiest: open **`aidoom_config`** and click **"Copy SSH key"** (it generates a key if
needed and installs it on `gpu_user@gpu_host`). Or manually:

```sh
# Linux / macOS / Windows (OpenSSH client):
ssh-keygen -t ed25519            # once, if you don't have a key yet
ssh-copy-id -p 22 user@host      # installs your public key on the target

# Windows without ssh-copy-id — append your key by hand:
type %USERPROFILE%\.ssh\id_ed25519.pub | ssh user@host "mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys"
```

Verify it's passwordless (this must NOT ask for a password):

```sh
ssh -o BatchMode=yes user@host echo ok
```

gpumon connects with `BatchMode=yes` (never prompts) and `StrictHostKeyChecking=accept-new`,
so the **first** connection to a new host trusts and records its key automatically
(trust-on-first-use) — you don't have to `ssh` in once by hand first.

> **macOS target:** after `ssh-copy-id`, the first key login may still require that
> *Remote Login* is enabled (above) and that the user is allowed. If `~/.ssh` was just
> created, macOS wants `chmod 700 ~/.ssh && chmod 600 ~/.ssh/authorized_keys`.

> **Windows host (as the target):** install the *OpenSSH Server* optional feature and
> put your key in `C:\Users\<user>\.ssh\authorized_keys` (admins: also
> `%ProgramData%\ssh\administrators_authorized_keys`).

> **Windows note (gpumon as the client):** `gpumon.exe` is a 32-bit program, so it
> locates `nvidia-smi` through the `Sysnative` alias (a 32-bit process otherwise sees
> `System32` redirected to `SysWOW64`, where `nvidia-smi` isn't). It runs all commands
> via `CreateProcess`/`CREATE_NO_WINDOW`, so no console window flashes on each poll.

---

## Run

Run `run/gpumon` (Linux/macOS) / `run\gpumon.exe` (Windows). It polls every ~2 s and
draws load / memory / temp / power bars plus the loaded Ollama model. **On a
connection error it stops** (no silent auto-retry) and shows a **Reconnect** button —
click it to try again (this also re-probes which tool to use). `Esc` quits.

---

## Build

```sh
cmake -B build && cmake --build build      # any platform — also builds the game + config app
./build_all_win.bat                        # Windows one-shot (MSVC); stages into run/
tools/build_gpumon.sh                       # Linux only (gcc + pkg-config sdl3)
```

(Source is `tools/gpumon_sdl.c`; the output binary is named `gpumon`.)

---

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `ssh failed (user@host)` | SSH can't connect non-interactively. Test `ssh -o BatchMode=yes user@host echo ok`; fix the key (see SSH setup) or the host/port in `aidoom.cfg`. |
| `Host key verification failed` / `known_hosts: Permission denied` | On Windows, `%USERPROFILE%\.ssh\known_hosts` is unreadable or its ACL is broken. Fix with `icacls "%USERPROFILE%\.ssh\known_hosts" /grant "%USERNAME%:F"` (or delete the file — gpumon's `accept-new` recreates it). |
| `macmon not installed — run: brew install macmon` | The Mac was detected but `macmon` isn't installed at all. `brew install macmon` (gpumon finds it in Homebrew's bin automatically), then click **Reconnect**. |
| `no GPU tool found` | Neither `nvidia-smi` nor `macmon` is on the target's `PATH` for the SSH user. |
| `ssh/macmon failed` | macmon is present but errored/produced no JSON. Run `ssh user@host macmon pipe -s 1` by hand and check the output. |
| Temp bar shows **0 °C** on a Mac | Expected on some Apple Silicon chips — the GPU temp sensor isn't exposed via IOReport. |
| Power bar barely moves on a Mac | Apple GPUs draw far less than NVIDIA; full-scale is 60 W for Apple vs 350 W for NVIDIA. |
| Model shows "(none loaded)" | Ollama has unloaded the model (idle), or `ollama_host`/port 11434 isn't reachable over HTTP. |
