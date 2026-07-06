// extractor -- graphical (SDL3) asset extractor for aiDoom.
//
// Replaces the Python asset-extraction scripts (extract_heretic_monsters.py,
// extract_hexen.py, extract_freedoom2.py): reads YOUR OWN heretic.wad /
// hexen.wad / freedoom2.wad and builds the palette-converted, renamed monster
// asset PWADs the engine loads (hereticstuff.wad / hexenstuff.wad /
// freedoomstuff.wad in run/ID0/).  Nothing is downloaded or redistributed --
// it only re-packs assets from IWADs you already have.
//
// UI: a dropdown of the source IWADs that are actually present (scanned at
// startup from run/ and run/ID0/), and an Extract button underneath.  Text is
// drawn from the baked font atlas (tools/font_atlas.h), so there are no deps
// beyond SDL3 -- exactly like gpumon_sdl.c / launcher.c.
//
// Build: tools/build_extractor.sh (Linux/macOS) / build_extractor_win.sh (MinGW)
//        or tools/Makefile.msvc / CMakeLists.txt.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/stat.h>

#include "font_atlas.h"
#include "../files/aidoom_icon.h"	// shared 64x64 RGBA window icon (from aidoom.ico)

#define WINW 560
#define WINH 460

#define PAD    16
#define DD_Y   96
#define DD_H   26
#define BTN_Y  132
#define BTN_H  34
#define LOG_Y  190

static SDL_Window*   win;
static SDL_Renderer* ren;
static SDL_Texture*  font;

// ================================================================= WAD I/O
//
// A WAD is a 12-byte header (magic, lump count, directory offset) followed by
// the lump data and then a directory of 16-byte entries (filepos, size, name8).
// Lump names are up to 8 bytes, NUL-padded (the trailing NUL is the terminator).

typedef struct {
    unsigned char* data;
    long           len;
    int            n;
    struct { char name[9]; int pos, size; } *dir;
} wad_t;

static unsigned int rd32(const unsigned char* p)
{ return (unsigned)p[0] | ((unsigned)p[1]<<8) | ((unsigned)p[2]<<16) | ((unsigned)p[3]<<24); }

static void wad_free(wad_t* w)
{
    if (!w) return;
    free(w->data); free(w->dir); free(w);
}

// Load a WAD file fully into memory + parse its directory.  NULL on any error.
static wad_t* wad_load(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len < 12) { fclose(f); return NULL; }
    unsigned char* data = malloc(len);
    if (!data || fread(data, 1, len, f) != (size_t)len) { free(data); fclose(f); return NULL; }
    fclose(f);
    if (memcmp(data, "IWAD", 4) && memcmp(data, "PWAD", 4)) { free(data); return NULL; }

    int   n   = (int)rd32(data + 4);
    long  off = (long)rd32(data + 8);
    if (n < 0 || off < 0 || off + (long)n*16 > len) { free(data); return NULL; }

    wad_t* w = calloc(1, sizeof *w);
    w->data = data; w->len = len; w->n = n;
    w->dir = calloc(n ? n : 1, sizeof *w->dir);
    for (int i = 0; i < n; i++) {
        const unsigned char* e = data + off + i*16;
        w->dir[i].pos  = (int)rd32(e);
        w->dir[i].size = (int)rd32(e + 4);
        memcpy(w->dir[i].name, e + 8, 8);
        w->dir[i].name[8] = 0;                 // 8-char names have no in-band NUL
    }
    return w;
}

static int wad_find(wad_t* w, const char* name)
{
    for (int i = 0; i < w->n; i++)
        if (!strcmp(w->dir[i].name, name)) return i;
    return -1;
}

static const unsigned char* wad_lump(wad_t* w, const char* name, int* size)
{
    int i = wad_find(w, name);
    if (i < 0) return NULL;
    if (size) *size = w->dir[i].size;
    return w->data + w->dir[i].pos;
}

// ----------------------------------------------------------------- output WAD
typedef struct { char name[9]; unsigned char* data; int size; } olump_t;
typedef struct { olump_t* v; int n, cap; } obuf_t;

// Append a lump (name capped/padded to 8, data COPIED so the source WAD can go).
static void oadd(obuf_t* o, const char* name, const unsigned char* data, int size)
{
    if (o->n == o->cap) { o->cap = o->cap ? o->cap*2 : 64; o->v = realloc(o->v, o->cap*sizeof *o->v); }
    olump_t* L = &o->v[o->n++];
    memset(L->name, 0, sizeof L->name);
    strncpy(L->name, name, 8);
    L->size = size;
    L->data = size ? malloc(size) : NULL;
    if (size) memcpy(L->data, data, size);
}

static void ofree(obuf_t* o) { for (int i=0;i<o->n;i++) free(o->v[i].data); free(o->v); o->v=NULL; o->n=o->cap=0; }

static void wr32(FILE* f, unsigned int v)
{ unsigned char b[4]={v&255,(v>>8)&255,(v>>16)&255,(v>>24)&255}; fwrite(b,1,4,f); }

