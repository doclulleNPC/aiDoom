// aidoom_config -- tiny SDL3 settings editor for aiDoom.
//
// Edits the game config (~/.doomrc: action keys, mouse, video) and the AI
// Director config (~/.aidoom.cfg: Ollama host/port/model). No deps beyond SDL3;
// text is drawn from a baked DejaVuSansMono atlas (tools/font_atlas.h).
//
// Build: see tools/build_config.sh  (gcc + pkg-config sdl3)

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "font_atlas.h"

#define WINW 680
#define WINH 860
#define ROWH 26
#define HEADH 30
#define LABELX 28
#define VALX 300

// ---- Doom key codes (mirror doomdef.h) ----
enum { K_RIGHT=0xae,K_LEFT=0xac,K_UP=0xad,K_DOWN=0xaf,
       K_RCTRL=0x9d,K_RSHIFT=0xb6,K_RALT=0xb8,
       K_MWHEELUP=0xb0,K_MWHEELDOWN=0xb1,K_ESC=27,K_ENTER=13,K_TAB=9,K_BKSP=127 };

enum { T_KEY, T_INT, T_TOGGLE, T_TEXT };
enum { F_DOOMRC, F_AIDOOM };

typedef struct {
    const char* section;
    const char* label;
    const char* name;	// key in the config file
    int type, vmin, vmax, file;
    int ival;
    char sval[128];
    float y;		// filled at layout time
} setting_t;

static setting_t settings[] = {
    {"Action keys","Move forward",  "key_up",         T_KEY,0,0,F_DOOMRC},
    {"Action keys","Move back",     "key_down",       T_KEY,0,0,F_DOOMRC},
    {"Action keys","Turn left",     "key_left",       T_KEY,0,0,F_DOOMRC},
    {"Action keys","Turn right",    "key_right",      T_KEY,0,0,F_DOOMRC},
    {"Action keys","Strafe left",   "key_strafeleft", T_KEY,0,0,F_DOOMRC},
    {"Action keys","Strafe right",  "key_straferight",T_KEY,0,0,F_DOOMRC},
    {"Action keys","Fire",          "key_fire",       T_KEY,0,0,F_DOOMRC},
    {"Action keys","Use / open",    "key_use",        T_KEY,0,0,F_DOOMRC},
    {"Action keys","Strafe (hold)", "key_strafe",     T_KEY,0,0,F_DOOMRC},
    {"Action keys","Run / autorun", "key_speed",      T_KEY,0,0,F_DOOMRC},
    {"Action keys","Next weapon",   "key_nextweapon", T_KEY,0,0,F_DOOMRC},
    {"Action keys","Prev weapon",   "key_prevweapon", T_KEY,0,0,F_DOOMRC},

    {"Video / mouse","Mouse sensitivity","mouse_sensitivity",T_INT,0,9,F_DOOMRC},
    {"Video / mouse","Resolution (1-6)", "screen_resolution",T_INT,1,6,F_DOOMRC},
    {"Video / mouse","Screen size",      "screenblocks",     T_INT,3,11,F_DOOMRC},
    {"Video / mouse","SFX volume",       "sfx_volume",       T_INT,0,15,F_DOOMRC},
    {"Video / mouse","Music volume",     "music_volume",     T_INT,0,15,F_DOOMRC},
    {"Video / mouse","Fullscreen",       "fullscreen",       T_TOGGLE,0,1,F_DOOMRC},

    {"AI Director (Ollama)","Ollama host", "ollama_host",  T_TEXT,0,0,F_AIDOOM},
    {"AI Director (Ollama)","Ollama port", "ollama_port",  T_TEXT,0,0,F_AIDOOM},
    {"AI Director (Ollama)","Ollama model","ollama_model", T_TEXT,0,0,F_AIDOOM},
};
#define NSET ((int)(sizeof(settings)/sizeof(settings[0])))

static SDL_Window*   win;
static SDL_Renderer* ren;
static SDL_Texture*  font;

// ----------------------------------------------------------------- text
static void font_init(void)
{
    Uint32* px = malloc(FONT_AW*FONT_CH*4);
    for (int i=0;i<FONT_AW*FONT_CH;i++)
        px[i] = 0x00FFFFFFu | ((Uint32)font_alpha[i] << 24);	// white, alpha=coverage
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
    SDL_FRect q = {x,y,w,h}; SDL_SetRenderDrawColor(ren,r,g,b,255); SDL_RenderFillRect(ren,&q);
}

