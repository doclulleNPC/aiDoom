// aidoom_config -- tiny SDL3 settings editor for aiDoom.
//
// Edits the single game/tools config file aidoom.cfg (in the run/ folder): action
// keys, mouse, video, IWAD, the Ollama AI-Director host/port/model and the GPU
// monitor's SSH settings. No deps beyond SDL3; text is drawn from a baked
// DejaVuSansMono atlas (tools/font_atlas.h).
//
// Build: see tools/build_config.sh  (gcc + pkg-config sdl3)

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>	// for SDL_SetMainReady (not pulled in by SDL.h in SDL3)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef _MSC_VER
#include <io.h>			// MSVC: access() lives here
#ifndef F_OK
#define F_OK 0
#endif
#define access _access
#else
#include <unistd.h>
#endif

#include "font_atlas.h"
#include "../files/aidoom_icon.h"	// shared 64x64 RGBA window icon (from aidoom.ico)

// Two-column layout: each column is COLW wide (label at LABELX, value at VALX,
// both column-relative).  Wider + shorter so it fits a normal-height screen.
#define COLW 580
#define WINW (2*COLW)
#define WINH 700
#define ROWH 26
#define HEADH 30
#define LABELX 28
#define VALX 250

// ---- Doom key codes (mirror doomdef.h) ----
enum { K_RIGHT=0xae,K_LEFT=0xac,K_UP=0xad,K_DOWN=0xaf,
       K_RCTRL=0x9d,K_RSHIFT=0xb6,K_RALT=0xb8,
       K_MWHEELUP=0xb0,K_MWHEELDOWN=0xb1,K_ESC=27,K_ENTER=13,K_TAB=9,K_BKSP=127,
       K_MOUSE1=0xb2,K_MOUSE2=0xb3,K_MOUSE3=0xb4,
       K_F11=0xd7,K_F12=0xd8 };

enum { T_KEY, T_INT, T_TOGGLE, T_TEXT, T_CHOICE };
enum { F_DOOMRC, F_AIDOOM };

typedef struct {
    const char* section;
    const char* label;
    const char* name;	// key in the config file
    int type, vmin, vmax, file;
    int ival;
    char sval[128];
    int col;		// filled at layout time: which column (0=left, 1=right)
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
    {"Action keys","Jump",          "key_jump",       T_KEY,0,0,F_DOOMRC},
    {"Action keys","Console",       "key_console",    T_KEY,0,0,F_DOOMRC},

    {"Buddy keys","Buddy: come",    "key_buddy_come",   T_KEY,0,0,F_DOOMRC},
    {"Buddy keys","Buddy: attack",  "key_buddy_attack", T_KEY,0,0,F_DOOMRC},
    {"Buddy keys","Buddy: stay",    "key_buddy_stay",   T_KEY,0,0,F_DOOMRC},
    {"Buddy keys","Buddy: stay/follow","key_buddy_mode",T_KEY,0,0,F_DOOMRC},
    {"Buddy keys","Buddy: view (spy)","key_spy",        T_KEY,0,0,F_DOOMRC},
    {"Inventory keys","Inventory: previous item","key_inv_left", T_KEY,0,0,F_DOOMRC},
    {"Inventory keys","Inventory: next item","key_inv_right",T_KEY,0,0,F_DOOMRC},
    {"Inventory keys","Inventory: use item","key_inv_use",  T_KEY,0,0,F_DOOMRC},

    {"Video / mouse","Mouse sensitivity","mouse_sensitivity",T_INT,0,9,F_DOOMRC},
    {"Video / mouse","Resolution (1-6)", "screen_resolution",T_INT,1,6,F_DOOMRC},
    {"Video / mouse","Screen size",      "screenblocks",     T_INT,3,11,F_DOOMRC},
    {"Video / mouse","SFX volume",       "sfx_volume",       T_INT,0,15,F_DOOMRC},
    {"Video / mouse","Music volume",     "music_volume",     T_INT,0,15,F_DOOMRC},
    {"Video / mouse","Fullscreen",       "fullscreen",       T_TOGGLE,0,1,F_DOOMRC},

    {"Game","IWAD",                      "iwad",             T_CHOICE,0,0,F_DOOMRC},

    {"AI Director (Ollama)","Ollama host", "ollama_host",  T_TEXT,0,0,F_AIDOOM},
    {"AI Director (Ollama)","Ollama port", "ollama_port",  T_TEXT,0,0,F_AIDOOM},
    {"AI Director (Ollama)","Ollama model","ollama_model", T_TEXT,0,0,F_AIDOOM},

    {"GPU monitor (SSH)","SSH host", "gpu_host",     T_TEXT,0,0,F_AIDOOM},
    {"GPU monitor (SSH)","SSH user", "gpu_user",     T_TEXT,0,0,F_AIDOOM},
    {"GPU monitor (SSH)","SSH port", "gpu_ssh_port", T_TEXT,0,0,F_AIDOOM},
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
    if (k <= 0) { if (n) out[0] = 0; return; }	// unbound (cleared) -> empty field, not "key 0"
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
      case K_MOUSE1:    snprintf(out,n,"Mouse L");   return;
      case K_MOUSE2:    snprintf(out,n,"Mouse R");   return;
      case K_MOUSE3:    snprintf(out,n,"Mouse M");   return;
      case ' ':    snprintf(out,n,"Space");return;
      case K_ENTER:snprintf(out,n,"Enter");return;
      case K_TAB:  snprintf(out,n,"Tab");  return;
      case K_BKSP: snprintf(out,n,"Bksp"); return;
      case K_F11:  snprintf(out,n,"F11");  return;
      case K_F12:  snprintf(out,n,"F12");  return;
    }
    if (k>=0xbb && k<=0xc4) { snprintf(out,n,"F%d", k-0xbb+1); return; }	// F1..F10
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
      case SDLK_F11: return K_F11; case SDLK_F12: return K_F12;
    }
    if (s >= SDLK_F1 && s <= SDLK_F10) return 0xbb + (int)(s - SDLK_F1);	// F1..F10
    if (s>32 && s<127) return (int)s;	// ASCII (already lower-case in SDL3)
    return 0;
}