// Write `o` as a PWAD.  Returns 1 on success.
static int wad_write(const char* path, obuf_t* o)
{
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    unsigned int diroff = 12;
    for (int i = 0; i < o->n; i++) diroff += o->v[i].size;
    fwrite("PWAD", 1, 4, f);
    wr32(f, o->n);
    wr32(f, diroff);
    for (int i = 0; i < o->n; i++)
        if (o->v[i].size) fwrite(o->v[i].data, 1, o->v[i].size, f);
    unsigned int pos = 12;
    for (int i = 0; i < o->n; i++) {
        wr32(f, pos);
        wr32(f, o->v[i].size);
        char nm[8]; memset(nm, 0, 8); memcpy(nm, o->v[i].name, strlen(o->v[i].name) < 8 ? strlen(o->v[i].name) : 8);
        fwrite(nm, 1, 8, f);
        pos += o->v[i].size;
    }
    fclose(f);
    return 1;
}

// ----------------------------------------------------------------- palette/pixels
static const unsigned char* get_playpal(wad_t* w)
{ int sz; const unsigned char* p = wad_lump(w, "PLAYPAL", &sz); return (p && sz >= 768) ? p : NULL; }

// src palette index -> nearest dst palette index (both are 256*RGB triples).
static void build_xlat(const unsigned char* sp, const unsigned char* dp, unsigned char xl[256])
{
    for (int i = 0; i < 256; i++) {
        int r = sp[i*3], g = sp[i*3+1], b = sp[i*3+2];
        int best = 0; long bd = 1L<<30;
        for (int j = 0; j < 256; j++) {
            int dr = r-dp[j*3], dg = g-dp[j*3+1], db = b-dp[j*3+2];
            long d = (long)dr*dr + (long)dg*dg + (long)db*db;
            if (d < bd) { bd = d; best = j; if (!d) break; }
        }
        xl[i] = (unsigned char)best;
    }
}

// Remap a Doom patch's pixels through xl (structure/offsets unchanged); returns
// a fresh malloc'd copy of `size` bytes (caller frees).
static unsigned char* dup_remap(const unsigned char* raw, int size, const unsigned char* xl)
{
    unsigned char* b = malloc(size ? size : 1);
    memcpy(b, raw, size);
    if (size < 8) return b;
    int w = (short)(b[0] | (b[1]<<8));
    if (w <= 0 || 8 + 4*w > size) return b;              // not a patch -> leave alone
    for (int i = 0; i < w; i++) {
        unsigned int o = rd32(b + 8 + i*4);
        if (o == 0 || o >= (unsigned)size) continue;
        while (o + 1 < (unsigned)size && b[o] != 0xff) {
            int length = b[o+1];
            for (int k = 0; k < length; k++) {
                unsigned int p = o + 3 + k;
                if (p < (unsigned)size) b[p] = xl[b[p]];
            }
            o += length + 4;
        }
    }
    return b;
}

static int is_dmx(const unsigned char* raw, int size)
{ return size >= 8 && raw[0] == 0x03 && raw[1] == 0x00; }

// ================================================================= rename tables
//
// These MUST stay in sync with the C ports (files/heretic.c, files/freedoom.c,
// files/hexen.c) that reference the renamed sprite codes -- they are copied
// verbatim from the Python extractors they replace.

// --- Heretic: monster/projectile 4-char code -> DOOM-side code (heretic.c) ---
static const char* HERETIC_RENAME[][2] = {
    {"IMPX","HIMP"},{"MUMM","HMUM"},{"FX15","HMUF"},{"KNIG","HKNI"},{"SPAX","HKAX"},
    {"RAXE","HKRX"},{"BEAS","HBEA"},{"FRB1","HBEB"},{"CLNK","HCLK"},{"WZRD","HWIZ"},
    {"FX11","HWIB"},{"SNKE","HSNK"},{"SNFX","HSNB"},{"HEAD","HIRO"},{"FX05","HIRB"},
    {"FX06","HIRW"},{"FX07","HIRX"},{"MNTR","HMIN"},{"FX12","HMNA"},{"FX13","HMNB"},
    {"FX14","HMNC"},{"SRCR","HSR1"},{"SOR2","HSR2"},{"FX16","HSRB"},{"CHKN","HCHK"},
    // Heretic artifacts: no DOOM collision, kept as-is (same SPR_ codes as crispy-doom).
    {"PTN1","PTN1"},{"PTN2","PTN2"},{"SPHL","SPHL"},{"PWBK","PWBK"},{"TRCH","TRCH"},
    {"FBMB","FBMB"},{"EGGC","EGGC"},{"SOAR","SOAR"},{"INVU","INVU"},{"INVS","INVS"},
    {"ATLP","ATLP"},
    {NULL,NULL}
};
// DMX sound name substrings copied (DS-prefixed) for the Heretic monsters.
static const char* HERETIC_SND[] = {
    "imp","mum","bst","clk","snk","kgt","wiz","hed","minsit","minat","mindth",
    "minact","minpai","sbtsit","sorzap","sorsit","soratk","sorpai","soract", NULL
};

// --- Freedoom2: DOOM2-exclusive 4-char code -> F* code (freedoom.c) ----------
static const char* FREEDOOM_RENAME[][2] = {
    {"SKEL","FSKE"},{"FATT","FFAT"},{"VILE","FVIL"},{"BSPI","FBSP"},{"CPOS","FCPO"},
    {"BOS2","FBO2"},{"PAIN","FPAI"},{"SSWV","FSSW"},{"KEEN","FKEE"},
    {"FATB","FFAB"},{"FBXP","FFBX"},{"MANF","FMAN"},{"FIRE","FFIR"},
    {"APLS","FAPL"},{"APBX","FAPB"},
    {NULL,NULL}
};

