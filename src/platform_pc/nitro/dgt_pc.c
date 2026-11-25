/*
 * dgt_pc.c - NitroSDK DGT (MD5 = "hash 1", SHA-1 = "hash 2", HMAC) for the
 * PC port.
 *
 * Platinum links the SDK's DGT library as a prebuilt ARM archive
 * (subprojects/NitroSDK/libraries/dgt/prebuilt/libdgt.a), so there is no C to
 * compile. These are the standard algorithms (RFC 1321, FIPS 180-1, RFC 2104)
 * behind the SDK's API; the game uses them through MATH_CalcMD5/SHA1 and
 * MATH_CalcHMAC* (save data and Wi-Fi checks). The SDK context structures are
 * only used as storage, so their exact field layout does not matter.
 */
#include <nitro.h>
#include <nitro/dgt/dgt.h>

#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t s[4];
    uint64_t len;
    uint8_t buf[64];
} PcMd5;

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
} PcSha1;

_Static_assert(sizeof(PcMd5) <= sizeof(DGTHash1Context), "MD5 state must fit the SDK context");
_Static_assert(sizeof(PcSha1) <= sizeof(DGTHash2Context), "SHA-1 state must fit the SDK context");

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

/* ------------------------------------------------------------------ MD5 */
static const uint32_t K_MD5[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};
static const uint8_t R_MD5[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(PcMd5 *m, const uint8_t *p)
{
    uint32_t w[16], a = m->s[0], b = m->s[1], c = m->s[2], d = m->s[3];
    for (int i = 0; i < 16; i++) {
        w[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
    }
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t t = d;
        d = c;
        c = b;
        b = b + ROL(a + f + K_MD5[i] + w[g], R_MD5[i]);
        a = t;
    }
    m->s[0] += a;
    m->s[1] += b;
    m->s[2] += c;
    m->s[3] += d;
}

void DGT_Hash1Reset(DGTHash1Context *ctx)
{
    PcMd5 *m = (PcMd5 *)ctx;
    m->s[0] = 0x67452301;
    m->s[1] = 0xefcdab89;
    m->s[2] = 0x98badcfe;
    m->s[3] = 0x10325476;
    m->len = 0;
}

void DGT_Hash1SetSource(DGTHash1Context *ctx, const void *src, unsigned long len)
{
    PcMd5 *m = (PcMd5 *)ctx;
    const uint8_t *p = src;
    while (len > 0) {
        uint32_t used = (uint32_t)(m->len & 63), n = 64 - used;
        if (n > len) n = (uint32_t)len;
        memcpy(m->buf + used, p, n);
        m->len += n;
        p += n;
        len -= n;
        if ((m->len & 63) == 0) md5_block(m, m->buf);
    }
}

void DGT_Hash1GetDigest_R(void *digest, DGTHash1Context *ctx)
{
    PcMd5 *m = (PcMd5 *)ctx;
    uint64_t bits = m->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    DGT_Hash1SetSource(ctx, &pad, 1);
    while ((m->len & 63) != 56) DGT_Hash1SetSource(ctx, &zero, 1);
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (8 * i));
    DGT_Hash1SetSource(ctx, lenb, 8);
    uint8_t *out = digest;
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(m->s[i / 4] >> (8 * (i & 3)));
}

/* ---------------------------------------------------------------- SHA-1 */
static void sha1_block(PcSha1 *s, const uint8_t *p)
{
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

void DGT_Hash2Reset(DGTHash2Context *ctx)
{
    PcSha1 *s = (PcSha1 *)ctx;
    s->h[0] = 0x67452301;
    s->h[1] = 0xEFCDAB89;
    s->h[2] = 0x98BADCFE;
    s->h[3] = 0x10325476;
    s->h[4] = 0xC3D2E1F0;
    s->len = 0;
}

void DGT_Hash2SetSource(DGTHash2Context *ctx, const unsigned char *src, unsigned long len)
{
    PcSha1 *s = (PcSha1 *)ctx;
    while (len > 0) {
        uint32_t used = (uint32_t)(s->len & 63), n = 64 - used;
        if (n > len) n = (uint32_t)len;
        memcpy(s->buf + used, src, n);
        s->len += n;
        src += n;
        len -= n;
        if ((s->len & 63) == 0) sha1_block(s, s->buf);
    }
}

void DGT_Hash2GetDigest(DGTHash2Context *ctx, unsigned char *digest)
{
    PcSha1 *s = (PcSha1 *)ctx;
    uint64_t bits = s->len * 8;
    unsigned char pad = 0x80, zero = 0, lenb[8];
    DGT_Hash2SetSource(ctx, &pad, 1);
    while ((s->len & 63) != 56) DGT_Hash2SetSource(ctx, &zero, 1);
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    DGT_Hash2SetSource(ctx, lenb, 8);
    for (int i = 0; i < 20; i++) digest[i] = (unsigned char)(s->h[i / 4] >> (24 - 8 * (i & 3)));
}

/* ----------------------------------------------------------------- HMAC */
static void hmac(void *digest, const void *data, int len, const void *key, int keyLen, int sha1)
{
    uint8_t k[64] = { 0 }, pad[64], inner[20];
    int dlen = sha1 ? 20 : 16;
    union {
        DGTHash1Context md5;
        DGTHash2Context sha;
    } c;
    if (keyLen > 64) {
        if (sha1) {
            DGT_Hash2Reset(&c.sha);
            DGT_Hash2SetSource(&c.sha, key, (unsigned long)keyLen);
            DGT_Hash2GetDigest(&c.sha, k);
        } else {
            DGT_Hash1Reset(&c.md5);
            DGT_Hash1SetSource(&c.md5, key, (unsigned long)keyLen);
            DGT_Hash1GetDigest_R(k, &c.md5);
        }
    } else if (keyLen > 0) {
        memcpy(k, key, (size_t)keyLen);
    }
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < 64; i++) pad[i] = k[i] ^ (pass == 0 ? 0x36 : 0x5c);
        const void *msg = pass == 0 ? data : (const void *)inner;
        int msgLen = pass == 0 ? len : dlen;
        void *out = pass == 0 ? (void *)inner : digest;
        if (sha1) {
            DGT_Hash2Reset(&c.sha);
            DGT_Hash2SetSource(&c.sha, pad, 64);
            DGT_Hash2SetSource(&c.sha, msg, (unsigned long)msgLen);
            DGT_Hash2GetDigest(&c.sha, out);
        } else {
            DGT_Hash1Reset(&c.md5);
            DGT_Hash1SetSource(&c.md5, pad, 64);
            DGT_Hash1SetSource(&c.md5, msg, (unsigned long)msgLen);
            DGT_Hash1GetDigest_R(out, &c.md5);
        }
    }
}

void DGT_Hash1CalcHmac(void *digest, void *bin_ptr, int bin_len, void *key_ptr, int key_len)
{
    hmac(digest, bin_ptr, bin_len, key_ptr, key_len, 0);
}

void DGT_Hash2CalcHmac(void *digest, void *bin_ptr, int bin_len, void *key_ptr, int key_len)
{
    hmac(digest, bin_ptr, bin_len, key_ptr, key_len, 1);
}
