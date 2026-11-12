// Country-Box launcher: SHA-256 (sha256.h). The portable part follows FIPS 180-4 directly.
#include "sha256.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ---- libcrypto through dlopen (no build-time dependency)

typedef void *(*EvpNewFn)(void);
typedef const void *(*EvpMdFn)(void);
typedef int (*EvpInitFn)(void *, const void *, void *);
typedef int (*EvpUpdateFn)(void *, const void *, size_t);
typedef int (*EvpFinalFn)(void *, unsigned char *, unsigned int *);
typedef void (*EvpFreeFn)(void *);

static struct {
    int ok;
    EvpNewFn ctx_new;
    EvpMdFn sha256;
    EvpInitFn init;
    EvpUpdateFn update;
    EvpFinalFn final;
    EvpFreeFn ctx_free;
} g_evp;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void evp_load(void) {
    if (getenv("CB_LAUNCHER_PORTABLE_SHA")) return;
    const char *names[] = {"libcrypto.so.3", "libcrypto.so"};
    void *h = NULL;
    for (size_t i = 0; i < sizeof names / sizeof *names && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) return;
    g_evp.ctx_new = (EvpNewFn)dlsym(h, "EVP_MD_CTX_new");
    g_evp.sha256 = (EvpMdFn)dlsym(h, "EVP_sha256");
    g_evp.init = (EvpInitFn)dlsym(h, "EVP_DigestInit_ex");
    g_evp.update = (EvpUpdateFn)dlsym(h, "EVP_DigestUpdate");
    g_evp.final = (EvpFinalFn)dlsym(h, "EVP_DigestFinal_ex");
    g_evp.ctx_free = (EvpFreeFn)dlsym(h, "EVP_MD_CTX_free");
    g_evp.ok = g_evp.ctx_new && g_evp.sha256 && g_evp.init && g_evp.update && g_evp.final && g_evp.ctx_free;
}

const char *sha256_backend(void) {
    pthread_once(&g_once, evp_load);
    return g_evp.ok ? "libcrypto" : "portable";
}

// ---- portable

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

struct Sha256 {
    void *evp;
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t fill;
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

Sha256 *sha256_new(void) {
    pthread_once(&g_once, evp_load);
    Sha256 *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (g_evp.ok && (s->evp = g_evp.ctx_new()) && g_evp.init(s->evp, g_evp.sha256(), NULL) == 1) return s;
    if (s->evp) g_evp.ctx_free(s->evp), s->evp = NULL;
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    return s;
}

void sha256_update(Sha256 *s, const void *data, size_t n) {
    if (s->evp) {
        g_evp.update(s->evp, data, n);
        return;
    }
    const uint8_t *p = data;
    s->len += n;
    if (s->fill) {
        size_t take = 64 - s->fill < n ? 64 - s->fill : n;
        memcpy(s->buf + s->fill, p, take);
        s->fill += take, p += take, n -= take;
        if (s->fill < 64) return;
        block(s->h, s->buf), s->fill = 0;
    }
    for (; n >= 64; p += 64, n -= 64) block(s->h, p);
    memcpy(s->buf, p, n), s->fill = n;
}

void sha256_final(Sha256 *s, uint8_t out[32]) {
    if (s->evp) {
        unsigned int n = 32;
        g_evp.final(s->evp, out, &n);
        g_evp.ctx_free(s->evp);
        free(s);
        return;
    }
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, lenb[8];
    sha256_update(s, &pad, 1);
    while (s->fill != 56) sha256_update(s, &zero, 1);
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(s, lenb, 8);
    for (int i = 0; i < 8; i++) out[4 * i] = s->h[i] >> 24, out[4 * i + 1] = s->h[i] >> 16, out[4 * i + 2] = s->h[i] >> 8, out[4 * i + 3] = s->h[i];
    free(s);
}

int sha256_file(const char *path, char hex[65], volatile int64_t *progress, volatile int *cancel) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    enum { CHUNK = 4 << 20 };
    uint8_t *buf = malloc(CHUNK);
    Sha256 *s = sha256_new();
    int rc = 0;
    if (!buf || !s) rc = -1;
    while (!rc) {
        if (cancel && *cancel) {
            rc = -1;
            break;
        }
        ssize_t n = read(fd, buf, CHUNK);
        if (n < 0) rc = -1;
        if (n <= 0) break;
        sha256_update(s, buf, (size_t)n);
        if (progress) __atomic_add_fetch(progress, n, __ATOMIC_RELAXED);
    }
    uint8_t d[32];
    if (s) sha256_final(s, d);
    if (!rc)
        for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", d[i]);
    free(buf);
    close(fd);
    return rc;
}