// --- Hexen: monster + weapon sprite codes (renamed into an "X.." namespace) --
static const char* HEXEN_MONSTER_SPR[] = {
    "ETTN","ETTB","CENT","CTXD","CTFX","CTDP","DEMN","DEMA","DEMB","DEMC","DEMD",
    "DEME","DMFX","DEM2","DMBA","DMBB","DMBC","DMBD","DMBE","D2FX","WRTH","WRT2",
    "WRBL","MNTR","FX12","FX13","MNSM","SSPT","SSDV","SSXD","SSFX","BISH","BPFX",
    "DRAG","DRFX","FDMN","FDMB","ICEY","ICPR","ICWS","ICEC","SORC","SBMP","SBS1",
    "SBS2","SBS3","SBS4","SBMB","SBMG","SBFX","KORX","ABAT","PIGY","FDTH", NULL
};
static const char* HEXEN_WEAPON_SPR[] = {
    "FPCH","WFAX","FAXE","FSFX","WFHM","FHMR","FHFX","FSRD","CMCE","WCSS","CSSF",
    "WCFM","CFLM","CFFX","CHLY","SPIR","MWND","WMLG","MLNG","MLFX","MLF2","MSTF",
    "MSP1","MSP2","CONE","SHEX","WFR1","WFR2","WFR3","WCH1","WCH2","WCH3","WMS1",
    "WMS2","WMS3","WPIG","WMCS","AFWP","ACWP","AMWP","AGER","AGR2","AGR3","AGR4", NULL
};
// Wired Hexen monster sounds: sfx short name -> source Hexen DMX lump.  Copied
// as "DS"+short so the engine's "ds%s" lookup finds them (files/hexen.c sfx_x_*).
static const char* HEXEN_SND_WIRED[][2] = {
    {"xetsit","cent2"},{"xetpai","cent1"},{"xetatk","ethit1"},{"xetdth","cntdth1"},
    {"xcesit","taur1"},{"xceact","taur2"},{"xcepai","taur4"},{"xceatk","centhit2"},
    {"xcedth","cntdth1"},{"xslatk","cntshld4"},
    {"xdesit","sbtsit5"},{"xdepai","minact1"},{"xdeatk","dematk2"},{"xdedth","sbtdth3"},
    {"xfdact","fired5"},{"xfdpai","fired2"},{"xfdatk","spit6"},{"xfddth","fired3"},{"xfdhit","firedhit"},
    {"xwrsit","raith5a"},{"xwract","raith3"},{"xwrpai","raith4a"},{"xwratk","raith1b"},{"xwrdth","rathdth2"},
    {"xbisit","syab2d"},{"xbiact","stb1d"},{"xbipai","bshpn1"},{"xbiatk","pop"},{"xbidth","bishdth1"},{"xbihit","bshhit2"},
    {"xicsit","frosty1"},{"xicatk","frosty2"},{"xichit","shards1b"},
    {"xstsit","wtrcrt7"},{"xstact","srfc3"},{"xstpai","serppn1"},{"xstatk","wtrswip"},{"xstdth","srpdth1"},{"xsthit","glbhit4"},
    {"xdrsit","dragsit1"},{"xdrpai","dragpn2"},{"xdratk","mage4"},{"xdrdth","dragdie2"},{"xdrhit","mageball"},
    {NULL,NULL}
};
// Hexen monster/weapon sound name substrings copied verbatim (reference).
static const char* HEXEN_SND_KW[] = {
    "cent","cnt","eth","taur","minact","mindth","minpain","minsit","kor","serp",
    "srp","demat","raith","rath","wrbl","drag","sor","sbt","bish","bsh","fired",
    "fdmn","pig","icedth","icemv","icebrk","frosty","icpr","vamp","bats","glbh",
    "srfc","mumpun","squeal","slurp","shlurp","axe","ham","hmhit","punch","sword",
    "holy","spirt","clhmm","mageball","wand","blastr","mage4","cone3","gnt",
    "wepele","strike1","strike3","fgt","mgpain","mgdth","mggrunt","mgxdth","mgfall",
    "mghmm","mgcdth","clxdth","plrdth","plrpain","plrburn","plrcdth", NULL
};

#define AISTUFF_NOTE "aiDoom internal asset pack -- loaded by the game, not a user PWAD\n"

// ================================================================= sources
enum { K_HERETIC, K_HEXEN, K_FREEDOOM };

typedef struct {
    const char* src;      // source IWAD basename (lowercase)
    const char* out;      // output PWAD basename (written to run/ID0/)
    const char* label;    // shown in the dropdown
    int         kind;
} source_t;

static const source_t SOURCES[] = {
    { "heretic.wad",   "hereticstuff.wad",   "Heretic  \x1a  hereticstuff.wad",   K_HERETIC  },
    { "hexen.wad",     "hexenstuff.wad",     "Hexen  \x1a  hexenstuff.wad",       K_HEXEN    },
    { "freedoom2.wad", "freedoomstuff.wad",  "FreeDoom2  \x1a  freedoomstuff.wad", K_FREEDOOM },
};
#define NSOURCES ((int)(sizeof SOURCES / sizeof SOURCES[0]))

static int  avail[NSOURCES];          // indices into SOURCES[] that were found
static char avail_path[NSOURCES][600];// resolved absolute path of each found source
static int  navail = 0;
static int  sel = 0;                  // index into avail[] (the selected source)

static char g_rundir[512];            // dir the binary lives in (== run/)
static char g_id0[560];               // g_rundir + "/ID0" (where WADs live)

