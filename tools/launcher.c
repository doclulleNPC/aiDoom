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
#include <windows.h>	// CreateProcess -- launch the game non-blocking + windowless
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
#include "hero_launcher_img.h"		// 560x100 RGBA hero banner (replaces the text banner)
#include "md5.h"			// SIGIL PWAD checksum verification

// --- Layout (BASE = 320x200 reference, scaled at runtime) ---
#define WINW 560
#define WINH 594			// +32 for the second Options row, +50 for the lowered LAUNCH button
#define PAD 12

// Vertical bands, BASE pixels (we scale 2x for 320->640 feel on a 560px wide
// window -- comfortable on a 1024-wide desktop).
#define BANNER_H  100
// The four AI rows are evenly spaced by their VISUAL centres (Δ64): mode rows centre
// their content at Y+BUDDY_H/2 (=+30), the Options checkboxes at Y+CHK_BOX/2 (=+8).
// Buddy/Monster/Skill are tightly grouped (centres 120/152/184, Δ32); Options sits
// a row-gap below (centre 248) as a visual separator from the toggle checkboxes.
#define BUDDY_Y   110
#define BUDDY_H   60
#define MON_Y     142
#define MON_H     60
#define SKILL_Y   174			// difficulty row (5 pills)
#define SKILL_H   40
#define MAP_Y     206			// warp-map row (4 pills: 1/10/19/28), under Skill
#define OPTS_Y    260			// toggle row 1; centre 248 (Skill-Options gap kept as a separator)
#define CHK_BOX   16			// checkbox square size
#define OPT_NOFF_X (PAD + 90)		// "No friendly fire" checkbox
#define OPT_INF_X  (PAD + 300)		// "All Monster infight" checkbox
#define OPTS2_Y   292			// toggle row 2 (Autoaim / Monster over/under), Δ32 below row 1
#define OPT_AA_X  (PAD + 90)		// "Autoaim" checkbox (aligned under "No friendly fire")
#define OPT_OU_X  (PAD + 300)		// "Monster over/under" checkbox (under "All Monster infight")
#define MONSTERS_Y 330			// extra-monster WAD toggles (FreeDoom / Heretic) -- shifted +32 for row 2
#define MON_FD_X   (PAD + 100)		// "FreeDoom" checkbox (space after the "+Monster" label)
#define MON_HER_X  (PAD + 250)		// "Heretic" checkbox
#define MON_HEX_X  (PAD + 410)		// "Hexen" checkbox
#define IWAD_Y    362
#define IWAD_H    22
#define IWAD_DD_H 80			// dropdown open height
#define PWAD_Y    390			// extra PWAD selector (one row below IWAD)
#define LAUNCH_Y  544			// lowered 50px (window grew 50px to match)
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
#define COL_GRAY_R    72		// disabled control (dimmer than COL_DIM)
#define COL_GRAY_G    72
#define COL_GRAY_B    78
#define COL_GRAY      COL_GRAY_R,  COL_GRAY_G,  COL_GRAY_B
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
static SDL_Texture*  hero_tex;		// embedded hero banner image

// IWAD list (max 16, more than enough for anyone's Steam lib).
#define MAX_IWADS 16
static iwad_t  iwads[MAX_IWADS];
static int     iwad_count;
static int     iwad_sel;			// index into iwads[]

// PWAD list: every other .wad in the wad dirs (NOT a known IWAD, NOT aidoom.wad),
// offered as an optional extra "-file" load.  pwads[0] is always "(none)".
#define MAX_PWADS 48
static char    pwads[MAX_PWADS][64];
static int     pwad_count;
static int     pwad_sel;			// 0 = none, else index into pwads[]
static int     pwad_dd_open;			// PWAD dropdown open (mutually exclusive w/ IWAD)
static int     pwad_scroll;			// first visible row when the open list overflows

// Mode selections.
static int     buddy_mode = BUDDY_RULE;	// default: buddy on (rule-based)
static int     mon_mode   = MON_L4D;     // default: L4D pacing
static int     opt_noff;			// -nofriendlyfire: player & buddy can't hurt each other
static int     opt_infight;			// -infight: monster same-species infighting
static int     opt_autoaim    = 0;		// -autoaim: vertical aim-assist (default OFF). ON also forces infinitely-tall (over/under OFF).
static int     opt_overunder  = 1;		// over/under 3D clipping (default ON). OFF -> -infinitetall.
static int     opt_freedoom;			// -file freedoomstuff.wad (DOOM2 monsters, free art)
static int     opt_heretic;			// -file hereticstuff.wad   (Heretic monsters)
static int     opt_hexen;			// -file hexenstuff.wad     (Hexen monsters)
static int     opt_skill = 3;			// difficulty 0..4 -> -skill 1..5; default 3 = Ultra-Violence
static int     map_idx = 0;			// warp-map row index -> map_vals[]; default 0 = map 1
static const int map_vals[4] = { 1, 10, 19, 28 };	// DOOM2: MAPnn; DOOM1/Heretic: episode starts E1M1/E2M1/E3M1/E4M1
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
static void scan_pwads(void);
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
static void draw_pwad_dropdown(void);
static int  hit_pwad_dropdown(int mouse_px, int mouse_py);
static void draw_launch_button(void);
static int  hit_launch_button(int mouse_px, int mouse_py);

