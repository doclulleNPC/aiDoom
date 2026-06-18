// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	Clean-room implementation of the Chocolate-Doom network wire layer
//	(big-endian packet buffer + UDP transport), so this engine can talk to
//	Chocolate-Doom / Crispy-Doom peers.  This is a from-scratch reimpl of the
//	*protocol* (the wire format is an interop fact); none of Chocolate's
//	GPL source is used.  Stage 1: transport + packet buffer + server QUERY.
//
//-----------------------------------------------------------------------------

#ifndef __I_UDP__
#define __I_UDP__

#include "doomtype.h"

// Chocolate-Doom protocol constants (must match for interop).
#define NET_MAGIC_NUMBER	1454104972U
#define NET_DEFAULT_PORT	2342

typedef enum
{
    NET_PACKET_TYPE_SYN,
    NET_PACKET_TYPE_ACK,		// deprecated
    NET_PACKET_TYPE_REJECTED,
    NET_PACKET_TYPE_KEEPALIVE,
    NET_PACKET_TYPE_WAITING_DATA,
    NET_PACKET_TYPE_GAMESTART,
    NET_PACKET_TYPE_GAMEDATA,
    NET_PACKET_TYPE_GAMEDATA_ACK,
    NET_PACKET_TYPE_DISCONNECT,
    NET_PACKET_TYPE_DISCONNECT_ACK,
    NET_PACKET_TYPE_RELIABLE_ACK,
    NET_PACKET_TYPE_GAMEDATA_RESEND,
    NET_PACKET_TYPE_CONSOLE_MESSAGE,
    NET_PACKET_TYPE_QUERY,
    NET_PACKET_TYPE_QUERY_RESPONSE,
    NET_PACKET_TYPE_LAUNCH,
    NET_PACKET_TYPE_NAT_HOLE_PUNCH
} net_packet_type_t;

// --- growable packet buffer (big-endian on the wire) ----------------------
typedef struct
{
    byte*	data;
    size_t	len;		// bytes written / read position depending on use
    size_t	alloced;
    size_t	pos;		// read cursor
} net_packet_t;

net_packet_t*	NET_NewPacket (int initial_size);
void		NET_FreePacket (net_packet_t* packet);

void		NET_WriteInt8 (net_packet_t* p, unsigned int i);
void		NET_WriteInt16 (net_packet_t* p, unsigned int i);
void		NET_WriteInt32 (net_packet_t* p, unsigned int i);
void		NET_WriteString (net_packet_t* p, const char* s);

boolean		NET_ReadInt8 (net_packet_t* p, unsigned int* i);
boolean		NET_ReadInt16 (net_packet_t* p, unsigned int* i);
boolean		NET_ReadInt32 (net_packet_t* p, unsigned int* i);
boolean		NET_ReadSInt8 (net_packet_t* p, int* i);	// sign-extended
boolean		NET_ReadSInt16 (net_packet_t* p, int* i);
char*		NET_ReadString (net_packet_t* p);	// into a static buffer

// Wall-clock milliseconds (monotonic-ish), for keepalive/resend/tic pacing.
unsigned	NET_GetTimeMS (void);

// --- UDP transport --------------------------------------------------------
// Open a UDP socket (bound to any port for a client).  Returns false on error.
boolean		UDP_OpenClient (void);
void		UDP_Close (void);
// Resolve "host" or "host:port" into a sockaddr; returns false on failure.
boolean		UDP_Resolve (const char* hostport, int default_port);
// Send the packet to the resolved address.
boolean		UDP_Send (net_packet_t* packet);
// Receive a packet (waits up to timeout_ms).  Returns a packet to free, or NULL.
net_packet_t*	UDP_Recv (int timeout_ms);

// Stage-1 demo: query a Chocolate/Crispy server and print its info.  Invoked by
// the -querychoc <host[:port]> command line option (handled in d_main.c).
void		I_QueryChocServer (const char* hostport);

// Stage 2: SYN connection handshake (reports accept/reject).  -connectchoc.
void		I_ConnectChocServer (const char* hostport, const char* version,
				     int gamemode, int gamemission);

#endif
