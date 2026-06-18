// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Clean-room Chocolate-Doom-compatible UDP/packet layer (see i_udp.h).
//	Stage 1: big-endian packet buffer + UDP transport + server QUERY, so we
//	can interoperate with Chocolate/Crispy peers on the wire.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  // <rpcndr.h> (pulled in by winsock2.h) typedefs `boolean` as unsigned char,
  // which collides with DOOM's `typedef int boolean` (doomtype.h).  DOOM's
  // 4-byte boolean is load-bearing for on-disk WAD struct layouts, so it must
  // stay int -- rename Windows' boolean for the duration of the system-header
  // includes (DOOM's is established afterwards via i_udp.h).
  #define boolean win_boolean
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #undef boolean
  typedef int socklen_t;
  #define CLOSESOCK closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <sys/select.h>
  #include <sys/time.h>
  #define CLOSESOCK close
  #define INVALID_SOCKET (-1)
#endif

#include "i_udp.h"

//
// Packet buffer.  All multi-byte integers are big-endian on the wire, matching
// Chocolate-Doom's net_packet.c.
//
net_packet_t* NET_NewPacket (int initial_size)
{
    net_packet_t* p = malloc (sizeof(*p));
    if (initial_size < 16) initial_size = 16;
    p->alloced = initial_size;
    p->data = malloc (initial_size);
    p->len = 0;
    p->pos = 0;
    return p;
}

void NET_FreePacket (net_packet_t* p)
{
    if (p) { free (p->data); free (p); }
}

static void NET_Grow (net_packet_t* p, size_t need)
{
    while (p->len + need > p->alloced)
    {
	p->alloced *= 2;
	p->data = realloc (p->data, p->alloced);
    }
}

void NET_WriteInt8 (net_packet_t* p, unsigned int i)
{
    NET_Grow (p, 1);
    p->data[p->len++] = i & 0xff;
}

void NET_WriteInt16 (net_packet_t* p, unsigned int i)
{
    NET_Grow (p, 2);
    p->data[p->len++] = (i >> 8) & 0xff;
    p->data[p->len++] = i & 0xff;
}

void NET_WriteInt32 (net_packet_t* p, unsigned int i)
{
    NET_Grow (p, 4);
    p->data[p->len++] = (i >> 24) & 0xff;
    p->data[p->len++] = (i >> 16) & 0xff;
    p->data[p->len++] = (i >> 8) & 0xff;
    p->data[p->len++] = i & 0xff;
}

void NET_WriteString (net_packet_t* p, const char* s)
{
    size_t n = strlen (s) + 1;
    NET_Grow (p, n);
    memcpy (p->data + p->len, s, n);
    p->len += n;
}

boolean NET_ReadInt8 (net_packet_t* p, unsigned int* i)
{
    if (p->pos + 1 > p->len) return false;
    *i = p->data[p->pos++];
    return true;
}

boolean NET_ReadInt16 (net_packet_t* p, unsigned int* i)
{
    if (p->pos + 2 > p->len) return false;
    *i = (p->data[p->pos] << 8) | p->data[p->pos+1];
    p->pos += 2;
    return true;
}

boolean NET_ReadInt32 (net_packet_t* p, unsigned int* i)
{
    if (p->pos + 4 > p->len) return false;
    *i = ((unsigned)p->data[p->pos] << 24) | (p->data[p->pos+1] << 16)
       | (p->data[p->pos+2] << 8) | p->data[p->pos+3];
    p->pos += 4;
    return true;
}

boolean NET_ReadSInt8 (net_packet_t* p, int* i)
{
    unsigned int u;
    if (!NET_ReadInt8 (p, &u)) return false;
    *i = (u & 0x80) ? (int)u - 256 : (int)u;
    return true;
}

boolean NET_ReadSInt16 (net_packet_t* p, int* i)
{
    unsigned int u;
    if (!NET_ReadInt16 (p, &u)) return false;
    *i = (u & 0x8000) ? (int)u - 65536 : (int)u;
    return true;
}

unsigned NET_GetTimeMS (void)
{
#ifdef _WIN32
    return (unsigned) GetTickCount ();
#else
    struct timeval tv;
    gettimeofday (&tv, NULL);
    return (unsigned)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
#endif
}