// ----------------------------------------------------------------- key names
static void keyname(int k, char* out, int n)
{
    switch (k) {
      case K_LEFT: snprintf(out,n,"Left"); return;
      case K_RIGHT:snprintf(out,n,"Right");return;
      case K_UP:   snprintf(out,n,"Up");   return;
      case K_DOWN: snprintf(out,n,"Down"); return;
      case K_RCTRL:snprintf(out,n,"Ctrl"); return;
      case K_RSHIFT:snprintf(out,n,"Shift");return;
      case K_RALT: snprintf(out,n,"Alt");  return;
      case K_MWHEELUP:  snprintf(out,n,"Wheel Up");  return;
      case K_MWHEELDOWN:snprintf(out,n,"Wheel Down");return;
      case ' ':    snprintf(out,n,"Space");return;
      case K_ENTER:snprintf(out,n,"Enter");return;
      case K_TAB:  snprintf(out,n,"Tab");  return;
      case K_BKSP: snprintf(out,n,"Bksp"); return;
    }
    if (k>32 && k<127) snprintf(out,n,"%c", toupper(k));
    else snprintf(out,n,"key %d", k);
}

static int sdl_to_doomkey(SDL_Keycode s)
{
    switch (s) {
      case SDLK_LEFT: return K_LEFT;   case SDLK_RIGHT: return K_RIGHT;
      case SDLK_UP:   return K_UP;     case SDLK_DOWN:  return K_DOWN;
      case SDLK_LCTRL:case SDLK_RCTRL: return K_RCTRL;
      case SDLK_LSHIFT:case SDLK_RSHIFT:return K_RSHIFT;
      case SDLK_LALT:case SDLK_RALT:case SDLK_LGUI:case SDLK_RGUI: return K_RALT;
      case SDLK_RETURN: return K_ENTER; case SDLK_TAB: return K_TAB;
      case SDLK_BACKSPACE: return K_BKSP; case SDLK_SPACE: return ' ';
    }
    if (s>32 && s<127) return (int)s;	// ASCII (already lower-case in SDL3)
    return 0;
}

// ----------------------------------------------------------------- load
static void set_default_text(setting_t* s)
{
    if (!strcmp(s->name,"ollama_host"))  strcpy(s->sval,"192.168.2.114");
    else if (!strcmp(s->name,"ollama_port"))  strcpy(s->sval,"11434");
    else if (!strcmp(s->name,"ollama_model")) strcpy(s->sval,"mistral:7b-instruct");
}

// Built-in defaults matching the engine's m_misc.c defaults[] -- used when no
// aidoom.cfg exists yet, so a first save doesn't zero the bindings.
static void set_default_int(setting_t* s)
{
    const char* n = s->name; int v = 0;
    if      (!strcmp(n,"key_right"))        v = K_RIGHT;
    else if (!strcmp(n,"key_left"))         v = K_LEFT;
    else if (!strcmp(n,"key_up"))           v = K_UP;
    else if (!strcmp(n,"key_down"))         v = K_DOWN;
    else if (!strcmp(n,"key_strafeleft"))   v = ',';
    else if (!strcmp(n,"key_straferight"))  v = '.';
    else if (!strcmp(n,"key_fire"))         v = K_RCTRL;
    else if (!strcmp(n,"key_use"))          v = ' ';
    else if (!strcmp(n,"key_strafe"))       v = K_RALT;
    else if (!strcmp(n,"key_speed"))        v = K_RSHIFT;
    else if (!strcmp(n,"key_nextweapon"))   v = K_MWHEELUP;
    else if (!strcmp(n,"key_prevweapon"))   v = K_MWHEELDOWN;
    else if (!strcmp(n,"mouse_sensitivity"))v = 5;
    else if (!strcmp(n,"screen_resolution"))v = 1;
    else if (!strcmp(n,"screenblocks"))     v = 9;
    else if (!strcmp(n,"sfx_volume"))       v = 8;
    else if (!strcmp(n,"music_volume"))     v = 8;
    else if (!strcmp(n,"fullscreen"))       v = 0;
    s->ival = v;
}

