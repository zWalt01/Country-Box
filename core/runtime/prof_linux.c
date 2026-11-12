// From fe3h-pc-recomp runtime/prof_win.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4 only.
// From totk-pc-recomp runtime/prof_win.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied with totk renamed to fe3h only. Country-Box: renamed from prof_win.c; samples by a queued signal per thread instead of SuspendThread, without call chains; /proc and sysfs instead of the Win32 process APIs (Linux).
// Sampling profiler (MP4_PROFILE=<path>) and per-frame timing (MP4_TRACE=perf). tools/prof_report.py reads the profile.
// A host thread wakes every 1 ms and queues SIGPROF to every other thread of the process (rt_tgsigqueueinfo, the
// thread's slot in the signal's value); the handler, on the sampled thread, stores the interrupted Rip into the slot and
// returns, and the sampler reads the slots at its next tick. The handler takes no lock and touches nothing but its slot,
// and the sampler never runs guest code. SIGPROF, not a real-time signal: kernel.c's preemption owns SIGRTMIN+3. A
// thread blocked in a system call is sampled at its wait, as SuspendThread did; SA_RESTART restarts most calls, but
// poll / epoll_wait return EINTR, which the libraries that use them (SDL, PipeWire) retry. Sampling runs only with
// MP4_PROFILE set. The profile is text:
//   mp4-prof 1 / base <hex> / period_us / samples N / thread <tid> <samples> <in-exe samples> / module <base> <size> <path> /
//   rip <hex> <count> (the top PROF_TOP Rips)
// The Win32 sampler also unwound a copy of the stack with the .pdata tables (MP4_PROFILE_RENDER, MP4_PROFILE_CHAINS,
// MP4_PROFILE_SLOW). Linux has no unwind tables to walk without a DWARF unwinder, and the runtime is built without frame
// pointers, so call chains are off here: MP4_PROFILE_RENDER and MP4_PROFILE_MAIN still pick the thread sampled, and
// MP4_PROFILE_SLOW=<ms> still writes, after a present at least <ms> after the previous one, that interval's samples
// (plus the 20 ms before) to <MP4_PROFILE>.f<frame> as one-frame chains: `stack 1 <rip>` and
// `tstack <tid> <ms into the window> <rip>` lines. At most 64 dumps. Country-Box: MP4_PROFILE_THREAD=<prefix> samples only
// the threads whose name starts with it (guest threads are named g<handle> p<prio>, kernel.c), e.g. "g100a".
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "cfg.h"
#include "flight.h"
#include "gfx.h"
#include "host.h"
#include "perfmon.h"
#include "platform/platform.h"

#define PROF_SLOTS (1u << 16)   // Rip hash table, open addressing; a full table drops new Rips
#define PROF_TOP 4000           // more than the 200 asked for: the report sums per function
#define PROF_THREADS 512
#define PROF_SIG SIGPROF

typedef struct { uint64_t rip, n; } RipCount;
typedef struct { uint32_t tid; uint64_t n, in_exe; uint8_t want; } ThreadSlot;   // want: MP4_PROFILE_THREAD matched

static RipCount table[PROF_SLOTS];
static ThreadSlot threads[PROF_THREADS];
static uint64_t reply[PROF_THREADS];   // the handler's Rip for the slot's last signal, 0 none yet
static uint64_t sent_at[PROF_THREADS]; // kern_now_ns of that signal (MP4_PROFILE_SLOW), 0 none outstanding
static int n_threads;
static uint64_t total, dropped;
static const char *out_path;
static PlatThread *sampler;
static int32_t stop, written, sampler_done;
static uintptr_t exe_lo, exe_hi;
static uint32_t render_tid;         // the thread that presents (perf_present)
static int only_render;             // MP4_PROFILE_RENDER=1: sample the render thread only
static uint32_t main_tid;           // the guest main thread (kern_main_thread)
static int only_main;               // MP4_PROFILE_MAIN=1: sample the guest main thread only
static const char *only_name;       // Country-Box: MP4_PROFILE_THREAD=<prefix>: sample threads whose name starts so
static volatile uint64_t cur_frame; // last presented frame
static uint64_t frame_lo, frame_hi; // MP4_PROFILE_FRAMES=<lo>-<hi>: sample only while presents lo..hi-1 are pending
static int64_t world_lo, world_hi;  // MP4_PROFILE_FRAMES=w<lo>-<hi>: only while the world clock is in [lo, hi) s (us)
static int32_t key_gate = -1;       // MP4_PROFILE_FRAMES=key: 0 off, 1 on, toggled by F8 (prof_toggle); -1 not keyed

