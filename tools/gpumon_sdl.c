// gpumon_sdl -- graphical (SDL3) GPU monitor for the remote Ollama machine.
//
// Same data source as gpumon.py: it runs `nvidia-smi` over SSH and shows live
// bars for GPU utilisation, VRAM, temperature and power.  Host/user come from
// aidoom.cfg (gpu_host/gpu_user/gpu_ssh_port, falling back to ollama_host) next
// to the binary, or from --host/--user.  Text is drawn from the baked font atlas
// (tools/font_atlas.h), so there are no deps beyond SDL3.
//
// Build: tools/build_gpumon.sh  (Linux)  /  tools/build_gpumon_win.sh (MinGW)

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_atlas.h"
#include "../files/aidoom_icon.h"	// shared 64x64 RGBA window icon (from aidoom.ico)

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>	// CreateProcess (run commands without a console window)
#include <io.h>		// _access (locate nvidia-smi past WoW64 redirection)
#endif

#define WINW 460
#define WINH 322
#define POLL_SECS 2

static SDL_Window*   win;
static SDL_Renderer* ren;
static SDL_Texture*  font;

static char host[128] = "localhost";
static char user[64]  = "lubee";
static int  sshport   = 22;
static char ohost[128] = "";	// Ollama host for the model query (defaults to `host`)

// shared GPU state (worker thread -> main thread)
typedef struct {
    int   valid, util, mem_used, mem_total, temp;
    float power;
    float pmax;		// power-bar full scale in W (nvidia ~350, Apple ~60)
    char  name[64];
    char  src[24];	// data source for the footer ("nvidia-smi" / "macmon")
    char  model[64];	// Ollama model currently loaded on the GPU (from /api/ps)
    char  err[160];
} gpustat_t;
static gpustat_t   g_stat;
static SDL_Mutex*  g_lock;
static SDL_AtomicInt g_running;
static SDL_AtomicInt g_paused;   // 1 = stop auto-polling after an error; cleared by Reconnect

// Reconnect button (only shown/active in the error state).
static const SDL_FRect btn_reconnect = { WINW/2.0f-70, WINH-66, 140, 28 };

// ----------------------------------------------------------------- config
static void load_cfg(void)
{
    char path[1024]; const char* base = SDL_GetBasePath();
    snprintf(path, sizeof(path), "%saidoom.cfg", base ? base : "./");
    FILE* f = fopen(path, "r"); if (!f) return;
    char line[256], k[64], v[160];
    char ollama[128] = "";
    int  have_gpuhost = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " %63s %159s", k, v) != 2) continue;
        if      (!strcmp(k,"gpu_host"))     { strncpy(host,v,sizeof(host)-1); have_gpuhost=1; }
        else if (!strcmp(k,"ollama_host"))  { strncpy(ollama,v,sizeof(ollama)-1); }
        else if (!strcmp(k,"gpu_user"))     { strncpy(user,v,sizeof(user)-1); }
        else if (!strcmp(k,"gpu_ssh_port")) { sshport = atoi(v); }
    }
    fclose(f);
    if (!have_gpuhost && ollama[0]) strncpy(host, ollama, sizeof(host)-1);
    // Model query hits Ollama directly (HTTP): prefer ollama_host, else the GPU host.
    strncpy(ohost, ollama[0] ? ollama : host, sizeof(ohost)-1);
}

