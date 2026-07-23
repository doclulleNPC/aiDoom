// w_inflate.c -- zlib decompression, isolated from the DOOM headers.
//
// <zlib.h> transitively declares the POSIX close() function, which clashes with
// p_spec.h's `close` enum constant, so it can't be included in the play-sim TU.
// Keep the one zlib user here where no DOOM headers are pulled in.
//
// Used by p_setup.c for ZNOD (zlib-compressed extended BSP nodes).

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef unsigned char byte;

// Inflate a zlib stream into a malloc'd buffer that grows as needed.  Returns NULL on any
// error; on success *outlen holds the inflated length.  The caller free()s the result.
byte* W_InflateZlib (byte* src, unsigned srclen, unsigned* outlen)
{
    z_stream	zs;
    byte*	out;
    unsigned	cap = srclen ? srclen * 6u + 0x4000u : 0x4000u;
    int		r;

    memset (&zs, 0, sizeof zs);
    if (inflateInit (&zs) != Z_OK)
	return NULL;

    out = malloc (cap);
    if (!out) { inflateEnd (&zs); return NULL; }

    zs.next_in  = src;  zs.avail_in  = srclen;
    zs.next_out = out;  zs.avail_out = cap;

    for (;;)
    {
	r = inflate (&zs, Z_NO_FLUSH);
	if (r == Z_STREAM_END)
	    break;
	if (r != Z_OK)					// Z_DATA_ERROR / Z_MEM_ERROR / stuck
	{
	    free (out); inflateEnd (&zs); return NULL;
	}
	if (zs.avail_out == 0)				// filled the buffer -> grow, continue
	{
	    unsigned used = cap;
	    byte*    grown;
	    cap *= 2;
	    grown = realloc (out, cap);
	    if (!grown) { free (out); inflateEnd (&zs); return NULL; }
	    out = grown;
	    zs.next_out = out + used; zs.avail_out = cap - used;
	}
    }

    *outlen = cap - zs.avail_out;
    inflateEnd (&zs);
    return out;
}
