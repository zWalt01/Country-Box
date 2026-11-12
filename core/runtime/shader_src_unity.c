// Country-Box: program source of the shader preparation (shader_prep.h) for Unity NVN player data, as Donkey Kong
// Country Returns HD ships it (Unity 2022.3, RomFS Data/). Game-specific; the preparation itself is shader_prep.c.
//
// Where the programs are (measured on the retail RomFS, 2026-11-11, with a UnityPy cross-check outside the repo):
// - Shader objects (sharedassets*.assets, resources.assets, globalgamemanagers.assets, level*, Resources/unity default
//   resources): their subprograms sit in an LZ4 block blob at the end of the object, after the fields
//       platforms   vector<int>          [n][platform...]           (19: NVN)
//       offsets, compressedLengths, decompressedLengths   vector<vector<u32>>   [n][k][k values] each
//       compressedBlob vector<u8>        [N][N bytes]
//   The files carry no type trees, so this scans for that field sequence (n = 1, platform 19) instead of parsing the
//   objects, checks every length against the blob and the file, and LZ4-decodes the chunks: 16,628 distinct programs.
// - ComputeShader objects (globalgamemanagers.assets, Resources/unity default resources): plain, 78 programs.
// In both an NVN program is its control section (0x880 bytes, magic 0x98761234, code length at +0x704, stage at +0x714:
// 0 vertex, 1 fragment, 5 compute) directly followed by its code (magic 0x12345678, compute 0x12345679). Every match is
// handed on; shader_prep.c keeps the ones the list names (by code hash). .resS / .resource (textures, meshes, audio)
// and StreamingAssets/ (movies) hold none and are skipped.
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "shader_prep.h"

#define CONTROL_SIZE 0x880
#define CONTROL_MAGIC 0x98761234u
#define NVN_PLATFORM 19
#define MAX_FILES 4096
#define MAX_CHUNKS 4096

static int u_detect(const char *romfs) {
    char p[1024];
    snprintf(p, sizeof p, "%s/Data/globalgamemanagers", romfs);
    return plat_file_exists(p);
}

static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// LZ4 block decoder (the same as loader.c's NSO one): bytes written, 0 on corrupt input.
static size_t lz4_block(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen) {
    const uint8_t *s = src, *se = src + srclen;
    uint8_t *d = dst, *de = dst + dstlen;
    while (s < se) {
        uint8_t tok = *s++;
        size_t lit = tok >> 4;
        if (lit == 15) { uint8_t b; do { if (s >= se) return 0; b = *s++; lit += b; } while (b == 255); }
        if ((size_t)(de - d) < lit || (size_t)(se - s) < lit) return 0;
        memcpy(d, s, lit);
        d += lit;
        s += lit;
        if (s >= se) break;
        if (se - s < 2) return 0;
        size_t off = s[0] | (s[1] << 8);
        s += 2;
        size_t ml = tok & 15;
        if (ml == 15) { uint8_t b; do { if (s >= se) return 0; b = *s++; ml += b; } while (b == 255); }
        ml += 4;
        if (!off || off > (size_t)(d - dst) || (size_t)(de - d) < ml) return 0;
        uint8_t *m = d - off;
        while (ml--) *d++ = *m++;
    }
    return (size_t)(d - dst);
}

// Every control section in b[0..n) followed by its code goes to the sink.
static long programs_in(const uint8_t *b, size_t n, ShaderProgSink sink, void *ctx) {
    static const uint8_t magic[4] = {0x34, 0x12, 0x76, 0x98};
    long found = 0;
    for (const uint8_t *p = b; n - (size_t)(p - b) >= CONTROL_SIZE + 4;) {
        const uint8_t *m = memmem(p, n - (size_t)(p - b), magic, 4);
        if (!m || n - (size_t)(m - b) < CONTROL_SIZE + 4) break;
        p = m + 4;
        uint32_t c1 = rd32(m + 0x700), len = rd32(m + 0x704), st = rd32(m + 0x714);
        const uint8_t *code = m + CONTROL_SIZE;
        if (len < 4 || c1 > len || (size_t)(code - b) + len > n) continue;
        uint32_t cm = rd32(code);
        int stage = st == 0 ? GFX_STAGE_VS : st == 1 ? GFX_STAGE_FS : st == 5 ? GFX_STAGE_CS : -1;
        if (stage < 0 || cm != (stage == GFX_STAGE_CS ? 0x12345679u : 0x12345678u)) continue;
        sink(ctx, stage, m, code, len);
        found++;
        p = code + len;
    }
    return found;
}