// ----------------------------------------------------------------- font/draw
static void font_init(void)
{
    Uint32* px = malloc(FONT_AW*FONT_CH*4);
    for (int i=0;i<FONT_AW*FONT_CH;i++)
        px[i] = 0x00FFFFFFu | ((Uint32)font_alpha[i] << 24);
    SDL_Surface* s = SDL_CreateSurfaceFrom(FONT_AW, FONT_CH, SDL_PIXELFORMAT_ARGB8888, px, FONT_AW*4);
    font = SDL_CreateTextureFromSurface(ren, s);
    SDL_SetTextureBlendMode(font, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(font, SDL_SCALEMODE_NEAREST);
    SDL_DestroySurface(s); free(px);
}

static void text(float x, float y, const char* str, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetTextureColorMod(font, r, g, b);
    for (const char* p=str; *p; p++) {
        int c = (unsigned char)*p;
        if (c < FONT_FIRST || c >= FONT_FIRST+FONT_COUNT) c = '?';
        SDL_FRect src = { (float)((c-FONT_FIRST)*FONT_CW), 0, FONT_CW, FONT_CH };
        SDL_FRect dst = { x, y, FONT_CW, FONT_CH };
        SDL_RenderTexture(ren, font, &src, &dst);
        x += FONT_CW;
    }
}

static void fillrect(float x,float y,float w,float h, Uint8 r,Uint8 g,Uint8 b)
{
    SDL_FRect q={x,y,w,h}; SDL_SetRenderDrawColor(ren,r,g,b,255); SDL_RenderFillRect(ren,&q);
}

// a labelled bar: frac 0..1, colour ramps green->yellow->red with the value
static void bar(float y, const char* label, float frac, const char* valstr)
{
    Uint8 r,g,b;
    if (frac < 0.6f)      { r=60;  g=200; b=80;  }
    else if (frac < 0.85f){ r=220; g=190; b=40;  }
    else                  { r=220; g=60;  b=50;  }
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    text(16, y, label, 200,200,210);
    float bx=120, bw=WINW-120-16, bh=16;
    fillrect(bx, y, bw, bh, 40,40,48);                 // track
    fillrect(bx, y, bw*frac, bh, r,g,b);               // fill
    text(bx+6, y+1, valstr, 235,235,240);              // value (readable on track + fill)
}

// Run a shell command and capture its FULL combined stdout+stderr into `buf`.
// gpumon is a GUI app, so _popen/system would flash a console window on every
// poll -- on Windows we use CreateProcess with CREATE_NO_WINDOW instead.
// Returns the number of bytes captured (0 on failure).
#ifdef _WIN32
static int run_raw(const char* cmd, char* buf, int n)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr; si.hStdError = wr; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));

    char cl[1100]; snprintf(cl, sizeof(cl), "cmd /c %s", cmd);
    BOOL ok = CreateProcessA(NULL, cl, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return 0; }

    DWORD got = 0; int total = 0;
    while (total < n-1 &&
           ReadFile(rd, buf+total, n-1-total, &got, NULL) && got > 0)
        total += (int)got;
    buf[total] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return total;
}
#else
static int run_raw(const char* cmd, char* buf, int n)
{
    FILE* p = popen(cmd, "r");
    int total = 0, got;
    if (!p) return 0;
    while (total < n-1 && (got = (int)fread(buf+total, 1, n-1-total, p)) > 0)
        total += got;
    buf[total] = 0;
    pclose(p);
    return total;
}
#endif

// Run `cmd` and return its most relevant single line in `line`: the first line
// that starts with a digit (the nvidia-smi/CSV stats line, skipping shell noise
// like "bash: nvidia-smi: command not found"), else the first line so a real
// ssh/connection error still surfaces.  Returns 1 if any output was captured.
static int run_query(const char* cmd, char* line, int n)
{
    char buf[2048];
    if (run_raw(cmd, buf, sizeof(buf)) <= 0) return 0;
    char* best = NULL; char* p = buf;
    while (*p)
    {
        if (!best) best = p;
        if (*p >= '0' && *p <= '9') { best = p; break; }
        while (*p && *p != '\n' && *p != '\r') p++;
        while (*p == '\n' || *p == '\r') p++;
    }
    int i = 0;
    for (; i < n-1 && best && best[i] && best[i] != '\n' && best[i] != '\r'; i++) line[i] = best[i];
    line[i] = 0;
    return 1;
}

// ----------------------------------------------------------------- backends
//
// gpumon auto-detects which monitoring tool the target exposes, so the same
// binary works against an NVIDIA box, an Apple-Silicon Mac (Ollama via Metal),
// etc.  Supported sources:
//   - nvidia-smi  (NVIDIA GPUs; Linux/Windows/WSL)         -> CSV
//   - macmon      (Apple Silicon, `brew install macmon`)   -> JSON, no sudo
// See GPUMON.md.
//
enum { GM_DETECT = 0, GM_NVIDIA, GM_MACMON, GM_MACOS_NOMACMON, GM_NONE };
static int  g_mode = GM_DETECT;	// cached after the first successful probe
static char g_gpuname[64] = "";	// GPU/chip name learned during detection