// Single config file "aidoom.cfg" next to this binary (the run/ folder).
static void cfg_path(char* out, int n)
{
    const char* base = SDL_GetBasePath();	// exe dir (trailing sep), cached by SDL
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
        char* v = val; size_t L = strlen(v);		// strip quotes the engine may add
        if (L>=2 && v[0]=='"' && v[L-1]=='"') { v[L-1]=0; v++; }
        for (int i=0;i<NSET;i++) {
            if (strcmp(settings[i].name,name)) continue;
            if (settings[i].type==T_TEXT) { strncpy(settings[i].sval,v,sizeof(settings[i].sval)-1); settings[i].sval[sizeof(settings[i].sval)-1]=0; }
            else settings[i].ival = atoi(v);
        }
    }
    fclose(f);
}

// Rewrite aidoom.cfg preserving lines we don't manage (engine-only keys);
// update/append the ones we do.
static void save_cfg(void)
{
    char path[1024]; cfg_path(path,sizeof(path));
    char (*lines)[512] = malloc(2048*512); int nl=0;
    FILE* f = fopen(path,"r");
    if (f) { while (nl<2048 && fgets(lines[nl],512,f)) nl++; fclose(f); }
    int handled[NSET]; for (int i=0;i<NSET;i++) handled[i]=0;

    f = fopen(path,"w"); if (!f) { free(lines); return; }
    for (int l=0;l<nl;l++) {
        char name[128];
        if (sscanf(lines[l]," %127s",name)!=1) { fputs(lines[l],f); continue; }
        int m=-1;
        for (int i=0;i<NSET;i++) if (!strcmp(settings[i].name,name)) { m=i; break; }
        if (m<0) { fputs(lines[l],f); continue; }	// keep engine-only line verbatim
        handled[m]=1;
        if (settings[m].type==T_TEXT) fprintf(f,"%s\t\t%s\n",settings[m].name,settings[m].sval);
        else fprintf(f,"%s\t\t%d\n",settings[m].name,settings[m].ival);
    }
    for (int i=0;i<NSET;i++) {
        if (handled[i]) continue;
        if (settings[i].type==T_TEXT) fprintf(f,"%s\t\t%s\n",settings[i].name,settings[i].sval);
        else fprintf(f,"%s\t\t%d\n",settings[i].name,settings[i].ival);
    }
    fclose(f); free(lines);
}

// ----------------------------------------------------------------- UI state
static int mode=0;            // 0 normal, 1 capture key, 2 edit text
static int active=-1;         // setting being captured/edited
static char editbuf[128];
static char status[160]="";

static SDL_FRect btn_save, btn_quit;

static float layout(void)	// returns y of bottom; fills setting.y
{
    float y = 16;
    const char* sec = NULL;
    for (int i=0;i<NSET;i++) {
        if (sec != settings[i].section) {
            sec = settings[i].section;
            y += (i? 10:0);
            settings[i].y = -1;	// header marker drawn separately
            y += HEADH;
        }
        settings[i].y = y;
        y += ROWH;
    }
    return y;
}

static void draw(void)
{
    rect(0,0,WINW,WINH, 24,24,28);
    const char* sec=NULL;
    for (int i=0;i<NSET;i++) {
        setting_t* s=&settings[i];
        if (sec!=s->section) {
            sec=s->section;
            text(LABELX, s->y-HEADH+8, sec, 120,200,255);
            rect(LABELX, s->y-6, WINW-2*LABELX, 1, 60,60,70);
        }
        int hot = (mode==1&&active==i)||(mode==2&&active==i);
        text(LABELX, s->y+4, s->label, 220,220,220);
        char buf[160];
        if (s->type==T_KEY)      keyname(s->ival,buf,sizeof(buf));
        else if (s->type==T_TOGGLE) snprintf(buf,sizeof(buf), s->ival?"On":"Off");
        else if (s->type==T_INT) snprintf(buf,sizeof(buf),"< %d >", s->ival);
        else                     snprintf(buf,sizeof(buf),"%s", (mode==2&&active==i)?editbuf:s->sval);
        if (hot) rect(VALX-4, s->y+1, WINW-VALX-LABELX+4, ROWH-4, 50,50,70);
        if (mode==2&&active==i) { // text cursor
            text(VALX, s->y+4, buf, 255,255,160);
            text(VALX + (float)strlen(buf)*FONT_CW, s->y+4, "_", 255,255,160);
        } else text(VALX, s->y+4, buf, 255,235,150);
    }
    // buttons
    rect(btn_save.x,btn_save.y,btn_save.w,btn_save.h, 40,110,40); text(btn_save.x+18,btn_save.y+8,"Save",230,255,230);
    rect(btn_quit.x,btn_quit.y,btn_quit.w,btn_quit.h, 110,40,40); text(btn_quit.x+18,btn_quit.y+8,"Quit",255,230,230);
    if (status[0]) text(LABELX, WINH-26, status, 160,255,160);
    SDL_RenderPresent(ren);
}

