// Country-Box: shader preparation before play (shader_prep.h). Game-agnostic: a game brings its lists and a program
// source (shader_src_unity.c for Returns HD); this file finds the programs, writes the seed files the existing
// translation path reads (gfx_shader.c "preparation from record files"), runs the GPU half (vk_pipeline_prepare) and
// keeps the marker that says the work is done for this translator and this driver.
//
// Files (all in the pipeline cache's directory, next to the executable unless MP4_PIPELINE_CACHE moves it):
//   shader-list.bin      the game's variant list (shipped; tools/prepare_shaders.py --make-list): shader-answers.bin
//                        layout with zero input ids. CB_SHADER_LIST overrides the path.
//   pipeline-list.bin    the game's pipeline list (shipped; --make-pipeline-list): merged into pipeline-keys.bin before
//                        the device comes up. CB_PIPELINE_LIST overrides it.
//   shader-patches.bin   (shipped, optional) control-section fields the game writes at run time, per listed program
//                        whose recorded control differs from the one in the game files (below). CB_SHADER_PATCHES.
//   shader-answers.bin, shader-programs.bin   what this install's own play recorded (gfx_shader.c): its variants join
//                        the list, and its programs are the fallback for the ones the game files do not give.
//   seed-answers.bin, seed-programs.bin       written here, read by gfx_shader_prepare_seed, deleted afterwards
//                        (seed-programs.bin holds game code: never shipped, never kept).
//   shader-prepared.id   the marker: text lines naming what the prepared caches depend on (translator identity, key
//                        layout, GPU vendor/device, driver version and info, pipelineCacheUUID, the lists' hashes, the
//                        driver cache directory). Any difference re-runs the preparation.
//   shader-patches.found.bin   written when the game files miss a listed program whose recorded copy differs from a
//                        program in the files only in a few control bytes: the patches to ship (developer output).
//
// Driver cache: the stage libraries go to the GPU driver's own disk cache, so where it lives matters. The preparation
// pins it next to the caches (MESA_SHADER_CACHE_DIR / __GL_SHADER_DISK_CACHE_PATH = <dir>/driver-cache, and a size
// limit Mesa will not evict from at this size) unless CB_DRIVER_CACHE=system; shader_prep_env must run before the
// Vulkan instance exists.
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "shader_prep.h"
#include "ui/prep_screen.h"
#include "vk.h"

#define CONTROL_SIZE 0x880
#define ANS_REC (4 + 32 + 32 + 147 + 1)                    // gfx_shader.c ANS_REC
#define PRG_HEAD (12 + 32 + CONTROL_SIZE)                  // gfx_shader.c PRG_HEAD
#define PATCH_MAX_BYTES 64                                 // a control differing in more is another program

ShaderPrepCount shader_prep_count;

// ---- paths

static char g_dir[PATH_MAX];                               // the caches' directory
static char g_exe_dir[PATH_MAX];

static void dirs(void) {
    if (g_dir[0]) return;
    char exe[PATH_MAX];
    if (plat_exe_path(exe, sizeof exe)) {
        char *cut = strrchr(exe, '/');
        if (cut) *cut = 0, snprintf(g_exe_dir, sizeof g_exe_dir, "%s", exe);
    }
    if (!vk_pipeline_cache_dir(g_dir, sizeof g_dir)) snprintf(g_dir, sizeof g_dir, "%s", g_exe_dir);
}

// A shipped file: the variable's path, else <dir>/<name>, else <exe dir>/<name>.
static void shipped(const char *var, const char *name, char *out, size_t cap) {
    dirs();
    const char *e = getenv(var);
    if (e && *e) {
        snprintf(out, cap, "%s", e);
        return;
    }
    snprintf(out, cap, "%s/%s", g_dir, name);
    if (!plat_file_exists(out) && g_exe_dir[0]) snprintf(out, cap, "%s/%s", g_exe_dir, name);
}

static void in_dir(const char *name, char *out, size_t cap) {
    dirs();
    snprintf(out, cap, "%s/%s", g_dir, name);
}

static void hex(const uint8_t *b, size_t n, char *out) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
}

// ---- record files (gfx_shader.c layouts: 8-byte magic, version, record size)

static uint8_t *rec_file(const char *path, const char *magic, uint32_t rec_size, size_t *body_n) {
    uint64_t size = 0;
    uint8_t *b = plat_file_read(path, &size);
    *body_n = 0;
    if (!b) return NULL;
    uint32_t v[2];
    if (size < 16 || memcmp(b, magic, 8) || (memcpy(v, b + 8, 8), v[0] != 1 || v[1] != rec_size)) {
        rt_log("shader_prep: %s is not a %.8s file of this version; ignored", path, magic);
        free(b);
        return NULL;
    }
    *body_n = (size_t)size - 16;
    memmove(b, b + 16, *body_n);
    return b;
}

static void file_hash(const char *path, uint8_t out[32]) {
    uint64_t size = 0;
    uint8_t *b = plat_file_read(path, &size);
    if (b) rt_sha256(b, (size_t)size, out);
    else memset(out, 0, 32);
    free(b);
}

// ---- the marker

