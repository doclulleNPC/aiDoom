// launcher -- tiny SDL3 launcher for aiDoom.
//
// Pick an IWAD, choose Buddy/Monster mode, hit Launch.  Launches aidoom in the
// run/ folder with the right CLI flags for the picked combination.  Scans
// run/ and ~/.doom for IWADs at startup so the dropdown is always fresh.
//
// Mode mapping (the CLI flags passed to aidoom):
//
//   Buddy:  off          -> no flag
//           buddy        -> -coop
//           ai-buddy     -> -aicoop
//   Monster: vanilla      -> no flag (default DOOM monster AI)
//           l4d          -> -nomonsters -respawn + ... (TBD; see notes below)
//           ai-monsters  -> -aidirector <port>
//   AI layer: (off when Buddy is "off" and Monster is "vanilla")
//             auto-enables the director.exe sidecar with Ollama when needed.
//
// Build: see tools/build_launcher.sh
//
// Notes on "L4D mode":
//   This is a stretch goal -- vanilla DOOM has no Left4Dead squad mechanics.
//   What we approximate is a "horde / swarm" feel by removing the normal
//   monster spawn cap and forcing fast respawn.  Full L4D needs a TC/PWAD.
//   For now we just pass -nomonsters + -respawn which approximates a constant
//   monster presence by respawning monsters very quickly.  A future revision
//   can wire up a real L4D PWAD from run/l4d.wad if it exists.

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifndef _MSC_VER
#include <dirent.h>	// not shipped by MSVC; unused here (we probe known names)
#endif
#include <sys/stat.h>
#ifdef _MSC_VER
#include <io.h>
#include <process.h>
#define access _access
#ifndef F_OK
#define F_OK 0
#endif
#define pclose _pclose
#define popen _popen
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <unistd.h>
#endif

#include "font_atlas.h"
#include "../files/aidoom_icon.h"	// shared 64x64 RGBA window icon

// --- Layout (BASE = 320x200 reference, scaled at runtime) ---
#define WINW 560
#define WINH 504
#define PAD 12

// Vertical bands, BASE pixels (we scale 2x for 320->640 feel on a 560px wide
// window -- comfortable on a 1024-wide desktop).
#define BANNER_H  80
#define BUDDY_Y   110
#define BUDDY_H   60
#define MON_Y     200
#define MON_H     60
#define SKILL_Y   268			// difficulty row (5 pills)
#define SKILL_H   40
#define OPTS_Y    314			// toggle row (no-friendly-fire / infight)
#define CHK_BOX   16			// checkbox square size
#define OPT_NOFF_X (PAD + 90)		// "No friendly fire" checkbox
#define OPT_INF_X  (PAD + 300)		// "Monster infight" checkbox
#define IWAD_Y    346
#define IWAD_H    22
#define IWAD_DD_H 80			// dropdown open height
#define LAUNCH_Y  456
#define LAUNCH_H  34

// Colours (RGB).
#define COL_BG_R       16
#define COL_BG_G       16
#define COL_BG_B       16
#define COL_BANNER_R   48
#define COL_BANNER_G   16
#define COL_BANNER_B   16	// doom red
#define COL_TEXT_R    235
#define COL_TEXT_G    230
#define COL_TEXT_B    220
#define COL_DIM_R     130
#define COL_DIM_G     130
#define COL_DIM_B     130
#define COL_BOX_BG_R   40
#define COL_BOX_BG_G   40
#define COL_BOX_BG_B   48
#define COL_BOX_BD_R   96
#define COL_BOX_BD_G   96
#define COL_BOX_BD_B  110
#define COL_CHECK_R  240
#define COL_CHECK_G  180
#define COL_CHECK_B   40	// amber accent (matches STBAR palette)
#define COL_BTN_BG_R   72
#define COL_BTN_BG_G   24
#define COL_BTN_BG_B   24
#define COL_BTN_BG_H_R 110
#define COL_BTN_BG_H_G  40
#define COL_BTN_BG_H_B  40
#define COL_BTN_BD_R  180
#define COL_BTN_BD_G   60
#define COL_BTN_BD_B   60
#define COL_DD_BG_R    32
#define COL_DD_BG_G    32
#define COL_DD_BG_B    40
#define COL_DD_BD_R    96
#define COL_DD_BD_G    96
#define COL_DD_BD_B   110
#define COL_DD_HOV_R   56
#define COL_DD_HOV_G   56
#define COL_DD_HOV_B   68

