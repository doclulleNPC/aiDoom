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
#if defined(__GLIBC__)
#include <malloc.h>		// malloc_trim -- return freed arena pages to the OS
#endif

// Each round allocates+frees a JSON node tree for the Ollama reply.  glibc keeps
// those freed pages in its arena instead of returning them, so RSS climbs without
// bound across rounds even though nothing leaks.  Hand the freed memory back after
// every round so the director's footprint stays flat.
static void mem_trim (void)
{
#if defined(__GLIBC__)
    malloc_trim (0);
#endif
}

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <psapi.h>                 // GetProcessMemoryInfo (self RSS readout)
  #pragma comment(lib, "ws2_32.lib")
  #pragma comment(lib, "psapi.lib")
  typedef SOCKET sock_t;
  #define CLOSESOCK(s) closesocket(s)
  #define SOCKERR      INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int sock_t;
  #define CLOSESOCK(s) close(s)
  #define SOCKERR      (-1)
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include "font_atlas.h"
#include "../files/buddydoom_icon.h"	// shared 64x64 RGBA window icon (from buddydoom.ico)

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
    // Allocate proportional to THIS string (raw span to the closing quote), not
    // strlen(remaining).  The old worst-case-remaining sizing was O(strings x
    // document size) -- quadratic -- and ballooned the heap to multiple GB when
    // parsing a large (runaway) LLM reply.  The unescaped result is never longer
    // than the raw span (every escape consumes >= the bytes it emits).
    const char* e = s;
    while (*e && *e != '"') { if (*e == '\\' && e[1]) e += 2; else e++; }
    char*  out = malloc ((size_t)(e - s) + 1);
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
            const char* loop0 = s;	// guard: bail if an iteration makes no progress
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
            // Malformed input (e.g. a value strtod can't consume) could leave s
            // unmoved -> the loop would spin forever calloc'ing children and balloon
            // RAM to GBs.  If nothing was consumed this iteration, stop.
            if (s == loop0) break;
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
    else {
        const char* s0 = s;
        j->t = JNUM; j->num = strtod (s, (char**)&s);
        if (s == s0 && *s) s++;	// garbage strtod can't consume -> skip 1 char, always advance
    }
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
//  Config (buddydoom.cfg, next to the binary)
// ===========================================================================

static char cfg_host[128]  = "localhost";
static int  cfg_oport      = 11434;
static char cfg_model[128] = "ministral-3:8b";