static int driver_cache_dir(char *out, size_t cap) {
    const char *e = getenv("MESA_SHADER_CACHE_DIR");
    if (e && *e) return snprintf(out, cap, "%s", e), 1;
    const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(out, cap, "%s/mesa_shader_cache", x);
    else snprintf(out, cap, "%s/.cache/mesa_shader_cache", h ? h : "");
    return 0;
}

// Must run before the Vulkan instance: the driver reads these at device creation.
static void shader_prep_env(void) {
    static int done;
    if (done++) return;
    const char *e = getenv("CB_DRIVER_CACHE");
    if (e && !strcmp(e, "system")) return;
    dirs();
    char p[PATH_MAX + 32];
    snprintf(p, sizeof p, "%s/driver-cache", g_dir);
    mkdir(p, 0755);
    setenv("MESA_SHADER_CACHE_DIR", p, 1);
    setenv("MESA_SHADER_CACHE_MAX_SIZE", "4G", 1);   // Mesa's default (1 GB) could evict prepared libraries
    setenv("__GL_SHADER_DISK_CACHE_PATH", p, 1);     // NVIDIA's proprietary driver
    setenv("__GL_SHADER_DISK_CACHE_SKIP_CLEANUP", "1", 1);
}

static void marker_text(char *out, size_t cap) {
    VkCtx *c = vk_ctx();
    uint8_t id[32], lh[32], ph[32], xh[32];
    char idh[65], lhh[65], phh[65], xhh[65], uuid[33], list[PATH_MAX], pl[PATH_MAX], px[PATH_MAX], dc[PATH_MAX];
    gfx_shader_identity(id);
    shipped("CB_SHADER_LIST", "shader-list.bin", list, sizeof list);
    shipped("CB_PIPELINE_LIST", "pipeline-list.bin", pl, sizeof pl);
    shipped("CB_SHADER_PATCHES", "shader-patches.bin", px, sizeof px);
    file_hash(list, lh), file_hash(pl, ph), file_hash(px, xh);
    hex(id, 32, idh), hex(lh, 32, lhh), hex(ph, 32, phh), hex(xh, 32, xhh);
    VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp};
    if (c) vkGetPhysicalDeviceProperties2(c->phys, &p2);
    static const uint8_t no_uuid[16];
    hex(c ? c->props.pipelineCacheUUID : no_uuid, 16, uuid);
    driver_cache_dir(dc, sizeof dc);
    snprintf(out, cap,
             "country-box shader preparation 1\n"
             "translator %s\n"
             "gpu %04x:%04x %s\n"
             "driver %u %s %s\n"
             "pipeline-cache-uuid %s\n"
             "shader-list %s\n"
             "pipeline-list %s\n"
             "shader-patches %s\n"
             "driver-cache %s\n",
             idh, c ? c->props.vendorID : 0, c ? c->props.deviceID : 0, c ? c->props.deviceName : "-",
             c ? c->props.driverVersion : 0, c ? dp.driverName : "-", c ? dp.driverInfo : "-", uuid, lhh, phh, xhh, dc);
}

int shader_prep_needed(char *why, size_t cap) {
    if (why && cap) why[0] = 0;
    shader_prep_env();
    if (!vk_init()) return 0;
    char list[PATH_MAX], own[PATH_MAX], mk[PATH_MAX];
    shipped("CB_SHADER_LIST", "shader-list.bin", list, sizeof list);
    in_dir("shader-answers.bin", own, sizeof own);
    if (!plat_file_exists(list) && !plat_file_exists(own)) return 0;   // nothing to prepare
    char *want = malloc(16384);
    if (!want) return 0;
    marker_text(want, 16384);
    in_dir("shader-prepared.id", mk, sizeof mk);
    uint64_t n = 0;
    char *have = (char *)plat_file_read(mk, &n);
    int need = !have || n != strlen(want) || memcmp(have, want, n);
    if (need && why && cap) {
        const char *w = "first start";
        if (have) {   // the first differing line names the cause
            const char *a = have, *b = want;
            while (*a && *a == *b) a++, b++;
            while (b > want && b[-1] != '\n') b--;
            w = !strncmp(b, "translator", 10)                                       ? "the shader translator changed"
                : !strncmp(b, "gpu", 3)                                             ? "the GPU changed"
                : !strncmp(b, "driver ", 7) || !strncmp(b, "pipeline-cache-uuid", 19) ? "the graphics driver changed"
                : !strncmp(b, "driver-cache", 12)                                   ? "the driver cache moved"
                                                                                    : "the shader lists changed";
        }
        snprintf(why, cap, "%s", w);
    }
    free(have), free(want);
    return need;
}

static int marker_write(void) {
    char *t = malloc(16384), mk[PATH_MAX];
    if (!t) return 0;
    marker_text(t, 16384);
    in_dir("shader-prepared.id", mk, sizeof mk);
    int ok = plat_file_write_atomic(mk, t, strlen(t));
    free(t);
    return ok;
}

// ---- the wanted programs: code hash -> found record (open addressing, built before the scan, then read-only keys)

typedef struct {
    uint8_t h[32];
    int used;
    volatile int32_t found;                                // 1 once a program record is stored
} Want;