static int host_is_local(void)
{
    return (!host[0] || !strcmp(host,"localhost") || !strcmp(host,"127.0.0.1"));
}

// Wrap a POSIX-sh `inner` command to run on the target: directly for localhost,
// else over SSH.  `inner` must not contain double quotes (it rides inside ssh "...").
//
// Two portability fixes baked in for the macmon/Apple path:
//  - Homebrew (Apple Silicon /opt/homebrew/bin, Intel /usr/local/bin) is NOT on a
//    non-interactive shell's PATH, so `macmon` wouldn't be found -- prepend it.
//  - StrictHostKeyChecking=accept-new lets the very first SSH connection trust the
//    host key under BatchMode (which otherwise aborts on an unknown host).
static void wrap_cmd(char* out, int n, const char* inner)
{
    if (host_is_local())
#ifdef _WIN32
        snprintf(out, n, "%s 2>&1", inner);	// (Windows-local uses the nvidia Sysnative path, not this)
#else
        snprintf(out, n, "PATH=/opt/homebrew/bin:/usr/local/bin:$PATH; %s 2>&1", inner);
#endif
    else
        snprintf(out, n,
            "ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 "
            "-o ServerAliveInterval=3 -o ServerAliveCountMax=2 -p %d %s@%s "
            // The remote command MUST be expanded by the TARGET shell, not ours.  On POSIX
            // we run via popen() -> `sh -c`, which would expand $PATH (to the local box's
            // PATH) and the detect probe's $(uname -s)/$(nvidia-smi ...)/$(sysctl ...)
            // command-substitutions LOCALLY -- so a Linux/macOS gpumon talking to a remote
            // Mac sent it garbage and mis-detected/failed.  SINGLE-quote the remote command
            // so sh passes it through verbatim and the target expands it.  (Windows cmd /c
            // doesn't expand $... at all, so it keeps double quotes and already worked.)
#ifdef _WIN32
            "\"PATH=/opt/homebrew/bin:/usr/local/bin:$PATH; %s\" 2>&1",
#else
            "'PATH=/opt/homebrew/bin:/usr/local/bin:$PATH; %s' 2>&1",
#endif
            sshport, user, host, inner);
}

// nvidia-smi with its fallbacks, so the SAME command resolves on every target: bare
// nvidia-smi (Linux, and cmd.exe which appends .exe), then nvidia-smi.exe (WSL on PATH),
// then the explicit Windows path via WSL interop (WSL where neither is on the
// NON-interactive SSH PATH -- the usual case, and why detect must use this too, not just
// the per-poll query).
static void nvidia_chain(char* out, int n, const char* args)
{
    snprintf(out, n,
        "nvidia-smi %s || nvidia-smi.exe %s || /mnt/c/Windows/System32/nvidia-smi.exe %s",
        args, args, args);
}

// Like wrap_cmd but PLAIN: no homebrew PATH prefix and double-quoted, so it works on a
// Windows target whose SSH shell is cmd.exe / PowerShell (which can't parse the POSIX
// `PATH=...;` prefix or `if/fi`).  Use this for nvidia-smi -- it's on the system PATH on
// Windows, WSL and Linux alike, and the command carries no `$` to expand locally.
static void wrap_plain(char* out, int n, const char* inner)
{
    if (host_is_local())
        snprintf(out, n, "%s 2>&1", inner);
    else
        snprintf(out, n,
            "ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 "
            "-o ServerAliveInterval=3 -o ServerAliveCountMax=2 -p %d %s@%s \"%s\" 2>&1",
            sshport, user, host, inner);
}