// Locate `basename` in run/ID0 then run/, trying a few case variants.  1 on hit.
static int find_wad(const char* basename, char* out, int n)
{
    const char* dirs[2] = { g_id0, g_rundir };
    char lo[64], up[64];
    snprintf(lo, sizeof lo, "%s", basename);
    snprintf(up, sizeof up, "%s", basename);
    for (char* p = lo; *p; p++) *p = (char)tolower((unsigned char)*p);
    for (char* p = up; *p; p++) *p = (char)toupper((unsigned char)*p);
    const char* names[3] = { basename, lo, up };
    for (int d = 0; d < 2; d++)
        for (int c = 0; c < 3; c++) {
            char full[600];
            snprintf(full, sizeof full, "%s/%s", dirs[d], names[c]);
            struct stat st;
            if (stat(full, &st) == 0 && (st.st_mode & S_IFREG)) {
                snprintf(out, n, "%s", full);
                return 1;
            }
        }
    return 0;
}

// The DOOM IWAD whose PLAYPAL is the palette-conversion target (+ base sprites).
static int find_base(char* out, int n)
{
    static const char* order[] = { "doom2.wad", "doom.wad", "plutonia.wad", "tnt.wad", NULL };
    for (int i = 0; order[i]; i++)
        if (find_wad(order[i], out, n)) return 1;
    return 0;
}

static void scan_sources(void)
{
    navail = 0;
    for (int i = 0; i < NSOURCES; i++) {
        char path[600];
        if (find_wad(SOURCES[i].src, path, sizeof path)) {
            avail[navail] = i;
            snprintf(avail_path[navail], sizeof avail_path[navail], "%s", path);
            navail++;
        }
    }
    if (sel >= navail) sel = 0;
}

// ================================================================= extractors
//
// One function per source, faithfully porting the matching Python script.  Each
// appends a human-readable report into `log` and returns 1 on success.

static void logf_(char* log, int cap, const char* fmt, ...)
{
    int len = (int)strlen(log);
    if (len >= cap-1) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(log + len, cap - len, fmt, ap);
    va_end(ap);
}

static long obuf_bytes(obuf_t* o)
{ long t = 0; for (int i=0;i<o->n;i++) t += o->v[i].size; return t; }

// --- Heretic: build hereticstuff.wad ----------------------------------------
static int extract_heretic(const char* srcpath, const char* outpath, char* log, int lcap)
{
    char basepath[600];
    if (!find_base(basepath, sizeof basepath)) {
        logf_(log, lcap, "ERROR: no DOOM IWAD found in ID0/ for the target palette.\n"
                         "  (need doom2.wad / doom.wad / plutonia.wad / tnt.wad)\n");
        return 0;
    }
    wad_t* h = wad_load(srcpath);
    wad_t* b = wad_load(basepath);
    if (!h) { logf_(log, lcap, "ERROR: cannot read %s\n", srcpath); wad_free(b); return 0; }
    if (!b) { logf_(log, lcap, "ERROR: cannot read base %s\n", basepath); wad_free(h); return 0; }
    const unsigned char* sp = get_playpal(h);
    const unsigned char* dp = get_playpal(b);
    if (!sp || !dp) { logf_(log, lcap, "ERROR: missing PLAYPAL\n"); wad_free(h); wad_free(b); return 0; }
    unsigned char xl[256]; build_xlat(sp, dp, xl);

    obuf_t o = {0};
    oadd(&o, "AISTUFF", (const unsigned char*)AISTUFF_NOTE, (int)strlen(AISTUFF_NOTE));
    oadd(&o, "S_START", NULL, 0);
    int n_spr = 0;
    for (int i = 0; i < h->n; i++) {
        const char* nm = h->dir[i].name;
        if (strlen(nm) <= 4) continue;
        char code[5]; memcpy(code, nm, 4); code[4] = 0;
        for (int r = 0; HERETIC_RENAME[r][0]; r++) {
            if (!strcmp(code, HERETIC_RENAME[r][0])) {
                char nn[9]; snprintf(nn, sizeof nn, "%s%s", HERETIC_RENAME[r][1], nm+4);
                unsigned char* px = dup_remap(h->data + h->dir[i].pos, h->dir[i].size, xl);
                oadd(&o, nn, px, h->dir[i].size);
                free(px); n_spr++;
                break;
            }
        }
    }
    oadd(&o, "S_END", NULL, 0);

    int n_snd = 0;
    for (int i = 0; i < h->n; i++) {
        const unsigned char* raw = h->data + h->dir[i].pos;
        int sz = h->dir[i].size;
        if (!is_dmx(raw, sz)) continue;
        char low[16]; int j; for (j=0;j<15 && h->dir[i].name[j];j++) low[j]=(char)tolower((unsigned char)h->dir[i].name[j]); low[j]=0;
        int hit = 0;
        for (int k = 0; HERETIC_SND[k]; k++) if (strstr(low, HERETIC_SND[k])) { hit = 1; break; }
        if (!hit) continue;
        char nn[9]; snprintf(nn, sizeof nn, "DS%.6s", h->dir[i].name);
        oadd(&o, nn, raw, sz); n_snd++;
    }

    int ok = wad_write(outpath, &o);
    logf_(log, lcap, ok ? "wrote %s\n" : "ERROR: could not write %s\n", outpath);
    if (ok) {
        logf_(log, lcap, "  base palette : %s\n", strrchr(basepath,'/')?strrchr(basepath,'/')+1:basepath);
        logf_(log, lcap, "  sprites      : %d (converted + renamed)\n", n_spr);
        logf_(log, lcap, "  sounds       : %d (DS* lumps)\n", n_snd);
        logf_(log, lcap, "  total        : %d lumps, %.1f MB\n", o.n, obuf_bytes(&o)/1048576.0);
    }
    ofree(&o); wad_free(h); wad_free(b);
    return ok;
}

