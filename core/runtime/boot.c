// From fe3h-pc-recomp runtime/boot.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4; MP4 module list, init_array sizes, main stack, per-version boot checkpoint; nn::init::Start takes three callbacks again, as in TotK's SDK; shots are turned over for the lower-left window origin.
// From totk-pc-recomp runtime/boot.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), renamed to fe3h; FE3H module list, init_array sizes, main stack, per-version boot checkpoint, and SDK 6.5.0's two-callback nn::init::Start. Country-Box: plat threads, process start time from /proc/self/stat (Linux).
// Adapted from MacNativeGames games/totk/runtime/loader.c (lines 456-477) and services.c (lines 219-235) @ b75be8cf
// (MIT, Copyright (c) 2026 MacNativeGames contributors) and LAS Recomp runtime/main.c (lines 16-35, 141-159) @
// da35b83 (MIT, Copyright (c) 2026 Peiikai).
// The boot driver: the guest main thread runs the game's own nn::init::Start
// with three host callbacks, the second of which runs DT_INIT of main, subsdk0 and sdk; Start then calls nnMain.
// --boot ends at the first nvnBootstrapLoader call:
//   nvnBootstrapLoader     Country-Box, Donkey Kong Country Returns HD 1.0.0: called from main+0xf0a1e4, the only bl to
//                          its PLT stub (main+0x1c93220), after every module's constructor flag reads 2; logs the run
//                          statistics, exits 0. Tropical Freeze 1.0.0: from main+0x4eb0c, the only bl to its PLT stub
//                          (main+0xb926f0)
// The SetFocusHandlingMode checkpoint (ov_system.c) fires on the way: Returns HD imports it, but its only caller
// (main+0x18a4b80) reaches the PLT stub by a tail branch, so the return address is that wrapper's caller and is not
// checked; the checkpoint still requires DT_INIT done and every constructor flag at 2.
// With --seconds S the run continues past that call; rt_frame_presented ends it once the world clock passed S.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cfg.h"
#include "platform/platform.h"
#include "shader_prep.h"
#include "fs.h"
#include "gfx.h"
#include "nvn_api.h"
#include "vk.h"

// nn::init::Start(main thread handle, arguments region, loader_ready, module_inits, module_finis): SDK 19.3 exports
// the three-callback form TotK's SDK has (FE3H's 6.5.0 had two).
#define SYM_INIT_START "_ZN2nn4init5StartEmmPFvvES2_S2_"
// Country-Box: SDK 4.4.0 (Tropical Freeze) exports the two-callback form FE3H's 6.5.0 had. Its body (measured with
// capstone on the 1.0.0 sdk) calls the first callback where the three-callback form calls loader_ready and the second
// where it calls module_inits; there is no finalizer callback.
#define SYM_INIT_START2 "_ZN2nn4init5StartEmmPFvvES2_"
// main: return address of the first nvnBootstrapLoader call per game version (rt_game_version, build_game.py), found
// by the GOT slot of its import (Country-Box: measured on the Returns HD 1.0.0 exefs, recomp/nso.py + a scan for the
// PLT stub and the bl to it); nvn_ret() picks the running version's. CHECKPOINT_RET 0: the checkpoint's caller is not
// checked (see the header).
#define CHECKPOINT_RET 0ull
#define CTOR_DONE 2                      // a module's constructor flag after its DT_INIT ran the init_array

// The init_array sizes (DT_INIT_ARRAYSZ / 8, measured with tools/measure_modules.py), in the order the constructors
// run, the same three modules as TotK. Country-Box: one row per game (main.npdm's program id, rt_program_id) and
// version, since two games share version 1.0.0.
typedef struct { const char *name; uint64_t entries; } InitCount;
// nvn_api: the NVN API version nvnDeviceGetInteger reports (major, minor); the game compares it with the one it was built
// against (Tropical Freeze: NVNManaged::CDevice::Init wants major 53 and minor >= 105, measured on 1.0.0).
// nv_allocator: nv::SetGraphicsAllocator also reaches the SDK's own (lifted) one, so NvOsAlloc allocates from the game's
// allocator. Tropical Freeze's SDK 4.4.0 movie library decodes AAC through ACodec, whose android::HeapObject allocations
// go to NvOsAlloc; without the game's allocator it returns NULL after its small static pool (measured: a crash in
// ACodec::UninitializedState::onAllocateComponent at the first cinematic).
typedef struct { uint64_t program_id; int version; uint64_t nvn_ret; InitCount inits[3]; int nvn_api[2]; int nv_allocator;
                 int movie_self_paced; } GameBoot;   // movie_self_paced: ov_movie.c pace_on
