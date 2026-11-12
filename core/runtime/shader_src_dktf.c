// Country-Box: program source of the shader preparation (shader_prep.h) for Donkey Kong Country: Tropical Freeze
// (1.0.0, Retro's GameFramework). Game-specific; the preparation itself is shader_prep.c.
//
// Where the programs are (measured on the retail RomFS, 2026-11-12, with a Python cross-check outside the repo): in SNVN
// chunks (big-endian header: fourcc, u64 size, then a table of entries): the one of MaterialArchive.arc (an RFRM file,
// 21.6 MB, 1,374 entries) and one in each MTRL resource of the level and UI packs (*.pak: RFRM PACK files whose ADIR
// chunk lists fourcc, GUID, u64 offset and u64 size per resource; an MTRL resource is a 0x20-byte RFRM header and a zlib
// stream; 795 of them). Of the 4,876 programs one install met in play, 4,652 have their code here (the rest unseen). Each shader's binaries form one "binary pool" stored as a raw deflate stream (no zlib
// header); between the streams sit the shaders' uncompressed descriptions. A pool inflates to the stages' code blocks
// back to back (each starts with magic 0x12345678, compute 0x12345679), then their control sections, 0x840 bytes apart
// in this SDK (magic 0x98761234; code length at +0x704, stage at +0x714: 0 vertex, 1 fragment, 5 compute), the i-th
// control describing the i-th code block. The 1,374 streams (one per table entry) are found by trying to inflate at each
// offset of the chunk whose first byte is a valid deflate block header and keeping those whose output starts with the
// code magic: 4.4 MB in about a second, and the count matches the table. The game reads the pools at the same layout
// (CShaderCore_NVN::LoadBinaryPools, main+0x53f48), so a program here is the one the game hands to NVN.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "shader_prep.h"

#define CONTROL_SIZE 0x880            // what the preparation copies of a control section (shader_prep.c)
#define CONTROL_STRIDE 0x840          // this SDK's control sections, back to back in a pool
#define CONTROL_MAGIC 0x98761234u
#define POOL_MAX (1u << 20)

static uint32_t rd32le(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64be(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = v << 8 | p[i]; return v; }

static int t_detect(const char *romfs) {
    char p[1024];
    snprintf(p, sizeof p, "%s/MaterialArchive.arc", romfs);
    return plat_file_exists(p);
}

// Inflates a raw deflate stream at src (at most n bytes) into out (cap bytes): the bytes written, 0 when it is not a
// whole stream; *used gets the input it took. probe: stop after 8 output bytes.
static size_t inflate_at(const uint8_t *src, size_t n, uint8_t *out, size_t cap, size_t *used, int probe) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK) return 0;
    z.next_in = (Bytef *)src, z.avail_in = (uInt)(n > (1u << 30) ? (1u << 30) : n);
    z.next_out = out, z.avail_out = (uInt)(probe ? 8 : cap);
    int r = inflate(&z, probe ? Z_SYNC_FLUSH : Z_FINISH);
    size_t got = probe ? (r == Z_OK || r == Z_STREAM_END || r == Z_BUF_ERROR ? z.total_out : 0) : (r == Z_STREAM_END ? z.total_out : 0);
    if (used) *used = z.total_in;
    inflateEnd(&z);
    return got;
}

// The programs of one inflated pool: control i describes code block i, the code blocks back to back from the start.
// A control may name a stage the sink has no bucket for: measured on the retail RomFS (2026-11-12) the pools hold
// VS+GS+FS triples — raw control stage 0x714 values {0 vertex, 1 fragment, 2 geometry} and nothing else — and the
// geometry control sits between the fragment and the vertex one, so stopping at it would lose the vertex program
// after it (768 of them wanted by the list, measured: 763 of the 777 "not found" programs of the 2026-11-12 list).
// The geometry block is still part of the code chain: consume it and keep walking. Geometry programs never reach
// the sink (every recording of four installs holds stages vertex/fragment only), so nothing is emitted for them.
static long pool_programs(const uint8_t *b, size_t n, ShaderProgSink sink, void *ctx) {
    static const uint8_t magic[4] = {0x34, 0x12, 0x76, 0x98};
    const uint8_t *first = memmem(b, n, magic, 4);
    long found = 0;
    size_t code_at = 0;
    for (const uint8_t *m = first; m && (size_t)(m - b) + CONTROL_STRIDE <= n; m += CONTROL_STRIDE) {
        if (rd32le(m) != CONTROL_MAGIC) break;
        uint32_t len = rd32le(m + 0x704), st = rd32le(m + 0x714);
        int stage = st == 0 ? GFX_STAGE_VS : st == 1 ? GFX_STAGE_FS : st == 5 ? GFX_STAGE_CS : -1;
        if (len < 4 || code_at + len > (size_t)(first - b)) break;
        uint32_t cm = rd32le(b + code_at);
        if (cm != (st == 5 ? 0x12345679u : 0x12345678u)) break;
        if (stage >= 0) sink(ctx, stage, m, b + code_at, len);   // the buffer has CONTROL_SIZE readable bytes past the last control
        found++;
        code_at += len;
    }
    return found;
}