// Shorthand: pass-through so callsites can use the (r,g,b) form without
// per-macro comma expansion multiplying arg counts.
#define COL_BG        COL_BG_R,    COL_BG_G,    COL_BG_B
#define COL_BANNER    COL_BANNER_R,COL_BANNER_G,COL_BANNER_B
#define COL_TEXT      COL_TEXT_R,  COL_TEXT_G,  COL_TEXT_B
#define COL_DIM       COL_DIM_R,   COL_DIM_G,   COL_DIM_B
#define COL_BOX_BG    COL_BOX_BG_R,COL_BOX_BG_G,COL_BOX_BG_B
#define COL_BOX_BD    COL_BOX_BD_R,COL_BOX_BD_G,COL_BOX_BD_B
#define COL_CHECK     COL_CHECK_R, COL_CHECK_G, COL_CHECK_B
#define COL_BTN_BG    COL_BTN_BG_R,COL_BTN_BG_G,COL_BTN_BG_B
#define COL_BTN_BG_H  COL_BTN_BG_H_R,COL_BTN_BG_H_G,COL_BTN_BG_H_B
#define COL_BTN_BD    COL_BTN_BD_R,COL_BTN_BD_G,COL_BTN_BD_B
#define COL_DD_BG     COL_DD_BG_R, COL_DD_BG_G, COL_DD_BG_B
#define COL_DD_BD     COL_DD_BD_R, COL_DD_BD_G, COL_DD_BD_B
#define COL_DD_HOV    COL_DD_HOV_R,COL_DD_HOV_G,COL_DD_HOV_B

// --- Mode enums (kept short so the launcher UI is uncluttered) ---
enum {
    BUDDY_OFF = 0, BUDDY_RULE = 1, BUDDY_AI = 2,
};
enum {
    MON_VANILLA = 0, MON_L4D = 1, MON_AI = 2,
};

typedef struct {
    char     name[64];		// e.g. "DOOM2: Hell on Earth" (+ optional " (Steam)")
    char     path[260];		// absolute path
    int      detected;		// 1 if IdentifyVersion-style auto-detected this as a known IWAD
    int      from_steam;		// 1 if found under a Steam install path
} iwad_t;

// --- Globals (single-instance app, no threading) ---
static SDL_Window*   win;
static SDL_Renderer* ren;
static SDL_Texture*  font;

// IWAD list (max 16, more than enough for anyone's Steam lib).
#define MAX_IWADS 16
static iwad_t  iwads[MAX_IWADS];
static int     iwad_count;
static int     iwad_sel;			// index into iwads[]

// Mode selections.
static int     buddy_mode = BUDDY_RULE;	// default: buddy on (rule-based)
static int     mon_mode   = MON_L4D;     // default: L4D pacing
static int     opt_noff;			// -nofriendlyfire: player & buddy can't hurt each other
static int     opt_infight;			// -infight: monster same-species infighting
static int     opt_skill = 3;			// difficulty 0..4 -> -skill 1..5; default 3 = Ultra-Violence
static int     dropdown_open;

// Status line shown under the controls (e.g. a launch error).  err -> red.
static char    g_status[160];
static int     g_status_err;

// Mouse state for hover/clicks.
static int     mouse_x, mouse_y, mouse_down;

// ----- Forward decls -----
static void font_init(void);
static void text(float x, float y, const char* str, Uint8 r, Uint8 g, Uint8 b);
static void rect(float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b);
static void draw_rect_outline(float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b);

static void scan_iwads(void);
static const char* iwad_label(int idx);
static int  is_known_iwad(const char* filename);

static void draw_banner(void);
static void draw_checkbox(float x, float y, int checked, const char* label);
static int  hit_checkbox(float x, float y, const char* label, int mouse_px, int mouse_py);
static void draw_mode_row(float y, const char* title,
                          const char* const* options, int n_opts, int sel,
                          float pill_w, float* hitboxes_out);
static int  pick_mode(float* hitboxes, int n_opts, int mouse_px, int mouse_py);
static void draw_iwad_dropdown(void);
static int  hit_iwad_dropdown(int mouse_px, int mouse_py);
static void draw_launch_button(void);
static int  hit_launch_button(int mouse_px, int mouse_py);

static void do_launch(void);
static void build_command(char* out, int n, const char* iwad_path);
static const char* run_dir(void);