char* NET_ReadString (net_packet_t* p)
{
    static char	buf[1024];
    size_t	n = 0;
    while (p->pos < p->len && p->data[p->pos] != '\0' && n < sizeof(buf)-1)
	buf[n++] = p->data[p->pos++];
    if (p->pos >= p->len)		// no terminator -> malformed
	return NULL;
    p->pos++;				// skip the NUL
    buf[n] = 0;
    return buf;
}

//
// UDP transport.
//
#ifdef _WIN32
static SOCKET		sock = INVALID_SOCKET;
#else
static int		sock = INVALID_SOCKET;
#endif
static struct sockaddr_in destaddr;

boolean UDP_OpenClient (void)
{
#ifdef _WIN32
    WSADATA wsa;
    static int wsainit;
    if (!wsainit) { if (WSAStartup(MAKEWORD(2,2),&wsa)) return false; wsainit=1; }
#endif
    sock = socket (AF_INET, SOCK_DGRAM, 0);
    return sock != INVALID_SOCKET;
}

void UDP_Close (void)
{
    if (sock != INVALID_SOCKET) { CLOSESOCK (sock); sock = INVALID_SOCKET; }
}

boolean UDP_Resolve (const char* hostport, int default_port)
{
    char		host[256];
    int			port = default_port;
    char*		colon;
    struct addrinfo	hints, *res;

    strncpy (host, hostport, sizeof(host)-1);
    host[sizeof(host)-1] = 0;
    colon = strrchr (host, ':');
    if (colon) { *colon = 0; port = atoi (colon+1); }

    memset (&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo (host, NULL, &hints, &res) != 0 || !res)
	return false;

    memcpy (&destaddr, res->ai_addr, sizeof(struct sockaddr_in));
    destaddr.sin_port = htons (port);
    freeaddrinfo (res);
    return true;
}

boolean UDP_Send (net_packet_t* packet)
{
    int n = sendto (sock, (const char*)packet->data, packet->len, 0,
		    (struct sockaddr*)&destaddr, sizeof(destaddr));
    return n == (int)packet->len;
}

net_packet_t* UDP_Recv (int timeout_ms)
{
    fd_set		fds;
    struct timeval	tv;
    byte		buf[2048];
    int			n;

    FD_ZERO (&fds);
    FD_SET (sock, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select (sock+1, &fds, NULL, NULL, &tv) <= 0)
	return NULL;

    n = recvfrom (sock, (char*)buf, sizeof(buf), 0, NULL, NULL);
    if (n <= 0)
	return NULL;

    {
	net_packet_t* p = NET_NewPacket (n);
	memcpy (p->data, buf, n);
	p->len = n;
	p->pos = 0;
	return p;
    }
}

static void NET_WriteSHA1 (net_packet_t* p, const unsigned char* d)
{
    int i;
    for (i = 0 ; i < 20 ; i++)
	NET_WriteInt8 (p, d ? d[i] : 0);	// sha1_digest_t = 20 bytes
}

