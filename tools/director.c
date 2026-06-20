// Emacs style mode select   -*- C -*-
//-----------------------------------------------------------------------------
//
// director.c -- native (C / SDL3) replacement for ollama_director.py.
//
//   The LLM "AI Director" for aiDoom's monsters.  Connects to the game's
//   -aidirector TCP server, observes the game state, asks a local Ollama model
//   for tactical orders, and issues them -- exactly the protocol implemented in
//   ollama_director.py:
//
//     observe\n  -> one JSON line {tic, player, monsters:[{id,...}], count}
//     act order=<name> ids=1,2 for=<tics>\n
//     wake\n
//
//   No Python required.  A worker thread runs the network/LLM loop (the Ollama
//   call blocks for seconds) while the main thread draws a small SDL3 window that
//   shows the live status + a scrolling log, so the GUI stays responsive.
//
//   Self-contained: a tiny in-house JSON parser + a hand-rolled HTTP/1.1 POST
//   (Connection: close) -- no libcurl, no cJSON, no third-party code.
//
//   Build: tools/build_director.sh (Linux/macOS) -> run/director
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define CLOSESOCK(s) closesocket(s)
  #define SOCKERR      INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  typedef int sock_t;
  #define CLOSESOCK(s) close(s)
  #define SOCKERR      (-1)
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "font_atlas.h"

// ===========================================================================
//  Tiny JSON parser (object/array/string/number/bool/null) -- in-house.
// ===========================================================================

typedef enum { JNULL, JBOOL, JNUM, JSTR, JARR, JOBJ } jtype;

typedef struct json {
    jtype          t;
    double         num;     // JNUM / JBOOL (0/1)
    char*          str;     // JSTR (owned, unescaped) / object member key for children
    struct json*   child;   // JARR/JOBJ first child
    struct json*   next;    // sibling
    char*          key;     // member name when this node is an object member
} json;

static const char* js_skip (const char* s) { while (*s && (unsigned char)*s <= ' ') s++; return s; }

static char* js_str (const char** ps)   // parse a "..." string (input at opening quote)
{
    const char* s = *ps;
    if (*s != '"') return NULL;
    s++;
    // worst-case length = remaining; allocate generously then shrink-by-use
    size_t cap = strlen (s) + 1;
    char*  out = malloc (cap);
    size_t n = 0;
    while (*s && *s != '"')
    {
        char c = *s++;
        if (c == '\\' && *s)
        {
            char e = *s++;
            switch (e)
            {
                case 'n': c = '\n'; break;
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'b': c = '\b'; break;
                case 'f': c = '\f'; break;
                case '/': c = '/';  break;
                case '"': c = '"';  break;
                case '\\': c = '\\'; break;
                case 'u': {            // \uXXXX -> UTF-8 (BMP only, good enough)
                    int cp = 0, k;
                    for (k = 0; k < 4 && isxdigit ((unsigned char)*s); k++)
                        cp = cp*16 + (isdigit((unsigned char)*s) ? *s-'0'
                                      : (tolower(*s)-'a'+10)), s++;
                    if (cp < 0x80) { out[n++] = (char)cp; }
                    else if (cp < 0x800) { out[n++]=(char)(0xC0|(cp>>6)); out[n++]=(char)(0x80|(cp&0x3F)); }
                    else { out[n++]=(char)(0xE0|(cp>>12)); out[n++]=(char)(0x80|((cp>>6)&0x3F)); out[n++]=(char)(0x80|(cp&0x3F)); }
                    continue;
                }
                default: c = e; break;
            }
        }
        out[n++] = c;
    }
    if (*s == '"') s++;
    out[n] = '\0';
    *ps = s;
    return out;
}

static json* js_parse_val (const char** ps);

static void json_free (json* j)
{
    while (j)
    {
        json* nx = j->next;
        if (j->child) json_free (j->child);
        free (j->str); free (j->key);
        free (j);
        j = nx;
    }
}