// --- tiny JSON scrapers for macmon's `pipe` output (no JSON lib needed) ------
// Number right after the first `"key":` (handles ints and floats).
static int json_num(const char* buf, const char* key, double* out)
{
    const char* p = strstr(buf, key);
    if (!p) return 0;
    p += strlen(key);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (*p == '"' || *p == '[' || *p == '{') return 0;
    char* end; double v = strtod(p, &end);
    if (end == p) return 0;
    *out = v; return 1;
}
// Second element of the array after `"key":` (macmon's [freq, usage] tuples).
static int json_arr1(const char* buf, const char* key, double* out)
{
    const char* p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p, '['); if (!p) return 0;
    p = strchr(p, ','); if (!p) return 0;	// skip element 0 (the frequency)
    char* end; double v = strtod(p+1, &end);
    if (end == p+1) return 0;
    *out = v; return 1;
}

// Probe the target once for which tool is present (+ the GPU/chip name).
// Sets g_mode and g_gpuname.  Returns 0 only if the probe itself failed (e.g.
// ssh down), so the caller can show a connection error and let the user retry.
// Is `line` a real nvidia-smi GPU name rather than a "command not found" style error?
static int looks_like_gpu_name(const char* s)
{
    if (!*s) return 0;
    static const char* errs[] = {
        "not recognized", "not found", "No such", "command not", "is not",
        "Permission", "cannot", "Error", "error:", "usage:", "Usage:", NULL };
    for (int i = 0; errs[i]; i++)
        if (strstr(s, errs[i])) return 0;
    return 1;
}