static const GameBoot games[] = {
    {0x01009D901BC56000ull, 100, 0xf0a1e8ull, {{"main", 310}, {"subsdk0", 45}, {"sdk", 94}}, {55, 15}, 0, 0},    // Returns HD 1.0.0
    {0x0100C1F0051B6000ull, 100, 0x4eb10ull, {{"main", 217}, {"subsdk0", 45}, {"sdk", 105}}, {53, 105}, 1, 1},    // Tropical Freeze 1.0.0
};

static volatile int32_t inits_done, checkpoint_seen;
uint64_t rt_guest_start_ns;

static const RtLoaded *module_named(const char *name) {
    for (int i = 0; i < ldr_count(); i++)
        if (!strcmp(ldr_module(i)->name, name)) return ldr_module(i);
    rt_fatal("boot: module %s is not loaded", name);
}

static uint32_t ctor_state(const RtLoaded *m) { return LD32(m->ctor_flag); }

static const GameBoot *game_boot(void) {
    for (unsigned k = 0; k < sizeof games / sizeof *games; k++)
        if (games[k].program_id == rt_program_id() && games[k].version == rt_game_version) return &games[k];
    rt_fatal("boot: no boot table for program %016llx version %d (boot.c games)", (unsigned long long)rt_program_id(),
             rt_game_version);
}

static uint64_t nvn_ret(void) { return game_boot()->nvn_ret; }

int rt_game_movie_self_paced(void) {
    for (unsigned k = 0; k < sizeof games / sizeof *games; k++)
        if (games[k].program_id == rt_program_id() && games[k].version == rt_game_version) return games[k].movie_self_paced;
    return 0;
}

int rt_game_nv_allocator(void) {
    for (unsigned k = 0; k < sizeof games / sizeof *games; k++)
        if (games[k].program_id == rt_program_id() && games[k].version == rt_game_version) return games[k].nv_allocator;
    return 0;
}

// Without a row (probes, an unknown game) the devinfo table's 55.15 (nvn_api_core.c) stands.
void rt_nvn_api_version(int *major, int *minor) {
    *major = 55, *minor = 15;
    for (unsigned k = 0; k < sizeof games / sizeof *games; k++)
        if (games[k].program_id == rt_program_id() && games[k].version == rt_game_version)
            *major = games[k].nvn_api[0], *minor = games[k].nvn_api[1];
}

static const char *caller(const Cpu *c, char *buf, int n) { return ldr_describe(c->x[30] - 4, buf, n); }

// main.npdm META: the main thread's priority (+0xE), core (+0xF) and stack size (+0x1C). kernel.c builds the main
// thread with MP4's values (the same in 1.0.0 and 1.1.0); a different npdm means a different game build.
static void check_npdm(void) {
    size_t n;
    uint8_t *npdm = ldr_read_verified(rt_config()->exefs, "main.npdm", &n);
    if (n < 0x80 || memcmp(npdm, "META", 4)) rt_fatal("boot: main.npdm has no META header");
    uint32_t stack;
    memcpy(&stack, npdm + 0x1C, 4);
    int prio = npdm[0xE], core = npdm[0xF];
    free(npdm);
    if (prio != 44 || core != 0 || stack != 0x100000)
        rt_fatal("boot: main.npdm asks for priority %d, core %d, stack %#x; kernel.c builds 44, 0, 0x100000", prio,
                 core, stack);
}

// ---- the three callbacks of nn::init::Start (rtld's job on hardware)
static void cb_loader_ready(Cpu *c) {
    (void)c;
    rt_log("boot: rtld callback: modules linked");
}

