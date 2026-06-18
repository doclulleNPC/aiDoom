// aicoop_config -- tiny SDL3 editor for aiDoom's AI behaviour.
//
// Edits the same aidoom.cfg (in the run/ folder) the game reads: the AI co-op
// companion knobs (coop_*) and the monster pack-hunt AI (monster_*).  HP values
// are absolute (max 100, so 50 = 50%); ranges are in map units.  Other config
// keys are preserved untouched.
//
// Build: CMake / build_all_win.bat (Windows) or tools/build_config.sh-style gcc.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>	// SDL_SetMainReady (not pulled in by SDL.h in SDL3)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font_atlas.h"
#include "../files/aidoom_icon.h"	// shared 64x64 RGBA window icon (from aidoom.ico)

#define WINW 560
#define WINH 410
#define ROWH 30
#define HEADH 30
#define LABELX 26
#define VALX 330

typedef struct {
    const char* section;
    const char* label;
    const char* name;	// key in aidoom.cfg
    int		val, vmin, vmax, step;
    const char* unit;
    float	y;
} setting_t;

static setting_t settings[] = {
    {"AI co-op companion (-aicoop)","Defend below HP",  "coop_defend_hp",   35,  0,  100,   5, "HP"},
    {"AI co-op companion (-aicoop)","Flee/hide below",  "coop_heal_hp",     20,  0,  100,   5, "HP"},
    {"AI co-op companion (-aicoop)","Sight range",      "coop_sight",     1280,256, 4096, 128, "mu"},
    {"AI co-op companion (-aicoop)","Follow distance",  "coop_follow",     256, 64, 1024,  32, "mu"},
    {"AI co-op companion (-aicoop)","Heal search range","coop_heal_range",1024,256, 4096, 128, "mu"},
    {"Monsters (pack hunt)","Pack hunt  0=off 1=on","monster_pack",          1,  0,    1,   1, ""},
    {"Monsters (pack hunt)","Search / group range", "monster_pack_range", 2048,256, 8192, 256, "mu"},
};
#define NSET ((int)(sizeof(settings)/sizeof(settings[0])))
static const int DEFVAL[NSET] = { 50, 30, 1280, 256, 1024, 1, 2048 };

static SDL_Window*   win;
static SDL_Renderer* ren;
static SDL_Texture*  font;
static char status[160] = "";

// ----------------------------------------------------------------- text/draw
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

static void rect(float x,float y,float w,float h, Uint8 r,Uint8 g,Uint8 b)
{
    SDL_FRect q={x,y,w,h}; SDL_SetRenderDrawColor(ren,r,g,b,255); SDL_RenderFillRect(ren,&q);
}

// ----------------------------------------------------------------- config I/O
static void cfg_path(char* out, int n)
{
    const char* base = SDL_GetBasePath();	// the exe's dir (run/)
    snprintf(out, n, "%saidoom.cfg", base ? base : "./");
}

static void load_cfg(void)
{
    char path[1024]; cfg_path(path,sizeof(path));
    FILE* f = fopen(path,"r"); if (!f) return;
    char line[512];
    while (fgets(line,sizeof(line),f)) {
        char name[128], val[256];
        if (sscanf(line," %127s %255[^\n]",name,val)!=2) continue;
        for (int i=0;i<NSET;i++)
            if (!strcmp(settings[i].name,name)) settings[i].val = atoi(val);
    }
    fclose(f);
}

// Rewrite aidoom.cfg, updating the keys we manage and preserving the rest.
static void save_cfg(void)
{
    char path[1024]; cfg_path(path,sizeof(path));
    char (*lines)[512] = malloc(2048*512); int nl=0;
    FILE* f = fopen(path,"r");
    if (f) { while (nl<2048 && fgets(lines[nl],512,f)) nl++; fclose(f); }
    int handled[NSET]; for (int i=0;i<NSET;i++) handled[i]=0;

    f = fopen(path,"w"); if (!f) { free(lines); snprintf(status,sizeof(status),"cannot write %s",path); return; }
    for (int l=0;l<nl;l++) {
        char name[128];
        if (sscanf(lines[l]," %127s",name)!=1) { fputs(lines[l],f); continue; }
        int m=-1;
        for (int i=0;i<NSET;i++) if (!strcmp(settings[i].name,name)) { m=i; break; }
        if (m<0) { fputs(lines[l],f); continue; }
        handled[m]=1;
        fprintf(f,"%s\t\t%d\n",settings[m].name,settings[m].val);
    }
    for (int i=0;i<NSET;i++)
        if (!handled[i]) fprintf(f,"%s\t\t%d\n",settings[i].name,settings[i].val);
    fclose(f); free(lines);
    snprintf(status,sizeof(status),"Saved aidoom.cfg (next to the binary)");
}

