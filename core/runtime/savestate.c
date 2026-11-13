// Country-Box: save states, the part that runs them (savestate.h). One host thread ("savestate") does every save and
// load, asked by F5 / F6 (gfx_window.c) or the menu; the guest is frozen only while it works.
//
// A state file (<profile>/savestates/state-<n>.cbss), little-endian, readable only by the process that wrote it:
//   SsHeader, the kernel's part (threads, stacks, tables: kern_ss_capture), the memory layout (mem_ss_layout), the
//   run list (SsRun[nruns]), then the bytes of every run in that order.
// Save: freeze, the GPU finishes (texels only the GPU wrote go back to guest memory first), capture, write, thaw.
// Load: read everything but the run bytes, freeze, check the same threads live and the memory layout did not change,
// the GPU finishes, pages holding data now but not in the state are zeroed, the run bytes go back (CPU writes, so the
// write tracking makes the Vulkan back end upload them again), the kernel's part goes back, thaw. The newest
// SS_SLOTS states are kept; F6 loads the newest, the menu also the ones before.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"
#include "savestate.h"

#define SS_MAGIC "CBSTATE1"
#define SS_FREEZE_NS 3000000000ull   // a thread that reaches no stop point in 3 s fails the save, nothing changes
#define SS_CHUNK (8u << 20)

typedef struct {
    char magic[8];
    uint64_t nonce;        // this process (ss_init); a file of another one is not loaded
    uint64_t seq;          // the save's number in this process
    uint64_t saved_ns;     // plat_now_ns at the save
    uint64_t kern_len, layout_len, nruns, data_bytes;
} SsHeader;

// ---- buffers
void ss_buf_put(SsBuf *b, const void *p, size_t n) {
    if (!n) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 1u << 16;
        while (cap < b->n + n) cap *= 2;
        uint8_t *q = realloc(b->p, cap);
        if (!q) rt_fatal("savestate: out of memory (%zu bytes)", cap);
        b->p = q, b->cap = cap;
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}

void ss_buf_free(SsBuf *b) {
    free(b->p);
    *b = (SsBuf){0};
}

const void *ss_in_take(SsIn *in, size_t n) {
    if (in->n - in->at < n) return NULL;
    const void *p = in->p + in->at;
    in->at += n;
    return p;
}

int ss_in_get(SsIn *in, void *out, size_t n) {
    const void *p = ss_in_take(in, n);
    if (p) memcpy(out, p, n);
    return p != NULL;
}

// ---- state
static PlatMutex g_lock = PLAT_MUTEX_INIT;
static PlatCond g_cv = PLAT_COND_INIT;
static int g_req;                 // 0 none, 1 save, 2 load
static int g_req_back;            // load: how many states back from the newest
static volatile int g_busy;
static uint64_t g_nonce, g_seq;   // g_seq: saves made (the newest is state-<g_seq>)
static uint64_t g_have[SS_SLOTS];   // the save number in each slot (seq % SS_SLOTS), 0 empty
static char g_dir[4096];
static char g_msg[256];
static uint64_t g_msg_at;
static int g_msg_ok;