// DT_INIT registers the module with musl (__nnmusl_init_dso) and runs its init_array; the constructor flag reads 2
// afterwards (MacNativeGames loader.c:458-462).
static void cb_module_inits(Cpu *c) {
    if (plat_atomic_cas(&inits_done, 0, 0)) rt_fatal("boot: module constructors requested twice");
    const InitCount *expected_inits = game_boot()->inits;
    for (unsigned k = 0; k < 3; k++) {
        const RtLoaded *m = module_named(expected_inits[k].name);
        uint64_t entries = m->init_array_size / 8;
        if (!m->init) rt_fatal("boot: %s has no DT_INIT", m->name);
        rt_log("boot: ENTER %s DT_INIT (%llu init_array entries)", m->name, (unsigned long long)entries);
        rt_call(c, m->init, 0);
        uint32_t state = ctor_state(m);
        rt_logf("boot: DT_INIT %s state=%u (%llu)\n", m->name, state, (unsigned long long)entries);
        if (state != CTOR_DONE) rt_fatal("boot: %s DT_INIT left its constructor flag at %u, not 2", m->name, state);
        if (entries != expected_inits[k].entries)
            rt_fatal("boot: %s has %llu init_array entries, this game version has %llu", m->name, (unsigned long long)entries,
                     (unsigned long long)expected_inits[k].entries);
    }
    plat_atomic_xchg(&inits_done, 1);
}

static void cb_module_finis(Cpu *c) {
    char lr[128];
    rt_fatal("boot: the game asked for module finalizers (from %s): the guest exited", caller(c, lr, sizeof lr));
}

// ---- checkpoints (host.h)
static void stats_line(void) {
    RtStats s = rt_stats();
    rt_logf("boot: passed (%llu SVCs, %llu threads, %llu host calls)\n", (unsigned long long)s.svcs,
           (unsigned long long)s.threads, (unsigned long long)s.host_calls);
}

void rt_boot_event(const char *what, Cpu *c, const char *detail) {
    char at[128];
    const RtLoaded *main_mod = module_named("main");
    uint64_t ret = c->x[30];
    caller(c, at, sizeof at);
    if (!strcmp(what, "checkpoint")) {
        if (CHECKPOINT_RET && ret != main_mod->base + CHECKPOINT_RET)
            rt_fatal("boot: checkpoint %s reached from %s, expected main+%#llx", detail, at,
                     (unsigned long long)CHECKPOINT_RET);
        if (!plat_atomic_cas(&inits_done, 0, 0)) rt_fatal("boot: checkpoint %s before DT_INIT", detail);
        for (int i = 0; i < ldr_count(); i++)
            if (ctor_state(ldr_module(i)) != CTOR_DONE)
                rt_fatal("boot: checkpoint %s: %s constructor flag reads %u", detail, ldr_module(i)->name,
                         ctor_state(ldr_module(i)));
        plat_atomic_xchg(&checkpoint_seen, 1);
        rt_logf("boot: checkpoint %s from main+%#llx\n", detail, (unsigned long long)(ret - main_mod->base));
        return;
    }
    if (!strcmp(what, "nvn")) {
        if (ret != main_mod->base + nvn_ret())
            rt_fatal("boot: first NVN call %s(\"%s\") from %s, expected main+%#llx", "nvnBootstrapLoader", detail, at,
                     (unsigned long long)nvn_ret());
        if (!plat_atomic_cas(&inits_done, 0, 0)) rt_fatal("boot: first NVN call before DT_INIT");
        rt_logf("boot: first NVN call nvnBootstrapLoader(\"%s\") from main+%#llx\n", detail,
               (unsigned long long)(ret - main_mod->base));
        if (rt_config()->seconds) return;   // --seconds: the run goes on into the world
        if (rt_trace & RT_TRACE_HOSTCALL) hostfn_report();
        stats_line();
        fflush(stdout);
        fflush(stderr);
        _exit(0);   // other guest threads are still running; nothing of theirs needs an orderly end
    }
    rt_fatal("boot: unknown boot event %s (%s) from %s", what, detail ? detail : "", at);
}