// --- Freedoom2: build freedoomstuff.wad -------------------------------------
static int extract_freedoom(const char* srcpath, const char* outpath, char* log, int lcap)
{
    wad_t* fw = wad_load(srcpath);
    if (!fw) { logf_(log, lcap, "ERROR: cannot read %s\n", srcpath); return 0; }
    int s0 = wad_find(fw, "S_START"), s1 = wad_find(fw, "S_END");
    if (s0 < 0 || s1 < 0 || s1 < s0) {
        logf_(log, lcap, "ERROR: freedoom2 has no S_START/S_END sprite range\n");
        wad_free(fw); return 0;
    }
    // DOOM1 sound diff (optional): keep only DOOM2-exclusive sounds.
    char d1path[600]; wad_t* d1 = NULL;
    if (find_wad("doom.wad", d1path, sizeof d1path)) d1 = wad_load(d1path);

    obuf_t o = {0};
    oadd(&o, "AISTUFF", (const unsigned char*)AISTUFF_NOTE, (int)strlen(AISTUFF_NOTE));
    oadd(&o, "S_START", NULL, 0);
    int n_spr = 0;
    for (int i = s0+1; i < s1; i++) {
        const char* nm = fw->dir[i].name;
        if (strlen(nm) < 4) continue;
        char code[5]; memcpy(code, nm, 4); code[4] = 0;
        for (int r = 0; FREEDOOM_RENAME[r][0]; r++) {
            if (!strcmp(code, FREEDOOM_RENAME[r][0])) {
                char nn[9]; snprintf(nn, sizeof nn, "%s%s", FREEDOOM_RENAME[r][1], nm+4);
                oadd(&o, nn, fw->data + fw->dir[i].pos, fw->dir[i].size);  // doom palette -> verbatim
                n_spr++;
                break;
            }
        }
    }
    oadd(&o, "S_END", NULL, 0);

    int n_snd = 0;
    for (int i = 0; i < fw->n; i++) {
        const char* nm = fw->dir[i].name;
        const unsigned char* raw = fw->data + fw->dir[i].pos;
        int sz = fw->dir[i].size;
        if (!(nm[0]=='D' && (nm[1]=='S' || nm[1]=='P'))) continue;
        if (!is_dmx(raw, sz)) continue;
        if (d1 && wad_find(d1, nm) >= 0) continue;   // present in DOOM1 -> not exclusive
        oadd(&o, nm, raw, sz); n_snd++;
    }

    int ok = wad_write(outpath, &o);
    logf_(log, lcap, ok ? "wrote %s\n" : "ERROR: could not write %s\n", outpath);
    if (ok) {
        logf_(log, lcap, "  sound diff   : %s\n", d1 ? "doom.wad (exclusive only)" : "(none: all DS*/DP*)");
        logf_(log, lcap, "  sprites      : %d (renamed F*)\n", n_spr);
        logf_(log, lcap, "  sounds       : %d (orig names)\n", n_snd);
        logf_(log, lcap, "  total        : %d lumps, %.1f MB\n", o.n, obuf_bytes(&o)/1048576.0);
    }
    ofree(&o); wad_free(fw); wad_free(d1);
    return ok;
}

// Deterministic, collision-free 4-char rename into the "X" (heXen) namespace.
// stem = 'X' + code[:2]; 4th char tries code[2], code[3], then 2..9/A..Z.
static void hexen_make_rename(char used[][5], int* nused, const char* code, char out[5])
{
    char stem[4] = { 'X', code[0], code[1], 0 };
    char cands[64]; int nc = 0;
    cands[nc++] = code[2]; cands[nc++] = code[3];
    for (const char* p = "23456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"; *p; p++) cands[nc++] = *p;
    for (int c = 0; c < nc; c++) {
        char cand[5]; snprintf(cand, sizeof cand, "%s%c", stem, cands[c]);
        int taken = 0;
        for (int u = 0; u < *nused; u++) if (!strcmp(used[u], cand)) { taken = 1; break; }
        if (!taken) { strcpy(out, cand); strcpy(used[(*nused)++], cand); return; }
    }
    strcpy(out, stem); // unreachable in practice
}

