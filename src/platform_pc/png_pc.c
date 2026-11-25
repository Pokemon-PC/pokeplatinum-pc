/*
 * png_pc.c - minimal PNG writer (RGBA8, uncompressed "stored" deflate
 * blocks): screenshots and frame dumps without an image library.
 */
#include "platform_pc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc = crc_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* Writes a chunk; `data` may be split in two parts (header + payload). */
static void chunk(FILE *f, const char *type, const uint8_t *data, size_t len) {
    uint8_t hdr[8];
    put32(hdr, (uint32_t)len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) {
        fwrite(data, 1, len, f);
    }
    uint32_t crc = crc_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
    crc = crc_update(crc, data, len) ^ 0xFFFFFFFFu;
    uint8_t c[4];
    put32(c, crc);
    fwrite(c, 1, 4, f);
}

bool pc_write_png(const char *path, const uint32_t *rgba, int width, int height) {
    static bool crc_ready;
    if (!crc_ready) {
        crc_init();
        crc_ready = true;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    fwrite(sig, 1, 8, f);

    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)width);
    put32(ihdr + 4, (uint32_t)height);
    ihdr[8] = 8;    /* bit depth */
    ihdr[9] = 6;    /* RGBA */
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, sizeof ihdr);

    /* Raw scanlines (filter byte 0 + pixels), wrapped in a zlib stream of
     * stored deflate blocks (max 65535 bytes each). */
    size_t row = (size_t)width * 4 + 1;
    size_t raw_len = row * (size_t)height;
    size_t blocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + blocks * 5 + 4;
    uint8_t *z = malloc(z_len);
    uint8_t *raw = malloc(raw_len);
    if (z == NULL || raw == NULL) {
        free(z);
        free(raw);
        fclose(f);
        return false;
    }
    for (int y = 0; y < height; y++) {
        uint8_t *r = raw + row * (size_t)y;
        r[0] = 0;
        for (int x = 0; x < width; x++) {
            uint32_t p = rgba[(size_t)y * (size_t)width + (size_t)x];   /* R | G<<8 | B<<16 | A<<24 */
            r[1 + x * 4 + 0] = (uint8_t)p;
            r[1 + x * 4 + 1] = (uint8_t)(p >> 8);
            r[1 + x * 4 + 2] = (uint8_t)(p >> 16);
            r[1 + x * 4 + 3] = (uint8_t)(p >> 24);
        }
    }
    size_t o = 0;
    z[o++] = 0x78;
    z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < raw_len;) {
        size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        z[o++] = pos + n == raw_len ? 1 : 0;
        z[o++] = (uint8_t)n;
        z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n;
        z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + pos, n);
        for (size_t i = 0; i < n; i++) {
            a = (a + raw[pos + i]) % 65521;
            b = (b + a) % 65521;
        }
        o += n;
        pos += n;
    }
    put32(z + o, (b << 16) | a);
    o += 4;
    chunk(f, "IDAT", z, o);
    chunk(f, "IEND", NULL, 0);
    free(z);
    free(raw);
    return fclose(f) == 0;
}
