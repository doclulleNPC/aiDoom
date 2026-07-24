// Minimal public-domain MD5 (RFC 1321) -- used by the launcher to verify SIGIL PWADs by
// checksum.  Self-contained; md5_file_hex() hashes a whole file to a lowercase hex string.
#ifndef BUDDYDOOM_MD5_H
#define BUDDYDOOM_MD5_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t s[4]; uint64_t len; uint8_t buf[64]; uint32_t n; } md5_ctx;

static const uint32_t md5_k[64] = {
  0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
  0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
  0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
  0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
  0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
  0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
  0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
  0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const uint8_t md5_sh[64] = {
  7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
  5,9,14,20,  5,9,14,20,  5,9,14,20,  5,9,14,20,
  4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
  6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};
#define MD5_ROL(x,c) (((x)<<(c))|((x)>>(32-(c))))

static void md5_block(md5_ctx* ctx, const uint8_t* p)
{
    uint32_t M[16], A=ctx->s[0], B=ctx->s[1], C=ctx->s[2], D=ctx->s[3], F; int i, g;
    for (i=0;i<16;i++)
        M[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1]<<8) | ((uint32_t)p[i*4+2]<<16) | ((uint32_t)p[i*4+3]<<24);
    for (i=0;i<64;i++) {
        if      (i<16) { F=(B&C)|(~B&D);   g=i; }
        else if (i<32) { F=(D&B)|(~D&C);   g=(5*i+1)&15; }
        else if (i<48) { F=B^C^D;          g=(3*i+5)&15; }
        else           { F=C^(B|~D);       g=(7*i)&15; }
        F = F + A + md5_k[i] + M[g];
        A=D; D=C; C=B; B = B + MD5_ROL(F, md5_sh[i]);
    }
    ctx->s[0]+=A; ctx->s[1]+=B; ctx->s[2]+=C; ctx->s[3]+=D;
}
static void md5_init(md5_ctx* c)
{ c->s[0]=0x67452301; c->s[1]=0xefcdab89; c->s[2]=0x98badcfe; c->s[3]=0x10325476; c->len=0; c->n=0; }

static void md5_update(md5_ctx* c, const uint8_t* p, size_t n)
{
    c->len += n;
    while (n) {
        size_t k = 64 - c->n; if (k > n) k = n;
        memcpy(c->buf + c->n, p, k); c->n += (uint32_t)k; p += k; n -= k;
        if (c->n == 64) { md5_block(c, c->buf); c->n = 0; }
    }
}
static void md5_final(md5_ctx* c, uint8_t out[16])
{
    uint64_t bits = c->len * 8;		// the real message length (before padding)
    uint8_t one = 0x80, zero = 0, lb[8]; int i;
    md5_update(c, &one, 1);
    while (c->n != 56) md5_update(c, &zero, 1);
    for (i=0;i<8;i++) lb[i] = (uint8_t)(bits >> (8*i));
    md5_update(c, lb, 8);
    for (i=0;i<4;i++) {
        out[i*4]   = (uint8_t)(c->s[i]);
        out[i*4+1] = (uint8_t)(c->s[i]>>8);
        out[i*4+2] = (uint8_t)(c->s[i]>>16);
        out[i*4+3] = (uint8_t)(c->s[i]>>24);
    }
}
// Hash the file at `path` -> 32-char lowercase hex in out[33].  Returns 1 on success, 0 if the
// file can't be opened.
static int md5_file_hex(const char* path, char out[33])
{
    FILE* f = fopen(path, "rb"); uint8_t buf[65536], d[16]; size_t r; md5_ctx c; int i;
    if (!f) return 0;
    md5_init(&c);
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) md5_update(&c, buf, r);
    fclose(f);
    md5_final(&c, d);
    for (i=0;i<16;i++) sprintf(out + i*2, "%02x", d[i]);
    out[32] = 0;
    return 1;
}
#endif