// The binary pools of the SNVN chunk in b[0..n): their programs to the sink. pool: POOL_MAX + CONTROL_SIZE bytes.
static long snvn_programs(const uint8_t *d, size_t n, uint8_t *pool, ShaderProgSink sink, void *ctx, long *pools) {
    const uint8_t *snvn = memmem(d, n, "SNVN", 4);
    if (!snvn || (size_t)(snvn - d) + 12 > n) return 0;
    size_t lo = (size_t)(snvn - d) + 12, hi = lo + rd64be(snvn + 4);
    if (hi > n || hi < lo) hi = n;
    long found = 0;
    uint8_t probe[16];
    for (size_t i = lo; i < hi;) {
        if (((d[i] >> 1) & 3) == 3) { i++; continue; }   // not a deflate block header
        size_t used = 0;
        if (inflate_at(d + i, hi - i < 512 ? hi - i : 512, probe, sizeof probe, NULL, 1) < 4 ||
            (rd32le(probe) != 0x12345678u && rd32le(probe) != 0x12345679u)) { i++; continue; }
        size_t k = inflate_at(d + i, hi - i, pool, POOL_MAX, &used, 0);
        if (!k || !used) { i++; continue; }
        memset(pool + k, 0, CONTROL_SIZE);   // as the game's zero-filled pool memory past the last control
        (*pools)++;
        found += pool_programs(pool, k, sink, ctx);
        i += used;
    }
    return found;
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = sz > 0 ? malloc((size_t)sz) : NULL;
    *n = d ? fread(d, 1, (size_t)sz, f) : 0;
    fclose(f);
    if (d && *n != (size_t)sz) { free(d); return NULL; }
    return d;
}

// A whole zlib stream (with its header) into a growing buffer; NULL when it is not one.
static uint8_t *inflate_all(const uint8_t *src, size_t n, size_t *out_n) {
    z_stream z;
    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK) return NULL;
    size_t cap = n * 4 + 65536, len = 0;
    uint8_t *out = malloc(cap);
    z.next_in = (Bytef *)src, z.avail_in = (uInt)n;
    int r = Z_OK;
    while (out && r == Z_OK) {
        if (len == cap) {
            uint8_t *t = realloc(out, cap * 2);
            if (!t) { free(out); out = NULL; break; }
            out = t, cap *= 2;
        }
        z.next_out = out + len, z.avail_out = (uInt)(cap - len);
        r = inflate(&z, Z_NO_FLUSH);
        len = z.total_out;
    }
    inflateEnd(&z);
    if (r != Z_STREAM_END) { free(out); return NULL; }
    *out_n = len;
    return out;
}

// ---- the pack files, several at a time
typedef struct {
    char (*paths)[1024];
    int64_t *sizes;   // bytes of each pack (the progress screen counts bytes)
    int n;
    volatile int32_t next;
    ShaderProgSink sink;
    void *ctx;
    volatile int64_t *done;
    volatile long found, pools, mtrl;
} TScan;