#define SLOW_RING (1u << 15)        // MP4_PROFILE_SLOW: about 0.5 s of samples of every thread
#define SLOW_DUMPS 64
typedef struct { uint64_t t; uint32_t tid; uint64_t rip; } Sample;
static Sample ring[SLOW_RING];
static uint64_t ring_n;
static uint64_t slow_ns;            // MP4_PROFILE_SLOW in ns, 0 off
static int64_t dump_lo, dump_hi, dump_frame;   // the render thread asks for a window, the sampler writes it
static uint32_t replay_tid;         // asynchronous replay (vk_backend.c): its waits hold up the frame too

// The calling thread's id without a system call per use (perf_end runs on every wait of the render thread).
static __thread uint32_t tls_tid;
static uint32_t my_tid(void) {
    if (!tls_tid) tls_tid = plat_thread_id();
    return tls_tid;
}

static void count_rip(uint64_t rip) {
    uint32_t i = (uint32_t)((rip * 0x9E3779B97F4A7C15ull) >> 48);
    for (uint32_t k = 0; k < 64; k++, i = (i + 1) & (PROF_SLOTS - 1)) {
        if (table[i].rip == rip || !table[i].rip) {
            table[i].rip = rip;
            table[i].n++;
            return;
        }
    }
    dropped++;
}

// On the sampled thread: async-signal-safe, one store.
static void prof_handler(int sig, siginfo_t *si, void *ucv) {
    (void)sig;
    int slot = si->si_value.sival_int;
    if (si->si_code != SI_QUEUE || slot < 0 || slot >= PROF_THREADS) return;
    const ucontext_t *uc = ucv;
    uint64_t rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    __atomic_store_n(&reply[slot], rip ? rip : 1, __ATOMIC_RELEASE);
}

static int send_sample(int slot) {
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = PROF_SIG;
    si.si_code = SI_QUEUE;
    si.si_pid = getpid();
    si.si_uid = getuid();
    si.si_value.sival_int = slot;
    return syscall(SYS_rt_tgsigqueueinfo, getpid(), (pid_t)threads[slot].tid, PROF_SIG, &si) == 0;
}

// The replies to the previous tick's signals.
static void collect(void) {
    for (int i = 0; i < n_threads; i++) {
        uint64_t rip = __atomic_exchange_n(&reply[i], 0, __ATOMIC_ACQUIRE);
        uint64_t t = sent_at[i];
        sent_at[i] = 0;
        if (!rip) continue;   // not delivered yet (a thread in an uninterruptible wait), or none sent
        ThreadSlot *s = &threads[i];
        count_rip(rip);
        total++;
        s->n++;
        if (rip >= exe_lo && rip < exe_hi) s->in_exe++;
        if (slow_ns) ring[ring_n++ & (SLOW_RING - 1)] = (Sample){t, s->tid, rip};
    }
}

// Re-lists the process's threads (/proc/self/task): keeps the counts of a thread seen before, adds new ones, drops the
// gone ones. Between ticks only (after collect), so no reply is outstanding.
static void refresh_threads(uint32_t self) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    static ThreadSlot next[PROF_THREADS];
    int n = 0;
    for (struct dirent *e; (e = readdir(d)) && n < PROF_THREADS;) {
        uint32_t tid = (uint32_t)strtoul(e->d_name, NULL, 10);
        if (!tid || tid == self) continue;
        ThreadSlot s = {.tid = tid};
        int known = 0;
        for (int i = 0; i < n_threads; i++)
            if (threads[i].tid == tid) s = threads[i], known = 1;   // exited threads' counts are lost with their slot
        if (only_name && !known) {   // the name is read once per thread (guest threads are named at creation)
            char path[64], comm[32] = "";
            snprintf(path, sizeof path, "/proc/self/task/%u/comm", tid);
            FILE *f = fopen(path, "r");
            if (f) {
                if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
                fclose(f);
            }
            s.want = !strncmp(comm, only_name, strlen(only_name));
        }
        next[n++] = s;
    }
    closedir(d);
    memcpy(threads, next, sizeof(ThreadSlot) * n);
    n_threads = n;
}

// Every file mapped executable, as one range from its lowest to its highest mapping (/proc/self/maps).
typedef void (*ModuleFn)(uintptr_t lo, uintptr_t hi, const char *path, void *ctx);
static void each_module(ModuleFn fn, void *ctx) {
    FILE *m = fopen("/proc/self/maps", "r");
    if (!m) return;
    char line[4352], cur[4096] = "";
    uintptr_t lo = 0, hi = 0;
    int exec = 0;
    while (fgets(line, sizeof line, m)) {
        unsigned long a, b;
        char perm[8], path[4096] = "";
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %4095[^\n]", &a, &b, perm, path) < 3 || path[0] != '/') continue;
        if (strcmp(path, cur)) {
            if (cur[0] && exec) fn(lo, hi, cur, ctx);
            snprintf(cur, sizeof cur, "%s", path);
            lo = a, hi = b, exec = 0;
        }
        if (a < lo) lo = a;
        if (b > hi) hi = b;
        exec |= perm[2] == 'x';
    }
    if (cur[0] && exec) fn(lo, hi, cur, ctx);
    fclose(m);
}