// ----------------------------------------------------------------- text
static void font_init(void)
{
    Uint32* px = malloc(FONT_AW*FONT_CH*4);
    for (int i=0; i<FONT_AW*FONT_CH; i++)
        px[i] = 0x00FFFFFFu | ((Uint32)font_alpha[i] << 24);
    SDL_Surface* s = SDL_CreateSurfaceFrom(FONT_AW, FONT_CH, SDL_PIXELFORMAT_ARGB8888, px, FONT_AW*4);
    font = SDL_CreateTextureFromSurface(ren, s);
    SDL_SetTextureBlendMode(font, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(font, SDL_SCALEMODE_NEAREST);
    SDL_SetTextureScaleMode(font, SDL_SCALEMODE_LINEAR);    // smoother scaling
    SDL_DestroySurface(s); free(px);
}

static void text(float x, float y, const char* str, Uint8 r, Uint8 g, Uint8 b)
{
    if (!str || !*str) return;
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

static void text_scaled(float x, float y, float scale, const char* str,
                       Uint8 r, Uint8 g, Uint8 b)
{
    if (!str || !*str) return;
    SDL_SetTextureColorMod(font, r, g, b);
    for (const char* p=str; *p; p++) {
        int c = (unsigned char)*p;
        if (c < FONT_FIRST || c >= FONT_FIRST+FONT_COUNT) c = '?';
        SDL_FRect src = { (float)((c-FONT_FIRST)*FONT_CW), 0, FONT_CW, FONT_CH };
        SDL_FRect dst = { x, y, FONT_CW*scale, FONT_CH*scale };
        SDL_RenderTexture(ren, font, &src, &dst);
        x += FONT_CW*scale;
    }
}

static void rect(float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_FRect q = {x,y,w,h};
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderFillRect(ren, &q);
}

static void draw_rect_outline(float x, float y, float w, float h, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_FRect q = {x,y,w,h};
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderRect(ren, &q);
}

// ----------------------------------------------------------------- iwad scan
//
// Known IWAD names from DOOM's IdentifyVersion (d_main.c).  We use the
// filename as the canonical match -- aidoom's autodetect walks DOOMWADDIR,
// the cwd, ~/.doom, ~/wads, Steam.  We pre-discover files in run/ so the
// user can pick without typing a path.
//
typedef struct {
    const char* filename;  // canonical lowercase basename (what IdentifyVersion matches)
    const char* label;     // human label shown in the dropdown
} known_iwad_t;

static const known_iwad_t KNOWN_IWADS[] = {
    { "doom1.wad",     "DOOM1: Shareware"                  },
    { "doom.wad",      "DOOM1: Registered / Ultimate"      },
    { "doomu.wad",     "DOOM1: Ultimate (doomu.wad)"       },
    { "doom2.wad",     "DOOM2: Hell on Earth"              },
    { "doom2f.wad",    "DOOM2: French (doom2f.wad)"        },
    { "plutonia.wad",  "Final DOOM: Plutonia Experiment"   },
    { "tnt.wad",       "Final DOOM: TNT Evilution"         },
    { "freedoom1.wad", "FreeDOOM1"                         },
    { "freedoom2.wad", "FreeDOOM2"                         },
    { "freedm.wad",    "FreeDM"                            },
    { "chex3.wad",     "Chex Quest 3"                      },
    { NULL, NULL }
};

static int is_known_iwad(const char* filename)
{
    for (int i=0; KNOWN_IWADS[i].filename; i++)
        if (strcasecmp(filename, KNOWN_IWADS[i].filename) == 0)
            return i;
    return -1;
}

// Two paths refer to the same on-disk file?  On case-insensitive filesystems
// (Windows, macOS) "run/doom1.wad" and "run/DOOM1.WAD" are the SAME file, and /
// and \ are interchangeable -- compare accordingly so the same file probed under
// different casing isn't listed twice.  On Linux the FS is case-sensitive, so a
// plain strcmp is correct (distinct-case files really are distinct).
static int same_path(const char* a, const char* b)
{
#if defined(_WIN32) || defined(__APPLE__)
    for (; *a && *b; a++, b++) {
        char ca = (*a == '\\') ? '/' : (char)tolower((unsigned char)*a);
        char cb = (*b == '\\') ? '/' : (char)tolower((unsigned char)*b);
        if (ca != cb) return 0;
    }
    return *a == *b;
#else
    return strcmp(a, b) == 0;
#endif
}

static void try_add_iwad(const char* dir, const char* filename, int from_steam)
{
    if (iwad_count >= MAX_IWADS) return;
    char full[520];
    snprintf(full, sizeof full, "%s%s%s", dir,
             (dir[strlen(dir)-1] == '/' || dir[strlen(dir)-1] == '\\') ? "" : "/",
             filename);

    struct stat st;
    if (stat(full, &st) != 0) return;
    if (!(st.st_mode & S_IFREG)) return;       // regular file only

    // De-dupe by file identity (case-/slash-insensitive on Windows/macOS).
    // Different copies of the same IWAD in DIFFERENT locations (run/doom2.wad
    // vs ~/.steam/.../DOOM2.WAD) are intentionally kept -- the user may want to
    // know which one aidoom will pick if DOOMWADDIR isn't set, and Steam copies
    // get tagged "(Steam)" so they're easy to spot.  The same file probed under
    // different casing (doom1.wad / DOOM1.WAD) collapses to one entry.
    for (int i=0; i<iwad_count; i++)
        if (same_path(iwads[i].path, full)) return;

    iwad_t* e = &iwads[iwad_count];
    int known = is_known_iwad(filename);
    if (known >= 0) {
        snprintf(e->name, sizeof e->name, "%s%s",
                 KNOWN_IWADS[known].label,
                 from_steam ? " (Steam)" : "");
        e->detected = 1;
    } else {
        snprintf(e->name, sizeof e->name, "%s%s", filename,
                 from_steam ? " (Steam)" : "");
        e->detected = 0;
    }
    snprintf(e->path, sizeof e->path, "%s", full);
    e->from_steam = from_steam;
    iwad_count++;
}

// scan_dirs lists directory paths to search.  We always look in run/
// (next to the binary), plus a few conventional locations.
//
static void scan_iwads(void)
{
    iwad_count = 0;

    // Build list of search dirs (in priority order).  Keep it small and
    // predictable -- we only want REAL IWADs, never stray PWADs/INIs/etc.
    const char* dirs[16]; int nd = 0;
    dirs[nd++] = run_dir();

    const char* home = getenv("HOME");
    char home_d[260];
    if (home) {
        snprintf(home_d, sizeof home_d, "%s/.doom", home);
        dirs[nd++] = home_d;
    }
    char home_w[260];
    if (home) {
        snprintf(home_w, sizeof home_w, "%s/wads", home);
        dirs[nd++] = home_w;
    }

    // Steam roots -- the default Steam library "common" folder, per OS.
    // Mirrors d_main.c:IdentifyVersion so the launcher and engine agree on the
    // same canonical install paths.  We only probe the DEFAULT library; extra
    // library folders on other drives (libraryfolders.vdf) are not parsed.
    char steam_roots[8][512]; int nsteam = 0;
#if defined(__APPLE__)
    if (home)
        snprintf(steam_roots[nsteam++], 512, "%s/Library/Application Support/Steam/steamapps/common", home);
#elif defined(_WIN32)
    snprintf(steam_roots[nsteam++], 512, "C:/Program Files (x86)/Steam/steamapps/common");
    snprintf(steam_roots[nsteam++], 512, "C:/Program Files/Steam/steamapps/common");
#else   /* Linux / *BSD */
    if (home) {
        snprintf(steam_roots[nsteam++], 512, "%s/.steam/steam/steamapps/common", home);
        snprintf(steam_roots[nsteam++], 512, "%s/.local/share/Steam/steamapps/common", home);
        snprintf(steam_roots[nsteam++], 512, "%s/.steam/root/steamapps/common", home);
        // Flatpak Steam (com.valvesoftware.Steam) sandboxed data dir.
        snprintf(steam_roots[nsteam++], 512, "%s/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common", home);
    }
#endif

    // Steam sub-paths where each IWAD actually lives (relative to root).
    // Kept identical to steam_iwads[] in d_main.c -- if you add one there,
    // add it here.
    static const struct { const char* subdir; const char* iwad; } steam_subs[] = {
        { "Ultimate Doom/base",          "DOOM.WAD"   },
        { "Ultimate Doom/rerelease",     "DOOM.WAD"   },
        { "Ultimate Doom/base",          "doom.wad"   },
        { "DOOM 2/base",                 "DOOM2.WAD"  },
        { "Doom 2/base",                 "DOOM2.WAD"  },
        { "Doom 2/finaldoombase",        "TNT.WAD"    },
        { "Doom 2/finaldoombase",        "PLUTONIA.WAD" },
        { "Final Doom/base",             "TNT.WAD"    },
        { "Final Doom/base",             "PLUTONIA.WAD" },
        { NULL, NULL }
    };

    // 1) Plain search dirs (run/, ~/.doom, ~/wads).
    for (int d = 0; d < nd; d++) {
        for (int i = 0; KNOWN_IWADS[i].filename; i++) {
            try_add_iwad(dirs[d], KNOWN_IWADS[i].filename, 0);
            // Also try the uppercase variant -- aidoom's run/ and Steam
            // installs both ship DOOM2.WAD / DOOM.WAD etc.
            char upper[64];
            snprintf(upper, sizeof upper, "%s", KNOWN_IWADS[i].filename);
            for (char* p = upper; *p; p++)
                if (*p >= 'a' && *p <= 'z') *p -= 32;
            if (strcmp(upper, KNOWN_IWADS[i].filename) != 0)
                try_add_iwad(dirs[d], upper, 0);
        }
    }

    // 2) Steam install paths.  For each Steam root x each (subdir, basename)
    // pair, ask try_add_iwad to stat it.  Steam hits are flagged so the
    // dropdown can render them with a "(Steam)" suffix -- useful when both
    // run/ and Steam have a copy and the user wants to know which one
    // DOOMWADDIR would resolve to.
    for (int r = 0; r < nsteam; r++) {
        for (int s = 0; steam_subs[s].subdir; s++) {
            char full[1024];
            snprintf(full, sizeof full, "%s/%s", steam_roots[r], steam_subs[s].subdir);
            try_add_iwad(full, steam_subs[s].iwad, 1);
        }
    }

    if (iwad_count == 0) {
        // No IWAD found -- synthesise a placeholder so the user sees something.
        iwad_t* e = &iwads[0];
        snprintf(e->name, sizeof e->name, "(no IWAD found)");
        snprintf(e->path, sizeof e->path, "%s/doom2.wad", run_dir());
        e->detected = 0;
        e->from_steam = 0;
        iwad_count = 1;
    }

    // Default selection: first detected (known) IWAD, or first entry.
    iwad_sel = 0;
    for (int i=0; i<iwad_count; i++)
        if (iwads[i].detected) { iwad_sel = i; break; }
}

// ----------------------------------------------------------------- banner
//
// "DOOM" banner drawn as text scaled 4x, baseline-aligned to the bottom of
// the banner area, with a thin "horns"-style bracket underneath (ASCII art
// suggestion of the DOOM logo's silhouette).  We don't try to be pixel-true
// to the original DOOM logo (that's a trademarked asset); this is a
// type-set mark that reads as "DOOM".
//
static void draw_banner(void)
{
    // Background bar
    rect(0, 0, WINW, BANNER_H, COL_BANNER);

    // Inset rule
    draw_rect_outline(2, 2, WINW-4, BANNER_H-4, 160, 32, 32);

    // Title
    text_scaled(PAD, BANNER_H/2 - FONT_CH*2,
               4.0f, "DOOM",
               255, 230, 100);    // yellow title

    // Sub-line
    text(PAD + FONT_CW*4*5 + 12, BANNER_H/2 - FONT_CH/2,
         "aiDoom launcher", 200, 200, 200);

    // Right-aligned hint
    const char* hint = "ESC quits";
    int hint_w = (int)strlen(hint) * FONT_CW;
    text(WINW - hint_w - PAD, BANNER_H - FONT_CH - 4,
         hint, 180, 180, 180);
}

// ----------------------------------------------------------------- mode rows
//
// One row per mode (Buddy, Monster).  Each row has a label on the left and
// three radio-style "pill" options on the right.  We track hit-boxes
// per-option in an array so the click handler can map back to an index.
//
static void draw_mode_row(float y, const char* title,
                          const char* const* options, int n_opts, int sel,
                          float pill_w, float* hitboxes_out)
{
    // Row label
    text(PAD, y + (BUDDY_H - FONT_CH)/2, title, 220, 220, 220);

    // n_opts pills, right side
    const float pill_h = 26;
    const float gap   = 8;
    float rx = WINW - PAD - pill_w * n_opts - gap * (n_opts - 1);

    for (int i=0; i<n_opts; i++) {
        float px = rx + i * (pill_w + gap);
        float py = y + (BUDDY_H - pill_h)/2;

        // Background (selected = highlighted)
        if (i == sel) {
            rect(px, py, pill_w, pill_h, COL_BTN_BG_H);
            draw_rect_outline(px, py, pill_w, pill_h,
                         COL_CHECK_R, COL_CHECK_G, COL_CHECK_B);
        } else {
            rect(px, py, pill_w, pill_h, COL_BOX_BG);
            draw_rect_outline(px, py, pill_w, pill_h, COL_BOX_BD_R, COL_BOX_BD_G, COL_BOX_BD_B);
        }

        // Label centred
        const char* lbl = options[i];
        int lw = (int)strlen(lbl) * FONT_CW;
        text(px + (pill_w - lw)/2, py + (pill_h - FONT_CH)/2,
             lbl,
             i == sel ? 255 : COL_DIM);

        // Cache hitbox for click handling
        if (hitboxes_out) hitboxes_out[i*4 + 0] = px;
        if (hitboxes_out) hitboxes_out[i*4 + 1] = py;
        if (hitboxes_out) hitboxes_out[i*4 + 2] = px + pill_w;
        if (hitboxes_out) hitboxes_out[i*4 + 3] = py + pill_h;
    }
}

static int pick_mode(float* hitboxes, int n_opts, int mouse_px, int mouse_py)
{
    for (int i=0; i<n_opts; i++) {
        float x0 = hitboxes[i*4+0], y0 = hitboxes[i*4+1];
        float x1 = hitboxes[i*4+2], y1 = hitboxes[i*4+3];
        if (mouse_px >= x0 && mouse_px <= x1 &&
            mouse_py >= y0 && mouse_py <= y1)
            return i;
    }
    return -1;
}

// ----------------------------------------------------------------- iwad dropdown
static void draw_iwad_dropdown(void)
{
    // Header row: "IWAD: <name>" with dropdown chevron
    float x = PAD;
    float y = IWAD_Y;
    float w = WINW - 2*PAD;
    float h = IWAD_H;

    rect(x, y, w, h, COL_DD_BG);
    draw_rect_outline(x, y, w, h, COL_DD_BD_R, COL_DD_BD_G, COL_DD_BD_B);

    // "IWAD:" label (left), selected name shifted right of it
    text(x + 8, y + (h - FONT_CH)/2, "IWAD:", COL_DIM);
    if (iwad_count > 0)
        text(x + 8 + 5*FONT_CW + 6, y + (h - FONT_CH)/2,
             iwads[iwad_sel].name,
             iwads[iwad_sel].detected ? 255 : 200,
             iwads[iwad_sel].detected ? 230 : 200,
             100);

    // Dropdown chevron (right side)
    int n = iwad_count;
    const char* arrow = dropdown_open ? "^" : "v";
    text(x + w - FONT_CW - 6, y + (h - FONT_CH)/2, arrow, COL_DIM);

    // If open: draw options below
    if (dropdown_open && n > 0) {
        float dy = y + h;
        int show = n < 6 ? n : 6;
        for (int i=0; i<show; i++) {
            float oy = dy + i * IWAD_H;
            int hover = (mouse_x >= x && mouse_x <= x+w &&
                         mouse_y >= oy && mouse_y <= oy + IWAD_H);
            rect(x, oy, w, IWAD_H,
                 hover ? COL_DD_HOV : COL_DD_BG);
            draw_rect_outline(x, oy, w, IWAD_H,
                         COL_DD_BD_R, COL_DD_BD_G, COL_DD_BD_B);

            text(x + 8, oy + (IWAD_H - FONT_CH)/2,
                 iwads[i].name,
                 iwads[i].detected ? 255 : 200,
                 iwads[i].detected ? 230 : 200,
                 iwads[i].detected ? 100 : 180);
        }
    }
}

static int hit_iwad_dropdown(int mouse_px, int mouse_py)
{
    // Header bar
    if (mouse_px >= PAD && mouse_px <= WINW-PAD &&
        mouse_py >= IWAD_Y && mouse_py <= IWAD_Y + IWAD_H)
        return 0;        // toggles the dropdown

    if (!dropdown_open) return -1;

    // Open list
    int show = iwad_count < 6 ? iwad_count : 6;
    for (int i=0; i<show; i++) {
        float oy = IWAD_Y + IWAD_H + i * IWAD_H;
        if (mouse_px >= PAD && mouse_px <= WINW-PAD &&
            mouse_py >= oy && mouse_py <= oy + IWAD_H)
            return i + 1;   // 1-based option index
    }
    return -1;
}

// ----------------------------------------------------------------- checkboxes
//
// A small amber-accented toggle: empty box + label when off, filled box +
// bright label when on.  The whole box+label rectangle is the click target.
//
static void draw_checkbox(float x, float y, int checked, const char* label)
{
    rect(x, y, CHK_BOX, CHK_BOX, COL_BOX_BG);
    if (checked) {
        draw_rect_outline(x, y, CHK_BOX, CHK_BOX, COL_CHECK_R, COL_CHECK_G, COL_CHECK_B);
        rect(x+4, y+4, CHK_BOX-8, CHK_BOX-8, COL_CHECK_R, COL_CHECK_G, COL_CHECK_B);
    } else {
        draw_rect_outline(x, y, CHK_BOX, CHK_BOX, COL_BOX_BD_R, COL_BOX_BD_G, COL_BOX_BD_B);
    }
    text(x + CHK_BOX + 6, y + (CHK_BOX - FONT_CH)/2, label,
         checked ? COL_TEXT_R : COL_DIM_R,
         checked ? COL_TEXT_G : COL_DIM_G,
         checked ? COL_TEXT_B : COL_DIM_B);
}

static int hit_checkbox(float x, float y, const char* label, int mouse_px, int mouse_py)
{
    float w = CHK_BOX + 6 + (float)strlen(label) * FONT_CW;
    return mouse_px >= x && mouse_px <= x + w &&
           mouse_py >= y && mouse_py <= y + CHK_BOX;
}

// ----------------------------------------------------------------- launch button
static void draw_launch_button(void)
{
    float bw = 200, bh = 30;
    float bx = (WINW - bw)/2;
    float by = LAUNCH_Y;
    int hover = (mouse_x >= bx && mouse_x <= bx+bw &&
                 mouse_y >= by && mouse_y <= by+bh);
    rect(bx, by, bw, bh, hover ? COL_BTN_BG_H : COL_BTN_BG);
    draw_rect_outline(bx, by, bw, bh, COL_BTN_BD_R, COL_BTN_BD_G, COL_BTN_BD_B);
    const char* lbl = "LAUNCH";
    int lw = (int)strlen(lbl) * FONT_CW;
    text(bx + (bw - lw)/2, by + (bh - FONT_CH)/2, lbl, 255, 220, 100);
}

static int hit_launch_button(int mouse_px, int mouse_py)
{
    float bw = 200, bh = 30;
    float bx = (WINW - bw)/2;
    float by = LAUNCH_Y;
    return mouse_px >= bx && mouse_px <= bx+bw &&
           mouse_py >= by && mouse_py <= by+bh;
}

// ----------------------------------------------------------------- command builder
//
// We build the aidoom command line in `out`.  Flags are appended in the order
// the user would type them: -iwad first, then buddy mode, then monster mode.
// We also append -warp 1 1 so the user lands in a level rather than the
// title-screen demo (which has no buddy to see).
//
static void build_command(char* out, int n, const char* iwad_path)
{
    int off = 0;
    off += snprintf(out + off, n - off, "\"%s/aidoom\" -iwad \"%s\"",
                    run_dir(), iwad_path);

    // Buddy mode
    switch (buddy_mode) {
        case BUDDY_OFF:  break;
        case BUDDY_RULE: off += snprintf(out + off, n - off, " -coop"); break;
        case BUDDY_AI:   off += snprintf(out + off, n - off, " -aicoop"); break;
    }

    // Monster mode
    switch (mon_mode) {
        case MON_VANILLA: break;
        case MON_L4D:
            // Approximate L4D via aggressive respawn + no early exit.
            off += snprintf(out + off, n - off, " -respawn -fast");
            break;
        case MON_AI:
            off += snprintf(out + off, n - off, " -aidirector 31666");
            break;
    }

    // Toggles
    if (opt_noff)    off += snprintf(out + off, n - off, " -nofriendlyfire");
    if (opt_infight) off += snprintf(out + off, n - off, " -infight");

    // Always land in a level, never the title screen.  Skill 1..5 from the row.
    off += snprintf(out + off, n - off, " -warp 1 1 -skill %d", opt_skill + 1);

    (void)n;
}

// ----------------------------------------------------------------- launch
//
// We launch the game (and, for any AI layer, the director sidecar) DETACHED and
// return immediately, so the launcher's event loop keeps running -- otherwise
// system() blocks until the game exits and the launcher window can't be closed
// or reused.  On Linux the game's stderr is teed to run/aidoom_stderr.log so a
// fatal I_Error is recoverable after the window vanishes.
//
// Whenever ANY AI layer is picked -- AI monsters (-aidirector) OR the AI buddy
// (-aicoop) -- we ALSO launch the director sidecar so the LLM has someone to
// talk to.  Both make the game listen on port 31666 (the game's default
// ai_port == the director's default --port), so one director drives whichever
// is active.  Best-effort: if the director binary isn't in run/, we warn and
// launch aidoom alone.
//
static void do_launch(void)
{
    if (iwad_count == 0) return;

    char cmd[1024];
    build_command(cmd, sizeof cmd, iwads[iwad_sel].path);

    // The director drives the AI buddy (-aicoop) and/or the AI monsters
    // (-aidirector); start it for either.
    int needs_director = (mon_mode == MON_AI) || (buddy_mode == BUDDY_AI);
    char dir_path[260];
#ifdef _WIN32
    snprintf(dir_path, sizeof dir_path, "%s/director.exe", run_dir());
#else   /* Linux and macOS both ship the binary as plain "director" */
    snprintf(dir_path, sizeof dir_path, "%s/director", run_dir());
#endif
    int has_director = (access(dir_path, F_OK) == 0);

    // AI mode was picked but the director sidecar is missing: refuse to launch
    // (an AI mode without the director would silently fall back to vanilla
    // tactics, which is not what the user asked for).  Show why and bail.
    if (needs_director && !has_director)
    {
#ifdef _WIN32
        const char* exe = "director.exe";
#else
        const char* exe = "director";
#endif
        snprintf(g_status, sizeof g_status,
                 "%s not found in run/ -- AI mode needs it (build it, or pick a non-AI mode)",
                 exe);
        g_status_err = 1;
        fprintf(stderr, "[launcher] %s\n", g_status);
        return;   // do NOT start aidoom
    }

    // Build a DETACHED launch string so system() returns at once (non-blocking).
    char full[1400];
#ifdef _WIN32
    // cmd.exe: `start` detaches each process into its own session.  /B = no new
    // console for the director; the game gets its own window.
    if (needs_director && has_director)
        snprintf(full, sizeof full,
                 "start \"aiDoom Director\" /B \"%s\" --port 31666 & start \"aiDoom\" %s",
                 dir_path, cmd);
    else
        snprintf(full, sizeof full, "start \"aiDoom\" %s", cmd);
#else
    // POSIX: background both in subshells; tee the game's stderr to a log so a
    // fatal I_Error survives the window vanishing.
    if (needs_director && has_director)
        snprintf(full, sizeof full,
                 "( \"%s\" --port 31666 >/dev/null 2>&1 & ) ; ( %s >aidoom_stderr.log 2>&1 & )",
                 dir_path, cmd);
    else
        snprintf(full, sizeof full, "( %s >aidoom_stderr.log 2>&1 & )", cmd);
#endif

    fprintf(stderr, "[launcher] %s\n", full);
    system(full);   // returns immediately -- the launcher stays responsive
    snprintf(g_status, sizeof g_status, "launched aiDoom%s",
             needs_director ? " + director" : "");
    g_status_err = 0;

    // Log the command so users can see what was launched (exit code is N/A now
    // that we don't wait for it).
    FILE* f = fopen("launcher.log", "a");
    if (f) {
        fprintf(f, "launched: %s\n", full);
        fclose(f);
    }
}

// ----------------------------------------------------------------- paths
static const char* run_dir(void)
{
    // The launcher binary lives in run/ next to aidoom.exe.  argv[0] is the
    // path the OS used to invoke us; resolve its directory and return it.
    static char path[260];
    static int inited = 0;
    if (!inited) {
        // Best-effort: use argv[0] if available, else assume CWD/run.
        // We can't easily get argv from SDL, so we approximate: look at the
        // executable's location.  In practice, the launcher is launched from
        // run/ or via a symlink, and the actual binary lives next to aidoom.
        //
        // Simplest fallback: CWD if it contains aidoom/aidoom.exe, else the
        // directory the binary lives in.
        const char* cwd = SDL_GetCurrentDirectory();
        if (cwd && (strstr(cwd, "aidoom") || strstr(cwd, "run"))) {
            snprintf(path, sizeof path, "%s", cwd);
        } else {
            snprintf(path, sizeof path, "%s/run",
                     SDL_GetBasePath() ? SDL_GetBasePath() : ".");
        }
        inited = 1;
    }
    return path;
}

// ----------------------------------------------------------------- main
int main(int argc, char** argv)
{
    (void)argc; (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    win = SDL_CreateWindow("aiDoom Launcher", WINW, WINH,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Window icon from the shared aidoom.ico atlas.
    SDL_Surface* icon = SDL_CreateSurfaceFrom(64, 64, SDL_PIXELFORMAT_RGBA32,
                                              (void*)aidoom_icon_rgba, 64*4);
    if (icon) {
        SDL_SetWindowIcon(win, icon);
        SDL_DestroySurface(icon);
    }

    ren = SDL_CreateRenderer(win, NULL);
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(win); SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);

    font_init();
    scan_iwads();

    int running = 1;
    SDL_Event ev;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE) {
                    if (dropdown_open) dropdown_open = 0;
                    else running = 0;
                }
                if (ev.key.key == SDLK_RETURN && !dropdown_open) {
                    do_launch();
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                mouse_x = (int)ev.motion.x;
                mouse_y = (int)ev.motion.y;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    mouse_down = 1;
                    int hh = hit_iwad_dropdown(mouse_x, mouse_y);
                    if (hh == 0) {
                        dropdown_open = !dropdown_open;
                    } else if (hh > 0) {
                        iwad_sel = hh - 1;
                        dropdown_open = 0;
                    } else if (hit_launch_button(mouse_x, mouse_y)) {
                        do_launch();
                    } else {
                        // Buddy mode row
                        static float buddy_hit[12];  // 3 options * 4 coords
                        static int  buddy_hit_done;
                        (void)buddy_hit_done;
                        // We have to redraw to compute hitboxes -- simpler:
                        // recompute on click by calling draw_mode_row into a
                        // scratch buffer.  For ~3 options, just hardcode the
                        // math here matching draw_mode_row.
                        const float pill_w = 110, gap = 8;
                        float bx = WINW - PAD - pill_w*3 - gap*2;
                        float by = BUDDY_Y + (BUDDY_H - 26)/2;
                        if (mouse_y >= by && mouse_y <= by + 26) {
                            for (int i=0; i<3; i++) {
                                float px = bx + i * (pill_w + gap);
                                if (mouse_x >= px && mouse_x <= px + pill_w) {
                                    buddy_mode = i;
                                    g_status[0] = 0;   // clear stale launch error
                                }
                            }
                        }

                        // Monster mode row
                        float my = MON_Y + (MON_H - 26)/2;
                        if (mouse_y >= my && mouse_y <= my + 26) {
                            for (int i=0; i<3; i++) {
                                float px = bx + i * (pill_w + gap);
                                if (mouse_x >= px && mouse_x <= px + pill_w) {
                                    mon_mode = i;
                                    g_status[0] = 0;   // clear stale launch error
                                }
                            }
                        }

                        // Skill row (5 narrower pills)
                        const float spill_w = 64;
                        float sx = WINW - PAD - spill_w*5 - gap*4;
                        float sy = SKILL_Y + (BUDDY_H - 26)/2;	// rows centre on BUDDY_H
                        if (mouse_y >= sy && mouse_y <= sy + 26) {
                            for (int i=0; i<5; i++) {
                                float px = sx + i * (spill_w + gap);
                                if (mouse_x >= px && mouse_x <= px + spill_w) {
                                    opt_skill = i;
                                    g_status[0] = 0;
                                }
                            }
                        }

                        // Options row: toggle the two checkboxes.
                        if (hit_checkbox(OPT_NOFF_X, OPTS_Y, "No friendly fire", mouse_x, mouse_y))
                            opt_noff = !opt_noff;
                        if (hit_checkbox(OPT_INF_X, OPTS_Y, "Monster infight", mouse_x, mouse_y))
                            opt_infight = !opt_infight;
                    }
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                mouse_down = 0;
                break;
            case SDL_EVENT_WINDOW_RESIZED: {
                SDL_RenderViewportSet(ren);
            } break;
            default: break;
            }
        }

        // ---- Render ----
        SDL_SetRenderDrawColor(ren, COL_BG, 255);
        SDL_RenderClear(ren);

        draw_banner();

        // Buddy row
        {
            static const char* buddy_opts[] = { "Off", "Buddy", "AI Buddy" };
            draw_mode_row(BUDDY_Y, "Buddy",
                          buddy_opts, 3, buddy_mode, 110, NULL);
        }
        // Monster row
        {
            static const char* mon_opts[] = { "Vanilla", "L4D", "AI Director" };
            draw_mode_row(MON_Y, "Monster",
                          mon_opts, 3, mon_mode, 110, NULL);
        }
        // Skill row (5 pills; default Ultra-Violence)
        {
            static const char* skill_opts[] = { "ITYTD", "HNTR", "HMP", "UV", "NM" };
            draw_mode_row(SKILL_Y, "Skill",
                          skill_opts, 5, opt_skill, 64, NULL);
        }
        // Options row (toggles)
        {
            text(PAD, OPTS_Y + (CHK_BOX - FONT_CH)/2, "Options", COL_DIM);
            draw_checkbox(OPT_NOFF_X, OPTS_Y, opt_noff,    "No friendly fire");
            draw_checkbox(OPT_INF_X,  OPTS_Y, opt_infight, "Monster infight");
        }

        draw_iwad_dropdown();
        draw_launch_button();

        // Status line (launch error / confirmation) just above the button.
        if (g_status[0]) {
            if (g_status_err) text(PAD, LAUNCH_Y - 16, g_status, COL_BTN_BD);
            else              text(PAD, LAUNCH_Y - 16, g_status, COL_CHECK);
        }

        SDL_RenderPresent(ren);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}