// ---- --seconds S
// A PNG with stored (uncompressed) deflate blocks: no compression library, and the shot is only read by tools.
// rt_write_png (declared in vk.h) is shared with the MP4_DUMP frame dump (vk_dump.c).
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t hdr[8] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    memcpy(hdr + 4, type, 4);
    uint32_t crc = crc32_update(crc32_update(~0u, hdr + 4, 4), data, n) ^ ~0u;
    uint8_t tail[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(hdr, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    fwrite(tail, 1, 4, f);
}

void rt_write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    uint64_t row = 1 + 4ull * w, raw_n = row * h;
    uint8_t *raw = malloc(raw_n);
    if (!raw) rt_fatal("boot: out of memory for the shot");
    for (uint32_t y = 0; y < h; y++) {
        raw[y * row] = 0;   // filter: none
        memcpy(raw + y * row + 1, rgba + 4ull * w * y, 4ull * w);
    }
    uint64_t blocks = (raw_n + 65534) / 65535;
    uint8_t *z = malloc(2 + raw_n + 5 * blocks + 4);
    if (!z) rt_fatal("boot: out of memory for the shot");
    uint64_t zn = 0;
    z[zn++] = 0x78, z[zn++] = 0x01;
    uint32_t a = 1, b = 0;
    for (uint64_t off = 0; off < raw_n; off += 65535) {
        uint32_t k = raw_n - off > 65535 ? 65535 : (uint32_t)(raw_n - off);
        z[zn++] = off + k >= raw_n;
        z[zn++] = (uint8_t)k, z[zn++] = (uint8_t)(k >> 8), z[zn++] = (uint8_t)~k, z[zn++] = (uint8_t)(~k >> 8);
        memcpy(z + zn, raw + off, k);
        zn += k;
        for (uint32_t i = 0; i < k; i++) a = (a + raw[off + i]) % 65521, b = (b + a) % 65521;
    }
    uint32_t ad = b << 16 | a;
    z[zn++] = (uint8_t)(ad >> 24), z[zn++] = (uint8_t)(ad >> 16), z[zn++] = (uint8_t)(ad >> 8), z[zn++] = (uint8_t)ad;
    FILE *f = fopen(path, "wb");
    if (!f) rt_fatal("boot: cannot write %s", path);
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    uint8_t ihdr[13] = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                        (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 6, 0, 0, 0};
    fwrite(sig, 1, 8, f);
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    if (fclose(f)) rt_fatal("boot: cannot write %s", path);
    free(z);
    free(raw);
}

// A frame read back from the game's texture sits bottom-up with NVN's lower-left window origin (this game;
// nvn_api_core.c), as the present blit turns it over for the display; the files should read as the screen does.
void flip_rows(uint8_t *px, uint32_t w, uint32_t h) {
    if (!nvn_origin_lower_left()) return;
    uint8_t *tmp = malloc(4ull * w);
    if (!tmp) rt_fatal("boot: out of memory for a row");
    for (uint32_t y = 0; y < h / 2; y++) {
        uint8_t *a = px + 4ull * w * y, *b = px + 4ull * w * (h - 1 - y);
        memcpy(tmp, a, 4ull * w), memcpy(a, b, 4ull * w), memcpy(b, tmp, 4ull * w);
    }
    free(tmp);
}

static void save_shot(const tex_t *t, const char *path) {
    int fmt = t->d.format;
    int bgra = fmt == NVN_FORMAT_BGRA8 || fmt == NVN_FORMAT_BGRA8_SRGB;
    if (!bgra && fmt != NVN_FORMAT_RGBA8 && fmt != NVN_FORMAT_RGBA8_SRGB)
        rt_fatal("boot: --shot supports 8-bit RGBA and BGRA targets, the presented texture has NVN format %#x", fmt);
    uint32_t w = t->d.width, h = t->d.height;
    uint8_t *px = malloc(4ull * w * h);
    if (!px) rt_fatal("boot: out of memory for the shot");
    view_t view;
    memset(&view, 0, sizeof view);
    NVNcopyRegion r = {.width = (int)w, .height = (int)h, .depth = 1};
    if (gfx_replay_async()) gfx_backend()->finish();   // the presented frame may still be queued for the replay
    gfx_backend()->read_texels(t, &view, &r, px, 4ull * w, 4ull * w * h);
    flip_rows(px, w, h);
    if (bgra)
        for (uint64_t i = 0; i < 4ull * w * h; i += 4) {
            uint8_t x = px[i];
            px[i] = px[i + 2];
            px[i + 2] = x;
        }
    rt_write_png(path, px, w, h);
    free(px);
    rt_logf("boot: shot %s (%ux%u)\n", path, w, h);
}