// One counted u32 vector at *p (count 1..MAX_CHUNKS); its values in out. 0 when it does not fit.
static int vec_at(const uint8_t *b, size_t n, size_t *p, uint32_t *k, const uint8_t **vals) {
    if (*p + 4 > n) return 0;
    *k = rd32(b + *p);
    if (!*k || *k > MAX_CHUNKS || *p + 4 + (size_t)*k * 4 > n) return 0;
    *vals = b + *p + 4;
    *p += 4 + (size_t)*k * 4;
    return 1;
}

// Shader blobs in b: at each [1][19][1] the offsets / lengths / blob fields, checked, decoded and scanned.
static long blobs_in(const uint8_t *b, size_t n, ShaderProgSink sink, void *ctx, long *n_blobs) {
    static const uint8_t sig[12] = {1, 0, 0, 0, NVN_PLATFORM, 0, 0, 0, 1, 0, 0, 0};
    long found = 0;
    uint8_t *buf = NULL;
    size_t cap = 0;
    for (size_t at = 0; at + 12 <= n;) {
        const uint8_t *m = memmem(b + at, n - at, sig, 12);
        if (!m) break;
        size_t p = (size_t)(m - b) + 12;
        at = (size_t)(m - b) + 4;
        uint32_t ko, kc, kd;
        const uint8_t *offs, *cl, *dl;
        if (!vec_at(b, n, &p, &ko, &offs) || p + 4 > n || rd32(b + p) != 1) continue;
        p += 4;
        if (!vec_at(b, n, &p, &kc, &cl) || p + 4 > n || rd32(b + p) != 1) continue;
        p += 4;
        if (!vec_at(b, n, &p, &kd, &dl) || ko != kc || kc != kd || p + 4 > n) continue;
        uint32_t blob_n = rd32(b + p);
        p += 4;
        if (p + blob_n > n) continue;
        const uint8_t *blob = b + p;
        uint64_t total = 0;
        int ok = 1;
        for (uint32_t i = 0; i < ko && ok; i++) {
            uint32_t o = rd32(offs + 4 * i), c = rd32(cl + 4 * i), d = rd32(dl + 4 * i);
            ok = (uint64_t)o + c <= blob_n && d && d <= (64u << 20);
            total += d;
        }
        if (!ok || total > (512u << 20)) continue;
        if (total > cap) {
            uint8_t *t = realloc(buf, (size_t)total);
            if (!t) continue;
            buf = t, cap = (size_t)total;
        }
        size_t w = 0;
        for (uint32_t i = 0; i < ko && ok; i++) {
            uint32_t o = rd32(offs + 4 * i), c = rd32(cl + 4 * i), d = rd32(dl + 4 * i);
            ok = lz4_block(blob + o, c, buf + w, d) == d;
            w += d;
        }
        if (!ok) continue;
        found += programs_in(buf, w, sink, ctx);
        (*n_blobs)++;
        at = p + blob_n;
    }
    free(buf);
    return found;
}