// ----------------------------------------------------------------- load
static void set_default_text(setting_t* s)
{
    if (!strcmp(s->name,"ollama_host"))  strcpy(s->sval,"192.168.2.114");
    else if (!strcmp(s->name,"ollama_port"))  strcpy(s->sval,"11434");
    else if (!strcmp(s->name,"ollama_model")) strcpy(s->sval,"mistral:7b-instruct");
    else if (!strcmp(s->name,"gpu_host"))     strcpy(s->sval,"192.168.2.114");
    else if (!strcmp(s->name,"gpu_user"))     strcpy(s->sval,"lubee");
    else if (!strcmp(s->name,"gpu_ssh_port")) strcpy(s->sval,"22");
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
    else if (!strcmp(n,"key_jump"))         v = ' ';
    else if (!strcmp(n,"key_console"))      v = K_F12;
    else if (!strcmp(n,"key_buddy_come"))   v = ',';
    else if (!strcmp(n,"key_buddy_attack")) v = '.';
    else if (!strcmp(n,"key_buddy_stay"))   v = '-';
    else if (!strcmp(n,"key_spy"))          v = K_F12;
    else if (!strcmp(n,"key_inv_left"))     v = '[';
    else if (!strcmp(n,"key_inv_right"))    v = ']';
    else if (!strcmp(n,"key_inv_use"))      v = K_ENTER;
    else if (!strcmp(n,"mouse_sensitivity"))v = 5;
    else if (!strcmp(n,"screen_resolution"))v = 1;
    else if (!strcmp(n,"screenblocks"))     v = 9;
    else if (!strcmp(n,"sfx_volume"))       v = 8;
    else if (!strcmp(n,"music_volume"))     v = 8;
    else if (!strcmp(n,"fullscreen"))       v = 0;
    s->ival = v;
}

// ----------------------------------------------------------------- IWAD scan
// Same search order as the engine: iwads/ -> . -> Steam. [0] is "" = auto-detect.
static char iwadlist[64][512];
static int  niwad = 0;
static int  iwadsel = 0;

static const char* base_name(const char* p)
{
    const char* b = p, *s;
    if ((s = strrchr(b,'/')))  b = s+1;
    if ((s = strrchr(b,'\\'))) b = s+1;
    return b;
}