// Box-filters w x h RGBA (or BGRA) pixels to ow x oh RGBA with opaque alpha.
static void box_filter(const uint8_t *px, uint32_t w, uint32_t h, int bgra, uint8_t *out, uint32_t ow, uint32_t oh) {
    for (uint32_t y = 0; y < oh; y++)
        for (uint32_t x = 0; x < ow; x++) {
            uint32_t x0 = x * w / ow, x1 = (x + 1) * w / ow, y0 = y * h / oh, y1 = (y + 1) * h / oh;
            if (x1 <= x0) x1 = x0 + 1;
            if (y1 <= y0) y1 = y0 + 1;
            uint32_t sum[4] = {0}, k = 0;
            for (uint32_t sy = y0; sy < y1; sy++)
                for (uint32_t sx = x0; sx < x1; sx++, k++)
                    for (int c = 0; c < 4; c++) sum[c] += px[4ull * (sy * (uint64_t)w + sx) + (uint32_t)c];
            uint8_t *o = out + 4ull * (y * ow + x);
            for (int c = 0; c < 4; c++) o[c] = (uint8_t)(sum[bgra && c != 3 ? 2 - c : c] / k);
            o[3] = 255;
        }
}

// MP4_DUMP_WHEN=blue:<count>[@<present>] (with MP4_DUMP=when and the ring on): a present, from the given one on, with
// at least count pure-blue pixels in its 800x450 copy (b > 200, r < 80, g < 120: the stray blue triangles give some
// 3500, the beam's flash and the menus under 100) arms the frame dump for the present after the next, so a glitch
// that moves between runs is dumped where it happens.
// MP4_SHOT_RING=<n> [MP4_SHOTS_DIR=dir]: the last n presents are kept in memory at 800x450 and written as
// <dir>/ring_f<present>.png when P is pressed in the window (gfx_window.c, rt_shot_ring_dump) or at present
// MP4_SHOT_RING_AT=<present>, for glitches that last a frame: press P right after one. Reading each present back
// waits for its frame, which costs frame rate while the ring is on. The PNGs are written on a thread of their own.
#define RING_W 800
#define RING_H 450
static struct { uint8_t *px; uint64_t frame; } *ring;
static int ring_n, ring_next, ring_full;
static PlatMutex ring_lock = PLAT_MUTEX_INIT;
static void ring_shot(const tex_t *t, uint64_t n) {
    static int parsed, on;
    static unsigned long long at;
    if (!parsed) {
        const char *e = cfg_get("MP4_SHOT_RING");
        ring_n = e && *e ? atoi(e) : 0;
        if (ring_n > 0) {
            ring = calloc((size_t)ring_n, sizeof *ring);
            if (!ring) rt_fatal("boot: out of memory for the shot ring");
            for (int i = 0; i < ring_n; i++)
                if (!(ring[i].px = malloc(4ull * RING_W * RING_H))) rt_fatal("boot: out of memory for the shot ring");
            on = 1;
            const char *a = cfg_get("MP4_SHOT_RING_AT");
            at = a && *a ? strtoull(a, NULL, 10) : 0;
            rt_log("boot: shot ring of %d presents at %dx%d; P writes it%s", ring_n, RING_W, RING_H, at ? " (and present MP4_SHOT_RING_AT)" : "");
        }
        parsed = 1;
    }
    if (!on) return;
    int fmt = t->d.format;
    int bgra = fmt == NVN_FORMAT_BGRA8 || fmt == NVN_FORMAT_BGRA8_SRGB;
    if (!bgra && fmt != NVN_FORMAT_RGBA8 && fmt != NVN_FORMAT_RGBA8_SRGB) return;
    uint32_t w = t->d.width, h = t->d.height;
    static uint8_t *px;
    static uint64_t px_bytes;
    if (px_bytes < 4ull * w * h) free(px), px = malloc(px_bytes = 4ull * w * h);
    if (!px) rt_fatal("boot: out of memory for a shot ring frame");
    view_t view;
    memset(&view, 0, sizeof view);
    NVNcopyRegion r = {.width = (int)w, .height = (int)h, .depth = 1};
    if (gfx_replay_async()) gfx_backend()->finish();
    gfx_backend()->read_texels(t, &view, &r, px, 4ull * w, 4ull * w * h);
    flip_rows(px, w, h);
    plat_mutex_lock(&ring_lock);
    box_filter(px, w, h, bgra, ring[ring_next].px, RING_W, RING_H);
    ring[ring_next].frame = n;
    static unsigned long long when_blue = ULLONG_MAX, when_from;
    if (when_blue == ULLONG_MAX) {
        const char *e = cfg_get("MP4_DUMP_WHEN");
        char *q = NULL;
        when_blue = e && !strncmp(e, "blue:", 5) ? strtoull(e + 5, &q, 10) : 0;
        when_from = q && *q == '@' ? strtoull(q + 1, NULL, 10) : 0;
    }
    if (when_blue && n >= when_from) {
        const uint8_t *o = ring[ring_next].px;
        unsigned long long blue = 0;
        for (uint64_t i = 0; i < 4ull * RING_W * RING_H; i += 4)
            blue += o[i + 2] > 200 && o[i] < 80 && o[i + 1] < 120;
        if (blue >= when_blue) {
            rt_log("boot: present %llu has %llu pure-blue pixels: arming the dump", (unsigned long long)n, blue);
            vk_dump_arm(n + 2);
        }
    }
    if (++ring_next == ring_n) ring_next = 0, ring_full = 1;
    plat_mutex_unlock(&ring_lock);
    if (at && n == at) rt_shot_ring_dump();
}
typedef struct { int n; struct { uint8_t *px; uint64_t frame; } *f; char dir[PATH_MAX]; } RingDump;
static void ring_write_main(void *arg) {
    RingDump *d = arg;
    for (int i = 0; i < d->n; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/ring_f%05llu.png", d->dir, (unsigned long long)d->f[i].frame);
        rt_write_png(path, d->f[i].px, RING_W, RING_H);
        free(d->f[i].px);
    }
    rt_log("boot: shot ring: %d presents written to %s", d->n, d->dir);
    free(d->f), free(d);
}
void rt_shot_ring_dump(void) {
    if (!ring) return;
    RingDump *d = calloc(1, sizeof *d);
    if (!d) return;
    const char *dir = cfg_get("MP4_SHOTS_DIR") ? cfg_get("MP4_SHOTS_DIR") : ".";
    snprintf(d->dir, sizeof d->dir, "%s", dir);
    plat_file_mkdir_p(d->dir);
    plat_mutex_lock(&ring_lock);
    int n = ring_full ? ring_n : ring_next, first = ring_full ? ring_next : 0;
    d->f = calloc((size_t)(n ? n : 1), sizeof *d->f);
    if (d->f)
        for (int i = 0; i < n; i++) {   // oldest first; the ring keeps filling while the copies are written
            int k = (first + i) % ring_n;
            d->f[i].frame = ring[k].frame;
            d->f[i].px = malloc(4ull * RING_W * RING_H);
            if (d->f[i].px) memcpy(d->f[i].px, ring[k].px, 4ull * RING_W * RING_H);
            else n = i;
        }
    d->n = d->f ? n : 0;
    plat_mutex_unlock(&ring_lock);
    rt_log("boot: shot ring: writing %d presents to %s", d->n, d->dir);
    PlatThread *t = plat_thread_new(ring_write_main, d, "shot ring");
    if (t) plat_thread_detach(t);
    else ring_write_main(d);
}

