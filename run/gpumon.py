#!/usr/bin/env python3
"""
gpumon.py -- tiny live GPU monitor for the remote machine that runs Ollama.

Two data sources:
  * SSH + nvidia-smi  -> TRUE utilization %, VRAM, temperature, power (best).
  * Ollama /api/ps    -> only the VRAM held by currently-loaded models
                         (no Windows setup needed, but not real GPU load).

Usage:
  python3 gpumon.py --host 192.168.2.114 --user <WINDOWS_USERNAME>
  python3 gpumon.py --host 192.168.2.114 --user me --interval 1
  python3 gpumon.py --host 192.168.2.114 --ollama-only        # no SSH
"""
import argparse, json, subprocess, sys, time, urllib.request

SMI_QUERY = "utilization.gpu,memory.used,memory.total,temperature.gpu,power.draw,name"

# Defaults from ~/.aidoom.cfg (SDL3 config app), overridable on the CLI.
def _aidoom_cfg():
    import os
    cfg = {}
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        with open(os.path.join(here, "aidoom.cfg")) as f:
            for line in f:
                p = line.split()
                if len(p) >= 2:
                    cfg[p[0]] = p[1].strip('"')
    except OSError:
        pass
    return cfg
_CFG = _aidoom_cfg()
DEF_HOST = _CFG.get("ollama_host", "192.168.2.114")
DEF_OPORT = int(_CFG.get("ollama_port", "11434"))

CLR = "\033[2J\033[H"          # clear screen + home
DIM = "\033[2m"; RST = "\033[0m"; BOLD = "\033[1m"


def color(pct):
    return "\033[32m" if pct < 50 else "\033[33m" if pct < 85 else "\033[31m"


def bar(pct, width=34):
    pct = max(0.0, min(100.0, pct))
    n = int(round(width * pct / 100))
    return f"{color(pct)}[{'#'*n}{'-'*(width-n)}]{RST} {pct:5.1f}%"


def ssh_smi(user, host, key, port):
    cmd = ["ssh", "-p", str(port), "-o", "BatchMode=yes", "-o", "ConnectTimeout=4",
           "-o", "StrictHostKeyChecking=accept-new"]
    if key:
        cmd += ["-i", key]
    cmd += [f"{user}@{host}",
            f"nvidia-smi --query-gpu={SMI_QUERY} --format=csv,noheader,nounits"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=12)
    if r.returncode != 0:
        raise RuntimeError((r.stderr.strip() or "ssh/nvidia-smi failed").splitlines()[-1])
    gpus = []
    for line in r.stdout.strip().splitlines():
        p = [x.strip() for x in line.split(",")]
        if len(p) >= 6:
            gpus.append(dict(util=float(p[0]), mem_used=float(p[1]),
                             mem_total=float(p[2]), temp=p[3], power=p[4],
                             name=p[5]))
    return gpus


def ollama_models(host, port):
    url = f"http://{host}:{port}/api/ps"
    data = json.loads(urllib.request.urlopen(url, timeout=4).read())
    return data.get("models", [])


def render_ssh(gpus, host):
    lines = [f"{BOLD}GPU @ {host}{RST}   {DIM}{time.strftime('%H:%M:%S')}  (nvidia-smi/ssh){RST}", ""]
    for i, g in enumerate(gpus):
        mempct = 100 * g["mem_used"] / g["mem_total"] if g["mem_total"] else 0
        lines.append(f"  GPU {i}: {g['name']}")
        lines.append(f"    load {bar(g['util'])}")
        lines.append(f"    vram {bar(mempct)}  {g['mem_used']:.0f}/{g['mem_total']:.0f} MiB")
        lines.append(f"    {DIM}temp {g['temp']}C   power {g['power']} W{RST}")
        lines.append("")
    return "\n".join(lines)


def render_ollama(models, host):
    lines = [f"{BOLD}Ollama @ {host}{RST}   {DIM}{time.strftime('%H:%M:%S')}  (/api/ps -- VRAM of loaded models only){RST}", ""]
    if not models:
        lines.append(f"  {DIM}(no model loaded right now){RST}")
    for m in models:
        vram = m.get("size_vram", 0) / (1024**3)
        total = m.get("size", 0) / (1024**3)
        on_gpu = "GPU" if m.get("size_vram") else "CPU"
        lines.append(f"  {m.get('name','?'):28s} {vram:5.1f} GiB VRAM / {total:5.1f} GiB  [{on_gpu}]")
    lines.append("")
    lines.append(f"  {DIM}note: this is model VRAM, not compute utilization. Use --user for true GPU%.{RST}")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=DEF_HOST)
    ap.add_argument("--user", default="lubee",
                    help="SSH username for true GPU% (default: lubee; '' to disable SSH)")
    ap.add_argument("--ssh-port", type=int, default=22)
    ap.add_argument("--key", default=None, help="SSH private key (default: ssh's own)")
    ap.add_argument("--ollama-port", type=int, default=DEF_OPORT)
    ap.add_argument("--ollama-only", action="store_true")
    ap.add_argument("--once", action="store_true", help="print one snapshot and exit")
    ap.add_argument("--interval", type=float, default=2.0)
    args = ap.parse_args()

    use_ssh = bool(args.user) and not args.ollama_only

    # Startup self-test: if the SSH GPU read doesn't work, drop --user and fall
    # back to the Ollama mode instead of erroring on every frame.
    if use_ssh:
        try:
            ssh_smi(args.user, args.host, args.key, args.ssh_port)
        except Exception as e:
            sys.stderr.write(f"[gpumon] SSH GPU read via {args.user}@{args.host} failed: {e}\n"
                             f"[gpumon] falling back to Ollama /api/ps (model VRAM only).\n")
            use_ssh = False

    def frame():
        if use_ssh:
            return render_ssh(ssh_smi(args.user, args.host, args.key, args.ssh_port), args.host)
        return render_ollama(ollama_models(args.host, args.ollama_port), args.host)

    if args.once:
        print(frame())
        return

    try:
        while True:
            try:
                body = frame()
            except Exception as e:
                body = f"{BOLD}error:{RST} {e}\n\n{DIM}retrying in {args.interval}s ... (Ctrl-C to quit){RST}"
            sys.stdout.write(CLR + body + f"\n{DIM}Ctrl-C to quit{RST}\n")
            sys.stdout.flush()
            time.sleep(args.interval)
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