// ---- the SerializedFile object table (format 22, Unity 2022.3, no type trees), so a file's Shader (class 48) and
// ComputeShader (72) objects are read alone: with mmap only their pages come off the disk (about 20 MB of 3.6 GB), which
// matters on a microSD card. Layout (UnityPy SerializedFile, read): a 16-byte legacy header whose version (big-endian
// u32 at 8) is 22, the endianness byte at 16, then big-endian metadata size (u32 at 20), file size (u64 at 24) and data
// offset (u64 at 32); the metadata from 48: the Unity version string, the target platform (i32), enableTypeTree (u8),
// the types (class id i32, stripped u8, script type index i16, [script id 16 bytes for MonoBehaviour 114], old type
// hash 16 bytes), the object count (i32) and per object, 4-aligned from the metadata start: path id i64, byte start i64,
// byte size u32, type index i32. Anything else (another version, type trees, a value out of range) returns -1 and the
// caller scans the whole file instead.
#define MAX_RANGES 65536

static uint64_t be64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

typedef struct { uint64_t off, size; } Range;

static long shader_objects(const uint8_t *b, size_t n, Range *out) {
    if (n < 64 || be32(b + 8) != 22 || b[16] != 0) return -1;   // version 22, little-endian metadata
    uint64_t meta = be32(b + 20), data = be64(b + 32), fsize = be64(b + 24);
    if (fsize != n || data > n || 48 + meta > n) return -1;
    const uint8_t *p = b + 48, *e = b + 48 + meta;
    const uint8_t *z = memchr(p, 0, (size_t)(e - p));
    if (!z) return -1;
    p = z + 1 + 4;                                         // the version string, the target platform
    if (p + 5 > e || *p++ != 0) return -1;                 // enableTypeTree: only files without type trees
    int32_t nt;
    memcpy(&nt, p, 4), p += 4;
    if (nt < 0 || nt > 100000) return -1;
    uint8_t *want = calloc((size_t)nt + 1, 1);
    if (!want) return -1;
    for (int32_t i = 0; i < nt; i++) {
        if (p + 7 > e) return free(want), -1;
        int32_t cls;
        memcpy(&cls, p, 4);
        p += 4 + 1 + 2;
        if (cls == 114) p += 16;
        p += 16;
        want[i] = cls == 48 || cls == 72;
    }
    if (p + 4 > e) return free(want), -1;
    int32_t no;
    memcpy(&no, p, 4), p += 4;
    long k = 0;
    for (int32_t i = 0; i < no; i++) {
        p = b + 48 + (((size_t)(p - (b + 48)) + 3) & ~(size_t)3);
        if (p + 24 > e) return free(want), -1;
        uint64_t start;
        uint32_t size;
        int32_t ti;
        memcpy(&start, p + 8, 8), memcpy(&size, p + 16, 4), memcpy(&ti, p + 20, 4);
        p += 24;
        if (ti < 0 || ti >= nt || data + start + size > n) return free(want), -1;
        if (want[ti] && k < MAX_RANGES) out[k++] = (Range){data + start, size};
    }
    free(want);
    return k;
}

typedef struct {
    char path[1024];
    uint64_t size;
} UFile;

typedef struct {
    UFile *f;
    int n;
    volatile int64_t next, found, blobs, bad, whole, objects, read;   // whole: files scanned without their table
    volatile int64_t *done;
    int full;                                              // CB_SHADER_SCAN=full: every file whole
    ShaderProgSink sink;
    void *ctx;
} UScan;

static int skip_name(const char *name) {
    size_t l = strlen(name);
    return (l > 5 && !strcmp(name + l - 5, ".resS")) || (l > 9 && !strcmp(name + l - 9, ".resource")) ||
           !strcmp(name, "boot.config") || (l > 4 && (!strcmp(name + l - 4, ".dat") || !strcmp(name + l - 4, ".txt")));
}

static void list_dir(const char *dir, UFile *f, int *n) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *n < MAX_FILES) {
        if (e->d_name[0] == '.' || skip_name(e->d_name)) continue;
        struct stat st;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) || !S_ISREG(st.st_mode) || st.st_size < CONTROL_SIZE) continue;
        snprintf(f[*n].path, sizeof f[*n].path, "%s", p);
        f[(*n)++].size = (uint64_t)st.st_size;
    }
    closedir(d);
}