static int hit(float mx,float my, SDL_FRect r){ return mx>=r.x&&mx<r.x+r.w&&my>=r.y&&my<r.y+r.h; }

static void click(float mx,float my)
{
    if (hit(mx,my,btn_save)) { save_cfg(); snprintf(status,sizeof(status),"Saved aidoom.cfg (next to the binary)"); return; }
    if (hit(mx,my,btn_quit)) { SDL_Event q={.type=SDL_EVENT_QUIT}; SDL_PushEvent(&q); return; }
    for (int i=0;i<NSET;i++) {
        setting_t* s=&settings[i];
        SDL_FRect vr={VALX-4,s->y,WINW-VALX,ROWH};
        if (!hit(mx,my,vr)) continue;
        if (s->type==T_KEY) { mode=1; active=i; snprintf(status,sizeof(status),"Press a key (or mouse wheel) for \"%s\"  -  Esc cancels", s->label); }
        else if (s->type==T_TOGGLE) s->ival = !s->ival;
        else if (s->type==T_INT) {
            float mid = VALX-4 + (WINW-VALX)/2.0f;
            s->ival += (mx<mid)? -1 : 1;
            if (s->ival<s->vmin) s->ival=s->vmin; if (s->ival>s->vmax) s->ival=s->vmax;
        }
        else if (s->type==T_TEXT) { mode=2; active=i; strncpy(editbuf,s->sval,sizeof(editbuf)-1); editbuf[sizeof(editbuf)-1]=0; SDL_StartTextInput(win); snprintf(status,sizeof(status),"Type, Enter to confirm"); }
        return;
    }
}

int main(int argc, char** argv)
{
    (void)argc;(void)argv;
    for (int i=0;i<NSET;i++)
        if (settings[i].type==T_TEXT) set_default_text(&settings[i]);
        else set_default_int(&settings[i]);
    load_cfg();

    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1; }
    win = SDL_CreateWindow("aiDoom Config", WINW, WINH, 0);
    ren = SDL_CreateRenderer(win, NULL);
    font_init();

    float bottom = layout();
    float by = bottom + 18; if (by > WINH-40) by = WINH-40;
    btn_save = (SDL_FRect){ LABELX, by, 90, 30 };
    btn_quit = (SDL_FRect){ WINW-LABELX-90, by, 90, 30 };

    draw();		// show the UI before the first event arrives
    int run=1;
    while (run) {
        SDL_Event e;
        while (SDL_WaitEvent(&e)) {
            if (e.type==SDL_EVENT_QUIT) { run=0; break; }
            if (mode==1) { // capturing a key bind
                if (e.type==SDL_EVENT_KEY_DOWN) {
                    if (e.key.key==SDLK_ESCAPE) { mode=0; status[0]=0; }
                    else { int k=sdl_to_doomkey(e.key.key); if (k){ settings[active].ival=k; } mode=0; status[0]=0; }
                    break;
                }
                if (e.type==SDL_EVENT_MOUSE_WHEEL) {
                    settings[active].ival = (e.wheel.y>0)?K_MWHEELUP:K_MWHEELDOWN; mode=0; status[0]=0; break;
                }
                continue;
            }
            if (mode==2) { // editing text
                if (e.type==SDL_EVENT_TEXT_INPUT) { if (strlen(editbuf)+strlen(e.text.text)<sizeof(editbuf)-1) strcat(editbuf,e.text.text); break; }
                if (e.type==SDL_EVENT_KEY_DOWN) {
                    if (e.key.key==SDLK_BACKSPACE) { int n=strlen(editbuf); if (n) editbuf[n-1]=0; }
                    else if (e.key.key==SDLK_RETURN) { strncpy(settings[active].sval,editbuf,sizeof(settings[active].sval)-1); SDL_StopTextInput(win); mode=0; status[0]=0; }
                    else if (e.key.key==SDLK_ESCAPE) { SDL_StopTextInput(win); mode=0; status[0]=0; }
                    break;
                }
                continue;
            }
            if (e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button==SDL_BUTTON_LEFT) { click(e.button.x,e.button.y); break; }
            if (e.type==SDL_EVENT_KEY_DOWN && e.key.key==SDLK_ESCAPE) { run=0; break; }
        }
        draw();
    }
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