static void pak_programs(TScan *s, const char *path, uint8_t *pool) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t head[1 << 16];
    size_t hn = fread(head, 1, sizeof head, f);
    const uint8_t *adir = memmem(head, hn, "ADIR", 4);
    uint8_t *dir = NULL;
    if (adir && (size_t)(adir - head) + 0x1c <= hn) {
        size_t at = (size_t)(adir - head), size = (size_t)rd64be(adir + 4) + 12;
        if (size < (64u << 20) && (dir = malloc(size)) && !fseek(f, (long)at, SEEK_SET) && fread(dir, 1, size, f) == size) {
            uint32_t count = (uint32_t)dir[0x18] << 24 | (uint32_t)dir[0x19] << 16 | (uint32_t)dir[0x1a] << 8 | dir[0x1b];
            for (uint32_t i = 0; i < count && 0x1c + (size_t)(i + 1) * 0x24 <= size; i++) {
                const uint8_t *e = dir + 0x1c + (size_t)i * 0x24;
                if (memcmp(e, "MTRL", 4)) continue;
                uint64_t off = rd64be(e + 20), sz = rd64be(e + 28);
                if (sz <= 0x20 || sz > (256u << 20)) continue;
                uint8_t *r = malloc(sz);
                if (!r || fseek(f, (long)off, SEEK_SET) || fread(r, 1, sz, f) != sz) { free(r); continue; }
                size_t un = 0;
                uint8_t *u = inflate_all(r + 0x20, sz - 0x20, &un);   // a 0x20-byte RFRM header, then zlib
                long pools = 0, got = u ? snvn_programs(u, un, pool, s->sink, s->ctx, &pools) : snvn_programs(r, sz, pool, s->sink, s->ctx, &pools);
                __atomic_add_fetch(&s->found, got, __ATOMIC_RELAXED);
                __atomic_add_fetch(&s->pools, pools, __ATOMIC_RELAXED);
                __atomic_add_fetch(&s->mtrl, 1, __ATOMIC_RELAXED);
                free(u), free(r);
            }
        }
    }
    free(dir);
    fclose(f);
}

static void scan_worker(void *arg) {
    TScan *s = arg;
    uint8_t *pool = malloc(POOL_MAX + CONTROL_SIZE);
    if (!pool) return;
    for (int i; (i = __atomic_fetch_add(&s->next, 1, __ATOMIC_RELAXED)) < s->n;) {
        pak_programs(s, s->paths[i], pool);
        if (s->done) __atomic_add_fetch(s->done, s->sizes ? s->sizes[i] : 0, __ATOMIC_RELAXED);
    }
    free(pool);
}

static void list_paks(const char *dir, char (*paths)[1024], int *n, int cap, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d)) && *n < cap;) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        size_t l = strlen(e->d_name);
        if (l > 4 && !strcmp(e->d_name + l - 4, ".pak")) snprintf(paths[(*n)++], 1024, "%s", p);
        else if (depth > 0 && e->d_type == DT_DIR) list_paks(p, paths, n, cap, depth - 1);
    }
    closedir(d);
}

static long t_scan(const char *romfs, int threads, ShaderProgSink sink, void *ctx, volatile int64_t *done,
                   volatile int64_t *total) {
    char path[1024];
    snprintf(path, sizeof path, "%s/MaterialArchive.arc", romfs);
    uint64_t t0 = plat_now_ns();
    size_t n = 0;
    uint8_t *d = read_file(path, &n), *pool = malloc(POOL_MAX + CONTROL_SIZE);
    if (!d || !pool) { free(d), free(pool); return -1; }
    TScan s = {.sink = sink, .ctx = ctx, .done = done};
    s.paths = calloc(256, sizeof *s.paths);
    if (s.paths) list_paks(romfs, s.paths, &s.n, 256, 3);
    s.sizes = calloc(256, sizeof *s.sizes);
    int64_t bytes = (int64_t)n;
    for (int i = 0; i < s.n && s.sizes; i++) {
        FILE *f = fopen(s.paths[i], "rb");
        if (f) fseek(f, 0, SEEK_END), s.sizes[i] = ftell(f), fclose(f);
        bytes += s.sizes[i];
    }
    if (total) *total = bytes;
    long pools = 0, found = snvn_programs(d, n, pool, sink, ctx, &pools);
    free(d), free(pool);
    if (done) *done = (int64_t)n;
    threads = threads < 1 ? 1 : threads > 32 ? 32 : threads;
    PlatThread *t[32];
    int k = 0;
    for (int i = 0; i < threads && i < s.n; i++)
        if ((t[k] = plat_thread_new(scan_worker, &s, "prep-scan"))) k++;
    if (!k && s.n) scan_worker(&s);
    for (int i = 0; i < k; i++) plat_thread_join(t[i]);
    rt_log("shader_src_dktf: MaterialArchive.arc %ld pools, %ld programs; %d packs, %ld MTRL resources, %ld pools, %ld "
           "programs; %d threads, %.0f ms", pools, found, s.n, (long)s.mtrl, (long)s.pools, (long)s.found, k,
           (plat_now_ns() - t0) / 1e6);
    free(s.paths), free(s.sizes);
    return found + s.found;
}

const ShaderProgSource shader_src_dktf = {"dktf-snvn", t_detect, t_scan};