static void iwad_add(const char* p)
{
    for (int i=0;i<niwad;i++) if (!strcmp(iwadlist[i],p)) return;	// dedup
    if (niwad < 64) { strncpy(iwadlist[niwad],p,511); iwadlist[niwad][511]=0; niwad++; }
}

static void scan_iwads(void)
{
    static const char* names[] = {"doom2.wad","plutonia.wad","tnt.wad","doomu.wad",
        "doom.wad","doom1.wad","freedoom2.wad","freedoom1.wad","freedm.wad","doom2f.wad"};
    static const char* rel[] = {
        "Ultimate Doom/base/DOOM.WAD","Ultimate Doom/rerelease/DOOM.WAD",
        "DOOM 2/base/DOOM2.WAD","Doom 2/base/DOOM2.WAD",
        "Final Doom/base/TNT.WAD","Final Doom/base/PLUTONIA.WAD",
        "Doom 2/finaldoombase/TNT.WAD","Doom 2/finaldoombase/PLUTONIA.WAD"};
    const char* dirs[2] = {"iwads","."};
    char path[512];

    niwad = 0; iwad_add("");				// [0] = auto

    for (int d=0; d<2; d++)
        for (int i=0; i<(int)(sizeof(names)/sizeof(names[0])); i++) {
            snprintf(path,sizeof(path),"%s/%s",dirs[d],names[i]);
            if (!access(path,F_OK)) iwad_add(path);
        }

    const char* home = getenv("HOME");
    char roots[3][512]; int nr=0;
    if (home) {
        snprintf(roots[nr++],512,"%s/.steam/steam/steamapps/common",home);
        snprintf(roots[nr++],512,"%s/.local/share/Steam/steamapps/common",home);
        snprintf(roots[nr++],512,"%s/.steam/root/steamapps/common",home);
    }
    for (int r=0; r<nr; r++)
        for (int i=0; i<(int)(sizeof(rel)/sizeof(rel[0])); i++) {
            snprintf(path,sizeof(path),"%s/%s",roots[r],rel[i]);
            if (!access(path,F_OK)) iwad_add(path);
        }
}

// Point iwadsel at the entry matching the loaded "iwad" value (add it if unseen).
static void iwad_sync_selection(const char* val)
{
    iwadsel = 0;
    if (!val || !val[0]) return;
    for (int i=0;i<niwad;i++) if (!strcmp(iwadlist[i],val)) { iwadsel=i; return; }
    iwad_add(val); iwadsel = niwad-1;
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
            if (settings[i].type==T_TEXT || settings[i].type==T_CHOICE) { strncpy(settings[i].sval,v,sizeof(settings[i].sval)-1); settings[i].sval[sizeof(settings[i].sval)-1]=0; }
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
        if (settings[m].type==T_TEXT || settings[m].type==T_CHOICE) fprintf(f,"%s\t\t%s\n",settings[m].name,settings[m].sval);
        else fprintf(f,"%s\t\t%d\n",settings[m].name,settings[m].ival);
    }
    for (int i=0;i<NSET;i++) {
        if (handled[i]) continue;
        if (settings[i].type==T_TEXT || settings[i].type==T_CHOICE) fprintf(f,"%s\t\t%s\n",settings[i].name,settings[i].sval);
        else fprintf(f,"%s\t\t%d\n",settings[i].name,settings[i].ival);
    }
    fclose(f); free(lines);
}

// ----------------------------------------------------------------- UI state
static int mode=0;            // 0 normal, 1 capture key, 2 edit text
static int active=-1;         // setting being captured/edited
static int hover=-1;          // setting whose value the mouse is over (for Backspace-clear)
static char editbuf[128];
static char status[160]="";

static SDL_FRect btn_save, btn_quit, btn_sshkey;

static const char* find_sval(const char* name)
{
    for (int i=0;i<NSET;i++) if (!strcmp(settings[i].name,name)) return settings[i].sval;
    return "";
}