// ----------------------------------------------------------------- UI
static SDL_FRect btn_save, btn_quit, btn_reset;

static float layout(void)
{
    float y = 16;
    const char* sec = NULL;
    for (int i=0;i<NSET;i++) {
        if (sec != settings[i].section) { sec = settings[i].section; y += (i?10:0) + HEADH; }
        settings[i].y = y; y += ROWH;
    }
    return y;
}

static int hit(float mx,float my, SDL_FRect r){ return mx>=r.x&&mx<r.x+r.w&&my>=r.y&&my<r.y+r.h; }

static void draw(void)
{
    rect(0,0,WINW,WINH, 24,24,28);
    const char* sec = NULL;
    for (int i=0;i<NSET;i++) {
        setting_t* s=&settings[i];
        char buf[96];
        if (sec != s->section) {
            sec = s->section;
            text(LABELX, s->y - HEADH + 8, sec, 120,200,255);
            rect(LABELX, s->y - 6, WINW-2*LABELX, 1, 60,60,70);
        }
        text(LABELX, s->y+6, s->label, 220,220,220);
        snprintf(buf,sizeof(buf),"< %d %s >", s->val, s->unit);
        text(VALX, s->y+6, buf, 255,235,150);
    }

    rect(btn_save.x,btn_save.y,btn_save.w,btn_save.h, 40,110,40);  text(btn_save.x+16,btn_save.y+8,"Save",230,255,230);
    rect(btn_reset.x,btn_reset.y,btn_reset.w,btn_reset.h, 70,70,110); text(btn_reset.x+12,btn_reset.y+8,"Defaults",225,225,255);
    rect(btn_quit.x,btn_quit.y,btn_quit.w,btn_quit.h, 110,40,40);  text(btn_quit.x+16,btn_quit.y+8,"Quit",255,230,230);
    if (status[0]) text(LABELX, WINH-22, status, 160,255,160);
    SDL_RenderPresent(ren);
}

static void click(float mx,float my)
{
    if (hit(mx,my,btn_save)) { save_cfg(); return; }
    if (hit(mx,my,btn_quit)) { SDL_Event q={.type=SDL_EVENT_QUIT}; SDL_PushEvent(&q); return; }
    if (hit(mx,my,btn_reset)) {
        for (int i=0;i<NSET;i++) settings[i].val = DEFVAL[i];
        snprintf(status,sizeof(status),"reset to defaults (not yet saved)");
        return;
    }
    for (int i=0;i<NSET;i++) {
        setting_t* s=&settings[i];
        char t[96]; snprintf(t,sizeof(t),"< %d %s >", s->val, s->unit);
        float w = (float)strlen(t)*FONT_CW;
        SDL_FRect vr = { VALX, s->y, w, ROWH };
        if (!hit(mx,my,vr)) continue;
        s->val += (mx < VALX + w/2.0f) ? -s->step : s->step;
        if (s->val < s->vmin) s->val = s->vmin;
        if (s->val > s->vmax) s->val = s->vmax;
        return;
    }
}

int main(int argc, char** argv)
{
    (void)argc;(void)argv;
    SDL_SetMainReady();
    load_cfg();

    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1; }
    win = SDL_CreateWindow("aiDoom AI behaviour", WINW, WINH, 0);
    {
        SDL_Surface* icon = SDL_CreateSurfaceFrom(
            AIDOOM_ICON_W, AIDOOM_ICON_H, SDL_PIXELFORMAT_RGBA32,
            (void *)aidoom_icon_rgba, AIDOOM_ICON_W*4);
        if (icon) { SDL_SetWindowIcon(win, icon); SDL_DestroySurface(icon); }
    }
    ren = SDL_CreateRenderer(win, NULL);
    font_init();

    layout();
    float by = WINH - 44;
    btn_save  = (SDL_FRect){ LABELX, by, 90, 30 };
    btn_reset = (SDL_FRect){ WINW/2.0f-55, by, 110, 30 };
    btn_quit  = (SDL_FRect){ WINW-LABELX-90, by, 90, 30 };

    draw();
    int run=1;
    while (run) {
        SDL_Event e;
        while (SDL_WaitEvent(&e)) {
            if (e.type==SDL_EVENT_QUIT) { run=0; break; }
            if (e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button==SDL_BUTTON_LEFT) { click(e.button.x,e.button.y); break; }
            if (e.type==SDL_EVENT_KEY_DOWN && e.key.key==SDLK_ESCAPE) { run=0; break; }
        }
        draw();
    }
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