static void module_line(uintptr_t lo, uintptr_t hi, const char *path, void *ctx) {
    fprintf(ctx, "module %llx %lx %s\n", (unsigned long long)lo, (unsigned long)(hi - lo), path);
}

static void exe_range(uintptr_t lo, uintptr_t hi, const char *path, void *ctx) {
    if (strcmp(path, ctx)) return;
    exe_lo = lo;
    exe_hi = hi;
}

static void write_modules(FILE *f) { each_module(module_line, f); }

// The sampler, between ticks: the samples of the window the render thread asked for.
static void slow_dump(void) {
    uint64_t lo = (uint64_t)dump_lo, hi = (uint64_t)__atomic_load_n(&dump_hi, __ATOMIC_ACQUIRE), frame = (uint64_t)dump_frame;
    static int dumps;
    char path[1100];
    snprintf(path, sizeof path, "%s.f%llu", out_path, (unsigned long long)frame);
    FILE *f = dumps < SLOW_DUMPS ? fopen(path, "w") : NULL;
    if (f) {
        dumps++;
        fprintf(f, "mp4-prof 1\nbase %llx\nperiod_us 1000\nframe %llu interval_ms %.1f\nmain %lu render %lu replay %lu\n",
                (unsigned long long)exe_lo, (unsigned long long)frame, (hi - lo) / 1e6 - 20, (unsigned long)main_tid,
                (unsigned long)render_tid, (unsigned long)replay_tid);
        write_modules(f);
        uint64_t first = ring_n > SLOW_RING ? ring_n - SLOW_RING : 0;
        for (int pass = 0; pass < 2; pass++)
            for (uint64_t k = first; k < ring_n; k++) {
                const Sample *s = &ring[k & (SLOW_RING - 1)];
                if (s->t < lo || s->t > hi) continue;
                if (pass) fprintf(f, "tstack %lu %.1f %llx\n", (unsigned long)s->tid, (s->t - lo) / 1e6, (unsigned long long)s->rip);
                else fprintf(f, "stack 1 %llx\n", (unsigned long long)s->rip);
            }
        fclose(f);
        rt_log("prof: slow frame %llu (%.1f ms) -> %s", (unsigned long long)frame, (hi - lo) / 1e6 - 20, path);
    }
    __atomic_store_n(&dump_hi, 0, __ATOMIC_RELEASE);
}

static void sampler_main(void *arg) {
    (void)arg;
    uint32_t self = my_tid();
    prctl(PR_SET_TIMERSLACK, 1ul, 0ul, 0ul, 0ul);
    for (unsigned tick = 0; !__atomic_load_n(&stop, __ATOMIC_ACQUIRE); tick++) {
        struct timespec ts = {0, 1000000};   // 1 ms
        nanosleep(&ts, NULL);
        collect();
        if (tick % 64 == 0) refresh_threads(self);
        if (__atomic_load_n(&dump_hi, __ATOMIC_ACQUIRE)) slow_dump();
        if (frame_hi && (cur_frame < frame_lo || cur_frame >= frame_hi)) continue;
        if (!__atomic_load_n(&key_gate, __ATOMIC_RELAXED)) continue;
        if (world_hi) {   // -1 before the world clock starts
            int64_t w = frame_step_world_us();
            if (w < world_lo || w >= world_hi) continue;
        }
        uint64_t now = slow_ns ? kern_now_ns() : 1;
        uint32_t rt = __atomic_load_n(&render_tid, __ATOMIC_RELAXED), mt = __atomic_load_n(&main_tid, __ATOMIC_RELAXED);
        for (int i = 0; i < n_threads; i++) {
            ThreadSlot *t = &threads[i];
            if ((only_render && t->tid != rt) || (only_main && t->tid != mt) || (only_name && !t->want)) continue;
            if (send_sample(i)) sent_at[i] = now;
        }
    }
    collect();
    __atomic_store_n(&sampler_done, 1, __ATOMIC_RELEASE);
}

static int cmp_desc(const void *a, const void *b) {
    uint64_t x = ((const RipCount *)a)->n, y = ((const RipCount *)b)->n;
    return (x < y) - (x > y);
}

// F8 (gfx_window.c): with MP4_PROFILE_FRAMES=key, starts and stops sampling, for a stretch of play whose frames are
// not known ahead (an area reached by hand). Several stretches add up in the one profile.
void prof_toggle(void) {
    if (__atomic_load_n(&key_gate, __ATOMIC_RELAXED) < 0) return;
    int32_t on = __atomic_fetch_xor(&key_gate, 1, __ATOMIC_SEQ_CST) ^ 1;
    rt_log("perf: profile sampling %s at frame %llu", on ? "on" : "off", (unsigned long long)cur_frame);
}