// --- Hexen: build hexenstuff.wad --------------------------------------------
static int extract_hexen(const char* srcpath, const char* outpath, char* log, int lcap)
{
    char basepath[600];
    if (!find_base(basepath, sizeof basepath)) {
        logf_(log, lcap, "ERROR: no DOOM IWAD found in ID0/ for the target palette.\n");
        return 0;
    }
    wad_t* h = wad_load(srcpath);
    wad_t* b = wad_load(basepath);
    if (!h) { logf_(log, lcap, "ERROR: cannot read %s\n", srcpath); wad_free(b); return 0; }
    if (!b) { logf_(log, lcap, "ERROR: cannot read base %s\n", basepath); wad_free(h); return 0; }
    const unsigned char* sp = get_playpal(h);
    const unsigned char* dp = get_playpal(b);
    if (!sp || !dp) { logf_(log, lcap, "ERROR: missing PLAYPAL\n"); wad_free(h); wad_free(b); return 0; }
    unsigned char xl[256]; build_xlat(sp, dp, xl);

    // Which wanted sprite codes actually exist in this hexen.wad?
    static const char* wanted[128]; int nwanted = 0;
    static char ren[128][5]; static char used[128][5]; int nused = 0;
    for (int t = 0; t < 2; t++) {
        const char** list = t ? HEXEN_WEAPON_SPR : HEXEN_MONSTER_SPR;
        for (int c = 0; list[c]; c++) {
            int present = 0;
            for (int i = 0; i < h->n && !present; i++)
                if (strlen(h->dir[i].name) > 4 && !strncmp(h->dir[i].name, list[c], 4)) present = 1;
            if (present && nwanted < 128) {
                wanted[nwanted] = list[c];
                hexen_make_rename(used, &nused, list[c], ren[nwanted]);
                nwanted++;
            }
        }
    }

    obuf_t o = {0};
    oadd(&o, "AISTUFF", (const unsigned char*)AISTUFF_NOTE, (int)strlen(AISTUFF_NOTE));
    oadd(&o, "S_START", NULL, 0);
    int n_spr = 0;
    for (int i = 0; i < h->n; i++) {
        const char* nm = h->dir[i].name;
        if (strlen(nm) <= 4) continue;
        for (int c = 0; c < nwanted; c++) {
            if (!strncmp(nm, wanted[c], 4)) {
                char nn[9]; snprintf(nn, sizeof nn, "%s%s", ren[c], nm+4);
                unsigned char* px = dup_remap(h->data + h->dir[i].pos, h->dir[i].size, xl);
                oadd(&o, nn, px, h->dir[i].size);
                free(px); n_spr++;
                break;
            }
        }
    }
    oadd(&o, "S_END", NULL, 0);

    // Wired monster sounds: copy each chosen Hexen DMX lump as "DS"+short.
    int n_ds = 0;
    for (int r = 0; HEXEN_SND_WIRED[r][0]; r++) {
        const char* shortnm = HEXEN_SND_WIRED[r][0];
        const char* srclump = HEXEN_SND_WIRED[r][1];
        for (int i = 0; i < h->n; i++) {
            if (strcasecmp(h->dir[i].name, srclump)) continue;
            const unsigned char* raw = h->data + h->dir[i].pos; int sz = h->dir[i].size;
            if (!is_dmx(raw, sz)) break;
            char nn[9]; snprintf(nn, sizeof nn, "DS%.6s", shortnm);
            for (char* p = nn; *p; p++) *p = (char)toupper((unsigned char)*p);
            oadd(&o, nn, raw, sz); n_ds++;
            break;
        }
    }

    // Verbatim keyword sounds (reference), de-duplicated by name.
    int n_snd = 0;
    for (int i = 0; i < h->n; i++) {
        const char* nm = h->dir[i].name;
        const unsigned char* raw = h->data + h->dir[i].pos; int sz = h->dir[i].size;
        if (!is_dmx(raw, sz)) continue;
        char low[16]; int j; for (j=0;j<15 && nm[j];j++) low[j]=(char)tolower((unsigned char)nm[j]); low[j]=0;
        int hit = 0;
        for (int k = 0; HEXEN_SND_KW[k]; k++) if (strstr(low, HEXEN_SND_KW[k])) { hit = 1; break; }
        if (!hit) continue;
        // de-dupe: skip if already emitted verbatim
        int dup = 0;
        for (int e = 0; e < o.n; e++) if (!strcmp(o.v[e].name, nm)) { dup = 1; break; }
        if (dup) continue;
        oadd(&o, nm, raw, sz); n_snd++;
    }

    int ok = wad_write(outpath, &o);
    logf_(log, lcap, ok ? "wrote %s\n" : "ERROR: could not write %s\n", outpath);
    if (ok) {
        logf_(log, lcap, "  base palette : %s\n", strrchr(basepath,'/')?strrchr(basepath,'/')+1:basepath);
        logf_(log, lcap, "  sprites      : %d  (%d codes -> X* namespace)\n", n_spr, nwanted);
        logf_(log, lcap, "  wired sounds : %d (DS* for sfx_x_*)\n", n_ds);
        logf_(log, lcap, "  ref sounds   : %d (verbatim)\n", n_snd);
        logf_(log, lcap, "  total        : %d lumps, %.1f MB\n", o.n, obuf_bytes(&o)/1048576.0);

        // Sidecar rename map for the future files/hexen.c port (best-effort).
        char mappath[600];
        snprintf(mappath, sizeof mappath, "%s/../tools/hexen_sprite_map.txt", g_rundir);
        FILE* mf = fopen(mappath, "w");
        if (mf) {
            fprintf(mf, "# Hexen sprite code -> hexenstuff.wad code (use these in files/hexen.c)\n");
            for (int c = 0; c < nwanted; c++) {
                int weapon = 0;
                for (int w = 0; HEXEN_WEAPON_SPR[w]; w++) if (!strcmp(wanted[c], HEXEN_WEAPON_SPR[w])) { weapon = 1; break; }
                fprintf(mf, "%s -> %s   (%s)\n", wanted[c], ren[c], weapon ? "weapon" : "monster");
            }
            fclose(mf);
            logf_(log, lcap, "  rename map   -> tools/hexen_sprite_map.txt\n");
        }
    }
    ofree(&o); wad_free(h); wad_free(b);
    return ok;
}

// ================================================================= worker
enum { ST_IDLE, ST_RUNNING, ST_DONE };
static SDL_AtomicInt g_status;
static SDL_Mutex*    g_lock;
static char          g_log[4096];    // protected by g_lock
static int           g_job = -1;     // avail[] index to run (set by main thread)