// Copy the local SSH public key to <user>@<host> so the GPU monitor's
// "nvidia-smi over ssh" works without a password.  Windows has no ssh-copy-id,
// so we generate a key if needed and append the .pub via a piped ssh command,
// run in a visible console window for the (one-time) password prompt.
static void copy_ssh_key(const char* host, const char* user, const char* port)
{
    if (!host || !host[0] || !user || !user[0]) {
        snprintf(status,sizeof(status),"Set the SSH host and user first");
        return;
    }
    if (!port || !port[0]) port = "22";
#ifdef _WIN32
    const char* tmp = getenv("TEMP"); if (!tmp) tmp = ".";
    char bat[700]; snprintf(bat,sizeof(bat),"%s\\aidoom_sshkey.bat", tmp);
    FILE* f = fopen(bat,"w");
    if (!f) { snprintf(status,sizeof(status),"could not write %s", bat); return; }
    fprintf(f,"@echo off\r\n");
    fprintf(f,"echo Copying SSH public key to %s@%s (port %s) ...\r\n", user, host, port);
    fprintf(f,"if not exist \"%%USERPROFILE%%\\.ssh\\id_ed25519.pub\" "
              "ssh-keygen -t ed25519 -N \"\" -f \"%%USERPROFILE%%\\.ssh\\id_ed25519\"\r\n");
    fprintf(f,"type \"%%USERPROFILE%%\\.ssh\\id_ed25519.pub\" | "
              "ssh -p %s %s@%s \"mkdir -p .ssh && cat >> .ssh/authorized_keys\"\r\n", port, user, host);
    fprintf(f,"if errorlevel 1 (echo. & echo FAILED.) else (echo. & echo OK - key installed.)\r\n");
    fprintf(f,"echo. & pause\r\n");
    fclose(f);
    char cmd[800]; snprintf(cmd,sizeof(cmd),"start \"aiDoom SSH key copy\" cmd /c \"%s\"", bat);
    system(cmd);
    snprintf(status,sizeof(status),"Launched key copy to %s@%s -- enter the password in the console", user, host);
#else
    char cmd[512]; snprintf(cmd,sizeof(cmd),"ssh-copy-id -p %s %s@%s", port, user, host);
    int rc = system(cmd);
    if (rc==0) snprintf(status,sizeof(status),"SSH key copied to %s@%s", user, host);
    else       snprintf(status,sizeof(status),"ssh-copy-id failed -- run from a terminal");
#endif
}

// Which column a section lives in: the two key-binding sections fill the left
// column, everything else the right.  (Keeps the two columns roughly equal height.)
static int section_col(const char* sec)
{
    if (!strcmp(sec,"Action keys") || !strcmp(sec,"Buddy keys")) return 0;
    return 1;
}

static float layout(void)	// returns y of the taller column's bottom; fills setting.col/.y
{
    float cy[2] = { 16, 16 };
    const char* csec[2] = { NULL, NULL };
    for (int i=0;i<NSET;i++) {
        int c = section_col(settings[i].section);
        if (csec[c] != settings[i].section) {
            csec[c] = settings[i].section;
            cy[c] += (cy[c] > 16 ? 10 : 0);
            cy[c] += HEADH;
        }
        settings[i].col = c;
        settings[i].y   = cy[c];
        cy[c] += ROWH;
    }
    return cy[0] > cy[1] ? cy[0] : cy[1];
}