static void note(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void note(int ok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    plat_mutex_lock(&g_lock);
    vsnprintf(g_msg, sizeof g_msg, fmt, ap);
    g_msg_at = plat_now_ns();
    g_msg_ok = ok;
    plat_mutex_unlock(&g_lock);
    va_end(ap);
    rt_log("savestate: %s", g_msg);
}

void ss_status(char *out, size_t n, uint64_t *at_ns, int *ok) {
    plat_mutex_lock(&g_lock);
    snprintf(out, n, "%s", g_msg);
    *at_ns = g_msg_at;
    *ok = g_msg_ok;
    plat_mutex_unlock(&g_lock);
}

int ss_busy(void) { return __atomic_load_n(&g_busy, __ATOMIC_ACQUIRE); }

int ss_count(void) {
    plat_mutex_lock(&g_lock);
    int n = 0;
    for (int i = 0; i < SS_SLOTS; i++) n += g_have[i] != 0;
    plat_mutex_unlock(&g_lock);
    return n;
}

static void path_of(char *out, size_t n, uint64_t seq) { snprintf(out, n, "%s/state-%llu.cbss", g_dir, (unsigned long long)seq); }

static int write_all(int fd, const void *p, size_t n) {
    const uint8_t *q = p;
    while (n) {
        ssize_t w = write(fd, q, n > SS_CHUNK ? SS_CHUNK : n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 0;
        q += w, n -= (size_t)w;
    }
    return 1;
}

// A run's bytes: an all-zero 64 KiB block is skipped with lseek and becomes a hole of the file (it reads back as
// zeros), so the never-written pages the game committed cost no disk and no write time. *zero counts them.
#define SS_ZBLOCK 65536u
static int is_zero(const uint8_t *p, size_t n) {
    const uint64_t *q = (const uint64_t *)p;
    for (size_t i = 0; i < n / 8; i += 8)
        if (q[i] | q[i + 1] | q[i + 2] | q[i + 3] | q[i + 4] | q[i + 5] | q[i + 6] | q[i + 7]) return 0;
    return 1;
}

static int write_run(int fd, const uint8_t *p, uint64_t n, uint64_t *zero) {
    uint64_t at = 0;
    while (at < n) {
        uint64_t k = n - at < SS_ZBLOCK ? n - at : SS_ZBLOCK;
        if (k == SS_ZBLOCK && !((uintptr_t)(p + at) & 7) && is_zero(p + at, k)) {
            uint64_t z = k;   // a run of zero blocks: one seek
            while (at + z + SS_ZBLOCK <= n && is_zero(p + at + z, SS_ZBLOCK)) z += SS_ZBLOCK;
            if (lseek(fd, (off_t)z, SEEK_CUR) < 0) return 0;
            *zero += z;
            at += z;
            continue;
        }
        uint64_t w = k;   // a run of data blocks: one write
        while (at + w + SS_ZBLOCK <= n && w < SS_CHUNK && !is_zero(p + at + w, SS_ZBLOCK)) w += SS_ZBLOCK;
        if (!write_all(fd, p + at, w)) return 0;
        at += w;
    }
    return 1;
}

static int read_all(int fd, void *p, size_t n) {
    uint8_t *q = p;
    while (n) {
        ssize_t r = read(fd, q, n > SS_CHUNK ? SS_CHUNK : n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        q += r, n -= (size_t)r;
    }
    return 1;
}

static double ms_since(uint64_t t0) { return (double)(plat_now_ns() - t0) / 1e6; }

// ---- save
static void do_save(void) {
    uint64_t t0 = plat_now_ns();
    char why[256];
    uint64_t seq = g_seq + 1;
    int slot = (int)(seq % SS_SLOTS);
    if (kern_ss_freeze(SS_FREEZE_NS, why, sizeof why) != 0) {
        note(0, "Could not save: %s", why);
        return;
    }
    uint64_t t_frozen = plat_now_ns();
    const GfxBackend *b = gfx_backend();
    if (b && b->finish) b->finish();
    SsRun *runs = NULL;
    size_t nruns = mem_ss_runs(&runs);
    uint64_t data = 0;
    for (size_t i = 0; i < nruns; i++) {
        if (b && b->guest_read) b->guest_read(runs[i].addr, runs[i].len);   // GPU-written texels into guest memory
        data += runs[i].len;
    }
    SsBuf kern = {0}, layout = {0};
    if (kern_ss_capture(&kern, slot, why, sizeof why) != 0) {
        kern_ss_thaw();
        free(runs);
        note(0, "Could not save: %s", why);
        return;
    }
    mem_ss_layout(&layout);
    uint64_t t_captured = plat_now_ns();
    char tmp[4200], final[4200];
    path_of(final, sizeof final, seq);
    snprintf(tmp, sizeof tmp, "%s.part", final);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    SsHeader h = {{0}, g_nonce, seq, plat_now_ns(), kern.n, layout.n, nruns, data};
    memcpy(h.magic, SS_MAGIC, 8);
    int ok = fd >= 0 && write_all(fd, &h, sizeof h) && write_all(fd, kern.p, kern.n) && write_all(fd, layout.p, layout.n) &&
             write_all(fd, runs, nruns * sizeof *runs);
    uint64_t zero = 0;
    for (size_t i = 0; ok && i < nruns; i++) ok = write_run(fd, (const uint8_t *)(uintptr_t)runs[i].addr, runs[i].len, &zero);
    if (ok) {   // a trailing hole needs the size set
        off_t end = lseek(fd, 0, SEEK_CUR);
        ok = end >= 0 && ftruncate(fd, end) == 0;
    }
    int err = errno;
    kern_ss_thaw();   // the guest goes on while the file is closed and renamed
    uint64_t t_written = plat_now_ns();
    if (fd >= 0 && close(fd) != 0) ok = 0;
    if (ok && rename(tmp, final) != 0) ok = 0, err = errno;
    ss_buf_free(&kern);
    ss_buf_free(&layout);
    free(runs);
    if (!ok) {
        unlink(tmp);
        kern_ss_drop(slot);
        note(0, "Could not save: writing %s failed (%s)", final, strerror(err));
        return;
    }
    plat_mutex_lock(&g_lock);
    uint64_t old = g_have[slot];
    g_have[slot] = seq;
    g_seq = seq;
    plat_mutex_unlock(&g_lock);
    if (old) {   // the oldest state of the ring makes room
        char p[4200];
        path_of(p, sizeof p, old);
        unlink(p);
    }
    note(1, "State saved (%d of %d kept)", ss_count(), SS_SLOTS);
    rt_log("savestate: save %llu: %.1f MiB in %zu runs (%.1f MiB of it zero, not written), frozen %.0f ms (freeze %.0f, "
           "capture %.0f, write %.0f), kernel part %.1f KiB", (unsigned long long)seq, data / 1048576.0, nruns,
           zero / 1048576.0, (t_written - t0) / 1e6,
           (t_frozen - t0) / 1e6, (t_captured - t_frozen) / 1e6, (t_written - t_captured) / 1e6, h.kern_len / 1024.0);
}

// ---- load
// The parts of cur not covered by saved (both sorted, without overlaps inside each): zeroed, as a hole reads.
static uint64_t zero_uncovered(const SsRun *cur, size_t nc, const SsRun *saved, size_t ns, const GfxBackend *b) {
    uint64_t zeroed = 0;
    size_t j = 0;
    for (size_t i = 0; i < nc; i++) {
        uint64_t a = cur[i].addr, end = cur[i].addr + cur[i].len;
        while (j < ns && saved[j].addr + saved[j].len <= a) j++;
        for (size_t k = j; a < end; k++) {
            uint64_t lo = k < ns ? saved[k].addr : end;
            if (lo > end) lo = end;
            if (lo > a) {
                memset((void *)(uintptr_t)a, 0, lo - a);
                if (b && b->guest_written) b->guest_written(a, lo - a);
                zeroed += lo - a;
            }
            if (k >= ns) break;
            a = saved[k].addr + saved[k].len;
        }
    }
    return zeroed;
}

static void do_load(int back) {
    uint64_t t0 = plat_now_ns();
    char why[256], path[4200];
    plat_mutex_lock(&g_lock);
    uint64_t seq = g_seq >= (uint64_t)back ? g_seq - (uint64_t)back : 0;
    int have = seq && g_have[seq % SS_SLOTS] == seq;
    plat_mutex_unlock(&g_lock);
    if (!have) {
        note(0, back ? "No older state to load" : "No state saved yet (F5 saves one)");
        return;
    }
    path_of(path, sizeof path, seq);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    SsHeader h;
    if (fd < 0 || !read_all(fd, &h, sizeof h) || memcmp(h.magic, SS_MAGIC, 8) || h.nonce != g_nonce) {
        if (fd >= 0) close(fd);
        note(0, "Could not load: %s is not a state of this session", path);
        return;
    }
    SsBuf kern = {0}, layout = {0};
    SsRun *runs = malloc(h.nruns * sizeof *runs + 1);
    kern.p = malloc(h.kern_len + 1), kern.n = h.kern_len;
    layout.p = malloc(h.layout_len + 1), layout.n = h.layout_len;
    int ok = runs && kern.p && layout.p && read_all(fd, kern.p, kern.n) && read_all(fd, layout.p, layout.n) &&
             read_all(fd, runs, h.nruns * sizeof *runs);
    if (!ok) {
        close(fd);
        free(runs), free(kern.p), free(layout.p);
        note(0, "Could not load: %s is damaged", path);
        return;
    }
    if (kern_ss_freeze(SS_FREEZE_NS, why, sizeof why) != 0) {
        close(fd);
        free(runs), free(kern.p), free(layout.p);
        note(0, "Could not load: %s", why);
        return;
    }
    uint64_t t_frozen = plat_now_ns();
    SsIn kin = {kern.p, kern.n, 0}, lin = {layout.p, layout.n, 0};
    if (!kern_ss_check(kin, why, sizeof why) || !mem_ss_layout_check(lin, runs, h.nruns, why, sizeof why)) {
        kern_ss_thaw();
        close(fd);
        free(runs), free(kern.p), free(layout.p);
        note(0, "Could not load: %s", why);
        return;
    }
    const GfxBackend *b = gfx_backend();
    if (b && b->finish) b->finish();
    mem_ss_layout_apply(lin);   // the alias region's pages mapped as they were (the heap grew or shrank since)
    SsRun *cur = NULL;
    size_t ncur = mem_ss_runs(&cur);
    uint64_t zeroed = zero_uncovered(cur, ncur, runs, h.nruns, b);
    free(cur);
    uint8_t *bounce = malloc(SS_CHUNK);
    if (!bounce) rt_fatal("savestate: out of memory");
    for (uint64_t i = 0; i < h.nruns; i++) {
        uint8_t *dst = (uint8_t *)(uintptr_t)runs[i].addr;
        for (uint64_t off = 0; off < runs[i].len;) {
            size_t n = runs[i].len - off > SS_CHUNK ? SS_CHUNK : (size_t)(runs[i].len - off);
            // Read through a buffer and copy: a CPU write, which the write tracking sees (a read() into the pages
            // would write them from the kernel)
            if (!read_all(fd, bounce, n))
                rt_fatal("savestate: %s ended while its memory went back; the game cannot go on from half a state", path);
            memcpy(dst + off, bounce, n);
            off += n;
        }
        if (b && b->guest_written) b->guest_written(runs[i].addr, runs[i].len);
    }
    free(bounce);
    close(fd);
    uint64_t t_memory = plat_now_ns();
    kern_ss_restore(kin);
    kern_ss_thaw();
    free(runs), free(kern.p), free(layout.p);
    note(1, back ? "State loaded (%d back)" : "State loaded", back);
    rt_log("savestate: load %llu: %.1f MiB back, %.1f MiB zeroed, frozen %.0f ms (freeze %.0f, memory %.0f)",
           (unsigned long long)seq, h.data_bytes / 1048576.0, zeroed / 1048576.0, ms_since(t0), (t_frozen - t0) / 1e6,
           (t_memory - t_frozen) / 1e6);
}

// ---- the thread
static void *ss_main(void *arg) {
    (void)arg;
    for (;;) {
        plat_mutex_lock(&g_lock);
        while (!g_req) plat_cond_wait(&g_cv, &g_lock);
        int req = g_req, back = g_req_back;
        g_req = 0;
        __atomic_store_n(&g_busy, 1, __ATOMIC_RELEASE);
        plat_mutex_unlock(&g_lock);
        if (req == 1) do_save();
        else do_load(back);
        __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
    }
    return NULL;
}

// Deletes the files an earlier process left: their threads and objects are gone, so they cannot load.
static void clear_dir(void) {
    DIR *d = opendir(g_dir);
    if (!d) return;
    struct dirent *e;
    char p[4400];
    while ((e = readdir(d)))
        if (strstr(e->d_name, ".cbss")) {
            snprintf(p, sizeof p, "%s/%s", g_dir, e->d_name);
            unlink(p);
        }
    closedir(d);
}

void ss_init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char *prof = rt_config()->profile;
    snprintf(g_dir, sizeof g_dir, "%s/savestates", prof && *prof ? prof : ".");
    mkdir(g_dir, 0700);
    clear_dir();
    if (getrandom(&g_nonce, sizeof g_nonce, 0) != sizeof g_nonce) g_nonce = plat_now_ns() ^ (uint64_t)getpid() << 40;
    pthread_t th;
    if (pthread_create(&th, NULL, ss_main, NULL) == 0) pthread_detach(th);
    else rt_log("savestate: the save state thread could not start; F5 / F6 do nothing");
}

void ss_shutdown(void) {
    if (g_dir[0]) clear_dir();
}

static void request(int what, int back) {
    plat_mutex_lock(&g_lock);
    if (!g_busy && !g_req) {
        g_req = what, g_req_back = back;
        plat_cond_signal(&g_cv);
    }
    plat_mutex_unlock(&g_lock);
}

void ss_request_save(void) { request(1, 0); }
void ss_request_load(int back) { request(2, back < 0 ? 0 : back); }