void prof_write(void) {
    perf_summary_flush();
    if (!out_path || plat_atomic_xchg(&written, 1)) return;
    plat_atomic_xchg(&stop, 1);
    for (int i = 0; i < 2000 && !__atomic_load_n(&sampler_done, __ATOMIC_ACQUIRE); i++) plat_sleep_ns(1000000);
    if (!__atomic_load_n(&sampler_done, __ATOMIC_ACQUIRE)) return;   // sampler stuck: no half-written counts
    plat_thread_detach(sampler);
    FILE *f = fopen(out_path, "w");
    if (!f) {
        rt_log("prof: cannot write %s", out_path);
        return;
    }
    fprintf(f, "mp4-prof 1\nbase %llx\nperiod_us 1000\nsamples %llu\ndropped %llu\n", (unsigned long long)exe_lo,
            (unsigned long long)total, (unsigned long long)dropped);
    for (int i = 0; i < n_threads; i++)
        fprintf(f, "thread %lu %llu %llu\n", (unsigned long)threads[i].tid, (unsigned long long)threads[i].n,
                (unsigned long long)threads[i].in_exe);
    write_modules(f);
    int cnt = 0;
    for (uint32_t i = 0; i < PROF_SLOTS; i++)
        if (table[i].rip) table[cnt++] = table[i];
    qsort(table, cnt, sizeof *table, cmp_desc);
    for (int i = 0; i < cnt && i < PROF_TOP; i++) fprintf(f, "rip %llx %llu\n", (unsigned long long)table[i].rip, (unsigned long long)table[i].n);
    fclose(f);
    rt_log("prof: %llu samples, %d Rips -> %s", (unsigned long long)total, cnt, out_path);
}

// MP4_TRACE=perf, MP4_FLIGHT=1 or the full performance overlay: event counts are taken. Wait times are always taken
// (frame-time summary). perf_base: the part fixed at start.
static int perf_on, perf_base;
static int overlay;   // performance overlay mode (perf_set_overlay): 0 off, 1 fps, 2 full

void perf_set_overlay(int mode) {
    __atomic_store_n(&overlay, mode, __ATOMIC_RELAXED);
    __atomic_store_n(&perf_on, perf_base || mode == 2, __ATOMIC_RELAXED);
}

int perf_gpu_timing(void) { return (rt_trace & RT_TRACE_PERF) || __atomic_load_n(&overlay, __ATOMIC_RELAXED) == 2; }

static int64_t gpu_busy_total;

void perf_gpu_busy(uint64_t ns) { __atomic_fetch_add(&gpu_busy_total, (int64_t)ns, __ATOMIC_RELAXED); }

uint64_t perf_gpu_busy_total(void) { return (uint64_t)__atomic_load_n(&gpu_busy_total, __ATOMIC_RELAXED); }

void prof_start(void) {
    flight_init();
    atexit(perf_summary_flush);   // boot.c's _exit path goes through prof_write instead
    perf_base = (rt_trace & RT_TRACE_PERF) || flight_on;
    perf_on = perf_base || overlay == 2;
    out_path = cfg_get("MP4_PROFILE");
    if (!out_path || !*out_path) {
        out_path = NULL;
        return;
    }
    only_render = cfg_get("MP4_PROFILE_RENDER") != NULL;
    only_main = !only_render && cfg_get("MP4_PROFILE_MAIN") != NULL;
    only_name = cfg_get("MP4_PROFILE_THREAD");
    if (only_name && !*only_name) only_name = NULL;
    const char *ch = cfg_get("MP4_PROFILE_CHAINS");
    const char *sl = cfg_get("MP4_PROFILE_SLOW");
    slow_ns = sl && atoi(sl) > 0 ? (uint64_t)atoi(sl) * 1000000ull : 0;
    if (only_render || only_main || slow_ns || (ch && *ch && strcmp(ch, "0")))
        rt_log("prof: call chains are not sampled on Linux (no unwinder): Rips only");
    const char *fr = cfg_get("MP4_PROFILE_FRAMES");
    double wl, wh;
    if (fr && !strcmp(fr, "key")) key_gate = 0;
    else if (fr && fr[0] == 'w' && sscanf(fr + 1, "%lf-%lf", &wl, &wh) == 2 && wl >= 0 && wl < wh)
        world_lo = (int64_t)(wl * 1e6), world_hi = (int64_t)(wh * 1e6);
    else if (fr && sscanf(fr, "%llu-%llu", (unsigned long long *)&frame_lo, (unsigned long long *)&frame_hi) != 2) frame_hi = 0;
    char exe[4096];
    if (plat_exe_path(exe, sizeof exe)) each_module(exe_range, exe);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prof_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(PROF_SIG, &sa, NULL) != 0) {
        rt_log("prof: cannot install the SIGPROF handler (%s)", strerror(errno));
        out_path = NULL;
        return;
    }
    sampler = plat_thread_new(sampler_main, NULL, "prof");
    if (!sampler) {
        rt_log("prof: cannot start the sampler");
        out_path = NULL;
        return;
    }
    atexit(prof_write);   // a --seconds run ends in _exit: boot.c calls prof_write itself first
}