static void draw(void)
{
    rect(0,0,WINW,WINH, 24,24,28);
    rect(COLW, 12, 1, WINH-60, 50,50,60);	// divider between the two columns
    const char* sec[2]={NULL,NULL};
    for (int i=0;i<NSET;i++) {
        setting_t* s=&settings[i];
        float cx = s->col * COLW;
        if (sec[s->col]!=s->section) {
            sec[s->col]=s->section;
            text(cx+LABELX, s->y-HEADH+8, s->section, 120,200,255);
            rect(cx+LABELX, s->y-6, COLW-2*LABELX, 1, 60,60,70);
        }
        int hot = (mode==1&&active==i)||(mode==2&&active==i);
        text(cx+LABELX, s->y+4, s->label, 220,220,220);
        char buf[160];
        if (s->type==T_KEY)      keyname(s->ival,buf,sizeof(buf));
        else if (s->type==T_TOGGLE) snprintf(buf,sizeof(buf), s->ival?"On":"Off");
        else if (s->type==T_INT) snprintf(buf,sizeof(buf),"< %d >", s->ival);
        else if (s->type==T_CHOICE) snprintf(buf,sizeof(buf),"< %s >", s->sval[0]?base_name(s->sval):"auto (detect)");
        else                     snprintf(buf,sizeof(buf),"%s", (mode==2&&active==i)?editbuf:s->sval);
        if (hot) rect(cx+VALX-4, s->y+1, COLW-VALX-LABELX+4, ROWH-4, 50,50,70);
        else if (hover==i && mode==0) rect(cx+VALX-4, s->y+1, COLW-VALX-LABELX+4, ROWH-4, 40,40,55);  // Backspace clears this
        if (mode==2&&active==i) { // text cursor
            text(cx+VALX, s->y+4, buf, 255,255,160);
            text(cx+VALX + (float)strlen(buf)*FONT_CW, s->y+4, "_", 255,255,160);
        } else text(cx+VALX, s->y+4, buf, 255,235,150);
    }
    // buttons
    rect(btn_save.x,btn_save.y,btn_save.w,btn_save.h, 40,110,40); text(btn_save.x+18,btn_save.y+8,"Save",230,255,230);
    rect(btn_quit.x,btn_quit.y,btn_quit.w,btn_quit.h, 110,40,40); text(btn_quit.x+18,btn_quit.y+8,"Quit",255,230,230);
    rect(btn_sshkey.x,btn_sshkey.y,btn_sshkey.w,btn_sshkey.h, 40,80,120); text(btn_sshkey.x+12,btn_sshkey.y+8,"Copy SSH key",210,230,255);
    if (status[0]) text(LABELX, WINH-26, status, 160,255,160);
    SDL_RenderPresent(ren);
}

static int hit(float mx,float my, SDL_FRect r){ return mx>=r.x&&mx<r.x+r.w&&my>=r.y&&my<r.y+r.h; }

// Index of the setting whose value cell is under (mx,my), or -1.  Mirrors the
// value-rect test in click(); used to target the mouse-over value for Backspace-clear.
static int setting_at(float mx,float my)
{
    for (int i=0;i<NSET;i++) {
        float cx = settings[i].col * COLW;
        SDL_FRect vr={cx+VALX-4,settings[i].y,COLW-VALX,ROWH};
        if (hit(mx,my,vr)) return i;
    }
    return -1;
}

static void click(float mx,float my)
{
    if (hit(mx,my,btn_save)) { save_cfg(); snprintf(status,sizeof(status),"Saved aidoom.cfg (next to the binary)"); return; }
    if (hit(mx,my,btn_quit)) { SDL_Event q={.type=SDL_EVENT_QUIT}; SDL_PushEvent(&q); return; }
    if (hit(mx,my,btn_sshkey)) { copy_ssh_key(find_sval("gpu_host"), find_sval("gpu_user"), find_sval("gpu_ssh_port")); return; }
    for (int i=0;i<NSET;i++) {
        setting_t* s=&settings[i];
        float cx = s->col * COLW;
        SDL_FRect vr={cx+VALX-4,s->y,COLW-VALX,ROWH};
        if (!hit(mx,my,vr)) continue;
        if (s->type==T_KEY) { mode=1; active=i; snprintf(status,sizeof(status),"Press a key, mouse button, or wheel for \"%s\"  -  Esc cancels", s->label); }
        else if (s->type==T_TOGGLE) s->ival = !s->ival;
        else if (s->type==T_INT) {
            // hit-test against the actual "< N >" text: left half = '<', right = '>'
            char tmp[32]; snprintf(tmp,sizeof(tmp),"< %d >", s->ival);
            float w = (float)strlen(tmp)*FONT_CW;
            if (mx <= cx+VALX + w) {
                s->ival += (mx < cx+VALX + w/2.0f) ? -1 : 1;
                if (s->ival<s->vmin) s->ival=s->vmin;
                if (s->ival>s->vmax) s->ival=s->vmax;
            }
        }
        else if (s->type==T_CHOICE) {
            char t[160]; snprintf(t,sizeof(t),"< %s >", s->sval[0]?base_name(s->sval):"auto (detect)");
            float w = (float)strlen(t)*FONT_CW;
            if (mx <= cx+VALX + w && niwad>0) {
                iwadsel += (mx < cx+VALX + w/2.0f) ? -1 : 1;
                if (iwadsel<0) iwadsel = niwad-1;
                if (iwadsel>=niwad) iwadsel = 0;
                strncpy(s->sval, iwadlist[iwadsel], sizeof(s->sval)-1); s->sval[sizeof(s->sval)-1]=0;
                snprintf(status,sizeof(status),"IWAD: %s", s->sval[0]?s->sval:"auto-detect");
            }
        }
        else if (s->type==T_TEXT) { mode=2; active=i; strncpy(editbuf,s->sval,sizeof(editbuf)-1); editbuf[sizeof(editbuf)-1]=0; SDL_StartTextInput(win); snprintf(status,sizeof(status),"Type, Enter to confirm"); }
        return;
    }
}