typedef struct {
    uint8_t target[32], co[32];                            // the listed code hash; the code-only hash of its code
    uint16_t n;                                            // diff runs
    const uint8_t *runs;                                   // n x (offset u16, length u8, bytes)
} Patch;

typedef struct {
    Want *w;
    uint32_t cap, n;
    PlatMutex lock;                                        // progs, cand
    uint8_t *progs;                                        // seed-programs.bin body
    size_t progs_n, progs_cap;
    int64_t from_files, from_patches, from_recorded;
    Patch *patch;
    uint32_t n_patch;
    // patch candidates: controls in the files whose code equals a recorded program's that the files do not give
    uint8_t (*rec_co)[32];                                 // code-only hashes of the recorded programs (sorted)
    uint32_t n_rec_co;
    uint8_t *cand;                                         // records: co[32], control[CONTROL_SIZE]
    size_t cand_n, cand_cap;
} Wanted;

static Want *want_slot(Wanted *W, const uint8_t h[32]) {
    uint32_t i;
    memcpy(&i, h, 4);
    for (i &= W->cap - 1; W->w[i].used; i = (i + 1) & (W->cap - 1))
        if (!memcmp(W->w[i].h, h, 32)) return &W->w[i];
    return &W->w[i];
}

static void want_add(Wanted *W, const uint8_t h[32]) {
    Want *s = want_slot(W, h);
    if (!s->used) memcpy(s->h, h, 32), s->used = 1, W->n++;
}

static Want *want_find(Wanted *W, const uint8_t h[32]) {
    Want *s = want_slot(W, h);
    return s->used ? s : NULL;
}

static int bytes_put(uint8_t **p, size_t *n, size_t *cap, const void *src, size_t k) {
    if (*n + k > *cap) {
        size_t c = *cap ? *cap * 2 : 1 << 20;
        while (c < *n + k) c *= 2;
        uint8_t *t = realloc(*p, c);
        if (!t) return 0;
        *p = t, *cap = c;
    }
    memcpy(*p + *n, src, k), *n += k;
    return 1;
}

// gfx_shader.c build_code_hash: stage, control, length (u64), code[length], c1 length, c1 = code[c1 offset..length].
static void code_hash(int stage, const uint8_t *control, const uint8_t *code, uint32_t length, uint8_t out[32]) {
    uint32_t c1_off, st = (uint32_t)stage;
    memcpy(&c1_off, control + 0x700, 4);
    uint32_t c1_len = length - c1_off;
    uint64_t len64 = length;
    RtSha256 c;
    rt_sha256_begin(&c);
    rt_sha256_update(&c, &st, 4);
    rt_sha256_update(&c, control, CONTROL_SIZE);
    rt_sha256_update(&c, &len64, 8);
    rt_sha256_update(&c, code, length);
    rt_sha256_update(&c, &c1_len, 4);
    rt_sha256_update(&c, code + c1_off, c1_len);
    rt_sha256_end(&c, out);
}

static void code_only_hash(int stage, const uint8_t *code, uint32_t length, uint8_t out[32]) {
    uint32_t h[2] = {(uint32_t)stage, length};
    RtSha256 c;
    rt_sha256_begin(&c);
    rt_sha256_update(&c, "CBCO", 4);
    rt_sha256_update(&c, h, 8);
    rt_sha256_update(&c, code, length);
    rt_sha256_end(&c, out);
}

static int id_cmp(const void *a, const void *b) { return memcmp(a, b, 32); }

// A set of 32-byte digests (open addressing; grows at half full). 1 when d was new.
typedef struct { uint8_t (*v)[32]; uint8_t *used; uint32_t cap, n; } DSet;

static int dset_add(DSet *s, const uint8_t d[32]) {
    if ((s->n + 1) * 2 > s->cap) {
        uint32_t cap = s->cap ? s->cap * 2 : 4096;
        uint8_t (*v)[32] = calloc(cap, 32), *u = calloc(cap, 1);
        if (!v || !u) return free(v), free(u), 1;
        for (uint32_t i = 0; i < s->cap; i++) {
            if (!s->used[i]) continue;
            uint32_t j;
            memcpy(&j, s->v[i], 4);
            for (j &= cap - 1; u[j]; j = (j + 1) & (cap - 1)) {}
            memcpy(v[j], s->v[i], 32), u[j] = 1;
        }
        free(s->v), free(s->used);
        s->v = v, s->used = u, s->cap = cap;
    }
    uint32_t j;
    memcpy(&j, d, 4);
    for (j &= s->cap - 1; s->used[j]; j = (j + 1) & (s->cap - 1))
        if (!memcmp(s->v[j], d, 32)) return 0;
    memcpy(s->v[j], d, 32), s->used[j] = 1, s->n++;
    return 1;
}

static void dset_free(DSet *s) { free(s->v), free(s->used), memset(s, 0, sizeof *s); }