// ---- MP4_TRACE=perf: frame interval and the time the render thread spends waiting, logged every 60 presents. The
// flight recorder (MP4_FLIGHT=1, flight.c) takes the same numbers per frame.
static uint64_t wait_ns[PERF_N], frame_ns[PERF_N], worst_ns[PERF_N];   // window totals, this frame, the slowest frame

uint64_t perf_begin(void) { return kern_now_ns(); }

void perf_replay_thread(void) { __atomic_store_n(&replay_tid, my_tid(), __ATOMIC_RELAXED); }
void perf_main_thread(void) { __atomic_store_n(&main_tid, my_tid(), __ATOMIC_RELAXED); }

void perf_end(int kind, uint64_t t0) {
    if (!t0) return;
    uint32_t tid = my_tid();
    if (tid != render_tid && tid != replay_tid) return;
    uint64_t d = kern_now_ns() - t0;
    if (tid == render_tid && !replay_tid) {   // one thread adds: no atomics on the synchronous path
        wait_ns[kind] += d;
        frame_ns[kind] += d;
        return;
    }
    __atomic_fetch_add(&wait_ns[kind], d, __ATOMIC_RELAXED);
    __atomic_fetch_add(&frame_ns[kind], d, __ATOMIC_RELAXED);
}

static int64_t other_wait_ns;         // perf_gpu_wait on threads other than the render thread, over the window
static int64_t render_wait_ns;        // the same on the render thread
void (*perf_gpu_stats)(PerfGpu *out);

void perf_gpu_wait(uint64_t t0) {
    if (!t0 || !__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) return;
    int64_t d = (int64_t)(kern_now_ns() - t0);
    __atomic_fetch_add(my_tid() == render_tid ? &render_wait_ns : &other_wait_ns, d, __ATOMIC_RELAXED);
}

static int64_t replay_ns;         // perf_replay: summed over the window, any thread
static int64_t frame_replay_ns;   // the same since the previous present (frame-time summary)

void perf_replay(uint64_t t0) {
    if (!t0) return;
    int64_t d = (int64_t)(kern_now_ns() - t0);
    __atomic_fetch_add(&frame_replay_ns, d, __ATOMIC_RELAXED);
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) __atomic_fetch_add(&replay_ns, d, __ATOMIC_RELAXED);
}

static int64_t counts[PERF_CNT_N];   // running totals: the perf log and the flight recorder take differences

void perf_count(int kind) {
    if (__atomic_load_n(&perf_on, __ATOMIC_RELAXED)) __atomic_add_fetch(&counts[kind], 1, __ATOMIC_RELAXED);
}

// ---- Frame-time summary, always on: every 60 s of play and at exit one line of frame count, mean fps, 1% low and the
// frames over 25/50/100 ms, then the worst frames of the period with the render thread's waits in them. Per frame: a
// histogram increment, the compare against the worst list, a memset of the wait array. No allocation.
#define FT_BINS 1024              // 0.25 ms bins up to 256 ms; slower frames land in the last bin
#define FT_WORST 5
#define FT_PERIOD_NS 60000000000ull
typedef struct { uint64_t frame, ns, wait[PERF_N], replay; } FtWorst;
static struct {
    uint64_t t0, frames, sum, over25, over50, over100;
    uint32_t hist[FT_BINS];
    FtWorst worst[FT_WORST];   // slowest first
    int nworst;
} ft;
static uint64_t summary_t0;   // start of the current period (0: not started)
uint64_t gfx_shader_cache_hits(void);   // gfx_shader.c: shader gets answered from the caches (each hashes the code)
static uint64_t ft_hits0;