static int worker(void* unused)
{
    (void)unused;
    int job = g_job;
    int si = avail[job];
    const source_t* S = &SOURCES[si];

    char out[600];
    snprintf(out, sizeof out, "%s/%s", g_id0, S->out);
    SDL_CreateDirectory(g_id0);      // ensure run/ID0 exists

    char local[4096]; local[0] = 0;
    snprintf(local, sizeof local, "Extracting from %s ...\n\n",
             strrchr(avail_path[job],'/') ? strrchr(avail_path[job],'/')+1 : avail_path[job]);

    switch (S->kind) {
        case K_HERETIC:  extract_heretic (avail_path[job], out, local, sizeof local); break;
        case K_HEXEN:    extract_hexen   (avail_path[job], out, local, sizeof local); break;
        case K_FREEDOOM: extract_freedoom(avail_path[job], out, local, sizeof local); break;
    }

    SDL_LockMutex(g_lock);
    snprintf(g_log, sizeof g_log, "%s", local);
    SDL_UnlockMutex(g_lock);
    SDL_SetAtomicInt(&g_status, ST_DONE);
    return 0;
}

// ================================================================= UI draw
static int   dd_open = 0;
static float mouse_x, mouse_y;
static const SDL_FRect btn = { PAD, BTN_Y, WINW-2*PAD, BTN_H };

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
    if (!str) return;
    SDL_SetTextureColorMod(font, r, g, b);
    for (const char* p=str; *p; p++) {
        int c = (unsigned char)*p;
        if (c < FONT_FIRST || c >= FONT_FIRST+FONT_COUNT) c = (c==0x1a) ? '>' : '?';
        SDL_FRect src = { (float)((c-FONT_FIRST)*FONT_CW), 0, FONT_CW, FONT_CH };
        SDL_FRect dst = { x, y, FONT_CW, FONT_CH };
        SDL_RenderTexture(ren, font, &src, &dst);
        x += FONT_CW;
    }
}

static void fillrect(float x,float y,float w,float h, Uint8 r,Uint8 g,Uint8 b)
{ SDL_FRect q={x,y,w,h}; SDL_SetRenderDrawColor(ren,r,g,b,255); SDL_RenderFillRect(ren,&q); }

static void outline(float x,float y,float w,float h, Uint8 r,Uint8 g,Uint8 b)
{ SDL_FRect q={x,y,w,h}; SDL_SetRenderDrawColor(ren,r,g,b,255); SDL_RenderRect(ren,&q); }

static void draw(void)
{
    int st = SDL_GetAtomicInt(&g_status);
    fillrect(0,0,WINW,WINH, 22,22,28);

    text(PAD, 16, "aiDoom Asset Extractor", 120,200,255);
    text(PAD, 40, "Re-pack monster assets from your own IWADs into run/ID0/.", 150,150,160);
    fillrect(PAD, 64, WINW-2*PAD, 1, 60,60,72);

    if (navail == 0) {
        text(PAD, DD_Y, "No source IWADs found in run/ or run/ID0/.", 220,120,90);
        text(PAD, DD_Y+24, "Place heretic.wad / hexen.wad / freedoom2.wad there,", 170,170,180);
        text(PAD, DD_Y+44, "then reopen this tool.", 170,170,180);
        SDL_RenderPresent(ren);
        return;
    }

    // --- source dropdown (closed header) ---
    text(PAD, DD_Y-22, "Source IWAD:", 170,170,180);
    fillrect(PAD, DD_Y, WINW-2*PAD, DD_H, 34,34,44);
    outline(PAD, DD_Y, WINW-2*PAD, DD_H, 70,70,88);
    text(PAD+8, DD_Y+(DD_H-FONT_CH)/2, SOURCES[avail[sel]].label, 235,235,240);
    text(WINW-PAD-FONT_CW-6, DD_Y+(DD_H-FONT_CH)/2, dd_open ? "^" : "v", 160,160,175);

    // --- Extract button ---
    int busy = (st == ST_RUNNING);
    fillrect(btn.x, btn.y, btn.w, btn.h, busy ? 60:40, busy ? 60:90, busy ? 70:130);
    outline(btn.x, btn.y, btn.w, btn.h, 90,120,160);
    {
        const char* bl = busy ? "Extracting..." : "Extract Assets";
        float tw = (float)strlen(bl)*FONT_CW;
        text(btn.x + (btn.w-tw)/2, btn.y + (btn.h-FONT_CH)/2, bl, 220,235,255);
    }

    // --- result log ---
    fillrect(PAD, LOG_Y, WINW-2*PAD, WINH-LOG_Y-PAD, 16,16,20);
    outline(PAD, LOG_Y, WINW-2*PAD, WINH-LOG_Y-PAD, 50,50,62);
    if (st != ST_IDLE) {
        char buf[4096];
        SDL_LockMutex(g_lock); snprintf(buf, sizeof buf, "%s", g_log); SDL_UnlockMutex(g_lock);
        float ly = LOG_Y + 8;
        char* line = buf;
        while (line && *line && ly < WINH-PAD-FONT_CH) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = 0;
            Uint8 cr=200,cg=205,cb=210;
            if (strstr(line, "ERROR")) { cr=230; cg=110; cb=90; }
            else if (strstr(line, "wrote")) { cr=130; cg=220; cb=140; }
            text(PAD+8, ly, line, cr,cg,cb);
            ly += FONT_CH;
            line = nl ? nl+1 : NULL;
        }
    }

    // --- open dropdown list (drawn LAST so it overlays the log) ---
    if (dd_open) {
        float dy = DD_Y + DD_H;
        for (int i = 0; i < navail; i++) {
            float oy = dy + i*DD_H;
            int hover = (mouse_x >= PAD && mouse_x <= WINW-PAD && mouse_y >= oy && mouse_y <= oy+DD_H);
            fillrect(PAD, oy, WINW-2*PAD, DD_H, hover ? 55:38, hover ? 65:38, hover ? 85:48);
            outline(PAD, oy, WINW-2*PAD, DD_H, 70,70,88);
            text(PAD+8, oy+(DD_H-FONT_CH)/2, SOURCES[avail[i]].label, 225,225,235);
        }
    }

    SDL_RenderPresent(ren);
}