//
// Stage-2 milestone: perform the Chocolate/Crispy SYN connection handshake and
// report whether the server accepts us (into the lobby) or rejects us (with the
// reason).  Invoked by -connectchoc <host[:port]>.  The version string MUST
// match the target server's PACKAGE_STRING.
//
void I_ConnectChocServer (const char* hostport, const char* version, int gamemode, int gamemission)
{
    net_packet_t*	syn;
    net_packet_t*	resp;
    unsigned		type;
    int			tries;

    printf ("SYN handshake -> %s (as \"%s\", gamemode %d mission %d)...\n",
	    hostport, version, gamemode, gamemission);

    if (!UDP_OpenClient ())  { printf ("  socket error\n"); return; }
    if (!UDP_Resolve (hostport, NET_DEFAULT_PORT))
    { printf ("  could not resolve %s\n", hostport); UDP_Close (); return; }

    syn = NET_NewPacket (64);
    NET_WriteInt16 (syn, NET_PACKET_TYPE_SYN);
    NET_WriteInt32 (syn, NET_MAGIC_NUMBER);
    NET_WriteString (syn, version);		// PACKAGE_STRING
    NET_WriteInt8 (syn, 1);			// num protocols
    NET_WriteString (syn, "CHOCOLATE_DOOM_0");	// our supported protocol
    // net_connect_data_t
    NET_WriteInt8 (syn, gamemode);
    NET_WriteInt8 (syn, gamemission);
    NET_WriteInt8 (syn, 0);			// lowres_turn
    NET_WriteInt8 (syn, 0);			// drone
    NET_WriteInt8 (syn, 8);			// max_players
    NET_WriteInt8 (syn, 0);			// is_freedoom
    NET_WriteSHA1 (syn, NULL);			// wad_sha1sum (zeros for now)
    NET_WriteSHA1 (syn, NULL);			// deh_sha1sum
    NET_WriteInt8 (syn, 0);			// player_class
    NET_WriteString (syn, "sdldoom");		// player name

    // Send the SYN, then read several packets: a REJECTED means refused; a
    // (reliable) SYN, a KEEPALIVE or WAITING_DATA all mean the server created
    // our client connection, i.e. accepted us into the lobby.
    UDP_Send (syn);
    {
	int	got_accept = 0, got_reject = 0, packets = 0;
	for (tries = 0 ; tries < 12 && !got_reject ; tries++)
	{
	    resp = UDP_Recv (500);
	    if (!resp) { UDP_Send (syn); continue; }	// retransmit & keep waiting
	    packets++;
	    NET_ReadInt16 (resp, &type);
	    type &= 0x7fff;				// strip the reliable flag
	    if (type == NET_PACKET_TYPE_REJECTED)
	    {
		char* reason = NET_ReadString (resp);
		printf ("  REJECTED: %s\n", reason ? reason : "(no reason)");
		got_reject = 1;
	    }
	    else if (type == NET_PACKET_TYPE_SYN
		  || type == NET_PACKET_TYPE_KEEPALIVE
		  || type == NET_PACKET_TYPE_WAITING_DATA)
	    {
		if (!got_accept)
		    printf ("  ACCEPTED -- server created our connection"
			    " (first marker: type %u); we're in the lobby.\n", type);
		got_accept = 1;
	    }
	    NET_FreePacket (resp);
	    if (got_accept && packets >= 2)
		break;
	}
	if (!got_accept && !got_reject)
	    printf ("  no usable response from server\n");
    }
    NET_FreePacket (syn);
    UDP_Close ();
}

//
// Stage-1 milestone: query a Chocolate/Crispy server and print its info.
//
void I_QueryChocServer (const char* hostport)
{
    net_packet_t*	req;
    net_packet_t*	resp;
    unsigned		type, state, np, maxp, gm, gmis;
    char		version[256];	// NET_ReadString reuses a static buffer,
    char*		s;		//  so copy each string out before the next read
    char*		descr;
    int			tries;

    printf ("Querying %s (Chocolate/Crispy protocol)...\n", hostport);

    if (!UDP_OpenClient ())
    { printf ("  could not open UDP socket\n"); return; }
    if (!UDP_Resolve (hostport, NET_DEFAULT_PORT))
    { printf ("  could not resolve %s\n", hostport); UDP_Close (); return; }

    req = NET_NewPacket (4);
    NET_WriteInt16 (req, NET_PACKET_TYPE_QUERY);

    resp = NULL;
    for (tries = 0 ; tries < 3 && !resp ; tries++)
    {
	UDP_Send (req);
	resp = UDP_Recv (1000);
    }
    NET_FreePacket (req);

    if (!resp)
    { printf ("  no response (server down, firewalled, or wrong port?)\n"); UDP_Close (); return; }

    if (!NET_ReadInt16 (resp, &type) || type != NET_PACKET_TYPE_QUERY_RESPONSE)
    { printf ("  unexpected reply (type=%u)\n", type); NET_FreePacket (resp); UDP_Close (); return; }

    s = NET_ReadString (resp);
    strncpy (version, s ? s : "?", sizeof(version)-1);
    version[sizeof(version)-1] = 0;
    if (!NET_ReadInt8 (resp, &state) || !NET_ReadInt8 (resp, &np)
	|| !NET_ReadInt8 (resp, &maxp) || !NET_ReadInt8 (resp, &gm)
	|| !NET_ReadInt8 (resp, &gmis))
    { printf ("  malformed response\n"); NET_FreePacket (resp); UDP_Close (); return; }
    descr = NET_ReadString (resp);

    printf ("  server: \"%s\"\n", descr ? descr : "?");
    printf ("  version: %s\n", version);
    printf ("  players: %u/%u   state: %u   gamemode: %u   mission: %u\n",
	    np, maxp, state, gm, gmis);
    printf ("  -> wire-format interop OK.\n");

    NET_FreePacket (resp);
    UDP_Close ();
}