static void ft_report(uint64_t now, const char *tag) {
    if (ft.frames && ft.sum) {
        uint64_t cut = (ft.frames + 99) / 100, acc = 0, low = 0;   // the slowest 1% of frames
        for (int b = FT_BINS - 1; b >= 0; b--) {
            acc += ft.hist[b];
            if (acc >= cut) { low = (uint64_t)(b + 1) * 250000ull; break; }   // upper edge of the bin
        }
        uint64_t hits = gfx_shader_cache_hits();
        // Country-Box: shader gets that missed the draw path's memo (vk_exec_draw.c shader_for) and hashed the code.
        rt_log("frametime: %s: %llu frames in %.1f s, mean %.1f fps, 1%% low %.1f fps, over 25 ms %llu, 50 ms %llu, 100 ms %llu; "
               "shader cache gets %.1f per frame",
               tag, (unsigned long long)ft.frames, (now - ft.t0) / 1e9, ft.frames * 1e9 / ft.sum, low ? 1e9 / low : 0.0,
               (unsigned long long)ft.over25, (unsigned long long)ft.over50, (unsigned long long)ft.over100,
               (double)(hits - ft_hits0) / (double)ft.frames);
        ft_hits0 = hits;
        for (int i = 0; i < ft.nworst; i++) {
            const FtWorst *w = &ft.worst[i];
            rt_log("frametime:   worst frame %llu: %.1f ms; shader translation %.1f, pipeline build %.1f, texture upload %.1f (astc %.1f), replay %.1f, fence/present wait %.1f, pacing %.1f",
                   (unsigned long long)w->frame, w->ns / 1e6, w->wait[PERF_XLATE] / 1e6, w->wait[PERF_PIPE] / 1e6,
                   w->wait[PERF_TEX] / 1e6, w->wait[PERF_ASTC] / 1e6, w->replay / 1e6,
                   (w->wait[PERF_FENCE] + w->wait[PERF_DRAIN]) / 1e6, w->wait[PERF_PACE] / 1e6);
        }
    }
    memset(&ft, 0, sizeof ft);
    ft.t0 = now;
}

__attribute__((weak)) void gfx_exit_log(void) {}

void perf_summary_flush(void) {   // at exit (prof_write)
    if (summary_t0) {
        ft_report(kern_now_ns(), "exit");
        gfx_exit_log();
    }
    summary_t0 = 0;
}

// Returns the replay time since the previous present.
static uint64_t ft_frame(uint64_t frame, uint64_t now, uint64_t d) {
    int64_t rep = __atomic_exchange_n(&frame_replay_ns, 0, __ATOMIC_RELAXED);
    if (!summary_t0) {   // the first present starts the period; there is no interval yet
        summary_t0 = now;
        ft.t0 = now;
        return (uint64_t)rep;
    }
    ft.frames++;
    ft.sum += d;
    uint64_t b = d / 250000ull;
    ft.hist[b < FT_BINS ? b : FT_BINS - 1]++;
    if (d > 25000000ull) ft.over25++;
    if (d > 50000000ull) ft.over50++;
    if (d > 100000000ull) ft.over100++;
    if (d > 25000000ull && (ft.nworst < FT_WORST || d > ft.worst[FT_WORST - 1].ns)) {
        int i = ft.nworst < FT_WORST ? ft.nworst++ : FT_WORST - 1;
        while (i > 0 && ft.worst[i - 1].ns < d) ft.worst[i] = ft.worst[i - 1], i--;
        ft.worst[i].frame = frame;
        ft.worst[i].ns = d;
        memcpy(ft.worst[i].wait, frame_ns, sizeof frame_ns);
        ft.worst[i].replay = (uint64_t)rep;
    }
    if (now - summary_t0 >= FT_PERIOD_NS) {
        ft_report(now, "60 s");
        summary_t0 = now;
    }
    return (uint64_t)rep;
}

// The second line of the 60-frame window: GPU busy time (timestamps), the time threads waited for GPU fences, VRAM and
// process memory, and which side holds the frame back. pace: the frame waited for pacing (vsync, the frame limiter) at
// least 15% of the window; gpu: the GPU was busy for 80% of the frame interval or more (without timestamps: threads
// waited for fences for half the interval or more); cpu otherwise.
static void perf_gpu_line(uint64_t frame, double interval_ms, double pace_share, uint64_t n) {
    PerfGpu g;
    memset(&g, 0, sizeof g);
    if (perf_gpu_stats) perf_gpu_stats(&g);
    double rw = __atomic_exchange_n(&render_wait_ns, 0, __ATOMIC_RELAXED) / 1e6 / n,
           ow = __atomic_exchange_n(&other_wait_ns, 0, __ATOMIC_RELAXED) / 1e6 / n;
    double gpu_ms = g.have_gpu ? g.gpu_ns / 1e6 / n : 0;
    char gs[96], vs[96];
    if (g.have_gpu) snprintf(gs, sizeof gs, "gpu avg %.2f ms (%llu parts timed, %llu not)", gpu_ms,
                             (unsigned long long)g.gpu_parts, (unsigned long long)g.gpu_missed);
    else snprintf(gs, sizeof gs, "gpu avg n/a");
    if (g.have_vram) snprintf(vs, sizeof vs, "vram %.0f of %.0f MiB%s", g.vram_used / 1048576.0, g.vram_budget / 1048576.0,
                              g.vram_exact ? "" : " (own allocations of the heap size)");
    else snprintf(vs, sizeof vs, "vram n/a");
    float used_mb, total_mb, ws_mb;
    double priv = 0, sys_used, sys_limit;
    plat_mem_info(&used_mb, &total_mb, &ws_mb);              // resident set (VmRSS)
    plat_commit_info(&priv, &sys_used, &sys_limit);          // private bytes
    double ws = ws_mb > 0 ? ws_mb : 0;
    if (priv < 0) priv = 0;
    const char *bound = pace_share >= 0.15 ? "pace"
                      : g.have_gpu ? (gpu_ms >= 0.8 * interval_ms ? "gpu" : "cpu")
                      : (rw + ow >= 0.5 * interval_ms ? "gpu" : "cpu");
    rt_log("perf: frame %llu: %s; fence wait per frame: render thread %.2f ms, other threads %.2f ms; %s; ram working set "
           "%.0f MiB, private %.0f MiB; bound: %s",
           (unsigned long long)frame, gs, rw, ow, vs, ws, priv, bound);
}