// MP4_SHOTS=lo-hi:step [MP4_SHOTS_DIR=dir]: every step-th present in [lo, hi] saved as <dir>/f<n>.png (n the
// present), box-filtered to 480x270; MP4_SHOTS=w<lo>-<hi>:<step> takes them by world-clock seconds instead, the first
// present at or after each step (full size with MP4_SHOTS_FULL=1), for finding short glitches. Pipeline builds stay asynchronous, so the
// pictures show what a player sees.
static void series_shot(const tex_t *t, uint64_t n) {
    static int parsed;
    static unsigned long long lo, hi, step, next;
    static const char *dir;
    static int full, world;
    if (!parsed) {
        full = cfg_get("MP4_SHOTS_FULL") != NULL;
        const char *e = cfg_get("MP4_SHOTS");
        double wl, wh, ws;
        if (e && e[0] == 'w' && sscanf(e + 1, "%lf-%lf:%lf", &wl, &wh, &ws) == 3 && ws > 0)   // world-clock seconds
            world = 1, lo = (unsigned long long)(wl * 1e6), hi = (unsigned long long)(wh * 1e6),
            step = (unsigned long long)(ws * 1e6), next = lo;
        else if (!e || sscanf(e, "%llu-%llu:%llu", &lo, &hi, &step) != 3 || !step) hi = 0;
        dir = cfg_get("MP4_SHOTS_DIR") ? cfg_get("MP4_SHOTS_DIR") : ".";
        if (hi) plat_file_mkdir_p(dir);
        parsed = 1;
    }
    if (world) {   // the first present at or after each step of the world clock
        int64_t w = frame_step_world_us();
        if (w < 0 || (unsigned long long)w < next || next > hi) return;
        while (next <= (unsigned long long)w) next += step;
    } else if (n < lo || n > hi || (n - lo) % step) {
        return;
    }
    int fmt = t->d.format;
    int bgra = fmt == NVN_FORMAT_BGRA8 || fmt == NVN_FORMAT_BGRA8_SRGB;
    if (!bgra && fmt != NVN_FORMAT_RGBA8 && fmt != NVN_FORMAT_RGBA8_SRGB) return;
    uint32_t w = t->d.width, h = t->d.height, ow = full ? w : 480, oh = full ? h : 270;
    uint8_t *px = malloc(4ull * w * h), *out = malloc(4ull * ow * oh);
    if (!px || !out) rt_fatal("boot: out of memory for a MP4_SHOTS frame");
    view_t view;
    memset(&view, 0, sizeof view);
    NVNcopyRegion r = {.width = (int)w, .height = (int)h, .depth = 1};
    if (gfx_replay_async()) gfx_backend()->finish();   // the presented frame may still be queued for the replay
    gfx_backend()->read_texels(t, &view, &r, px, 4ull * w, 4ull * w * h);
    flip_rows(px, w, h);
    box_filter(px, w, h, bgra, out, ow, oh);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/f%05llu.png", dir, (unsigned long long)n);
    rt_write_png(path, out, ow, oh);
    free(out);
    free(px);
}