// Stores the program under h if it is wanted and not stored yet. 1 when stored.
static int store(Wanted *W, const uint8_t h[32], int stage, const uint8_t *control, const uint8_t *code, uint32_t length) {
    Want *s = want_find(W, h);
    if (!s || __atomic_load_n(&s->found, __ATOMIC_ACQUIRE)) return 0;
    uint32_t c1_off;
    memcpy(&c1_off, control + 0x700, 4);
    uint32_t head[3] = {(uint32_t)stage, length, length - c1_off};
    plat_mutex_lock(&W->lock);
    int ok = !s->found;
    if (ok) {
        ok = bytes_put(&W->progs, &W->progs_n, &W->progs_cap, head, 12) &&
             bytes_put(&W->progs, &W->progs_n, &W->progs_cap, h, 32) &&
             bytes_put(&W->progs, &W->progs_n, &W->progs_cap, control, CONTROL_SIZE) &&
             bytes_put(&W->progs, &W->progs_n, &W->progs_cap, code, length) &&
             bytes_put(&W->progs, &W->progs_n, &W->progs_cap, code + c1_off, length - c1_off);
        if (ok) __atomic_store_n(&s->found, 1, __ATOMIC_RELEASE);
    }
    plat_mutex_unlock(&W->lock);
    return ok;
}

static void apply_runs(uint8_t *ctl, const uint8_t *r, uint16_t n) {
    for (uint16_t i = 0; i < n; i++) {
        uint16_t off;
        memcpy(&off, r, 2);
        uint8_t len = r[2];
        if ((size_t)off + len <= CONTROL_SIZE) memcpy(ctl + off, r + 3, len);
        r += 3 + len;
    }
}

static void sink(void *ctx, int stage, const uint8_t *control, const uint8_t *code, uint32_t length) {
    Wanted *W = ctx;
    uint8_t h[32];
    code_hash(stage, control, code, length, h);
    if (store(W, h, stage, control, code, length)) plat_atomic_inc(&W->from_files);
    if (!W->n_patch && !W->n_rec_co) return;
    uint8_t co[32];
    code_only_hash(stage, code, length, co);
    for (uint32_t i = 0; i < W->n_patch; i++) {
        Patch *p = &W->patch[i];
        if (memcmp(p->co, co, 32)) continue;
        uint8_t ctl[CONTROL_SIZE], h2[32];
        memcpy(ctl, control, CONTROL_SIZE);
        apply_runs(ctl, p->runs, p->n);
        code_hash(stage, ctl, code, length, h2);
        if (!memcmp(h2, p->target, 32) && store(W, h2, stage, ctl, code, length)) plat_atomic_inc(&W->from_patches);
    }
    if (W->n_rec_co && bsearch(co, W->rec_co, W->n_rec_co, 32, id_cmp)) {
        plat_mutex_lock(&W->lock);
        bytes_put(&W->cand, &W->cand_n, &W->cand_cap, co, 32);
        bytes_put(&W->cand, &W->cand_n, &W->cand_cap, control, CONTROL_SIZE);
        plat_mutex_unlock(&W->lock);
    }
}

// shader-patches.bin: "CBSPATCH", version 1, 0; records: target code hash, code-only hash, run count (u16), runs.
static uint8_t *patches_read(Wanted *W) {
    char path[PATH_MAX];
    shipped("CB_SHADER_PATCHES", "shader-patches.bin", path, sizeof path);
    uint64_t size = 0;
    uint8_t *b = plat_file_read(path, &size);
    if (!b) return NULL;
    if (size < 16 || memcmp(b, "CBSPATCH", 8)) {
        rt_log("shader_prep: %s is not a patch file; ignored", path);
        free(b);
        return NULL;
    }
    size_t o = 16, n = 0;
    for (size_t q = o; q + 66 <= size; n++) {   // count, checking every run's bounds
        uint16_t k;
        memcpy(&k, b + q + 64, 2);
        q += 66;
        for (uint16_t i = 0; i < k && q + 3 <= size; i++) q += 3 + b[q + 2];
        if (q > size) break;
    }
    W->patch = calloc(n + 1, sizeof *W->patch);
    for (size_t i = 0; W->patch && i < n; i++) {
        Patch *p = &W->patch[W->n_patch++];
        memcpy(p->target, b + o, 32), memcpy(p->co, b + o + 32, 32), memcpy(&p->n, b + o + 64, 2);
        p->runs = b + o + 66;
        o += 66;
        for (uint16_t k = 0; k < p->n; k++) o += 3 + b[o + 2];
    }
    rt_log("shader_prep: %u control patches from %s", W->n_patch, path);
    return b;
}

// The patch from a file control to a recorded one: runs of differing bytes (gaps under 4 merged). 0 runs: too far.
static size_t make_runs(const uint8_t *from, const uint8_t *to, uint8_t *out, uint16_t *n_runs) {
    size_t o = 0, diff = 0;
    *n_runs = 0;
    for (int i = 0; i < CONTROL_SIZE;) {
        if (from[i] == to[i]) {
            i++;
            continue;
        }
        int j = i, last = i;
        while (j < CONTROL_SIZE && j - last < 4 && j - i < 255) {
            if (from[j] != to[j]) last = j;
            j++;
        }
        int len = last - i + 1;
        uint16_t off = (uint16_t)i;
        memcpy(out + o, &off, 2), out[o + 2] = (uint8_t)len, memcpy(out + o + 3, to + i, (size_t)len);
        o += 3 + (size_t)len, diff += (size_t)len, (*n_runs)++;
        i = last + 1;
        if (diff > PATCH_MAX_BYTES) return 0;
    }
    return o;
}