static void on_click(float mx, float my)
{
    if (navail == 0) return;

    // dropdown open: a click selects an option or closes it
    if (dd_open) {
        float dy = DD_Y + DD_H;
        for (int i = 0; i < navail; i++) {
            float oy = dy + i*DD_H;
            if (mx>=PAD && mx<=WINW-PAD && my>=oy && my<=oy+DD_H) { sel = i; dd_open = 0; return; }
        }
        dd_open = 0;
        return;
    }

    // toggle dropdown
    if (mx>=PAD && mx<=WINW-PAD && my>=DD_Y && my<=DD_Y+DD_H) { dd_open = 1; return; }

    // Extract button
    if (mx>=btn.x && mx<=btn.x+btn.w && my>=btn.y && my<=btn.y+btn.h) {
        if (SDL_GetAtomicInt(&g_status) == ST_RUNNING) return;
        g_job = sel;
        SDL_SetAtomicInt(&g_status, ST_RUNNING);
        SDL_LockMutex(g_lock); snprintf(g_log, sizeof g_log, "Working...\n"); SDL_UnlockMutex(g_lock);
        SDL_Thread* th = SDL_CreateThread(worker, "extract", NULL);
        SDL_DetachThread(th);
    }
}

// Run one source headlessly (no window): extract to `outdir`, print the log.
// Returns 0 on success.  Used by --cli so the tool can also fully replace the
// Python scripts in scripts/CI, and so its output can be diffed in tests.
static int run_one_cli(int si, const char* srcpath, const char* outdir)
{
    const source_t* S = &SOURCES[si];
    char out[600]; snprintf(out, sizeof out, "%s/%s", outdir, S->out);
    SDL_CreateDirectory(outdir);
    char log[4096]; log[0] = 0;
    int ok = 0;
    switch (S->kind) {
        case K_HERETIC:  ok = extract_heretic (srcpath, out, log, sizeof log); break;
        case K_HEXEN:    ok = extract_hexen   (srcpath, out, log, sizeof log); break;
        case K_FREEDOOM: ok = extract_freedoom(srcpath, out, log, sizeof log); break;
    }
    fputs(log, stdout);
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    SDL_SetMainReady();

    // run/ is where the binary lives; game WADs live in run/ID0/.
    const char* bp = SDL_GetBasePath();
    snprintf(g_rundir, sizeof g_rundir, "%s", bp ? bp : "./");
    // strip a trailing slash for clean joins
    size_t rl = strlen(g_rundir);
    while (rl > 1 && (g_rundir[rl-1]=='/' || g_rundir[rl-1]=='\\')) g_rundir[--rl] = 0;
    snprintf(g_id0, sizeof g_id0, "%s/ID0", g_rundir);

    // --- headless batch mode: extractor --cli [outdir] ---------------------
    // Extracts every source IWAD that's present, no window (for scripts/tests).
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cli")) {
            const char* outdir = (i+1 < argc && argv[i+1][0] != '-') ? argv[i+1] : g_id0;
            scan_sources();
            if (navail == 0) { fprintf(stderr, "no source IWADs found in %s or %s\n", g_id0, g_rundir); return 1; }
            int rc = 0;
            for (int a = 0; a < navail; a++) {
                printf("== %s ==\n", SOURCES[avail[a]].src);
                rc |= run_one_cli(avail[a], avail_path[a], outdir);
                putchar('\n');
            }
            return rc;
        }
    }

    scan_sources();

    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr,"SDL_Init: %s\n", SDL_GetError()); return 1; }
    win = SDL_CreateWindow("aiDoom Asset Extractor", WINW, WINH, 0);
    {
        SDL_Surface* icon = SDL_CreateSurfaceFrom(
            AIDOOM_ICON_W, AIDOOM_ICON_H, SDL_PIXELFORMAT_RGBA32,
            (void*)aidoom_icon_rgba, AIDOOM_ICON_W*4);
        if (icon) { SDL_SetWindowIcon(win, icon); SDL_DestroySurface(icon); }
    }
    ren = SDL_CreateRenderer(win, NULL);
    font_init();

    g_lock = SDL_CreateMutex();
    SDL_SetAtomicInt(&g_status, ST_IDLE);

    int run = 1;
    while (run) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 33)) {
            do {
                if (e.type == SDL_EVENT_QUIT) run = 0;
                else if (e.type == SDL_EVENT_MOUSE_MOTION) { mouse_x = e.motion.x; mouse_y = e.motion.y; }
                else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT)
                    on_click(e.button.x, e.button.y);
            } while (SDL_PollEvent(&e));
        }
        draw();
    }

    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