static void do_launch(void);
static void save_launcher_prefs(void);
static void load_launcher_prefs(void);
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

// Embedded hero banner -> texture (mirrors font_init / the window icon).
static void hero_init(void)
{
    SDL_Surface* s = SDL_CreateSurfaceFrom(HERO_W, HERO_H, SDL_PIXELFORMAT_RGBA32,
                                           (void*)hero_launcher_rgba, HERO_W*4);
    if (s) {
        hero_tex = SDL_CreateTextureFromSurface(ren, s);
        if (hero_tex) SDL_SetTextureScaleMode(hero_tex, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(s);
    }
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

    // De-dupe by FILENAME too: the same IWAD (e.g. doom.wad) sitting in more than one wad
    // dir (run/ and run/ID0) should appear ONCE -- keep the first (highest-priority) copy,
    // not list "DOOM1: Registered / Ultimate" twice.
    for (int i=0; i<iwad_count; i++) {
        const char* p = iwads[i].path;
        const char* fs = strrchr(p, '/');
        const char* bs = strrchr(p, '\\');
        const char* base = (fs > bs ? fs : bs);
        base = base ? base + 1 : p;
        if (strcasecmp(base, filename) == 0) return;
    }

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
    // Game WADs now live in run/ID0/ (refactor) -- scan it first.
    static char id0_d[300];
    snprintf(id0_d, sizeof id0_d, "%s/ID0", run_dir());
    dirs[nd++] = id0_d;

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

// ----------------------------------------------------------------- pwad scan
// Every .wad in the wad dirs that ISN'T a known IWAD and isn't our own aidoom.wad
// voice asset -- offered as an optional extra "-file" load (freedoomstuff,
// hereticstuff, hexenstuff, or any user PWAD dropped into run/ID0).

// A WAD's 4-byte magic tells PWAD from IWAD.  A full IWAD (a stand-alone game like
// hexen.wad / heretic.wad / strife1.wad) makes no sense as an extra "-file" load, so we
// drop it from the PWAD list even when its name isn't in KNOWN_IWADS.  Unreadable/odd files
// default to "not an IWAD" (kept), so a real PWAD is never hidden by an I/O hiccup.
static int wad_is_iwad(const char* dir, const char* fname)
{
    char path[1024]; char magic[4] = {0}; FILE* f; size_t n;
    snprintf(path, sizeof path, "%s/%s", dir, fname);
    if (!(f = fopen(path, "rb"))) return 0;
    n = fread(magic, 1, 4, f);
    fclose(f);
    return n == 4 && memcmp(magic, "IWAD", 4) == 0;
}

// Does the WAD contain a lump with this 8-char name?  aiDoom's own internal asset packs
// (freedoomstuff/hereticstuff/hexenstuff/doom2stuff.wad) carry a marker lump "AISTUFF" so
// the launcher can hide them from the user PWAD list -- they're loaded by the game / the
// monster checkboxes, not picked as a "-file".  (tools/extract_*.py add it; existing wads
// are tagged by tools/mark_internal_wad.py.)
static int wad_has_lump(const char* dir, const char* fname, const char* lump)
{
    char path[1024]; unsigned char hdr[12]; FILE* f; int found = 0;
    if (dir && dir[0]) snprintf(path, sizeof path, "%s/%s", dir, fname);
    else               snprintf(path, sizeof path, "%s", fname);	// fname is already a full path
    if (!(f = fopen(path, "rb"))) return 0;
    if (fread(hdr, 1, 12, f) == 12) {
        unsigned n  = hdr[4] | hdr[5]<<8 | hdr[6]<<16 | (unsigned)hdr[7]<<24;
        unsigned of = hdr[8] | hdr[9]<<8 | hdr[10]<<16 | (unsigned)hdr[11]<<24;
        if (n && n <= 100000 && fseek(f, (long)of, SEEK_SET) == 0) {
            unsigned char e[16];
            for (unsigned i = 0; i < n && !found; i++) {
                if (fread(e, 1, 16, f) != 16) break;
                char nm[9]; memcpy(nm, e+8, 8); nm[8] = 0;
                if (strncasecmp(nm, lump, 8) == 0) found = 1;
            }
        }
    }
    fclose(f);
    return found;
}

static void pwad_add(const char* dir, const char* fname)
{
    size_t L = fname ? strlen(fname) : 0;
    if (pwad_count >= MAX_PWADS || L < 5 || L >= 60) return;
    if (strcasecmp(fname + L - 4, ".wad") != 0)  return;    // .wad only
    if (is_known_iwad(fname) >= 0)                return;   // known DOOM IWADs are the OTHER dropdown
    if (strcasecmp(fname, "aidoom.wad") == 0)     return;   // our voice PWAD -- never list it
    if (wad_is_iwad(dir, fname))                  return;   // a full IWAD (hexen/heretic/...) -> not a PWAD
    if (wad_has_lump(dir, fname, "AISTUFF"))      return;   // aiDoom-internal asset pack -> not a user PWAD
    for (int i=1; i<pwad_count; i++)                        // de-dupe (same name in two dirs)
        if (strcasecmp(pwads[i], fname) == 0) return;
    snprintf(pwads[pwad_count], sizeof pwads[0], "%s", fname);
    pwad_count++;
}

static void scan_pwads(void)
{
    pwad_count = 0;
    snprintf(pwads[pwad_count++], sizeof pwads[0], "(none)");   // index 0 = no extra PWAD

    char id0[300];
    snprintf(id0, sizeof id0, "%s/ID0", run_dir());
    const char* dirs[2] = { id0, run_dir() };
    const char* pats[2] = { "*.wad", "*.WAD" };		// glob both cases (Linux is case-sensitive)
    for (int d = 0; d < 2; d++)
        for (int p = 0; p < 2; p++) {
            int count = 0;
            char** files = SDL_GlobDirectory(dirs[d], pats[p], 0, &count);
            if (!files) continue;
            for (int i = 0; i < count; i++) pwad_add(dirs[d], files[i]);
            SDL_free(files);
        }
    if (pwad_sel >= pwad_count) pwad_sel = 0;
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
    // Embedded hero image fills the banner area (replaces the old text banner).
    if (hero_tex) {
        SDL_FRect dst = { 0.0f, 0.0f, (float)WINW, (float)BANNER_H };
        SDL_RenderTexture(ren, hero_tex, NULL, &dst);
    } else {
        rect(0, 0, WINW, BANNER_H, COL_BANNER);
    }
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

// ----------------------------------------------------------------- pwad dropdown
// Mirror of the IWAD dropdown for the optional extra "-file" PWAD.  pwads[0] is
// "(none)"; a non-zero pick adds "-file <name>" to the launch command.
#define PWAD_SHOW 6			// rows shown open (fits within WINH)
static void draw_pwad_dropdown(void)
{
    float x = PAD, y = PWAD_Y, w = WINW - 2*PAD, h = IWAD_H;
    rect(x, y, w, h, COL_DD_BG);
    draw_rect_outline(x, y, w, h, COL_DD_BD_R, COL_DD_BD_G, COL_DD_BD_B);

    text(x + 8, y + (h - FONT_CH)/2, "PWAD:", COL_DIM);
    text(x + 8 + 5*FONT_CW + 6, y + (h - FONT_CH)/2,
         pwads[pwad_sel],
         pwad_sel ? 255 : 150, pwad_sel ? 230 : 150, pwad_sel ? 100 : 150);

    const char* arrow = pwad_dd_open ? "^" : "v";
    text(x + w - FONT_CW - 6, y + (h - FONT_CH)/2, arrow, COL_DIM);

    if (pwad_dd_open && pwad_count > 0) {
        float dy = y + h;
        int max_scroll = pwad_count - PWAD_SHOW; if (max_scroll < 0) max_scroll = 0;
        if (pwad_scroll > max_scroll) pwad_scroll = max_scroll;
        if (pwad_scroll < 0) pwad_scroll = 0;
        int show = pwad_count < PWAD_SHOW ? pwad_count : PWAD_SHOW;
        for (int i=0; i<show; i++) {
            int idx = pwad_scroll + i;
            float oy = dy + i * IWAD_H;
            int hover = (mouse_x >= x && mouse_x <= x+w &&
                         mouse_y >= oy && mouse_y <= oy + IWAD_H);
            rect(x, oy, w, IWAD_H, hover ? COL_DD_HOV : COL_DD_BG);
            draw_rect_outline(x, oy, w, IWAD_H, COL_DD_BD_R, COL_DD_BD_G, COL_DD_BD_B);
            text(x + 8, oy + (IWAD_H - FONT_CH)/2, pwads[idx],
                 idx==pwad_sel ? 255 : 210, idx==pwad_sel ? 230 : 210, idx==pwad_sel ? 100 : 200);
            // scroll hints (mouse-wheel scrolls when the list overflows PWAD_SHOW rows)
            if (i == 0 && pwad_scroll > 0)
                text(x + w - FONT_CW - 6, oy + (IWAD_H - FONT_CH)/2, "^", COL_DIM);
            if (i == show-1 && pwad_scroll < max_scroll)
                text(x + w - FONT_CW - 6, oy + (IWAD_H - FONT_CH)/2, "v", COL_DIM);
        }
    }
}

static int hit_pwad_dropdown(int mouse_px, int mouse_py)
{
    if (mouse_px >= PAD && mouse_px <= WINW-PAD &&
        mouse_py >= PWAD_Y && mouse_py <= PWAD_Y + IWAD_H)
        return 0;        // toggles the dropdown

    if (!pwad_dd_open) return -1;

    int show = pwad_count < PWAD_SHOW ? pwad_count : PWAD_SHOW;
    for (int i=0; i<show; i++) {
        float oy = PWAD_Y + IWAD_H + i * IWAD_H;
        if (mouse_px >= PAD && mouse_px <= WINW-PAD &&
            mouse_py >= oy && mouse_py <= oy + IWAD_H)
            return pwad_scroll + i + 1;   // 1-based, scroll-adjusted index
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

// A greyed-out, non-interactive checkbox -- shown when the backing PWAD is missing.
static void draw_checkbox_disabled(float x, float y, const char* label)
{
    rect(x, y, CHK_BOX, CHK_BOX, COL_BOX_BG);
    draw_rect_outline(x, y, CHK_BOX, CHK_BOX, COL_GRAY);
    text(x + CHK_BOX + 6, y + (CHK_BOX - FONT_CH)/2, label, COL_GRAY);
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
// True if a PWAD is present where the engine looks (run/ID0/ first, then run/).
static int wad_present(const char* name)
{
    char p[1024]; FILE* f;
    snprintf(p, sizeof p, "%s/ID0/%s", run_dir(), name);
    if ((f = fopen(p, "rb"))) { fclose(f); return 1; }
    snprintf(p, sizeof p, "%s/%s", run_dir(), name);
    if ((f = fopen(p, "rb"))) { fclose(f); return 1; }
    return 0;
}

// Find the selected PWAD's FIRST map and write the matching -warp args ("7" for MAP07,
// "2 4" for E2M4) into `warp`.  A custom map rarely sits on MAP01/E1M1, so blindly warping
// there would drop the player into the IWAD's vanilla first map instead of the PWAD.  Leaves
// `warp` empty if the PWAD has no map (then the caller keeps its default).
static void pwad_first_map_warp(const char* name, char* warp, int wn)
{
    warp[0] = 0;
    char p[1024]; FILE* f;
    snprintf(p, sizeof p, "%s/ID0/%s", run_dir(), name);
    if (!(f = fopen(p, "rb"))) { snprintf(p, sizeof p, "%s/%s", run_dir(), name); f = fopen(p, "rb"); }
    if (!f) return;
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12) { fclose(f); return; }
    unsigned nl = hdr[4] | hdr[5]<<8 | hdr[6]<<16 | (unsigned)hdr[7]<<24;
    unsigned of = hdr[8] | hdr[9]<<8 | hdr[10]<<16 | (unsigned)hdr[11]<<24;
    if (!nl || nl > 100000 || fseek(f, (long)of, SEEK_SET) != 0) { fclose(f); return; }
    int best = 1000, mode = 0, be = 0, bm = 0;   // mode 1 = MAPxx, 2 = ExMy
    for (unsigned i = 0; i < nl; i++) {
        unsigned char e[16];
        if (fread(e, 1, 16, f) != 16) break;
        char nm[9]; memcpy(nm, e+8, 8); nm[8] = 0;
        if (nm[0]=='M' && nm[1]=='A' && nm[2]=='P' &&
            isdigit((unsigned char)nm[3]) && isdigit((unsigned char)nm[4]) && !nm[5]) {
            int mp = (nm[3]-'0')*10 + (nm[4]-'0');
            if (mp < best) { best = mp; mode = 1; bm = mp; }
        } else if (nm[0]=='E' && nm[1]>='1' && nm[1]<='9' &&
                   nm[2]=='M' && nm[3]>='1' && nm[3]<='9' && !nm[4]) {
            int key = (nm[1]-'0')*10 + (nm[3]-'0');
            if (key < best) { best = key; mode = 2; be = nm[1]-'0'; bm = nm[3]-'0'; }
        }
    }
    fclose(f);
    if      (mode == 1) snprintf(warp, wn, "%d", bm);
    else if (mode == 2) snprintf(warp, wn, "%d %d", be, bm);
}

// --- SIGIL pack verification ------------------------------------------------------------------
// SIGIL/SIGIL II ship their own episode (E5 / E3-compat / E6).  We accept one ONLY when its name
// looks like SIGIL AND its MD5 matches a known release; a verified pack warps to its own E?M1.
static const struct { const char* md5; int episode; const char* label; } SIGIL_WADS[] = {
    { "edd5c3dfd3fb1c981cf7390c5c14454e", 5, "SIGIL (E5)"        },  // SIGIL_V1_23.wad
    { "e4c5ab58e226bfcc8761f35204aeb3fc", 3, "SIGIL compat (E3)" },  // SIGIL_COMPAT_V1_23.wad
    { "d0442f5a75f2faef3405c09a0c3acc58", 6, "SIGIL II (E6)"     },  // SIGIL_II_V1_0.WAD
};

// 1 = verified SIGIL (sets *ep to its episode), -1 = looks like SIGIL by name but the checksum
// doesn't match a known release (caller must NOT load it), 0 = not a SIGIL wad (handle normally).
static int launcher_sigil_classify(const char* fname, int* ep)
{
    const char* s;
    int looks_sigil = 0;
    for (s = fname; *s; s++)
        if (strncasecmp(s, "sigil", 5) == 0) { looks_sigil = 1; break; }
    if (!looks_sigil) return 0;				// not SIGIL by name -> normal pwad

    char path[1024], md5[33];
    size_t i;
    snprintf(path, sizeof path, "%s/ID0/%s", run_dir(), fname);
    if (!md5_file_hex(path, md5)) return -1;		// can't read it -> can't verify -> reject
    for (i = 0; i < sizeof(SIGIL_WADS)/sizeof(SIGIL_WADS[0]); i++)
        if (strcasecmp(md5, SIGIL_WADS[i].md5) == 0) { if (ep) *ep = SIGIL_WADS[i].episode; return 1; }
    fprintf(stderr, "launcher: \"%s\" looks like SIGIL but its MD5 (%s) is unknown -- ignoring it.\n",
            fname, md5);
    return -1;						// SIGIL name + unknown checksum -> reject
}

// --- Launcher preference persistence ------------------------------------------------------------
// The launcher remembers the last selection in its OWN file (run/launcher.ini) rather than
// aidoom.cfg -- the game rewrites aidoom.cfg on exit and would strip unknown keys.  Value is the
// rest of the line, so names with spaces (IWAD/PWAD) survive.
static const char* prefs_path(void)
{
    static char p[300];
    snprintf(p, sizeof p, "%s/launcher.ini", run_dir());
    return p;
}

static void save_launcher_prefs(void)
{
    FILE* f = fopen(prefs_path(), "w");
    if (!f) return;
    fprintf(f, "iwad %s\n",     (iwad_sel >= 0 && iwad_sel < iwad_count) ? iwads[iwad_sel].name : "");
    fprintf(f, "pwad %s\n",     (pwad_sel > 0 && pwad_sel < pwad_count) ? pwads[pwad_sel] : "");
    fprintf(f, "buddy %d\n",    buddy_mode);
    fprintf(f, "monster %d\n",  mon_mode);
    fprintf(f, "skill %d\n",    opt_skill);
    fprintf(f, "infight %d\n",  opt_infight);
    fprintf(f, "autoaim %d\n",  opt_autoaim);
    fprintf(f, "overunder %d\n",opt_overunder);
    fprintf(f, "noff %d\n",     opt_noff);
    fprintf(f, "freedoom %d\n", opt_freedoom);
    fprintf(f, "heretic %d\n",  opt_heretic);
    fprintf(f, "hexen %d\n",    opt_hexen);
    fclose(f);
}

static void load_launcher_prefs(void)
{
    FILE* f = fopen(prefs_path(), "r");
    if (!f) return;
    char line[320], key[32], val[280];
    int i;
    while (fgets(line, sizeof line, f))
    {
        if (sscanf(line, "%31s %279[^\n\r]", key, val) < 1) continue;
        if      (!strcmp(key, "iwad"))    { for (i=0;i<iwad_count;i++) if (!strcmp(iwads[i].name,val)) { iwad_sel=i; break; } }
        else if (!strcmp(key, "pwad"))    { pwad_sel=0; for (i=1;i<pwad_count;i++) if (!strcmp(pwads[i],val)) { pwad_sel=i; break; } }
        else if (!strcmp(key, "buddy"))     buddy_mode   = atoi(val);
        else if (!strcmp(key, "monster"))   mon_mode     = atoi(val);
        else if (!strcmp(key, "skill"))     opt_skill    = atoi(val);
        else if (!strcmp(key, "infight"))   opt_infight  = atoi(val);
        else if (!strcmp(key, "autoaim"))   opt_autoaim  = atoi(val);
        else if (!strcmp(key, "overunder")) opt_overunder= atoi(val);
        else if (!strcmp(key, "noff"))      opt_noff     = atoi(val);
        else if (!strcmp(key, "freedoom"))  opt_freedoom = atoi(val);
        else if (!strcmp(key, "heretic"))   opt_heretic  = atoi(val);
        else if (!strcmp(key, "hexen"))     opt_hexen    = atoi(val);
    }
    fclose(f);
}

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
            // The real Left 4 Dead-style rule director: stress-driven extra spawns AND the
            // spoken game-master "voice of god" narration (was -respawn -fast, which gave
            // aggressive monsters but never started the director, so it stayed silent).
            off += snprintf(out + off, n - off, " -director");
            break;
        case MON_AI:
            off += snprintf(out + off, n - off, " -aidirector 31666");
            break;
    }

    // Toggles
    if (opt_noff)    off += snprintf(out + off, n - off, " -nofriendlyfire");
    if (opt_infight) off += snprintf(out + off, n - off, " -infight");
    if (opt_autoaim) off += snprintf(out + off, n - off, " -autoaim");
    // over/under is ON by default; OFF (or autoaim ON, which forces it off) -> vanilla infinitely-tall.
    if (!opt_overunder) off += snprintf(out + off, n - off, " -infinitetall");

    // ALL extra wads ride under ONE "-file": the engine reads files after the FIRST -file
    // until the next "-arg" (d_main.c), so a SECOND "-file" is silently ignored -- which is
    // why a PWAD picked alongside a monster-pack checkbox never loaded.  Collect every wad
    // (only if present, so a checked box with a missing wad can't crash startup), then emit
    // them space-separated under a single -file.
    char files[640]; int fn = 0; files[0] = 0;
    if (opt_freedoom && wad_present("freedoomstuff.wad"))
        fn += snprintf(files + fn, sizeof files - fn, " freedoomstuff.wad");
    if (opt_heretic && wad_present("hereticstuff.wad"))
        fn += snprintf(files + fn, sizeof files - fn, " hereticstuff.wad");
    if (opt_hexen && wad_present("hexenstuff.wad"))
        fn += snprintf(files + fn, sizeof files - fn, " hexenstuff.wad");
    // SIGIL packs are checksum-verified: a verified pack loads + warps to its own episode; a
    // SIGIL-named file whose MD5 is unknown is rejected (the PWAD setting is silently ignored).
    int sigil_ep = 0;
    int sigil = (pwad_sel > 0 && pwad_sel < pwad_count)
              ? launcher_sigil_classify(pwads[pwad_sel], &sigil_ep) : 0;
    int load_pwad = (pwad_sel > 0 && pwad_sel < pwad_count) && (sigil != -1);
    if (load_pwad) {
        const char* pw = pwads[pwad_sel];
        int dup = (opt_freedoom && !strcasecmp(pw, "freedoomstuff.wad"))
               || (opt_heretic  && !strcasecmp(pw, "hereticstuff.wad"))
               || (opt_hexen    && !strcasecmp(pw, "hexenstuff.wad"));
        if (!dup)
            // Quote the name: PWADs like "Crispy and Brutal.wad" have spaces, and an unquoted
            // name reaches the game as several -file args ("Crispy", "and", ...) that all fail to
            // open, so the PWAD (and its DEHACKED) is silently not loaded -> looks like vanilla.
            fn += snprintf(files + fn, sizeof files - fn,
                           strchr(pw, ' ') ? " \"%s\"" : " %s", pw);
    }
    if (files[0])
        off += snprintf(out + off, n - off, " -file%s", files);

    // Warp target.  A selected PWAD's own first map wins (custom maps rarely sit on E1M1/MAP01);
    // otherwise warp to the "Map" row's value (1/10/19/28).  DOOM2 takes a flat MAPnn (-warp 10);
    // DOOM1 / Heretic are EPISODIC (-warp E M), so the flat map number is converted to episode+map
    // (map 10 -> E2M1 = "2 1", 19 -> E3M1, 28 -> E4M1).  DOOM2-vs-DOOM1 is detected by a MAP01 lump.
    char warp[32] = "";
    if (sigil == 1)
        snprintf(warp, sizeof warp, "%d 1", sigil_ep);		// verified SIGIL -> E5/E3/E6 M1
    else if (load_pwad) {
        char w[32]; pwad_first_map_warp(pwads[pwad_sel], w, sizeof w);
        if (w[0]) snprintf(warp, sizeof warp, "%s", w);
    }
    if (!warp[0]) {
        int mapnum = map_vals[map_idx];
        int is_doom2 = (iwad_sel >= 0 && iwad_sel < iwad_count
                        && wad_has_lump("", iwads[iwad_sel].path, "MAP01"));
        if (is_doom2)
            snprintf(warp, sizeof warp, "%d", mapnum);				// MAPnn
        else
            snprintf(warp, sizeof warp, "%d %d", (mapnum-1)/9 + 1, (mapnum-1)%9 + 1);  // ExMy
    }
    if (warp[0])
        off += snprintf(out + off, n - off, " -warp %s -skill %d", warp, opt_skill + 1);

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
// Is a director sidecar already running?  (so a second launch doesn't start a
// duplicate -- one director can serve whichever game is listening on port 31666).
static int director_running(void)
{
#ifdef _WIN32
    return system("tasklist /FI \"IMAGENAME eq director.exe\" 2>NUL | find /I \"director.exe\" >NUL") == 0;
#else
    return system("pgrep -x director >/dev/null 2>&1") == 0;
#endif
}

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

    // Only start a director if one isn't already running (avoid a duplicate sidecar).
    int reuse_director  = needs_director && has_director && director_running();
    int start_director  = needs_director && has_director && !reuse_director;

    save_launcher_prefs();   // remember this selection for next time

#ifdef _WIN32
    // Launch with CreateProcess (NOT system("start ...")): it returns immediately so the
    // launcher window never freezes, and CREATE_NO_WINDOW stops the CONSOLE-subsystem game
    // from popping up a console/cmd window.  The game's stdout+stderr are redirected to
    // run/aidoom_stderr.log so a fatal I_Error is still recoverable after the window closes.
    char gamexe[300], logpath[320];
    snprintf(gamexe,  sizeof gamexe,  "%s/aidoom.exe",        run_dir());
    snprintf(logpath, sizeof logpath, "%s/aidoom_stderr.log", run_dir());

    if (start_director)
    {
        char dircmd[320];
        snprintf(dircmd, sizeof dircmd, "\"%s\" --port 31666", dir_path);
        STARTUPINFOA dsi; PROCESS_INFORMATION dpi;
        memset(&dsi, 0, sizeof dsi); dsi.cb = sizeof dsi;
        if (CreateProcessA(dir_path, dircmd, NULL, NULL, FALSE,
                           CREATE_NO_WINDOW, NULL, run_dir(), &dsi, &dpi))
        { CloseHandle(dpi.hThread); CloseHandle(dpi.hProcess); }
    }

    // Inheritable log handle -> the game's stdout+stderr (windowless, but still logged).
    SECURITY_ATTRIBUTES sa; memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa; sa.bInheritHandle = TRUE;
    HANDLE hlog = CreateFileA(logpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    BOOL inherit = FALSE;
    if (hlog != INVALID_HANDLE_VALUE)
    { si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = si.hStdError = hlog; inherit = TRUE; }

    BOOL ok = CreateProcessA(gamexe, cmd, NULL, NULL, inherit,
                             CREATE_NO_WINDOW, NULL, run_dir(), &si, &pi);
    if (hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
    if (!ok)
    {
        snprintf(g_status, sizeof g_status,
                 "failed to launch aidoom.exe (error %lu)", (unsigned long)GetLastError());
        g_status_err = 1;
        return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
#else
    // POSIX: background both in subshells; tee the game's stderr to a log so a fatal
    // I_Error survives the window vanishing.  system() returns at once here.
    char full[1400];
    if (start_director)
        snprintf(full, sizeof full,
                 "( \"%s\" --port 31666 >/dev/null 2>&1 & ) ; ( %s >aidoom_stderr.log 2>&1 & )",
                 dir_path, cmd);
    else
        snprintf(full, sizeof full, "( %s >aidoom_stderr.log 2>&1 & )", cmd);
    fprintf(stderr, "[launcher] %s\n", full);
    system(full);   // returns immediately -- the launcher stays responsive
#endif

    snprintf(g_status, sizeof g_status, "launched aiDoom%s",
             start_director ? " + director"
             : reuse_director ? " (director already running)" : "");
    g_status_err = 0;

    // Log the command so users can see what was launched (exit code is N/A now
    // that we don't wait for it).
    FILE* f = fopen("launcher.log", "a");
    if (f) {
        fprintf(f, "launched: %s\n", cmd);
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
                           SDL_WINDOW_HIGH_PIXEL_DENSITY);	// fixed size (no SDL_WINDOW_RESIZABLE)
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
    hero_init();
    scan_iwads();
    scan_pwads();
    load_launcher_prefs();   // restore the last session's selection

    int running = 1;
    SDL_Event ev;
    while (running) {
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE) {
                    if (dropdown_open || pwad_dd_open) { dropdown_open = 0; pwad_dd_open = 0; }
                    else running = 0;
                }
                if (ev.key.key == SDLK_RETURN && !dropdown_open && !pwad_dd_open) {
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
                    // Modal dropdowns: while one is open it's the ONLY thing that
                    // takes clicks (a click anywhere closes it / picks an item).
                    if (dropdown_open) {
                        int hh = hit_iwad_dropdown(mouse_x, mouse_y);
                        if (hh > 0) iwad_sel = hh - 1;
                        dropdown_open = 0;
                    } else if (pwad_dd_open) {
                        int hh = hit_pwad_dropdown(mouse_x, mouse_y);
                        if (hh > 0) pwad_sel = hh - 1;
                        pwad_dd_open = 0;
                    } else if (hit_iwad_dropdown(mouse_x, mouse_y) == 0) {
                        dropdown_open = 1;
                    } else if (hit_pwad_dropdown(mouse_x, mouse_y) == 0) {
                        pwad_dd_open = 1; pwad_scroll = 0;
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

                        // Map row (4 pills) -- right-aligned like Skill, width 64
                        {
                            const float mpw = 80;	// wider pills for the "10(EP2)" labels
                            float mrx = WINW - PAD - mpw*4 - gap*3;
                            float mry = MAP_Y + (BUDDY_H - 26)/2;
                            if (mouse_y >= mry && mouse_y <= mry + 26) {
                                for (int i=0; i<4; i++) {
                                    float px = mrx + i * (mpw + gap);
                                    if (mouse_x >= px && mouse_x <= px + mpw) {
                                        map_idx = i;
                                        g_status[0] = 0;
                                    }
                                }
                            }
                        }

                        // Options row: toggle the two checkboxes.
                        if (hit_checkbox(OPT_NOFF_X, OPTS_Y, "No friendly fire", mouse_x, mouse_y))
                            opt_noff = !opt_noff;
                        if (hit_checkbox(OPT_INF_X, OPTS_Y, "All Monster infight", mouse_x, mouse_y))
                            opt_infight = !opt_infight;
                        // Second options row. Autoaim ON forces vanilla "infinitely tall actors"
                        // (over/under OFF); enabling over/under in turn clears Autoaim -- they can't
                        // both be on, so the launched flags stay consistent.
                        if (hit_checkbox(OPT_AA_X, OPTS2_Y, "Autoaim", mouse_x, mouse_y)) {
                            opt_autoaim = !opt_autoaim;
                            if (opt_autoaim) opt_overunder = 0;
                        }
                        if (hit_checkbox(OPT_OU_X, OPTS2_Y, "Monster over/under", mouse_x, mouse_y)) {
                            opt_overunder = !opt_overunder;
                            if (opt_overunder) opt_autoaim = 0;
                        }

                        // Monsters row: toggle the extra-monster WAD checkboxes
                        // (ignored when greyed out -- the PWAD isn't present).
                        if (wad_present("freedoomstuff.wad") &&
                            hit_checkbox(MON_FD_X, MONSTERS_Y, "FreeDoom", mouse_x, mouse_y))
                            opt_freedoom = !opt_freedoom;
                        if (wad_present("hereticstuff.wad") &&
                            hit_checkbox(MON_HER_X, MONSTERS_Y, "Heretic", mouse_x, mouse_y))
                            opt_heretic = !opt_heretic;
                        if (wad_present("hexenstuff.wad") &&
                            hit_checkbox(MON_HEX_X, MONSTERS_Y, "Hexen", mouse_x, mouse_y))
                            opt_hexen = !opt_hexen;
                    }
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                mouse_down = 0;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (pwad_dd_open) {     // scroll the open PWAD list
                    int ms = pwad_count - PWAD_SHOW; if (ms < 0) ms = 0;
                    pwad_scroll -= (int)ev.wheel.y;
                    if (pwad_scroll < 0)  pwad_scroll = 0;
                    if (pwad_scroll > ms) pwad_scroll = ms;
                }
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
            draw_mode_row(BUDDY_Y, "BuddyMode",
                          buddy_opts, 3, buddy_mode, 110, NULL);
        }
        // Monster row
        {
            static const char* mon_opts[] = { "Vanilla", "L4D", "AI Director" };
            draw_mode_row(MON_Y, "MonsterMode",
                          mon_opts, 3, mon_mode, 110, NULL);
        }
        // Skill row (5 pills; default Ultra-Violence)
        {
            static const char* skill_opts[] = { "ITYTD", "HNTR", "HMP", "UV", "NM" };
            draw_mode_row(SKILL_Y, "Skill",
                          skill_opts, 5, opt_skill, 64, NULL);
        }
        // Map row (4 pills): warp target -- 1/10/19/28 (build_command picks DOOM1/DOOM2 warp form)
        {
            static const char* map_opts[] = { "1(EP1)", "10(EP2)", "19(EP3)", "28(EP4)" };
            draw_mode_row(MAP_Y, "Map", map_opts, 4, map_idx, 80, NULL);
        }
        // Options row (toggles)
        {
            text(PAD, OPTS_Y + (CHK_BOX - FONT_CH)/2, "Options", COL_DIM);
            draw_checkbox(OPT_NOFF_X, OPTS_Y, opt_noff,    "No friendly fire");
            draw_checkbox(OPT_INF_X,  OPTS_Y, opt_infight, "All Monster infight");
            // Second options row: vertical autoaim (off = shoot where you look) and over/under
            // 3D clipping (on = walk under/stand on monsters).  Mutually-coupled (see click code).
            draw_checkbox(OPT_AA_X, OPTS2_Y, opt_autoaim,   "Autoaim");
            draw_checkbox(OPT_OU_X, OPTS2_Y, opt_overunder, "Monster over/under");

            text(PAD, MONSTERS_Y + (CHK_BOX - FONT_CH)/2, "+Monster", COL_DIM);
            if (wad_present("freedoomstuff.wad"))
                draw_checkbox(MON_FD_X, MONSTERS_Y, opt_freedoom, "FreeDoom");
            else
                draw_checkbox_disabled(MON_FD_X, MONSTERS_Y, "FreeDoom");
            if (wad_present("hereticstuff.wad"))
                draw_checkbox(MON_HER_X, MONSTERS_Y, opt_heretic, "Heretic");
            else
                draw_checkbox_disabled(MON_HER_X, MONSTERS_Y, "Heretic");
            if (wad_present("hexenstuff.wad"))
                draw_checkbox(MON_HEX_X, MONSTERS_Y, opt_hexen, "Hexen");
            else
                draw_checkbox_disabled(MON_HEX_X, MONSTERS_Y, "Hexen");
        }

        // Launch button + status first; the IWAD/PWAD dropdowns draw on top so an open
        // list overlays them (the OPEN one is drawn last for correct z-order).
        draw_launch_button();

        // Status line (launch error / confirmation) just above the button.
        if (g_status[0]) {
            if (g_status_err) text(PAD, LAUNCH_Y - 16, g_status, COL_BTN_BD);
            else              text(PAD, LAUNCH_Y - 16, g_status, COL_CHECK);
        }

        if (dropdown_open) { draw_pwad_dropdown(); draw_iwad_dropdown(); }
        else               { draw_iwad_dropdown(); draw_pwad_dropdown(); }

        SDL_RenderPresent(ren);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}