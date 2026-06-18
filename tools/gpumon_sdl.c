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

#ifdef _WIN32
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
    while (SDL_GetAtomicInt(&g_running)) {
        snprintf(cmd, sizeof(cmd),
            "ssh -o BatchMode=yes -o ConnectTimeout=5 -p %d %s@%s "
            "\"nvidia-smi --query-gpu=utilization.gpu,memory.used,memory.total,"
            "temperature.gpu,power.draw,name --format=csv,noheader,nounits\" 2>&1",
            sshport, user, host);
        gpustat_t s; memset(&s, 0, sizeof(s));
        FILE* p = popen(cmd, "r");
        if (p && fgets(line, sizeof(line), p)) {
            if (sscanf(line, "%d, %d, %d, %d, %f, %63[^\r\n]",
                       &s.util,&s.mem_used,&s.mem_total,&s.temp,&s.power,s.name) >= 5)
                s.valid = 1;
            else { line[sizeof(s.err)-1]=0; snprintf(s.err,sizeof(s.err),"%s",line); }
        } else {
            snprintf(s.err, sizeof(s.err), "ssh/nvidia-smi failed (%s@%s)", user, host);
        }
        if (p) pclose(p);

        SDL_LockMutex(g_lock); g_stat = s; SDL_UnlockMutex(g_lock);

        for (int i=0; i<POLL_SECS*10 && SDL_GetAtomicInt(&g_running); i++)
            SDL_Delay(100);
    }
    return 0;
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
        text(16, 60, "no data:", 220,80,70);
        text(16, 80, s.err, 200,160,160);
        text(16, WINH-22, "retrying...  (configure host in aidoom.cfg)", 120,120,130);
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
    ren = SDL_CreateRenderer(win, NULL);
    font_init();

    g_lock = SDL_CreateMutex();
    SDL_SetAtomicInt(&g_running, 1);
    SDL_Thread* th = SDL_CreateThread(fetch_thread, "gpufetch", NULL);

    int run = 1;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) run = 0;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) run = 0;
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