int rt_ab(void) {
    static int64_t period = -1;   // us of world clock; 0: off
    if (period < 0) {
        const char *e = cfg_get("MP4_AB");
        period = e && *e ? (int64_t)(atof(e) * 1e6) : 0;
    }
    int64_t w = period ? frame_step_world_us() : -1;
    if (w < 0) return 0;   // before the world clock: A
    int64_t k = w / period % 4;
    return k == 1 || k == 2;
}

int rt_knob(const char *name, const char *def) {
    char key[64];
    snprintf(key, sizeof key, "MP4_%s", name);
    const char *e = cfg_get(key);
    if (!e || !*e) e = def;
    return !strcmp(e, "ab") ? 2 : strcmp(e, "0") != 0;
}

// MP4_ONE_PER_CORE=1: the process keeps the first logical processor of each physical core, so busy threads (the
// render thread, the guest cores, the job workers) never share a core's execution units with another; "ab" switches
// it with the A/B blocks. Off by default until measured here; Savage Wind Recomp
// reported +4-10% from the same rule on an 8-core SMT CPU.
static uint64_t cpu_mask_of(const cpu_set_t *s) {
    uint64_t m = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, s)) m |= 1ull << i;
    return m;
}

// Linux affinity is per thread: every thread of the process gets the mask (threads started later inherit their
// creator's), as SetProcessAffinityMask did. Only when the knob changes the placement.
static void set_process_affinity(uint64_t mask) {
    cpu_set_t s;
    CPU_ZERO(&s);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &s);
    DIR *d = opendir("/proc/self/task");
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        pid_t tid = (pid_t)strtol(e->d_name, NULL, 10);
        if (tid > 0) sched_setaffinity(tid, sizeof s, &s);
    }
    closedir(d);
}

static void cpu_placement(void) {
    static int mode = -1, applied = -1;
    static uint64_t all, one;
    if (mode < 0) {
        mode = rt_knob("ONE_PER_CORE", "0");
        cpu_set_t s;
        CPU_ZERO(&s);
        if (sched_getaffinity(0, sizeof s, &s) == 0) all = cpu_mask_of(&s);
        int cores = 0;
        for (int c = 0; c < 64; c++) {   // the lowest logical processor of each core (sysfs thread_siblings_list)
            if (!(all >> c & 1)) continue;
            char path[96];
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", c);
            FILE *f = fopen(path, "r");
            int first = c;
            if (f) {
                if (fscanf(f, "%d", &first) != 1) first = c;
                fclose(f);
            }
            if (first == c) one |= 1ull << c, cores++;
        }
        if (mode && one && one != all)
            rt_log("cpu: %d cores, %d logical processors: one per core%s", cores, __builtin_popcountll(all),
                   mode == 2 ? " in the B blocks" : "");
        else
            mode = 0;
    }
    int on = RT_KNOB_ON(mode);
    if (on == applied) return;
    int was = applied;
    applied = on;
    if (on || was >= 0) set_process_affinity(on ? one : all);   // off from the start: the threads keep their own
}

