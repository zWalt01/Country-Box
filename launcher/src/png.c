// Country-Box launcher: PNG writer (png.h). PNG (RFC 2083) with a zlib stream (RFC 1950) of stored deflate blocks
// (RFC 1951 BTYPE 00): larger files than a compressor makes, which is fine for test screenshots.
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(uint32_t c, const uint8_t *b, size_t n) {
    c = ~c;
    while (n--) c = crc_table[(c ^ *b++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }

static int chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t h[8];
    be32(h, (uint32_t)n);
    memcpy(h + 4, type, 4);
    uint32_t c = crc(crc(0, h + 4, 4), data, n);
    uint8_t t[4];
    be32(t, c);
    return fwrite(h, 1, 8, f) == 8 && (n == 0 || fwrite(data, 1, n, f) == n) && fwrite(t, 1, 4, f) == 4 ? 0 : -1;
}

int png_write_rgba(const char *path, const uint8_t *px, int w, int h, int pitch) {
    if (!crc_table[1]) crc_init();
    size_t row = (size_t)w * 4 + 1, raw_n = row * h;
    uint8_t *raw = malloc(raw_n);
    if (!raw) return -1;
    for (int y = 0; y < h; y++) raw[y * row] = 0, memcpy(raw + y * row + 1, px + (size_t)y * pitch, (size_t)w * 4);
    size_t blocks = (raw_n + 65534) / 65535, z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(z_n);
    if (!z) return free(raw), -1;
    uint8_t *o = z;
    *o++ = 0x78, *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) a = (a + raw[i]) % 65521, b = (b + a) % 65521;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        *o++ = off + n == raw_n;
        *o++ = n & 0xFF, *o++ = n >> 8, *o++ = ~n & 0xFF, *o++ = (~n >> 8) & 0xFF;
        memcpy(o, raw + off, n), o += n;
    }
    be32(o, b << 16 | a), o += 4;
    FILE *f = fopen(path, "wb");
    int rc = -1;
    if (f) {
        static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        uint8_t ihdr[13];
        be32(ihdr, (uint32_t)w), be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = 0, ihdr[11] = 0, ihdr[12] = 0;
        rc = fwrite(sig, 1, 8, f) == 8 && !chunk(f, "IHDR", ihdr, 13) && !chunk(f, "IDAT", z, (size_t)(o - z)) &&
                     !chunk(f, "IEND", NULL, 0)
                 ? 0
                 : -1;
        if (fclose(f)) rc = -1;
    }
    free(raw), free(z);
    return rc;
}