// ---- progress

typedef struct {
    ShaderPrepProgressFn fn;
    void *ctx;
    volatile int32_t stop;
    double t0, phase_t0[PREP_PHASES], phase_ms[PREP_PHASES];
    int last_phase;
    ShaderPrepProgress p;
    PlatThread *th;
} Mon;

// Work per item on one thread, before a phase has its own rate (measured on the PC, 2026-11-11; the phase's own rate
// replaces it after 3% of its items).
static double prior_ms(int phase) {
    switch (phase) {
    case PREP_PHASE_SCAN: return 0.004;     // per KB
    case PREP_PHASE_XLATE: return 60.0;     // per variant
    case PREP_PHASE_PIPES: return 20.0;     // per pipeline
    case PREP_PHASE_LIBS: return 30.0;      // per variant
    default: return 0;
    }
}

static void mon_fill(Mon *m) {
    double now = plat_now_ns() / 1e6;
    int ph = __atomic_load_n(&shader_prep_count.phase, __ATOMIC_SEQ_CST);
    if (ph != m->last_phase) {
        if (m->last_phase >= 0) m->phase_ms[m->last_phase] = now - m->phase_t0[m->last_phase];
        m->phase_t0[ph] = now, m->last_phase = ph;
    }
    ShaderPrepProgress *p = &m->p;
    p->phase = ph;
    p->done = __atomic_load_n(&shader_prep_count.done, __ATOMIC_SEQ_CST);
    p->total = __atomic_load_n(&shader_prep_count.total, __ATOMIC_SEQ_CST);
    if (p->done > p->total) p->done = p->total;
    p->elapsed_ms = now - m->t0;
    double thr = p->threads > 0 ? p->threads : 1, eta = 0;
    for (int f = ph; f < PREP_PHASE_DONE; f++) {
        if (f == ph && p->done > 0 && p->done * 33 >= p->total) {   // the phase's own rate
            eta += (now - m->phase_t0[ph]) / (double)p->done * (double)(p->total - p->done);
            continue;
        }
        double left = f == ph ? (double)(p->total - p->done) : f == PREP_PHASE_XLATE || f == PREP_PHASE_LIBS ? (double)p->variants
                    : f == PREP_PHASE_PIPES ? (double)p->pipelines : 0;
        if (f == PREP_PHASE_SCAN) left /= 1024;   // the prior is per KB
        eta += prior_ms(f) * left / thr;
    }
    p->eta_ms = ph == PREP_PHASE_START ? -1 : eta;
}

static void mon_main(void *arg) {
    Mon *m = arg;
    while (!__atomic_load_n(&m->stop, __ATOMIC_SEQ_CST)) {
        mon_fill(m);
        m->fn(&m->p, m->ctx);
        plat_sleep_ns(100 * 1000000ull);
    }
}

// ---- the run

static const char *phase_name(int ph) {
    static const char *const n[] = {"lists", "scan", "translate", "pipelines", "libraries", "done"};
    return ph >= 0 && ph < PREP_PHASES ? n[ph] : "?";
}

static const ShaderPrepGame *g_game;
static ShaderPrepGame g_game_store;

const ShaderPrepGame *shader_prep_game(void) {
    if (g_game) return g_game;
    g_game_store.romfs = rt_config()->romfs;
    if (shader_src_unity.detect(g_game_store.romfs)) {
        g_game_store.name = "Donkey Kong Country Returns HD";
        g_game_store.source = &shader_src_unity;
    } else if (shader_src_dktf.detect(g_game_store.romfs)) {
        g_game_store.name = "Donkey Kong Country: Tropical Freeze";
        g_game_store.source = &shader_src_dktf;
    } else {
        g_game_store.name = "this game";
    }
    return g_game = &g_game_store;
}

// Adds the list's records (input ids zeroed) to *ans unless (stage, code hash, answers) is there. Wants their programs.
typedef struct { uint8_t *p; size_t n, cap; } RecBuf;

static size_t add_variants(RecBuf *ans, Wanted *W, const uint8_t *b, size_t n, uint8_t prio, DSet *seen) {
    size_t added = 0;
    for (size_t o = 0; o + ANS_REC <= n; o += ANS_REC) {
        const uint8_t *r = b + o;
        uint8_t id[32];   // the variant's identity: stage, code hash, answers
        RtSha256 c;
        rt_sha256_begin(&c);
        rt_sha256_update(&c, r, 36);
        rt_sha256_update(&c, r + 68, 147);
        rt_sha256_end(&c, id);
        if (!dset_add(seen, id)) continue;
        uint8_t rec[ANS_REC];
        memcpy(rec, r, ANS_REC);
        memset(rec + 36, 0, 32);
        rec[ANS_REC - 1] = prio;
        bytes_put(&ans->p, &ans->n, &ans->cap, rec, ANS_REC);
        want_add(W, r + 4);
        added++;
    }
    return added;
}