static json* js_parse_val (const char** ps)
{
    const char* s = js_skip (*ps);
    json* j = calloc (1, sizeof (json));
    if (*s == '{' || *s == '[')
    {
        int   obj = (*s == '{');
        char  close = obj ? '}' : ']';
        j->t = obj ? JOBJ : JARR;
        s = js_skip (s + 1);
        json* tail = NULL;
        while (*s && *s != close)
        {
            char* key = NULL;
            if (obj)
            {
                s = js_skip (s);
                key = js_str (&s);
                s = js_skip (s);
                if (*s == ':') s = js_skip (s + 1);
            }
            const char* vp = s;
            json* ch = js_parse_val (&vp);
            s = js_skip (vp);
            if (!ch) { free (key); break; }
            ch->key = key;
            if (tail) tail->next = ch; else j->child = ch;
            tail = ch;
            if (*s == ',') s = js_skip (s + 1);
        }
        if (*s == close) s++;
    }
    else if (*s == '"')
    {
        j->t = JSTR;
        j->str = js_str (&s);
    }
    else if (!strncmp (s, "true", 4))  { j->t = JBOOL; j->num = 1; s += 4; }
    else if (!strncmp (s, "false", 5)) { j->t = JBOOL; j->num = 0; s += 5; }
    else if (!strncmp (s, "null", 4))  { j->t = JNULL; s += 4; }
    else { j->t = JNUM; j->num = strtod (s, (char**)&s); }
    *ps = s;
    return j;
}

static json* json_parse (const char* s) { return js_parse_val (&s); }

static json* json_get (json* o, const char* key)   // object member by name
{
    if (!o || o->t != JOBJ) return NULL;
    for (json* c = o->child; c; c = c->next)
        if (c->key && !strcmp (c->key, key)) return c;
    return NULL;
}

// ===========================================================================
//  Config (aidoom.cfg, next to the binary)
// ===========================================================================

static char cfg_host[128]  = "192.168.2.114";
static int  cfg_oport      = 11434;
static char cfg_model[128] = "mistral:7b-instruct";

static void load_cfg (const char* dir)
{
    char path[512];
    snprintf (path, sizeof path, "%s/aidoom.cfg", dir);
    FILE* f = fopen (path, "r");
    if (!f) return;
    char line[512];
    while (fgets (line, sizeof line, f))
    {
        char k[128], v[256];
        if (sscanf (line, "%127s %255s", k, v) == 2)
        {
            char* vv = v;
            if (*vv == '"') { vv++; char* q = strchr (vv, '"'); if (q) *q = 0; }
            if      (!strcmp (k, "ollama_host"))  strncpy (cfg_host, vv, sizeof cfg_host - 1);
            else if (!strcmp (k, "ollama_port"))  cfg_oport = atoi (vv);
            else if (!strcmp (k, "ollama_model")) strncpy (cfg_model, vv, sizeof cfg_model - 1);
        }
    }
    fclose (f);
}

// ===========================================================================
//  Shared state between the worker (network) thread and the GUI thread.
// ===========================================================================

#define LOG_MAX   256
#define LOG_COLS  120

static SDL_Mutex* g_lock;
static char       g_log[LOG_MAX][LOG_COLS];
static int        g_log_n;          // total lines ever (use % LOG_MAX for ring)
static volatile int g_quit;

// status line fields (drawn in the header)
static char g_status[160] = "starting ...";
static char g_sub[160]    = "";

static void logln (const char* fmt, ...)
{
    char buf[LOG_COLS];
    va_list ap; va_start (ap, fmt);
    vsnprintf (buf, sizeof buf, fmt, ap);
    va_end (ap);
    SDL_LockMutex (g_lock);
    snprintf (g_log[g_log_n % LOG_MAX], LOG_COLS, "%s", buf);
    g_log_n++;
    SDL_UnlockMutex (g_lock);
    fprintf (stderr, "[director] %s\n", buf);
}

static void setstatus (const char* s) { SDL_LockMutex (g_lock); snprintf (g_status, sizeof g_status, "%s", s); SDL_UnlockMutex (g_lock); }
static void setsub    (const char* s) { SDL_LockMutex (g_lock); snprintf (g_sub,    sizeof g_sub,    "%s", s); SDL_UnlockMutex (g_lock); }

// ===========================================================================
//  Sockets
// ===========================================================================