int main(int argc, char** argv)
{
    (void)argc;(void)argv;
    SDL_SetMainReady();		// we own main() (built with -DSDL_MAIN_HANDLED)
    for (int i=0;i<NSET;i++)
        if (settings[i].type==T_TEXT || settings[i].type==T_CHOICE) set_default_text(&settings[i]);
        else set_default_int(&settings[i]);
    scan_iwads();
    load_cfg();
    for (int i=0;i<NSET;i++)
        if (settings[i].type==T_CHOICE) iwad_sync_selection(settings[i].sval);

    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1; }
    win = SDL_CreateWindow("aiDoom Config", WINW, WINH, 0);
    {
        // Window/taskbar icon from the shared aidoom.ico (same as the game).
        SDL_Surface* icon = SDL_CreateSurfaceFrom(
            AIDOOM_ICON_W, AIDOOM_ICON_H, SDL_PIXELFORMAT_RGBA32,
            (void *)aidoom_icon_rgba, AIDOOM_ICON_W*4);
        if (icon) { SDL_SetWindowIcon(win, icon); SDL_DestroySurface(icon); }
    }
    ren = SDL_CreateRenderer(win, NULL);
    font_init();

    float bottom = layout();
    float by = bottom + 18; if (by > WINH-40) by = WINH-40;
    btn_save   = (SDL_FRect){ LABELX, by, 90, 30 };
    btn_quit   = (SDL_FRect){ WINW-LABELX-90, by, 90, 30 };
    btn_sshkey = (SDL_FRect){ WINW/2.0f-90, by, 180, 30 };

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
                if (e.type==SDL_EVENT_MOUSE_BUTTON_DOWN) {
                    int mk = (e.button.button==SDL_BUTTON_LEFT)?K_MOUSE1
                           : (e.button.button==SDL_BUTTON_RIGHT)?K_MOUSE2
                           : (e.button.button==SDL_BUTTON_MIDDLE)?K_MOUSE3:0;
                    if (mk) { settings[active].ival=mk; mode=0; status[0]=0; }
                    break;
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
            if (e.type==SDL_EVENT_MOUSE_MOTION) { hover = setting_at(e.motion.x,e.motion.y); break; }
            if (e.type==SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button==SDL_BUTTON_LEFT) { click(e.button.x,e.button.y); break; }
            // Backspace (or Delete) clears the value the mouse is over: text/choice -> empty,
            // key bind -> none, int/toggle -> minimum.  A quick "delete value" without editing.
            if (e.type==SDL_EVENT_KEY_DOWN && (e.key.key==SDLK_BACKSPACE||e.key.key==SDLK_DELETE) && hover>=0 && hover<NSET) {
                setting_t* s=&settings[hover];
                if (s->type==T_TEXT||s->type==T_CHOICE) { s->sval[0]=0; if (s->type==T_CHOICE) iwadsel=-1; }
                else if (s->type==T_KEY) s->ival=0;
                else s->ival = s->vmin;        // T_INT / T_TOGGLE -> minimum / off
                snprintf(status,sizeof(status),"Cleared \"%s\"", s->label);
                break;
            }
            if (e.type==SDL_EVENT_KEY_DOWN && e.key.key==SDLK_ESCAPE) { run=0; break; }
        }
        draw();
    }
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