// Merges pipeline-list.bin's records into pipeline-keys.bin (its own records first), as tools/prepare_shaders.py
// merge_pipelines does. Returns the records in the result.
static long merge_pipelines(void) {
    char pl[PATH_MAX], keys[PATH_MAX];
    shipped("CB_PIPELINE_LIST", "pipeline-list.bin", pl, sizeof pl);
    in_dir("pipeline-keys.bin", keys, sizeof keys);
    uint64_t ln = 0, kn = 0;
    uint8_t *l = plat_file_read(pl, &ln), *k = plat_file_read(keys, &kn);
    long out = -1;
    uint32_t lv[2] = {0, 0}, kv[2] = {0, 0};
    if (l && ln >= 16) memcpy(lv, l + 8, 8);
    if (k && kn >= 16) memcpy(kv, k + 8, 8);
    int l_ok = l && ln >= 16 && !memcmp(l, "TOTKPKEY", 8) && lv[1];
    int k_ok = k && kn >= 16 && !memcmp(k, "TOTKPKEY", 8) && kv[1];
    if (!l_ok) {
        out = k_ok ? (long)((kn - 16) / kv[1]) : 0;
    } else {
        uint32_t rs = lv[1];
        if (k_ok && (kv[0] != lv[0] || kv[1] != rs)) k_ok = 0;   // another version: replaced by the list
        size_t kr = k_ok ? (size_t)(kn - 16) / rs : 0, lr = (size_t)(ln - 16) / rs;
        uint8_t *o = malloc(16 + (kr + lr) * rs);
        if (o) {
            memcpy(o, l, 16);
            if (kr) memcpy(o + 16, k + 16, kr * rs);
            size_t n = kr, added = 0;
            DSet seen = {0};
            uint8_t d[32];
            for (size_t j = 0; j < kr; j++) rt_sha256(o + 16 + j * rs, rs, d), dset_add(&seen, d);
            for (size_t i = 0; i < lr; i++) {
                const uint8_t *r = l + 16 + i * rs;
                rt_sha256(r, rs, d);
                if (dset_add(&seen, d)) memcpy(o + 16 + n * rs, r, rs), n++, added++;
            }
            dset_free(&seen);
            if (added || !k_ok) plat_file_write_atomic(keys, o, 16 + n * rs);
            rt_log("shader_prep: %zu listed pipelines: %zu added to %zu in %s", lr, added, kr, keys);
            out = (long)n;
            free(o);
        }
    }
    free(l), free(k);
    return out;
}