static int size_desc(const void *a, const void *b) {   // the biggest files first: the threads end together
    uint64_t x = ((const UFile *)a)->size, y = ((const UFile *)b)->size;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void scan_worker(void *arg) {
    UScan *s = arg;
    Range *r = malloc(MAX_RANGES * sizeof *r);
    for (;;) {
        int64_t i = plat_atomic_inc(&s->next) - 1;
        if (i >= s->n || !r) break;
        UFile *f = &s->f[i];
        int fd = open(f->path, O_RDONLY | O_CLOEXEC);
        const uint8_t *b = fd >= 0 ? mmap(NULL, (size_t)f->size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
        if (fd >= 0) close(fd);
        if (b == MAP_FAILED) {
            plat_atomic_inc(&s->bad);
        } else {
            size_t n = (size_t)f->size;
            long nb = 0, k = 0, nr = s->full ? -1 : shader_objects(b, n, r);
            if (nr < 0) {   // no table this reader knows: the whole file
                madvise((void *)b, n, MADV_SEQUENTIAL);
                k = programs_in(b, n, s->sink, s->ctx) + blobs_in(b, n, s->sink, s->ctx, &nb);
                plat_atomic_inc(&s->whole);
                plat_atomic_add(&s->read, (int64_t)n);
            } else {
                for (long q = 0; q < nr; q++) {
                    const uint8_t *o = b + r[q].off;
                    k += programs_in(o, (size_t)r[q].size, s->sink, s->ctx) + blobs_in(o, (size_t)r[q].size, s->sink, s->ctx, &nb);
                    plat_atomic_add(&s->read, (int64_t)r[q].size);
                }
                plat_atomic_add(&s->objects, nr);
            }
            plat_atomic_add(&s->found, k);
            plat_atomic_add(&s->blobs, nb);
            munmap((void *)b, n);
        }
        if (s->done) plat_atomic_add(s->done, (int64_t)f->size);
    }
    free(r);
}

static long u_scan(const char *romfs, int threads, ShaderProgSink sink, void *ctx, volatile int64_t *done,
                   volatile int64_t *total) {
    const char *mode = getenv("CB_SHADER_SCAN");
    UScan s = {.sink = sink, .ctx = ctx, .done = done, .full = mode && !strcmp(mode, "full")};
    s.f = calloc(MAX_FILES, sizeof *s.f);
    if (!s.f) return -1;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/Data", romfs);
    list_dir(dir, s.f, &s.n);
    snprintf(dir, sizeof dir, "%s/Data/Resources", romfs);
    list_dir(dir, s.f, &s.n);
    qsort(s.f, (size_t)s.n, sizeof *s.f, size_desc);
    uint64_t bytes = 0;
    for (int i = 0; i < s.n; i++) bytes += s.f[i].size;
    if (total) *total = (int64_t)bytes;
    uint64_t t0 = plat_now_ns();
    threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
    PlatThread *t[64];
    int k = 0;
    for (int i = 0; i < threads && i < s.n; i++)
        if ((t[k] = plat_thread_new(scan_worker, &s, "prep-scan"))) k++;
    if (!k) scan_worker(&s);
    for (int i = 0; i < k; i++) plat_thread_join(t[i]);
    rt_log("shader_src_unity: %d files, %.0f MB: %lld shader objects read (%.1f MB), %lld files scanned whole, %lld "
           "unreadable; %lld programs (%lld Shader blobs), %d threads, %.0f ms", s.n, bytes / 1048576.0,
           (long long)s.objects, s.read / 1048576.0, (long long)s.whole, (long long)s.bad, (long long)s.found,
           (long long)s.blobs, k, (plat_now_ns() - t0) / 1e6);
    free(s.f);
    return s.n ? (long)s.found : -1;
}

const ShaderProgSource shader_src_unity = {"unity-nvn", u_detect, u_scan};