// A --seconds run ends at the first present after the world clock (game time since the save's first frame,
// frame_step_world_us) passed S seconds, so a run covers the same game time at any frame rate; any other frame is
// counted by the back end only.
void rt_frame_presented(const tex_t *t, uint64_t n) {
    const RtConfig *cfg = rt_config();
    series_shot(t, n);
    ring_shot(t, n);
    dkcr_options_frame(n);
    int level_done = dkcr_tools_frame(n);   // Country-Box: CB_WARP's level ended (dkcr_tools.c)
    level_done |= dktf_tools_frame(n);      // Country-Box: CB_TF_WARP's level ended / CB_TF_MEASURE (ov_dktf.c)
    const char *spike_shot = rt_spike_shot();   // Country-Box: CB_SPIKE_SHOTS, the frame a spike produced
    if (spike_shot) save_shot(t, spike_shot);
    int64_t w = cfg->seconds ? frame_step_world_us() : -1;
    if (!level_done && (w < 0 || (double)w < cfg->seconds * 1e6)) return;
    if (!level_done) dkcr_tools_finish("time limit");
    if (cfg->shot) save_shot(t, cfg->shot);
    const char *end_shot = getenv("CB_END_SHOT");   // Country-Box: the last frame, without --shot's synchronous builds
    if (end_shot && *end_shot) save_shot(t, end_shot);
    GfxStats g = gfx_stats();
    rt_logf("boot: frames %llu presented (nvn calls %llu, distinct %llu, unimplemented %llu, validation errors %llu, "
           "shader failures %llu, draws skipped %llu)\n", (unsigned long long)g.frames,
           (unsigned long long)g.nvn_calls, (unsigned long long)g.nvn_distinct, (unsigned long long)g.unimplemented,
           (unsigned long long)g.validation_errors, (unsigned long long)g.shader_failures,
           (unsigned long long)g.draws_skipped);
    if (rt_trace & RT_TRACE_HOSTCALL) hostfn_report();   // MP4_TRACE=host: calls per host function
    stats_line();
    fflush(stdout);
    fflush(stderr);
    fs_save_flush();
    vk_pipeline_save();   // no-op without the Vulkan back end
    prof_write();
    fflush(stderr);
    _exit(0);
}

// ---- steps 9-14
static uint64_t cb_addr[3];