static sock_t tcp_connect (const char* host, int port, int timeout_s)
{
    char portstr[16]; snprintf (portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset (&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo (host, portstr, &hints, &res) != 0 || !res) return SOCKERR;
    sock_t s = socket (res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == SOCKERR) { freeaddrinfo (res); return SOCKERR; }
    if (connect (s, res->ai_addr, (int)res->ai_addrlen) != 0) { CLOSESOCK (s); freeaddrinfo (res); return SOCKERR; }
    freeaddrinfo (res);
    (void) timeout_s;
    return s;
}

static int send_all (sock_t s, const char* buf, int len)
{
    int off = 0;
    while (off < len)
    {
        int n = (int) send (s, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += n;
    }
    return 0;
}

// read one '\n'-terminated line from the game socket
static int recv_line (sock_t s, char* out, int cap)
{
    int n = 0;
    while (n < cap - 1)
    {
        char c;
        int r = (int) recv (s, &c, 1, 0);
        if (r <= 0) return (n > 0) ? n : -1;
        if (c == '\n') break;
        out[n++] = c;
    }
    out[n] = '\0';
    return n;
}

// ===========================================================================
//  HTTP POST to Ollama (hand-rolled, Connection: close).
//  Returns malloc'd body (caller frees), or NULL.
// ===========================================================================

static char* http_post_json (const char* host, int port, const char* path, const char* json_body)
{
    sock_t s = tcp_connect (host, port, 5);
    if (s == SOCKERR) return NULL;

    char hdr[512];
    int  blen = (int) strlen (json_body);
    int  hn = snprintf (hdr, sizeof hdr,
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n", path, host, port, blen);
    if (send_all (s, hdr, hn) != 0 || send_all (s, json_body, blen) != 0)
        { CLOSESOCK (s); return NULL; }

    // read the whole response until the server closes the connection
    size_t cap = 65536, len = 0;
    char*  resp = malloc (cap);
    for (;;)
    {
        if (len + 4096 > cap) { cap *= 2; resp = realloc (resp, cap); }
        int r = (int) recv (s, resp + len, 4096, 0);
        if (r <= 0) break;
        len += r;
    }
    resp[len] = '\0';
    CLOSESOCK (s);

    char* body = strstr (resp, "\r\n\r\n");
    if (!body) { free (resp); return NULL; }
    body += 4;
    char* out = strdup (body);
    free (resp);
    return out;
}

// ===========================================================================
//  Director worker thread -- the protocol loop (mirrors ollama_director.py).
// ===========================================================================

static int   opt_gameport = 31666;
static char  opt_model[128];
static char  opt_host[128];
static int   opt_oport;
static double opt_period = 1.0;

static const char* SYSTEM_PROMPT =
    "You are the AI director for a single-player DOOM session and you control TWO things. "
    "(1) The MONSTERS, as a coordinated squad challenging the human: pincer (some flank_left, "
    "some flank_right), 1-2 strong monsters focus_fire, keep low-HP monsters back with fallback "
    "or hold, ambush for monsters that can't see the player yet. Monster orders: chase, hold, "
    "fallback, flank_left, flank_right, ambush, focus_fire, use_door. "
    "(2) The human's AI COMPANION 'buddy' (the \\\"buddy\\\" field of the state), which you direct "
    "to HELP the human: engage dangerous monsters (add \\\"focus\\\":<monster id> to pick one), "
    "regroup or retreat it when its health is low, defend when the human is safe, grab to collect "
    "items. Buddy orders: engage, defend, hold, regroup, retreat, grab. "
    "Respond ONLY with JSON of the form {\\\"commands\\\":[{\\\"order\\\":\\\"flank_left\\\","
    "\\\"ids\\\":[1,3]}],\\\"buddy\\\":{\\\"order\\\":\\\"engage\\\",\\\"focus\\\":2}}. "
    "Every monster id must appear in exactly one command. Omit \\\"buddy\\\" if the state has no buddy.";

static int is_order (const char* o)
{
    static const char* k[] = { "chase","hold","fallback","flank_left","flank_right",
                               "ambush","focus_fire","use_door", NULL };
    for (int i = 0; k[i]; i++) if (!strcmp (o, k[i])) return 1;
    return 0;
}

static int is_buddy_order (const char* o)
{
    static const char* k[] = { "engage","defend","hold","regroup","retreat","grab","auto", NULL };
    for (int i = 0; k[i]; i++) if (!strcmp (o, k[i])) return 1;
    return 0;
}

// JSON-escape src into dst (for embedding the state into the chat body)
static void json_escape (char* dst, size_t cap, const char* src)
{
    size_t n = 0;
    for (; *src && n + 2 < cap; src++)
    {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[n++]='\\'; dst[n++]=c; }
        else if (c == '\n') { dst[n++]='\\'; dst[n++]='n'; }
        else if (c == '\r') { }
        else if (c == '\t') { dst[n++]='\\'; dst[n++]='t'; }
        else if (c < 0x20)  { }
        else dst[n++] = c;
    }
    dst[n] = '\0';
}

static int worker (void* unused)
{
    (void) unused;
    char line[8192];

    // 1. connect to the game (retry while it starts up)
    sock_t gs = SOCKERR;
    for (int i = 0; i < 60 && !g_quit; i++)
    {
        gs = tcp_connect ("127.0.0.1", opt_gameport, 5);
        if (gs != SOCKERR) break;
        if (i == 0) { setstatus ("waiting for aiDoom ..."); logln ("waiting for aiDoom on 127.0.0.1:%d", opt_gameport); }
        SDL_Delay (500);
    }
    if (gs == SOCKERR) { setstatus ("FAILED: no game connection"); logln ("could not connect to the game; giving up."); return 1; }

    char st[200];
    snprintf (st, sizeof st, "connected  model=%s  ollama=%s:%d", opt_model, opt_host, opt_oport);
    setstatus (st);
    logln ("connected to game; model=%s ollama=http://%s:%d/api/chat", opt_model, opt_host, opt_oport);

    send_all (gs, "wake\n", 5);
    recv_line (gs, line, sizeof line);     // "ok"

    int round = 0;
    while (!g_quit)
    {
        round++;
        if (send_all (gs, "observe\n", 8) != 0) { logln ("connection closed by game."); break; }
        if (recv_line (gs, line, sizeof line) <= 0) { logln ("connection closed by game."); break; }

        json* state = json_parse (line);
        json* mons  = json_get (state, "monsters");
        json* plr   = json_get (state, "player");
        json* bud   = json_get (state, "buddy");	// present only with -aicoop

        // collect valid ids
        int  ids[256], nids = 0;
        if (mons && mons->t == JARR)
            for (json* m = mons->child; m && nids < 256; m = m->next)
            {
                json* id = json_get (m, "id");
                if (id && id->t == JNUM) ids[nids++] = (int) id->num;
            }
        if (nids == 0 && !bud)
        {
            char s2[160]; snprintf (s2, sizeof s2, "round %d: no live monsters / no buddy yet", round); setsub (s2);
            json_free (state);
            SDL_Delay ((int)(opt_period * 1000));
            continue;
        }

        // build the chat request body (state embedded as escaped strings)
        char esc[6000];
        json_escape (esc, sizeof esc, line);
        char* body = malloc (16000);
        snprintf (body, 16000,
            "{\"model\":\"%s\",\"stream\":false,\"format\":\"json\","
            "\"options\":{\"temperature\":0.6},\"messages\":["
            "{\"role\":\"system\",\"content\":\"%s\"},"
            "{\"role\":\"user\",\"content\":\"Game state: %s\\nAssign orders now as JSON.\"}]}",
            opt_model, SYSTEM_PROMPT, esc);

        Uint64 t0 = SDL_GetTicks ();
        char*  resp = http_post_json (opt_host, opt_oport, "/api/chat", body);
        free (body);
        double dt = (SDL_GetTicks () - t0) / 1000.0;

        if (!resp) { logln ("round %d: ollama error (no response)", round); json_free (state); SDL_Delay ((int)(opt_period*1000)); continue; }

        // outer: {"message":{"content":"<json string>"}, ...}
        json* outer   = json_parse (resp);
        json* message = json_get (outer, "message");
        json* content = message ? json_get (message, "content") : NULL;
        int   issued  = 0;

        if (content && content->t == JSTR)
        {
            json* plan = json_parse (content->str);    // {"commands":[{order,ids}]}
            json* cmds = json_get (plan, "commands");
            char  summary[LOG_COLS] = "";
            if (cmds && cmds->t == JARR)
                for (json* c = cmds->child; c; c = c->next)
                {
                    json* ord = json_get (c, "order");
                    json* idl = json_get (c, "ids");
                    if (!ord || ord->t != JSTR || !idl || idl->t != JARR) continue;
                    char ol[64]; snprintf (ol, sizeof ol, "%s", ord->str);
                    for (char* p = ol; *p; p++) *p = (char) tolower ((unsigned char)*p);
                    if (!is_order (ol)) continue;

                    char idcsv[256] = ""; int any = 0;
                    for (json* id = idl->child; id; id = id->next)
                    {
                        if (id->t != JNUM) continue;
                        int v = (int) id->num, ok = 0;
                        for (int i = 0; i < nids; i++) if (ids[i] == v) { ok = 1; break; }
                        if (!ok) continue;
                        char one[16]; snprintf (one, sizeof one, "%s%d", any ? "," : "", v);
                        strncat (idcsv, one, sizeof idcsv - strlen (idcsv) - 1);
                        any = 1;
                    }
                    if (!any) continue;

                    char cmd[320];
                    int  cn = snprintf (cmd, sizeof cmd, "act order=%s ids=%s for=105\n", ol, idcsv);
                    send_all (gs, cmd, cn);
                    recv_line (gs, line, sizeof line);   // "ok"
                    char piece[48]; snprintf (piece, sizeof piece, "%s<-[%s] ", ol, idcsv);
                    strncat (summary, piece, sizeof summary - strlen (summary) - 1);
                    issued++;
                }

            // buddy order (the player's ally) -- {"buddy":{"order":"engage","focus":2}}
            if (bud)
            {
                json* bo = json_get (plan, "buddy");
                json* bord = bo ? json_get (bo, "order") : NULL;
                if (bord && bord->t == JSTR)
                {
                    char bl[32]; snprintf (bl, sizeof bl, "%s", bord->str);
                    for (char* p = bl; *p; p++) *p = (char) tolower ((unsigned char)*p);
                    if (is_buddy_order (bl))
                    {
                        json* bf = json_get (bo, "focus");
                        int fid = (bf && bf->t == JNUM) ? (int) bf->num : 0;
                        int fok = 0;
                        for (int i = 0; i < nids; i++) if (ids[i] == fid) { fok = 1; break; }
                        char bc[128];
                        int bn = fok ? snprintf (bc, sizeof bc, "buddy order=%s focus=%d for=105\n", bl, fid)
                                     : snprintf (bc, sizeof bc, "buddy order=%s for=105\n", bl);
                        send_all (gs, bc, bn);
                        recv_line (gs, line, sizeof line);   // "ok"
                        char piece[48]; snprintf (piece, sizeof piece, "buddy:%s%s ", bl, fok ? "*" : "");
                        strncat (summary, piece, sizeof summary - strlen (summary) - 1);
                        issued++;
                    }
                }
            }
            json_free (plan);

            json* hp = plr ? json_get (plr, "health") : NULL;
            logln ("round %d  mon=%d  hp=%d  llm=%.1fs  %s", round, nids,
                   hp && hp->t==JNUM ? (int)hp->num : -1, dt,
                   issued ? summary : "(no valid orders)");
        }
        else
        {
            logln ("round %d: ollama returned no content (llm=%.1fs)", round, dt);
        }

        char s2[160];
        snprintf (s2, sizeof s2, "round %d  monsters=%d  last LLM %.1fs  orders=%d", round, nids, dt, issued);
        setsub (s2);

        json_free (outer); free (resp); json_free (state);
        SDL_Delay ((int)(opt_period * 1000));
    }

    CLOSESOCK (gs);
    setstatus ("disconnected");
    return 0;
}

// ===========================================================================
//  SDL3 GUI
// ===========================================================================

#define WINW 760
#define WINH 460

static SDL_Renderer* ren;
static SDL_Texture*  font;

static void font_init (void)
{
    Uint32* px = malloc (FONT_AW * FONT_CH * 4);
    for (int i = 0; i < FONT_AW * FONT_CH; i++)
        px[i] = 0x00FFFFFFu | ((Uint32) font_alpha[i] << 24);
    SDL_Surface* s = SDL_CreateSurfaceFrom (FONT_AW, FONT_CH, SDL_PIXELFORMAT_ARGB8888, px, FONT_AW * 4);
    font = SDL_CreateTextureFromSurface (ren, s);
    SDL_SetTextureBlendMode (font, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode (font, SDL_SCALEMODE_NEAREST);
    SDL_DestroySurface (s); free (px);
}

static void text (float x, float y, const char* str, Uint8 r, Uint8 g, Uint8 b)
{
    SDL_SetTextureColorMod (font, r, g, b);
    for (const char* p = str; *p; p++)
    {
        int c = (unsigned char) *p;
        if (c < FONT_FIRST || c >= FONT_FIRST + FONT_COUNT) c = '?';
        SDL_FRect src = { (float)((c - FONT_FIRST) * FONT_CW), 0, FONT_CW, FONT_CH };
        SDL_FRect dst = { x, y, FONT_CW, FONT_CH };
        SDL_RenderTexture (ren, font, &src, &dst);
        x += FONT_CW;
    }
}

int main (int argc, char** argv)
{
    // --- args (mirror ollama_director.py) ---
    strncpy (opt_host,  cfg_host,  sizeof opt_host - 1);
    strncpy (opt_model, cfg_model, sizeof opt_model - 1);
    opt_oport = cfg_oport;

    // load aidoom.cfg from the binary's directory first
    {
        const char* base = SDL_GetBasePath ();
        if (base) { load_cfg (base); }
        strncpy (opt_host,  cfg_host,  sizeof opt_host - 1);
        strncpy (opt_model, cfg_model, sizeof opt_model - 1);
        opt_oport = cfg_oport;
    }

    for (int i = 1; i < argc; i++)
    {
        if      (!strcmp (argv[i], "--port")    && i+1 < argc) opt_gameport = atoi (argv[++i]);
        else if (!strcmp (argv[i], "--model")   && i+1 < argc) strncpy (opt_model, argv[++i], sizeof opt_model - 1);
        else if (!strcmp (argv[i], "--host")    && i+1 < argc) strncpy (opt_host,  argv[++i], sizeof opt_host - 1);
        else if (!strcmp (argv[i], "--ollama-port") && i+1 < argc) opt_oport = atoi (argv[++i]);
        else if (!strcmp (argv[i], "--period")  && i+1 < argc) opt_period = atof (argv[++i]);
        else if (!strcmp (argv[i], "--ollama")  && i+1 < argc)
        {
            // parse http://HOST:PORT/path
            const char* u = argv[++i];
            const char* h = strstr (u, "://"); h = h ? h + 3 : u;
            char hb[160]; int k = 0;
            while (*h && *h != ':' && *h != '/' && k < (int)sizeof hb - 1) hb[k++] = *h++;
            hb[k] = 0;
            if (k) strncpy (opt_host, hb, sizeof opt_host - 1);
            if (*h == ':') opt_oport = atoi (h + 1);
        }
    }

#ifdef _WIN32
    WSADATA wsa; WSAStartup (MAKEWORD (2,2), &wsa);
#endif

    SDL_SetMainReady ();
    if (!SDL_Init (SDL_INIT_VIDEO)) { fprintf (stderr, "SDL_Init: %s\n", SDL_GetError ()); return 1; }
    SDL_Window* win = SDL_CreateWindow ("aiDoom AI Director", WINW, WINH, 0);
    ren = SDL_CreateRenderer (win, NULL);
    SDL_SetRenderVSync (ren, 1);
    font_init ();

    g_lock = SDL_CreateMutex ();
    SDL_Thread* th = SDL_CreateThread (worker, "director", NULL);

    int running = 1;
    while (running)
    {
        SDL_Event e;
        while (SDL_PollEvent (&e))
            if (e.type == SDL_EVENT_QUIT) running = 0;

        SDL_SetRenderDrawColor (ren, 16, 18, 24, 255);
        SDL_RenderClear (ren);

        // header
        SDL_LockMutex (g_lock);
        char status[160], sub[160];
        snprintf (status, sizeof status, "%s", g_status);
        snprintf (sub,    sizeof sub,    "%s", g_sub);
        int n = g_log_n;
        SDL_UnlockMutex (g_lock);

        text (10, 8,  "aiDoom AI Director  (Ollama LLM -> monster tactics)", 120, 200, 255);
        text (10, 26, status, 230, 230, 120);
        text (10, 42, sub,    180, 200, 180);
        SDL_SetRenderDrawColor (ren, 60, 64, 80, 255);
        { SDL_FRect ln = { 8, 60, WINW - 16, 1 }; SDL_RenderFillRect (ren, &ln); }

        // log (last lines that fit)
        int rows = (WINH - 70) / (FONT_CH + 2);
        int start = n - rows; if (start < 0) start = 0;
        float y = 66;
        for (int i = start; i < n; i++)
        {
            SDL_LockMutex (g_lock);
            char buf[LOG_COLS]; snprintf (buf, sizeof buf, "%s", g_log[i % LOG_MAX]);
            SDL_UnlockMutex (g_lock);
            text (10, y, buf, 200, 205, 210);
            y += FONT_CH + 2;
        }

        SDL_RenderPresent (ren);
    }

    g_quit = 1;
    SDL_WaitThread (th, NULL);
    SDL_DestroyMutex (g_lock);
    SDL_DestroyRenderer (ren);
    SDL_DestroyWindow (win);
    SDL_Quit ();
#ifdef _WIN32
    WSACleanup ();
#endif
    return 0;
}