static void load_cfg (const char* dir)
{
    char path[512];
    snprintf (path, sizeof path, "%s/buddydoom.cfg", dir);
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
static double g_tokps     = 0;   // Ollama generation speed (tok/s), smoothed; 0 = none yet

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

// Update the live tok/s readout from one Ollama reply's eval_count / eval_duration (ns).
// Free: the numbers ride in the response the director already fetched -- no extra request.
// Lightly smoothed (EMA) so the header figure doesn't jitter round to round.
static void set_tokps (double eval_count, double eval_duration_ns)
{
    if (eval_count <= 0 || eval_duration_ns <= 0) return;
    double tps = eval_count * 1e9 / eval_duration_ns;
    SDL_LockMutex (g_lock);
    g_tokps = (g_tokps > 0) ? (0.6 * g_tokps + 0.4 * tps) : tps;
    SDL_UnlockMutex (g_lock);
}

// ===========================================================================
//  Sockets
// ===========================================================================

static void set_blocking (sock_t s, int blocking)
{
#ifdef _WIN32
    u_long nb = blocking ? 0 : 1;
    ioctlsocket (s, FIONBIO, &nb);
#else
    int fl = fcntl (s, F_GETFL, 0);
    if (fl >= 0) fcntl (s, F_SETFL, blocking ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

// Bound recv()/send() so a stalled peer (e.g. Ollama dropping mid-reply) can never
// block the worker forever.
static void set_io_timeout (sock_t s, int sec)
{
#ifdef _WIN32
    DWORD ms = (DWORD) sec * 1000;
    setsockopt (s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
    setsockopt (s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof ms);
#else
    struct timeval tv; tv.tv_sec = sec; tv.tv_usec = 0;
    setsockopt (s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt (s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

static sock_t tcp_connect (const char* host, int port, int timeout_s)
{
    char portstr[16]; snprintf (portstr, sizeof portstr, "%d", port);
    struct addrinfo hints, *res = NULL;
    memset (&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo (host, portstr, &hints, &res) != 0 || !res) return SOCKERR;
    sock_t s = socket (res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == SOCKERR) { freeaddrinfo (res); return SOCKERR; }
    if (timeout_s <= 0) timeout_s = 5;

    // Non-blocking connect with a real timeout: an unreachable/lost host now fails
    // in `timeout_s` instead of blocking the director loop until the OS TCP timeout.
    set_blocking (s, 0);
    int rc = connect (s, res->ai_addr, (int)res->ai_addrlen);
    if (rc != 0)
    {
#ifdef _WIN32
        int pending = (WSAGetLastError () == WSAEWOULDBLOCK);
#else
        int pending = (errno == EINPROGRESS);
#endif
        if (!pending) { CLOSESOCK (s); freeaddrinfo (res); return SOCKERR; }
        fd_set wf; FD_ZERO (&wf); FD_SET (s, &wf);
        struct timeval tv; tv.tv_sec = timeout_s; tv.tv_usec = 0;
        if (select ((int)s + 1, NULL, &wf, NULL, &tv) <= 0)
            { CLOSESOCK (s); freeaddrinfo (res); return SOCKERR; }	// timed out
        int err = 0;
#ifdef _WIN32
        int el = sizeof err;
#else
        socklen_t el = sizeof err;
#endif
        getsockopt (s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
        if (err) { CLOSESOCK (s); freeaddrinfo (res); return SOCKERR; }
    }
    freeaddrinfo (res);
    set_blocking (s, 1);
    set_io_timeout (s, timeout_s);
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

// recv_line() outcomes that aren't a byte count.  A recv() timeout
// (SO_RCVTIMEO) means "no reply yet, but the peer is still connected" -- it must
// NOT be treated as a disconnect, because the game stops answering during the
// inter-map intermission yet keeps the TCP connection open.
#define RECV_EOF      (-1)   // peer closed / hard error -> reconnect
#define RECV_TIMEOUT  (-2)   // SO_RCVTIMEO elapsed, peer still up -> keep waiting

// True if the last socket error was a timeout / would-block (not a real drop).
static int sock_would_block (void)
{
#ifdef _WIN32
    int e = WSAGetLastError ();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

// read one '\n'-terminated line from the game socket.  Returns the line length
// (>=0), or RECV_EOF / RECV_TIMEOUT.  out[] is always NUL-terminated.
static int recv_line (sock_t s, char* out, int cap)
{
    int n = 0;
    while (n < cap - 1)
    {
        char c;
        int r = (int) recv (s, &c, 1, 0);
        if (r == 0) { out[n] = '\0'; return RECV_EOF; }                          // peer closed
        if (r < 0)  { out[n] = '\0'; return sock_would_block () ? RECV_TIMEOUT   // stall, still up
                                                                : RECV_EOF; }    // reset/error
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

// Ceiling on a single HTTP response body.  A director plan is a few KB; this is
// the runaway-generation backstop (see the read loop below).
#define HTTP_MAX_RESP (1 * 1024 * 1024)   /* 1 MB -- a real director reply is < 10 KB */

static char* http_post_json (const char* host, int port, const char* path, const char* json_body)
{
    sock_t s = tcp_connect (host, port, 5);
    if (s == SOCKERR) return NULL;
    set_io_timeout (s, 120);	// allow slow generation, but never hang forever

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

    // Read the whole response until the server closes the connection, but
    // HARD-CAP the buffer.  A stuck/runaway Ollama generation (some models loop
    // and emit tokens without end) made this grow by doubling -- 64K, 128K, ...
    // -- to multiple GB, exhausting RAM and freezing the machine.  A real
    // director reply is a few KB; anything past the cap is a malfunction, so we
    // stop and treat it as an error rather than buffering gigabytes.
    size_t cap = 65536, len = 0;
    char*  resp = malloc (cap);
    if (!resp) { CLOSESOCK (s); return NULL; }
    int over = 0;
    for (;;)
    {
        if (len + 4096 + 1 > cap)
        {
            if (cap >= HTTP_MAX_RESP) { over = 1; break; }   // runaway -> stop reading
            cap *= 2;
            char* nb = realloc (resp, cap);
            if (!nb) { free (resp); CLOSESOCK (s); return NULL; }
            resp = nb;
        }
        int r = (int) recv (s, resp + len, 4096, 0);
        if (r <= 0) break;
        len += r;
    }
    CLOSESOCK (s);
    if (over) { free (resp); return NULL; }   // oversized response: treat as error
    resp[len] = '\0';

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
    "items, or goto a specific spot. The buddy field includes \\\"route\\\":[[x,y],...] -- reachable "
    "waypoints toward the human; for a goto, copy x,y from that route (or aim near a monster/item) "
    "so the spot is walkable. Buddy orders: engage, defend, hold, regroup, retreat, grab, goto. "
    "(3) PACING via the \\\"director\\\" field {\\\"intensity\\\":0-100,\\\"state\\\":0|1|2,\\\"recent_dmg\\\":n,"
    "\\\"ammo_pct\\\":0-100}: shape the Left-4-Dead tension curve. When intensity is LOW (<30) and few "
    "monsters are alive, SPAWN a few extra monsters behind the survivors to build pressure; when "
    "intensity is HIGH (>65) -- or ammo_pct is low and the human is hurt -- ease off: relax and/or "
    "drop an item so they recover. Spawn sparingly (a wave, then let it breathe), not every round. "
    "MAP AWARENESS: every entity has a \\\"region\\\" (room id); \\\"regions\\\":[[id,x,y],...] are room "
    "centres and \\\"links\\\":[[a,b,type],...] how rooms connect (type open/door/locked). Monsters "
    "also give \\\"see_buddy\\\" and distances \\\"d_player\\\"/\\\"d_buddy\\\". Use these: prioritise "
    "monsters close to or seeing the buddy, flank via alternate links, and don't send the buddy "
    "through a locked door (route around it or regroup with the human who holds the key). "
    "Respond ONLY with JSON of the form {\\\"commands\\\":[{\\\"order\\\":\\\"flank_left\\\","
    "\\\"ids\\\":[1,3]}],\\\"buddy\\\":{\\\"order\\\":\\\"engage\\\",\\\"focus\\\":2}}. "
    "For goto use {\\\"buddy\\\":{\\\"order\\\":\\\"goto\\\",\\\"x\\\":123,\\\"y\\\":456}}. "
    "To pace, optionally add a \\\"spawn\\\" field: {\\\"spawn\\\":{\\\"type\\\":\\\"imp\\\",\\\"count\\\":3}} spawns "
    "monsters (types: zombie, shotgun, chaingun, imp, pinky, spectre, lost, caco, pain, knight, baron, "
    "revenant, mancubus, arachnotron), {\\\"spawn\\\":{\\\"item\\\":\\\"medkit\\\"}} (or ammo) drops an item, "
    "{\\\"spawn\\\":{\\\"relax\\\":true}} calms the encounter. Omit \\\"spawn\\\" most rounds. "
    "Every monster id must appear in exactly one command. Omit \\\"buddy\\\" if the state has no buddy. "
    "Distances are precomputed in map units (d_player / d_buddy; buddy has d_player). You also get "
    "the last few rounds above the state -- use them for continuity: keep the buddy on a consistent "
    "tactic and don't flip-flop the same monsters every round.";

static int is_order (const char* o)
{
    static const char* k[] = { "chase","hold","fallback","flank_left","flank_right",
                               "ambush","focus_fire","use_door", NULL };
    for (int i = 0; k[i]; i++) if (!strcmp (o, k[i])) return 1;
    return 0;
}

static int is_buddy_order (const char* o)
{
    static const char* k[] = { "engage","defend","hold","regroup","retreat","grab","goto","auto", NULL };
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
    char hist[4][120];          // rolling cross-round memory (last N rounds)
    int  nh = 0;
    int  round = 0;
    sock_t gs = SOCKERR;

reconnect:
    // 1. (re)connect to the game.  Retry forever while it starts up OR while it
    //    is between maps -- never give up, so the director auto-rejoins the next
    //    level instead of dying on the first hiccup.
    gs = SOCKERR;
    for (int i = 0; !g_quit; i++)
    {
        gs = tcp_connect ("127.0.0.1", opt_gameport, 5);
        if (gs != SOCKERR) break;
        if (i == 0) { setstatus ("waiting for BuddyDoom ..."); logln ("waiting for BuddyDoom on 127.0.0.1:%d", opt_gameport); }
        SDL_Delay (500);
    }
    if (g_quit) { setstatus ("disconnected"); return 0; }

    char st[200];
    snprintf (st, sizeof st, "connected  model=%s  ollama=%s:%d", opt_model, opt_host, opt_oport);
    setstatus (st);
    logln ("connected to game; model=%s ollama=http://%s:%d/api/chat", opt_model, opt_host, opt_oport);

    send_all (gs, "wake\n", 5);
    recv_line (gs, line, sizeof line);     // "ok"

    while (!g_quit)
    {
        round++;
        if (send_all (gs, "observe\n", 8) != 0) { logln ("game send failed; reconnecting..."); goto dropped; }

        // The game stops answering during the inter-map intermission/finale but
        // keeps the TCP connection open, so a recv timeout there is NOT a
        // disconnect -- keep waiting for the reply.  Do NOT resend observe on a
        // timeout: a second request would desync the request/reply pairing.
        int rr;
        do {
            rr = recv_line (gs, line, sizeof line);
            if (rr == RECV_TIMEOUT && !g_quit)
                setsub ("game busy (intermission?) -- waiting...");
        } while (rr == RECV_TIMEOUT && !g_quit);
        if (g_quit) break;
        if (rr <= 0) { logln ("connection closed by game; reconnecting..."); goto dropped; }

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
            mem_trim ();
            SDL_Delay ((int)(opt_period * 1000));
            continue;
        }

        // build the chat request body (state embedded as escaped strings)
        char esc[6000];
        json_escape (esc, sizeof esc, line);
        // cross-round memory: the last few rounds' state+decision, so the model has
        // continuity (trends, what it just ordered) beyond one stateless call.
        char histesc[800] = "";
        if (nh) { char j[700] = ""; int hi;
            for (hi = 0; hi < nh; hi++) { strncat (j, hist[hi], sizeof j - strlen(j) - 1);
                                          strncat (j, "\n",    sizeof j - strlen(j) - 1); }
            json_escape (histesc, sizeof histesc, j); }
        char* body = malloc (16000);
        snprintf (body, 16000,
            // "think":false turns OFF reasoning -- a thinking model (deepseek-r1, qwen3,
            // supergemma, ...) otherwise spends its whole token budget in a "thinking" field
            // and returns an EMPTY message.content, so the director got no orders.  Harmless
            // on non-thinking models.  With it the model writes the JSON straight to content.
            "{\"model\":\"%s\",\"stream\":false,\"format\":\"json\",\"think\":false,"
            // num_predict caps generation so a model that loops can't run away
            // (the runaway response was what ballooned the director's RAM).
            "\"options\":{\"temperature\":0.6,\"num_predict\":768},\"messages\":["
            "{\"role\":\"system\",\"content\":\"%s\"},"
            "{\"role\":\"user\",\"content\":\"Recent rounds (oldest first):\\n%s\\nGame state: %s\\nAssign orders now as JSON.\"}]}",
            opt_model, SYSTEM_PROMPT, histesc, esc);

        Uint64 t0 = SDL_GetTicks ();
        char*  resp = http_post_json (opt_host, opt_oport, "/api/chat", body);
        free (body);
        double dt = (SDL_GetTicks () - t0) / 1000.0;

        if (!resp) { logln ("round %d: ollama error (no response)", round); json_free (state); mem_trim (); SDL_Delay ((int)(opt_period*1000)); continue; }

        // A real director reply is a few KB.  If the model returned a huge (HTTP-
        // capped) blob, parsing it into a node tree is what balloons RAM -- skip it.
        if (strlen (resp) > 200000)
        {
            logln ("round %d: oversized ollama reply (%zu B) -- skipping", round, strlen (resp));
            free (resp); json_free (state); mem_trim ();
            SDL_Delay ((int)(opt_period*1000)); continue;
        }

        // outer: {"message":{"content":"<json string>"}, ...}
        json* outer   = json_parse (resp);
        json* message = json_get (outer, "message");
        json* content = message ? json_get (message, "content") : NULL;
        int   issued  = 0;

        // Live generation speed: Ollama reports eval_count + eval_duration (ns) on every
        // non-streamed reply, so the tok/s figure is free -- no extra benchmark request.
        json* ec = json_get (outer, "eval_count");
        json* ed = json_get (outer, "eval_duration");
        if (ec && ec->t == JNUM && ed && ed->t == JNUM)
            set_tokps (ec->num, ed->num);

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
                        char bc[128]; int bn; char tag[24];
                        if (!strcmp (bl, "goto"))
                        {
                            json* jx = json_get (bo, "x");
                            json* jy = json_get (bo, "y");
                            int gx = (jx && jx->t == JNUM) ? (int) jx->num : 0;
                            int gy = (jy && jy->t == JNUM) ? (int) jy->num : 0;
                            bn = snprintf (bc, sizeof bc, "buddy order=goto x=%d y=%d for=105\n", gx, gy);
                            snprintf (tag, sizeof tag, "goto(%d,%d)", gx, gy);
                        }
                        else
                        {
                            json* bf = json_get (bo, "focus");
                            int fid = (bf && bf->t == JNUM) ? (int) bf->num : 0;
                            int fok = 0;
                            for (int i = 0; i < nids; i++) if (ids[i] == fid) { fok = 1; break; }
                            bn = fok ? snprintf (bc, sizeof bc, "buddy order=%s focus=%d for=105\n", bl, fid)
                                     : snprintf (bc, sizeof bc, "buddy order=%s for=105\n", bl);
                            snprintf (tag, sizeof tag, "%s%s", bl, fok ? "*" : "");
                        }
                        send_all (gs, bc, bn);
                        recv_line (gs, line, sizeof line);   // "ok"
                        char piece[48]; snprintf (piece, sizeof piece, "buddy:%s ", tag);
                        strncat (summary, piece, sizeof summary - strlen (summary) - 1);
                        issued++;
                    }
                }
            }
            // L4D spawn director -- {"spawn":{"type":"imp","count":2}} |
            //   {"spawn":{"item":"medkit"}} | {"spawn":{"relax":true}}.  Names are
            //   sanitised to lowercase letters so they can't inject extra commands.
            {
                json* sp = json_get (plan, "spawn");
                if (sp && sp->t == JOBJ)
                {
                    json* jrelax = json_get (sp, "relax");
                    json* jitem  = json_get (sp, "item");
                    json* jtype  = json_get (sp, "type");
                    char  sc[128]; int sn = 0; char tag[48] = "";
                    char  nm[32];
                    if (jrelax && jrelax->t == JBOOL && jrelax->num)
                        { sn = snprintf (sc, sizeof sc, "director relax\n"); snprintf (tag, sizeof tag, "relax"); }
                    else if (jitem && jitem->t == JSTR)
                    {
                        int i = 0; for (const char* p = jitem->str; *p && i < 31; p++)
                            if (*p >= 'A' && *p <= 'Z') nm[i++] = *p + 32; else if (*p >= 'a' && *p <= 'z') nm[i++] = *p;
                        nm[i] = 0;
                        if (i) { sn = snprintf (sc, sizeof sc, "spawn item=%s\n", nm); snprintf (tag, sizeof tag, "item:%s", nm); }
                    }
                    else if (jtype && jtype->t == JSTR)
                    {
                        json* jc = json_get (sp, "count");
                        int cnt = (jc && jc->t == JNUM) ? (int) jc->num : 1;
                        int i = 0; for (const char* p = jtype->str; *p && i < 31; p++)
                            if (*p >= 'A' && *p <= 'Z') nm[i++] = *p + 32; else if (*p >= 'a' && *p <= 'z') nm[i++] = *p;
                        nm[i] = 0;
                        if (cnt < 1) cnt = 1; if (cnt > 8) cnt = 8;
                        if (i) { sn = snprintf (sc, sizeof sc, "spawn type=%s count=%d\n", nm, cnt);
                                 snprintf (tag, sizeof tag, "spawn:%sx%d", nm, cnt); }
                    }
                    if (sn > 0)
                    {
                        send_all (gs, sc, sn);
                        recv_line (gs, line, sizeof line);   // "ok"
                        char piece[56]; snprintf (piece, sizeof piece, "dir:%s ", tag);
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

            // remember this round (ring buffer) for next round's prompt
            {
                json* bud2 = json_get (state, "buddy");
                json* bhp  = bud2 ? json_get (bud2, "health") : NULL;
                char  h[120];
                snprintf (h, sizeof h, "r%d: php=%d bhp=%d mon=%d -> %s", round,
                          hp  && hp->t==JNUM  ? (int)hp->num  : -1,
                          bhp && bhp->t==JNUM ? (int)bhp->num : -1, nids,
                          issued ? summary : "(none)");
                if (nh < 4) snprintf (hist[nh++], 120, "%s", h);
                else { for (int k = 0; k < 3; k++) snprintf (hist[k], 120, "%s", hist[k+1]);
                       snprintf (hist[3], 120, "%s", h); }
            }
        }
        else
        {
            // No usable message.content -- surface WHY.  Ollama puts failures in a top-level
            // {"error":"..."} (most often: the model isn't pulled on this host), which the old
            // generic "no content" hid.  Else log a raw snippet so the real reply is visible.
            json* err = json_get (outer, "error");
            if (err && err->t == JSTR)
                logln ("round %d: ollama error: %.150s", round, err->str);
            else {
                char snip[200]; snprintf (snip, sizeof snip, "%.180s", resp ? resp : "(null)");
                for (char* q = snip; *q; q++) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
                logln ("round %d: ollama returned no content (llm=%.1fs): %s", round, dt, snip);
            }
        }

        char s2[160];
        snprintf (s2, sizeof s2, "round %d  monsters=%d  last LLM %.1fs  orders=%d", round, nids, dt, issued);
        setsub (s2);

        json_free (outer); free (resp); json_free (state);
        mem_trim ();			// hand this round's freed pages back to the OS
        SDL_Delay ((int)(opt_period * 1000));
    }

    // clean exit: the GUI window was closed (g_quit).
    CLOSESOCK (gs);
    setstatus ("disconnected");
    return 0;

dropped:
    // The game socket dropped for real (EOF/reset -- game quit or crashed).  Close
    // it and loop back to reconnect so the director rejoins automatically when the
    // game returns; only stop if the user is quitting the director itself.
    CLOSESOCK (gs);
    if (g_quit) { setstatus ("disconnected"); return 0; }
    setstatus ("reconnecting...");
    SDL_Delay (500);
    goto reconnect;
}

// ===========================================================================
//  SDL3 GUI
// ===========================================================================

#define WINW 760
#define WINH 460

// Resident memory of THIS process, in KB (0 if unavailable).  Shown live in the
// header so a memory leak is visible at a glance (this director had one).
static long self_rss_kb (void)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo (GetCurrentProcess (), &pmc, sizeof pmc))
        return (long)(pmc.WorkingSetSize / 1024);
    return 0;
#else
    FILE* f = fopen ("/proc/self/statm", "r");
    if (!f) return 0;
    long pages_total = 0, pages_rss = 0;
    if (fscanf (f, "%ld %ld", &pages_total, &pages_rss) != 2) pages_rss = 0;
    fclose (f);
    return pages_rss * (sysconf (_SC_PAGESIZE) / 1024);
#endif
}

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

    // load buddydoom.cfg from the binary's directory first
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
    SDL_Window* win = SDL_CreateWindow ("BuddyDoom AI Director", WINW, WINH, 0);
    {
        // Window/taskbar icon from the shared buddydoom.ico (same as the game).
        SDL_Surface* icon = SDL_CreateSurfaceFrom (
            BUDDYDOOM_ICON_W, BUDDYDOOM_ICON_H, SDL_PIXELFORMAT_RGBA32,
            (void *)buddydoom_icon_rgba, BUDDYDOOM_ICON_W*4);
        if (icon) { SDL_SetWindowIcon (win, icon); SDL_DestroySurface (icon); }
    }
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
        double tokps = g_tokps;
        int n = g_log_n;
        SDL_UnlockMutex (g_lock);

        text (10, 8,  "aiDoom AI Director  (Ollama LLM -> monster + buddy tactics)", 120, 200, 255);

        // live memory readout (right-aligned on the title row): green when small,
        // amber/red as it grows -- a runaway leak is now obvious at a glance.
        long rss = self_rss_kb ();
        char mem[48];
        snprintf (mem, sizeof mem, "mem %.1f MB", rss / 1024.0);
        Uint8 mr = 120, mg = 220, mb = 120;
        if      (rss > 500*1024) { mr = 240; mg = 80;  mb = 80;  }   // > 500 MB: red
        else if (rss > 200*1024) { mr = 240; mg = 200; mb = 80;  }   // > 200 MB: amber
        text (WINW - (int)strlen (mem) * FONT_CW - 10, 8, mem, mr, mg, mb);

        text (10, 26, status, 230, 230, 120);
        // live Ollama generation speed, right-aligned on the status row (green)
        if (tokps > 0) {
            char tk[48]; snprintf (tk, sizeof tk, "%.1f tok/s", tokps);
            text (WINW - (int)strlen (tk) * FONT_CW - 10, 26, tk, 130, 210, 150);
        }
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