static int detect_mode(void)
{
#ifdef _WIN32
    if (host_is_local()) { g_mode = GM_NVIDIA; return 1; }	// Windows-local: NVIDIA via Sysnative
#endif
    char cmd[1024], buf[2048], inner[640];

    // 1) NVIDIA first, SHELL-AGNOSTIC: the nvidia-smi name query (+ WSL fallbacks) works on a
    //    Windows SSH shell (cmd.exe / PowerShell), on WSL bash and on Linux -- no POSIX
    //    `if/fi`, no homebrew PATH prefix (which a Windows shell can't parse).  Uses the SAME
    //    fallback chain as the per-poll query so detect can't fail where the query would work.
    nvidia_chain(inner, sizeof(inner), "--query-gpu=name --format=csv,noheader");
    wrap_plain(cmd, sizeof(cmd), inner);
    if (run_raw(cmd, buf, sizeof(buf)) > 0) {
        for (char* p = buf; *p; ) {
            while (*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
            char* eol = p; while (*eol && *eol!='\r' && *eol!='\n') eol++;
            char save = *eol; *eol = 0;
            if (eol > p && looks_like_gpu_name(p)) {
                g_mode = GM_NVIDIA;
                snprintf(g_gpuname, sizeof g_gpuname, "%s", p);
                return 1;
            }
            *eol = save; p = (*eol) ? eol + 1 : eol;
        }
    }

    // 2) macmon (Apple Silicon): a POSIX shell + the homebrew PATH (set by wrap_cmd).
    static const char* probe =
        "if command -v macmon >/dev/null 2>&1; then "
          "echo GMTOOL=MACMON NAME=$(sysctl -n machdep.cpu.brand_string 2>/dev/null); "
        "elif [ $(uname -s 2>/dev/null) = Darwin ]; then echo GMTOOL=MACOS; "
        "else echo GMTOOL=NONE; fi";
    wrap_cmd(cmd, sizeof(cmd), probe);
    if (run_raw(cmd, buf, sizeof(buf)) <= 0) return 0;		// ssh/exec failed

    char* t = strstr(buf, "GMTOOL=");
    if (!t) return 0;						// no verdict -> retry
    t += 7;
    if      (!strncmp(t,"MACMON",6)) g_mode = GM_MACMON;
    else if (!strncmp(t,"MACOS",5))  g_mode = GM_MACOS_NOMACMON;
    else if (!strncmp(t,"NONE",4))   g_mode = GM_NONE;
    else return 0;
    g_gpuname[0] = 0;
    char* nm = strstr(buf, "NAME=");
    if (nm) sscanf(nm + 5, " %63[^\r\n]", g_gpuname);
    return 1;
}

// ----------------------------------------------------------------- worker
static int fetch_thread(void* unused)
{
    (void)unused;
    char cmd[1024];
    const char* smiargs =
        "--query-gpu=utilization.gpu,memory.used,memory.total,"
        "temperature.gpu,power.draw,name --format=csv,noheader,nounits";
    while (SDL_GetAtomicInt(&g_running)) {
        // Paused after an error -- wait for the user to hit Reconnect.
        if (SDL_GetAtomicInt(&g_paused)) { SDL_Delay(100); continue; }

        int is_local = host_is_local();
        gpustat_t s; memset(&s, 0, sizeof(s));

        // 1) Detect which monitoring tool the target has (cached after success).
        if (g_mode == GM_DETECT && !detect_mode()) {
            snprintf(s.err, sizeof(s.err),
                     is_local ? "probe failed (local)" : "ssh failed (%s@%s)", user, host);
            SDL_LockMutex(g_lock); g_stat = s; SDL_UnlockMutex(g_lock);
            SDL_SetAtomicInt(&g_paused, 1); continue;
        }
        if (g_mode == GM_NONE || g_mode == GM_MACOS_NOMACMON) {
            snprintf(s.err, sizeof(s.err), g_mode == GM_MACOS_NOMACMON
                     ? "macmon not installed -- run: brew install macmon"
                     : "no GPU tool found (need nvidia-smi or macmon)");
            SDL_LockMutex(g_lock); g_stat = s; SDL_UnlockMutex(g_lock);
            SDL_SetAtomicInt(&g_paused, 1); continue;
        }

        // 2) Build + run the per-tool data query and parse it into `s`.
        if (g_mode == GM_NVIDIA) {
            char line[256];
            strcpy(s.src, "nvidia-smi"); s.pmax = 350.0f;
            if (is_local) {
#ifdef _WIN32
                // 32-bit process: PATH's System32 is redirected to SysWOW64 (no
                // nvidia-smi there) -- reach real System32 via the Sysnative alias.
                const char* root = getenv("SystemRoot"); if (!root) root = "C:\\Windows";
                char smi[320];
                snprintf(smi, sizeof(smi), "%s\\Sysnative\\nvidia-smi.exe", root);
                if (_access(smi, 0) != 0) snprintf(smi, sizeof(smi), "nvidia-smi");
                snprintf(cmd, sizeof(cmd), "\"%s\" %s 2>&1", smi, smiargs);
#else
                snprintf(cmd, sizeof(cmd), "nvidia-smi %s 2>&1", smiargs);
#endif
            } else {
                // Same nvidia-smi fallback chain as detect; wrap_PLAIN (no homebrew PATH
                // prefix, which a Windows shell can't parse -- that prefix is macmon-only).
                char inner[640];
                nvidia_chain(inner, sizeof(inner), smiargs);
                wrap_plain(cmd, sizeof(cmd), inner);
            }
            if (run_query(cmd, line, sizeof(line))) {
                if (sscanf(line, "%d, %d, %d, %d, %f, %63[^\r\n]",
                           &s.util,&s.mem_used,&s.mem_total,&s.temp,&s.power,s.name) >= 5)
                    s.valid = 1;
                else snprintf(s.err, sizeof(s.err), "%.159s", line);
            } else snprintf(s.err, sizeof(s.err), is_local
                     ? "nvidia-smi failed (local)" : "ssh/nvidia-smi failed (%s@%s)", user, host);
        }
        else if (g_mode == GM_MACMON) {
            char buf[2048]; const char* j;
            strcpy(s.src, "macmon"); s.pmax = 60.0f;
            strncpy(s.name, g_gpuname[0] ? g_gpuname : "Apple GPU", sizeof(s.name)-1);
            // macmon `pipe` emits one JSON sample (no sudo needed -- it uses IOReport).
            wrap_cmd(cmd, sizeof(cmd), "macmon pipe -s 1 -i 400");
            if (run_raw(cmd, buf, sizeof(buf)) > 0 && (j = strchr(buf, '{'))) {
                double v;
                // unified memory -> "VRAM"; gpu_usage is a 0..1 ratio.
                if (json_arr1(j, "\"gpu_usage\"",    &v)) s.util      = (int)(v*100.0 + 0.5);
                if (json_num (j, "\"ram_total\"",    &v)) s.mem_total = (int)(v/1048576.0);
                if (json_num (j, "\"ram_usage\"",    &v)) s.mem_used  = (int)(v/1048576.0);
                if (json_num (j, "\"gpu_temp_avg\"", &v)) s.temp      = (int)(v + 0.5);
                if (json_num (j, "\"gpu_power\"",    &v)) s.power     = (float)v;
                if (s.mem_total > 0) s.valid = 1;
                else snprintf(s.err, sizeof(s.err), "macmon: could not parse output");
            } else snprintf(s.err, sizeof(s.err), is_local
                     ? "macmon failed (local)" : "ssh/macmon failed (%s@%s)", user, host);
        }

        // Ask Ollama which model is loaded -- a direct HTTP GET (no SSH); /api/ps returns
        // {"models":[{"name":"llama3.1:8b",...}]} (empty when the model has been unloaded).
        if (s.valid) {
            char mcmd[400], mline[512];
            snprintf(mcmd, sizeof(mcmd),
                "curl -s --max-time 4 http://%s:11434/api/ps 2>&1", ohost[0] ? ohost : host);
            if (run_query(mcmd, mline, sizeof(mline))) {
                char* p = strstr(mline, "\"name\":\"");
                if (p) {
                    p += 8; int i = 0;
                    while (p[i] && p[i] != '"' && i < (int)sizeof(s.model)-1) { s.model[i] = p[i]; i++; }
                    s.model[i] = 0;
                }
            }
        }

        SDL_LockMutex(g_lock); g_stat = s; SDL_UnlockMutex(g_lock);

        // On error: stop here (no auto-reconnect) until the user hits Reconnect.
        if (!s.valid) { SDL_SetAtomicInt(&g_paused, 1); continue; }

        for (int i=0; i<POLL_SECS*10 && SDL_GetAtomicInt(&g_running)
                      && !SDL_GetAtomicInt(&g_paused); i++)
            SDL_Delay(100);
    }
    return 0;
}

// Clear the error and let the worker poll again (manual reconnect).
static void do_reconnect(void)
{
    SDL_LockMutex(g_lock);
    g_stat.err[0] = 0; g_stat.valid = 0;
    SDL_UnlockMutex(g_lock);
    g_mode = GM_DETECT;		// re-probe (host may have gained nvidia-smi/macmon)
    SDL_SetAtomicInt(&g_paused, 0);
}

static void draw(void)
{
    char buf[128];
    gpustat_t s;
    SDL_LockMutex(g_lock); s = g_stat; SDL_UnlockMutex(g_lock);

    fillrect(0,0,WINW,WINH, 22,22,28);
    snprintf(buf,sizeof(buf),"GPU @ %s", host);
    text(16, 12, buf, 120,200,255);
    fillrect(16, 32, WINW-32, 1, 60,60,72);

    if (s.err[0]) {
        text(16, 60, "disconnected:", 220,80,70);
        text(16, 80, s.err, 200,160,160);
        text(16, 110, "(host/user from aidoom.cfg)", 120,120,130);
        fillrect(btn_reconnect.x,btn_reconnect.y,btn_reconnect.w,btn_reconnect.h, 40,80,120);
        text(btn_reconnect.x+24, btn_reconnect.y+6, "Reconnect", 210,230,255);
        SDL_RenderPresent(ren);
        return;
    }
    if (!s.valid) {
        text(16, 60, "connecting...", 180,180,120);
        SDL_RenderPresent(ren);
        return;
    }

    text(16, 44, s.name, 180,180,190);

    if (s.model[0]) { snprintf(buf,sizeof(buf),"model: %s", s.model); text(16, 66, buf, 130,210,150); }
    else            text(16, 66, "model: (none loaded)", 110,110,125);

    snprintf(buf,sizeof(buf),"%d%%", s.util);
    bar(98,  "GPU load", s.util/100.0f, buf);

    // Apple Silicon has unified memory (no dedicated VRAM) -- label it "RAM".
    int is_apple = !strcmp(s.src, "macmon");
    snprintf(buf,sizeof(buf),"%d / %d MiB", s.mem_used, s.mem_total);
    bar(134, is_apple ? "RAM" : "VRAM", s.mem_total ? (float)s.mem_used/s.mem_total : 0, buf);

    snprintf(buf,sizeof(buf),"%d C", s.temp);
    bar(170, "Temp", s.temp/100.0f, buf);

    snprintf(buf,sizeof(buf),"%.0f W", s.power);
    bar(206, "Power", s.power / (s.pmax > 0 ? s.pmax : 350.0f), buf);

    snprintf(buf,sizeof(buf),"live (%s%s)",
             s.src[0] ? s.src : "?", host_is_local() ? "" : " over ssh");
    text(16, WINH-22, buf, 110,110,122);
    SDL_RenderPresent(ren);
}

int main(int argc, char** argv)
{
    SDL_SetMainReady();
    load_cfg();
    for (int i=1;i<argc-1;i++) {
        if (!strcmp(argv[i],"--host")) strncpy(host,argv[++i],sizeof(host)-1);
        else if (!strcmp(argv[i],"--user")) strncpy(user,argv[++i],sizeof(user)-1);
        else if (!strcmp(argv[i],"--port")) sshport = atoi(argv[++i]);
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1; }
    win = SDL_CreateWindow("aiDoom GPU monitor", WINW, WINH, 0);
    {
        // Window/taskbar icon from the shared aidoom.ico (same as the game).
        SDL_Surface* icon = SDL_CreateSurfaceFrom(
            AIDOOM_ICON_W, AIDOOM_ICON_H, SDL_PIXELFORMAT_RGBA32,
            (void *)aidoom_icon_rgba, AIDOOM_ICON_W*4);
        if (icon) { SDL_SetWindowIcon(win, icon); SDL_DestroySurface(icon); }
    }
    ren = SDL_CreateRenderer(win, NULL);
    font_init();

    g_lock = SDL_CreateMutex();
    SDL_SetAtomicInt(&g_running, 1);
    SDL_SetAtomicInt(&g_paused, 0);
    SDL_Thread* th = SDL_CreateThread(fetch_thread, "gpufetch", NULL);

    int run = 1;
    while (run) {
        SDL_Event e;
        // Block up to ~33 ms for an event so the window reacts to input (move/click/close)
        // and WM events INSTANTLY, then redraw.  The old PollEvent + SDL_Delay(100) slept
        // 100 ms per frame ignoring input, which felt frozen/unresponsive while the worker's
        // first SSH connect (a few seconds, "connecting...") was in flight.  On timeout we
        // fall through and redraw (~30 Hz) so the live bars stay smooth.
        if (SDL_WaitEventTimeout(&e, 33)) {
            do {
                if (e.type == SDL_EVENT_QUIT) run = 0;	// only the window-close button quits (Esc is ignored)
                else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                    int err; SDL_LockMutex(g_lock); err = (g_stat.err[0]!=0); SDL_UnlockMutex(g_lock);
                    float mx=e.button.x, my=e.button.y;
                    if (err && mx>=btn_reconnect.x && mx<btn_reconnect.x+btn_reconnect.w
                            && my>=btn_reconnect.y && my<btn_reconnect.y+btn_reconnect.h)
                        do_reconnect();
                }
            } while (SDL_PollEvent(&e));   // drain the rest of the batch, then one redraw
        }
        draw();
    }

    SDL_SetAtomicInt(&g_running, 0);
    // Do NOT block the close on the worker: if the remote GPU box vanished it may be
    // stuck in ssh/fgets for a few seconds, and SDL_WaitThread would hang the quit
    // (the bug -- the window wouldn't close).  Detach it and tear down now; process
    // exit kills the worker and closes its ssh pipe.  (Skip DestroyMutex to avoid
    // racing the detached worker; the OS reclaims it.)
    SDL_DetachThread(th);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