void perf_present(uint64_t frame) {
    static uint64_t last, sum, max, n, win_start, ab_b;
    static int64_t win_prev[PERF_CNT_N];
    __atomic_store_n(&render_tid, my_tid(), __ATOMIC_RELAXED);
    cur_frame = frame;
    if (rt_trace & RT_TRACE_SCHED) kern_sched_report(frame);
    uint64_t now = kern_now_ns(), d = last ? now - last : 0;
    if (slow_ns && last && d >= slow_ns && !__atomic_load_n(&dump_hi, __ATOMIC_ACQUIRE)) {
        dump_lo = (int64_t)(now - d - 20000000ull);
        dump_frame = (int64_t)frame;
        __atomic_store_n(&dump_hi, (int64_t)now, __ATOMIC_RELEASE);
    }
    uint64_t rep = ft_frame(frame, now, d);   // d is 0 on the first present: it only starts the period
    ab_b += rt_ab();
    cpu_placement();
    if (__atomic_load_n(&overlay, __ATOMIC_RELAXED))   // the overlay's only cost while it is off: this branch
        perfmon_frame(now, d, frame_ns, rep, __atomic_load_n(&counts[PERF_CNT_TEX_UP], __ATOMIC_RELAXED));
    kern_core_diag_frame(frame, d);
    if (flight_on) flight_frame(frame, now, d, frame_ns, counts);
    if (!(rt_trace & RT_TRACE_PERF)) {
        memset(frame_ns, 0, sizeof frame_ns);
        last = now;
        return;
    }
    static uint64_t slow_ns;   // MP4_PERF_SLOW_MS (default 50): frames at least this long get a line of their own
    if (!slow_ns) {
        const char *e = cfg_get("MP4_PERF_SLOW_MS");
        slow_ns = (uint64_t)(e && atoi(e) > 0 ? atoi(e) : 50) * 1000000ull;
    }
    if (last) {
        sum += d;
        n++;
        if (d > max) max = d, memcpy(worst_ns, frame_ns, sizeof worst_ns);
        if (d >= slow_ns)
            rt_log("perf: slow frame %llu: %.1f ms; blocked pipeline %.1f xlate %.1f texture %.1f astc %.1f fence %.1f pacing %.1f",
                   (unsigned long long)frame, d / 1e6, frame_ns[PERF_PIPE] / 1e6, frame_ns[PERF_XLATE] / 1e6,
                   frame_ns[PERF_TEX] / 1e6, frame_ns[PERF_ASTC] / 1e6, frame_ns[PERF_FENCE] / 1e6,
                   frame_ns[PERF_PACE] / 1e6);
    }
    memset(frame_ns, 0, sizeof frame_ns);
    last = now;
    if (!win_start) win_start = now;
    if (frame % 60) return;
    uint64_t span = now - win_start;
    int64_t win[PERF_CNT_N];
    for (int i = 0; i < PERF_CNT_N; i++) {
        int64_t v = __atomic_load_n(&counts[i], __ATOMIC_RELAXED);
        win[i] = v - win_prev[i];
        win_prev[i] = v;
    }
    int64_t async = win[PERF_CNT_PIPE_ASYNC], skips = win[PERF_CNT_PIPE_SKIP], up = win[PERF_CNT_TEX_UP];
    int64_t reup = win[PERF_CNT_TEX_REUP], sskip = win[PERF_CNT_SHADER_SKIP], waited = win[PERF_CNT_PIPE_WAIT];
    int64_t oneshot = win[PERF_CNT_ONESHOT_WAIT];
    uint64_t shader_keys, shader_xlated;
    gfx_shader_counts(&shader_keys, &shader_xlated);
    if (n && span)
        rt_log("perf: frame %llu: interval avg %.2f ms max %.2f ms (%.1f fps); render thread blocked: pacing %.0f%% fence %.0f%% drain %.0f%% pipeline build %.0f%% shader xlate %.0f%% texture %.0f%% (astc %.0f%%); pipelines built in background %lld, draws skipped pending %lld, waited %lld, waiting for a shader %lld, one-shot waited %lld; textures uploaded %lld re-uploaded %lld; slowest frame: xlate %.1f ms texture %.1f ms (astc %.1f) pipeline %.1f ms fence %.1f ms; shaders: %llu keys, %llu translated; render busy avg %.2f ms; replay avg %.2f ms; ab %llu/%llu; world %.1f s",
               (unsigned long long)frame, sum / 1e6 / n, max / 1e6, n * 1e9 / sum, 100.0 * wait_ns[PERF_PACE] / span,
               100.0 * wait_ns[PERF_FENCE] / span, 100.0 * wait_ns[PERF_DRAIN] / span,
               100.0 * wait_ns[PERF_PIPE] / span, 100.0 * wait_ns[PERF_XLATE] / span,
               100.0 * wait_ns[PERF_TEX] / span, 100.0 * wait_ns[PERF_ASTC] / span, (long long)async, (long long)skips, (long long)waited, (long long)sskip, (long long)oneshot,
               (long long)up, (long long)reup, worst_ns[PERF_XLATE] / 1e6, worst_ns[PERF_TEX] / 1e6,
               worst_ns[PERF_ASTC] / 1e6, worst_ns[PERF_PIPE] / 1e6, worst_ns[PERF_FENCE] / 1e6,
               (unsigned long long)shader_keys, (unsigned long long)shader_xlated,
               (span > wait_ns[PERF_PACE] ? span - wait_ns[PERF_PACE] : 0) / 1e6 / n,
               __atomic_exchange_n(&replay_ns, 0, __ATOMIC_RELAXED) / 1e6 / n, (unsigned long long)ab_b, (unsigned long long)n, frame_step_world_us() / 1e6);
    if (n && span) perf_gpu_line(frame, sum / 1e6 / n, (double)wait_ns[PERF_PACE] / span, n);
    sum = max = n = ab_b = 0;
    memset(wait_ns, 0, sizeof wait_ns);
    win_start = now;
}