int shader_prep_run(const ShaderPrepGame *g, ShaderPrepProgressFn progress, void *ctx) {
    shader_prep_env();
    dirs();
    double t0 = plat_now_ns() / 1e6;
    int cpus = (int)plat_cpu_count();
    shader_prep_phase(PREP_PHASE_START, 0);
    Mon m = {.fn = progress, .ctx = ctx, .t0 = t0, .last_phase = -1};
    m.p.threads = cpus;
    static char why[128];
    if (!shader_prep_needed(why, sizeof why)) snprintf(why, sizeof why, "%s", "asked for");
    m.p.why = why;
    if (progress) m.th = plat_thread_new(mon_main, &m, "prep-progress");
    rt_log("shader_prep: preparing shaders for %s (%s), %d threads, caches in %s", g->name, why, cpus, g_dir);

    // 1. the variants: the game's list, then this install's own recordings
    Wanted W = {.lock = PLAT_MUTEX_INIT, .cap = 1 << 16};
    W.w = calloc(W.cap, sizeof *W.w);
    RecBuf ans = {0};
    DSet seen = {0};
    size_t n_list = 0, n_own = 0, ln = 0, an = 0, pn = 0;
    char list[PATH_MAX], own[PATH_MAX], ownp[PATH_MAX];
    shipped("CB_SHADER_LIST", "shader-list.bin", list, sizeof list);
    in_dir("shader-answers.bin", own, sizeof own);
    in_dir("shader-programs.bin", ownp, sizeof ownp);
    uint8_t *lb = rec_file(list, "TOTKSANS", ANS_REC, &ln), *ab = rec_file(own, "TOTKSANS", ANS_REC, &an);
    if (W.w && lb) n_list = add_variants(&ans, &W, lb, ln, 1, &seen);
    if (W.w && ab) n_own = add_variants(&ans, &W, ab, an, 0, &seen);
    free(lb), free(ab), dset_free(&seen);
    uint8_t *patch_file = W.w ? patches_read(&W) : NULL;
    long pipelines = merge_pipelines();
    m.p.pipelines = pipelines > 0 ? pipelines : 0;
    m.p.variants = (int64_t)(ans.n / ANS_REC);
    rt_log("shader_prep: %zu variants (%zu listed in %s, %zu more recorded here) of %u programs; %ld pipelines",
           ans.n / ANS_REC, n_list, list, n_own, W.n, pipelines);

    // The recorded programs the files may lack: their code-only hashes make the scan keep candidate controls.
    uint8_t *pb = W.w ? rec_file(ownp, "TOTKSPRG", 0, &pn) : NULL;
    size_t n_rec = 0;
    for (size_t o = 0; pb && o + PRG_HEAD <= pn;) {
        uint32_t len, c1;
        memcpy(&len, pb + o + 4, 4), memcpy(&c1, pb + o + 8, 4);
        if (len > (64u << 20) || c1 > len || o + PRG_HEAD + len + c1 > pn) break;
        n_rec++, o += PRG_HEAD + len + c1;
    }
    W.rec_co = n_rec ? calloc(n_rec, 32) : NULL;
    for (size_t o = 0, i = 0; W.rec_co && i < n_rec; i++) {
        uint32_t st, len, c1;
        memcpy(&st, pb + o, 4), memcpy(&len, pb + o + 4, 4), memcpy(&c1, pb + o + 8, 4);
        if (want_find(&W, pb + o + 12)) code_only_hash((int)st, pb + o + PRG_HEAD, len, W.rec_co[W.n_rec_co++]);
        o += PRG_HEAD + len + c1;
    }
    if (W.n_rec_co) qsort(W.rec_co, W.n_rec_co, 32, id_cmp);

    // 2. the programs, from the user's game files
    int rc = 1;
    if (!W.w || !W.n) {
        rt_log("shader_prep: nothing to prepare (no list, nothing recorded)");
        goto done;
    }
    if (g->source) {
        int64_t total = 0;
        shader_prep_phase(PREP_PHASE_SCAN, 1);
        long found = g->source->scan(g->romfs, cpus, sink, &W, &shader_prep_count.done, &total);
        __atomic_store_n(&shader_prep_count.total, total, __ATOMIC_SEQ_CST);
        rt_log("shader_prep: %s: %ld programs in the game files; %lld listed ones from them, %lld through control "
               "patches", g->source->name, found, (long long)W.from_files, (long long)W.from_patches);
    }
    // the fallback: this install's recorded copies
    for (size_t o = 0, i = 0; pb && i < n_rec; i++) {
        uint32_t st, len, c1;
        memcpy(&st, pb + o, 4), memcpy(&len, pb + o + 4, 4), memcpy(&c1, pb + o + 8, 4);
        const uint8_t *h = pb + o + 12, *ctl = pb + o + 44, *code = ctl + CONTROL_SIZE;
        if (store(&W, h, (int)st, ctl, code, len)) {
            W.from_recorded++;
            // the patch that would give it from the files, when a file control with the same code is close
            uint8_t co[32], runs[3 * CONTROL_SIZE], best[3 * CONTROL_SIZE];
            uint16_t nr = 0, best_nr = 0;
            size_t best_n = 0;
            code_only_hash((int)st, code, len, co);
            for (size_t q = 0; q + 32 + CONTROL_SIZE <= W.cand_n; q += 32 + CONTROL_SIZE) {
                if (memcmp(W.cand + q, co, 32)) continue;
                size_t k = make_runs(W.cand + q + 32, ctl, runs, &nr);
                if (k && (!best_n || k < best_n)) best_n = k, best_nr = nr, memcpy(best, runs, k);
            }
            if (best_n) {
                char path[PATH_MAX];
                in_dir("shader-patches.found.bin", path, sizeof path);
                FILE *f = fopen(path, "ab");
                if (f) {
                    if (ftell(f) == 0) fwrite("CBSPATCH\1\0\0\0\0\0\0\0", 1, 16, f);
                    fwrite(h, 1, 32, f), fwrite(co, 1, 32, f), fwrite(&best_nr, 2, 1, f), fwrite(best, 1, best_n, f);
                    fclose(f);
                }
                rt_log("shader_prep: recorded program %02x%02x%02x%02x differs from one in the game files in %u "
                       "control runs; patch appended to %s", h[0], h[1], h[2], h[3], best_nr, path);
            }
        }
        o += PRG_HEAD + len + c1;
    }
    int64_t missing = (int64_t)W.n - W.from_files - W.from_patches - W.from_recorded;
    rt_log("shader_prep: programs: %u wanted; %lld from the game files, %lld through patches, %lld from this "
           "install's recordings, %lld not found (their variants are dropped)", W.n, (long long)W.from_files,
           (long long)W.from_patches, (long long)W.from_recorded, (long long)missing);

    // 3. the seed files: every variant whose program was found
    {
        char sa[PATH_MAX], sp[PATH_MAX];
        in_dir("seed-answers.bin", sa, sizeof sa);
        in_dir("seed-programs.bin", sp, sizeof sp);
        RecBuf seed = {0};
        bytes_put(&seed.p, &seed.n, &seed.cap, "TOTKSANS\1\0\0\0", 12);
        uint32_t rs = ANS_REC;
        bytes_put(&seed.p, &seed.n, &seed.cap, &rs, 4);
        int64_t kept = 0;
        for (size_t o = 0; o + ANS_REC <= ans.n; o += ANS_REC) {
            Want *s = want_find(&W, ans.p + o + 4);
            if (s && s->found) bytes_put(&seed.p, &seed.n, &seed.cap, ans.p + o, ANS_REC), kept++;
        }
        RecBuf prg = {0};
        bytes_put(&prg.p, &prg.n, &prg.cap, "TOTKSPRG\1\0\0\0\0\0\0\0", 16);
        bytes_put(&prg.p, &prg.n, &prg.cap, W.progs, W.progs_n);
        int ok = seed.p && prg.p && plat_file_write_atomic(sa, seed.p, seed.n) && plat_file_write_atomic(sp, prg.p, prg.n);
        free(seed.p), free(prg.p);
        m.p.variants = kept;
        if (!ok) {
            rt_log("shader_prep: cannot write the seed files in %s", g_dir);
            goto done;
        }
        // 4. the GPU half: translation, pipelines, stage libraries, pipeline-cache.bin
        rc = vk_pipeline_prepare(1);
        remove(sa), remove(sp);   // seed-programs.bin is game code
    }
    if (!rc && !marker_write()) rt_log("shader_prep: cannot write the marker in %s", g_dir), rc = 1;

done:
    m.phase_ms[m.last_phase >= 0 ? m.last_phase : 0] = plat_now_ns() / 1e6 - m.phase_t0[m.last_phase >= 0 ? m.last_phase : 0];
    shader_prep_phase(PREP_PHASE_DONE, 0);
    if (m.th) {
        __atomic_store_n(&m.stop, 1, __ATOMIC_SEQ_CST);
        plat_thread_join(m.th);
    }
    mon_fill(&m);
    rt_log("shader_prep: %s in %.0f ms (scan %.0f, translate %.0f, pipelines %.0f, libraries %.0f)",
           rc ? "FAILED" : "done", plat_now_ns() / 1e6 - t0, m.phase_ms[PREP_PHASE_SCAN], m.phase_ms[PREP_PHASE_XLATE],
           m.phase_ms[PREP_PHASE_PIPES], m.phase_ms[PREP_PHASE_LIBS]);
    if (progress) {
        m.p.failed = rc != 0;
        m.p.eta_ms = 0;
        progress(&m.p, ctx);
    }
    free(W.w), free(W.progs), free(W.patch), free(W.rec_co), free(W.cand), free(patch_file), free(pb), free(ans.p);
    return rc;
}

