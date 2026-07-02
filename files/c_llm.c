// c_llm.c -- console "llm"/"tellme"/"buddy" command: send a line to the local Ollama server and
// print the reply in the console.  The HTTP request runs on a background SDL thread so the game
// loop never blocks; C_LLM_Poll() (called each frame from C_Drawer) prints the reply when it lands.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define CLOSESOCK closesocket
  typedef SOCKET sock_t;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <errno.h>
  #define CLOSESOCK close
  typedef int sock_t;
  #define INVALID_SOCKET (-1)
#endif

void C_Printf (const char* fmt, ...);

// --- Ollama config (read once from run/aidoom.cfg, with sane defaults) ---
static char llm_host[128] = "127.0.0.1";
static int  llm_port = 11434;
static char llm_model[128] = "mistral";

static void llm_load_cfg (void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    // the config lives next to the game data; try run/ID0-relative and cwd
    const char* paths[] = { "aidoom.cfg", "run/aidoom.cfg", "../aidoom.cfg", NULL };
    FILE* f = NULL; int i;
    for (i = 0; paths[i] && !f; i++) f = fopen (paths[i], "r");
    if (!f) return;
    char line[256], key[128], val[128];
    while (fgets (line, sizeof line, f))
    {
        if (sscanf (line, "%127s %127[^\n\r]", key, val) != 2) continue;
        if      (!strcmp (key, "ollama_host"))  { strncpy (llm_host, val, sizeof llm_host - 1); }
        else if (!strcmp (key, "ollama_port"))  { llm_port = atoi (val); }
        else if (!strcmp (key, "ollama_model")) { strncpy (llm_model, val, sizeof llm_model - 1); }
    }
    fclose (f);
}

// --- JSON escape a string into dst (for the request body) ---
static void json_escape (char* dst, size_t cap, const char* src)
{
    size_t o = 0;
    for (; *src && o + 2 < cap; src++)
    {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
        else if (c == '\n') { dst[o++] = '\\'; if (o<cap) dst[o++]='n'; }
        else if (c == '\r') { }
        else if (c == '\t') { dst[o++] = '\\'; if (o<cap) dst[o++]='t'; }
        else if (c < 0x20)  { }
        else dst[o++] = c;
    }
    dst[o] = 0;
}

// --- minimal blocking HTTP POST; returns malloc'd body (caller frees) or NULL ---
static char* http_post (const char* host, int port, const char* path, const char* body)
{
    struct addrinfo hints, *res = NULL;
    char portstr[16];
    sock_t s;
    memset (&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    snprintf (portstr, sizeof portstr, "%d", port);
    if (getaddrinfo (host, portstr, &hints, &res) != 0 || !res) return NULL;
    s = socket (res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) { freeaddrinfo (res); return NULL; }
    if (connect (s, res->ai_addr, (int)res->ai_addrlen) != 0) { CLOSESOCK (s); freeaddrinfo (res); return NULL; }
    freeaddrinfo (res);

    char hdr[512];
    int blen = (int) strlen (body);
    int hn = snprintf (hdr, sizeof hdr,
        "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n", path, host, port, blen);
    if (send (s, hdr, hn, 0) < 0 || send (s, body, blen, 0) < 0) { CLOSESOCK (s); return NULL; }

    size_t cap = 65536, len = 0;
    char* resp = malloc (cap);
    if (!resp) { CLOSESOCK (s); return NULL; }
    for (;;)
    {
        if (len + 4096 + 1 > cap)
        {
            if (cap >= 8*1024*1024) break;   // runaway guard
            cap *= 2; char* nb = realloc (resp, cap);
            if (!nb) { free (resp); CLOSESOCK (s); return NULL; }
            resp = nb;
        }
        int r = (int) recv (s, resp + len, 4096, 0);
        if (r <= 0) break;
        len += r;
    }
    CLOSESOCK (s);
    resp[len] = 0;
    char* b = strstr (resp, "\r\n\r\n");
    char* out = b ? strdup (b + 4) : NULL;
    free (resp);
    return out;
}

// --- extract "content":"..." from the /api/chat reply, unescaping into dst ---
static void extract_content (char* dst, size_t cap, const char* json)
{
    dst[0] = 0;
    const char* p = strstr (json, "\"content\"");
    if (!p) { strncpy (dst, "(no reply)", cap-1); dst[cap-1]=0; return; }
    p = strchr (p, ':'); if (!p) return;
    p++; while (*p==' ') p++;
    if (*p != '"') return;
    p++;
    size_t o = 0;
    for (; *p && *p != '"' && o + 1 < cap; p++)
    {
        if (*p == '\\')
        {
            p++;
            if      (*p=='n') dst[o++]='\n';
            else if (*p=='t') dst[o++]='\t';
            else if (*p=='"') dst[o++]='"';
            else if (*p=='\\') dst[o++]='\\';
            else if (*p=='/') dst[o++]='/';
            else if (!*p) break;
            else dst[o++]=*p;
        }
        else dst[o++] = *p;
    }
    dst[o] = 0;
    if (!o) { strncpy (dst, "(empty reply)", cap-1); dst[cap-1]=0; }
}

// --- shared state between the worker thread and the main-loop poll ---
static SDL_AtomicInt llm_ready;     // set by worker when reply/err is stored
static SDL_AtomicInt llm_busy;      // a request is in flight
static char          llm_reply[8192];

static int SDLCALL llm_worker (void* data)
{
    char* prompt = (char*) data;
    char esc[4096], *body = malloc (8192), *resp;
    llm_load_cfg ();
    json_escape (esc, sizeof esc, prompt);
    free (prompt);
    snprintf (body, 8192,
        "{\"model\":\"%s\",\"stream\":false,\"think\":false,"
        "\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}", llm_model, esc);
    resp = http_post (llm_host, llm_port, "/api/chat", body);
    free (body);
    if (!resp)
        snprintf (llm_reply, sizeof llm_reply, "[llm] no response from %s:%d (is Ollama running?)",
                  llm_host, llm_port);
    else { extract_content (llm_reply, sizeof llm_reply, resp); free (resp); }
    SDL_SetAtomicInt (&llm_ready, 1);
    return 0;
}

// Public: kick off a query.  Text is echoed; the reply arrives asynchronously.
void C_LLM_Ask (const char* prompt)
{
    if (SDL_GetAtomicInt (&llm_busy)) { C_Printf ("[llm] still waiting for the previous reply..."); return; }
    if (!prompt || !*prompt) { C_Printf ("usage: llm <message>   (alias: tellme, buddy)"); return; }
    llm_load_cfg ();
    SDL_SetAtomicInt (&llm_busy, 1);
    SDL_SetAtomicInt (&llm_ready, 0);
    C_Printf ("[llm -> %s] %s", llm_model, prompt);
    SDL_Thread* th = SDL_CreateThread (llm_worker, "llm", strdup (prompt));
    if (th) SDL_DetachThread (th);
    else { SDL_SetAtomicInt (&llm_busy, 0); C_Printf ("[llm] could not start request thread"); }
}

// Public: called each frame; prints the reply once the worker has it.
void C_LLM_Poll (void)
{
    if (SDL_GetAtomicInt (&llm_ready))
    {
        SDL_SetAtomicInt (&llm_ready, 0);
        SDL_SetAtomicInt (&llm_busy, 0);
        C_Printf ("[llm] %s", llm_reply);
    }
}