static void guest_main(Cpu *c) {
    uint32_t handle = kern_main_thread(&c);
    uint64_t start = ldr_symbol(SYM_INIT_START), start2 = start ? 0 : ldr_symbol(SYM_INIT_START2);
    if (!start && !start2) rt_fatal("boot: neither %s nor %s is exported", SYM_INIT_START, SYM_INIT_START2);
    char d[128];
    rt_log("boot: ENTER nn::init::Start at %s, main thread %#x, %d callbacks",
           ldr_describe(start ? start : start2, d, sizeof d), handle, start ? 3 : 2);
    if (start) rt_call(c, start, 5, (uint64_t)handle, (uint64_t)0, cb_addr[0], cb_addr[1], cb_addr[2]);
    else rt_call(c, start2, 4, (uint64_t)handle, (uint64_t)0, cb_addr[0], cb_addr[1]);
    rt_fatal("boot: nn::init::Start returned");
}

static void prewarm_main(void *arg) {
    (void)arg;
    vk_pipeline_prewarm();
    const char *prep = cfg_get("MP4_TEST_PREPARE_SHADERS");   // Task 2.3 test hook; Task 2.6 builds the preparation
    if (prep && !strcmp(prep, "1")) {
        int cpus = (int)plat_cpu_count();
        gfx_shader_prepare_recorded(cpus, cpus / 2 > 16 ? 16 : cpus / 2 < 1 ? 1 : cpus / 2);
    }
}

// Milliseconds since this process started: its start time (field 22 of /proc/self/stat, in clock ticks since boot)
// against CLOCK_BOOTTIME, the clock those ticks count. 0 when /proc cannot say.
static uint64_t ms_since_process_start(void) {
    char buf[1024];
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    const char *p = strrchr(buf, ')');   // the command name may hold spaces and parentheses; the fields follow it
    if (p) p++;
    unsigned long long start_ticks = 0;
    // after ')' come the fields from the third (state): the 22nd, starttime, is the 20th token after it
    for (int field = 3; p && field <= 22; field++) {
        while (*p == ' ') p++;
        if (field == 22) start_ticks = strtoull(p, NULL, 10);
        while (*p && *p != ' ') p++;
        if (!*p && field < 22) return 0;
    }
    struct timespec now;
    long hz = sysconf(_SC_CLK_TCK);
    if (!start_ticks || hz <= 0 || clock_gettime(CLOCK_BOOTTIME, &now) != 0) return 0;
    uint64_t now_ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    uint64_t start_ms = start_ticks * 1000 / (uint64_t)hz;
    return now_ms > start_ms ? now_ms - start_ms : 0;
}

void rt_boot(void) {
    check_npdm();
    movie_require_codec(!rt_config()->boot);   // ov_movie.c: a player's start (no --boot/--seconds) gets a message box
    if (!rt_config()->shot) vk_pipeline_async_enable();   // a --shot image must not depend on build timing
    shader_prep_boot();   // Country-Box: the blocking "Preparing shaders" screen when the caches are not ready (shader_prep.c)
    // Shaders and pipelines recorded in earlier runs are made ready while the modules load; the guest waits for them.
    PlatThread *warm = plat_thread_new(prewarm_main, NULL, "prewarm");
    ldr_load(rt_config()->exefs);
    rt_patches_init();
    frame_step_init();
    res_init();
    rt_breaks_resolve();   // Country-Box: MP4_BREAK_AT addresses into FN_ENTER's filter
    dkcr_tools_init();   // Country-Box: CB_WARP / CB_INVINCIBLE (dkcr_tools.c), after the modules load
    dktf_tools_init();   // Country-Box: CB_TF_TRACE (ov_dktf.c)
    cb_addr[0] = hostfn_register("<rtld loader_ready>", cb_loader_ready);
    cb_addr[1] = hostfn_register("<rtld module_inits>", cb_module_inits);
    cb_addr[2] = hostfn_register("<rtld module_finis>", cb_module_finis);
    hostfn_seal();
    uint64_t held = (plat_now_ns() / 1000000ull);
    if (warm) plat_thread_join(warm);
    else vk_pipeline_prewarm();
    held = (plat_now_ns() / 1000000ull) - held;
    rt_log("boot: guest starts %llu ms after process start (%llu ms of it waiting for the prewarm)",
           (unsigned long long)ms_since_process_start(), (unsigned long long)held);
    rt_guest_start_ns = kern_now_ns();
    kern_run_on_guest_main(guest_main);
    rt_fatal("boot: the guest main thread ended");
}