// ---- the command line: --prepare-shaders

static void cli_progress(const ShaderPrepProgress *p, void *ctx) {
    int *last = ctx;
    static double last_ms;
    if (p->phase == *last && p->elapsed_ms - last_ms < 2000 && p->phase != PREP_PHASE_DONE) return;
    *last = p->phase, last_ms = p->elapsed_ms;
    printf("prepare-shaders: %-10s %lld / %lld  elapsed %.1f s", phase_name(p->phase), (long long)p->done,
           (long long)p->total, p->elapsed_ms / 1000);
    if (p->eta_ms >= 0 && p->phase != PREP_PHASE_DONE) printf("  about %.0f s left", p->eta_ms / 1000);
    printf("%s\n", p->phase == PREP_PHASE_DONE ? (p->failed ? "  FAILED" : "  done") : "");
}

int shader_prep_status_cli(void) {
    char why[128];
    shader_prep_env();
    if (!vk_init()) { printf("{\"status\": \"unknown\", \"reason\": \"no Vulkan device\"}\n"); return 1; }
    int need = shader_prep_needed(why, sizeof why);
    const char *st = !need ? "prepared" : strstr(why, "driver") || strstr(why, "GPU") ? "driver-changed" : "needs-prep";
    printf("{\"status\": \"%s\", \"reason\": \"%s\"}\n", st, need ? why : "");
    return 0;
}

int shader_prep_cli(void) {
    int last = -1;
    return shader_prep_run(shader_prep_game(), cli_progress, &last);
}

// ---- the boot screen

typedef struct {
    const ShaderPrepGame *g;
    volatile int32_t done;
    int rc;
} BootPrep;

static void boot_progress(const ShaderPrepProgress *p, void *ctx) {
    (void)ctx;
    prep_screen_set(p);
}

static void boot_worker(void *arg) {
    BootPrep *b = arg;
    b->rc = shader_prep_run(b->g, boot_progress, NULL);
    __atomic_store_n(&b->done, 1, __ATOMIC_SEQ_CST);
}

void shader_prep_boot(void) {
    shader_prep_env();
    const char *e = getenv("CB_SHADER_PREP");
    const char *gpu = rt_config()->gpu;
    // --shot runs and MP4_PIPE_SYNC=1 build pipelines synchronously on purpose: the preparation would switch that
    if ((e && !strcmp(e, "0")) || (gpu && !strcmp(gpu, "null")) || rt_config()->shot || !vk_pipeline_async_on()) return;
    char why[128];
    if (!(e && !strcmp(e, "force")) && !shader_prep_needed(why, sizeof why)) return;
    BootPrep b = {.g = shader_prep_game()};
    double t0 = plat_now_ns() / 1e6;
    prep_screen_begin(b.g->name);
    PlatThread *t = plat_thread_new(boot_worker, &b, "shader-prep");
    if (!t) boot_worker(&b);
    while (!__atomic_load_n(&b.done, __ATOMIC_SEQ_CST)) {
        vk_present_screen();   // FIFO: paced by the display
        plat_sleep_ns(8 * 1000000ull);
    }
    if (t) plat_thread_join(t);
    for (double end = plat_now_ns() / 1e6 + (b.rc ? 4000 : 600); plat_now_ns() / 1e6 < end;) {   // the last state, briefly
        vk_present_screen();
        plat_sleep_ns(8 * 1000000ull);
    }
    prep_screen_end();
    rt_log("shader_prep: boot screen %.0f ms; %s", plat_now_ns() / 1e6 - t0,
           b.rc ? "the preparation failed, the game starts without it" : "the game starts");
}
