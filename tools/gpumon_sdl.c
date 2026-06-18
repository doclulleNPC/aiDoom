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
#include <io.h>		// _access (locate nvidia-smi past WoW64 redirection)
#define popen  _popen
#define pclose _pclose
#endif

#define WINW 460
#define WINH 300
#define POLL_SECS 2

static SDL_Window*   win;
static SDL_Renderer* ren;
static SDL_Texture*  font;

static char host[128] = "192.168.2.114";
static char user[64]  = "lubee";
static int  sshport   = 22;

// shared GPU state (worker thread -> main thread)
typedef struct {
    int   valid, util, mem_used, mem_total, temp;
    float power;
    char  name[64];
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

// ----------------------------------------------------------------- worker
static int fetch_thread(void* unused)
{
    (void)unused;
    char cmd[512], line[256];
    const char* smiargs =
        "--query-gpu=utilization.gpu,memory.used,memory.total,"
        "temperature.gpu,power.draw,name --format=csv,noheader,nounits";
    while (SDL_GetAtomicInt(&g_running)) {
        // Paused after an error -- wait for the user to hit Reconnect.
        if (SDL_GetAtomicInt(&g_paused)) { SDL_Delay(100); continue; }

        // localhost runs nvidia-smi directly (no SSH/key needed); otherwise SSH.
        int is_local = (!host[0] || !strcmp(host,"localhost") || !strcmp(host,"127.0.0.1"));
        if (is_local) {
#ifdef _WIN32
            // This is a 32-bit process, so the PATH's C:\Windows\System32 is
            // redirected to SysWOW64 -- where nvidia-smi.exe isn't.  Reach the
            // real System32 via the Sysnative alias; fall back to PATH otherwise.
            const char* root = getenv("SystemRoot"); if (!root) root = "C:\\Windows";
            char smi[320];
            snprintf(smi, sizeof(smi), "%s\\Sysnative\\nvidia-smi.exe", root);
            if (_access(smi, 0) != 0) snprintf(smi, sizeof(smi), "nvidia-smi");
            snprintf(cmd, sizeof(cmd), "\"%s\" %s 2>&1", smi, smiargs);
#else
            snprintf(cmd, sizeof(cmd), "nvidia-smi %s 2>&1", smiargs);
#endif
        }
        else
            snprintf(cmd, sizeof(cmd),
                "ssh -o BatchMode=yes -o ConnectTimeout=5 -p %d %s@%s \"nvidia-smi %s\" 2>&1",
                sshport, user, host, smiargs);

        gpustat_t s; memset(&s, 0, sizeof(s));
        FILE* p = popen(cmd, "r");
        if (p && fgets(line, sizeof(line), p)) {
            if (sscanf(line, "%d, %d, %d, %d, %f, %63[^\r\n]",
                       &s.util,&s.mem_used,&s.mem_total,&s.temp,&s.power,s.name) >= 5)
                s.valid = 1;
            else { line[sizeof(s.err)-1]=0; snprintf(s.err,sizeof(s.err),"%s",line); }
        } else if (is_local) {
            snprintf(s.err, sizeof(s.err), "nvidia-smi failed (local)");
        } else {
            snprintf(s.err, sizeof(s.err), "ssh/nvidia-smi failed (%s@%s)", user, host);
        }
        if (p) pclose(p);

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

    snprintf(buf,sizeof(buf),"%d%%", s.util);
    bar(76,  "GPU load", s.util/100.0f, buf);

    snprintf(buf,sizeof(buf),"%d / %d MiB", s.mem_used, s.mem_total);
    bar(112, "VRAM", s.mem_total ? (float)s.mem_used/s.mem_total : 0, buf);

    snprintf(buf,sizeof(buf),"%d C", s.temp);
    bar(148, "Temp", s.temp/100.0f, buf);

    snprintf(buf,sizeof(buf),"%.0f W", s.power);
    bar(184, "Power", s.power/350.0f, buf);

    text(16, WINH-22, "live (nvidia-smi over ssh) -- Esc to quit", 110,110,122);
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
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) run = 0;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) run = 0;
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                int err; SDL_LockMutex(g_lock); err = (g_stat.err[0]!=0); SDL_UnlockMutex(g_lock);
                float mx=e.button.x, my=e.button.y;
                if (err && mx>=btn_reconnect.x && mx<btn_reconnect.x+btn_reconnect.w
                        && my>=btn_reconnect.y && my<btn_reconnect.y+btn_reconnect.h)
                    do_reconnect();
            }
        }
        draw();
        SDL_Delay(100);
    }

    SDL_SetAtomicInt(&g_running, 0);
    SDL_WaitThread(th, NULL);
    SDL_DestroyMutex(g_lock);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
