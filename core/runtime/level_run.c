// Country-Box: the measured run of one warped level (level_run.h), moved out of dkcr_tools.c so Tropical Freeze's tools
// print the same lines; Returns HD's lines are unchanged.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "host.h"
#include "level_run.h"
#include "platform/platform.h"

#define PLAY_RUN_FRAMES 30
enum { RUN_OFF, RUN_LOADING, RUN_MEASURE, RUN_DONE };
static volatile int g_run = RUN_OFF;
static int32_t g_target_id;
static char g_target_name[128];
static uint64_t g_t_warp, g_t_load, g_t0, g_t_last, g_frames;
// End of the level: the level-clear screen (level_run_clear, an entry hook of the game's tools); the play time is no
// end signal: it stands still through a boss fight (Returns' Mugly, measured). A boss level may also go straight back
// to the map: leaving the level ends the run too.
static volatile uint64_t g_clear_t;       // plat_now_ns at LEVEL CLEAR, 0 before
static uint64_t g_clear_frames;
static size_t g_clear_niv;
static double g_pt;                       // the play time at the last present (start detection)
static uint64_t g_pt_moves;
// Control: the player can move. A frame's drops are the vblanks it missed at 60 Hz (an interval of 2 x 16.7 ms is 1
// dropped frame); they are counted with and without control, since a drop in a cutscene, a death or a transition is not
// felt as one in play.
static uint64_t g_ctl_frames, g_ctl_drops, g_noctl_frames, g_noctl_drops;
static float g_ctl_worst;
static float *g_iv;                       // present intervals in ms while measuring
static size_t g_niv, g_civ;
// The same frames timed where the player sees them: the host flip (vk_backend.c present thread, level_run_flip), not
// the game's present call. A game that does not wait for vsync between frames (Tropical Freeze: the host pacer holds
// it) calls present with its own work's jitter while the flips stay on the period. Logged as its own line, beside the
// call-time one, until the two are compared on both games.
#define FLIP_MAX (1u << 19)               // 2.4 h at 60 Hz
static float g_fv[FLIP_MAX];
static volatile uint32_t g_nfv;
static uint64_t g_flip_last;
#define MAX_TASKS 256
typedef struct { int tid; char name[16]; uint64_t ticks; } TaskTime;
static TaskTime g_task0[MAX_TASKS];
static int g_ntask0;
// CB_TURBO_HZ=<N>: fast-forward a level once it plays (gfx_window.c gfx_vsync_set_hz: vsync events and present pacing
// at N; the games step a fixed 1/60 s per frame, so N/60 times faster when the CPU keeps up); the menus and the load
// ran at the boot rate (CB_VSYNC_HZ, default 60), which comes back when the run ends. Weak: a runtime without the
// live switch keeps its rate.
uint64_t gfx_vsync_hz(void) __attribute__((weak));
void gfx_vsync_set_hz(uint64_t hz) __attribute__((weak));
static uint64_t g_boot_hz;                // the rate before the switch; 0: no switch made

static void turbo_on(void) {
    const char *tz = getenv("CB_TURBO_HZ");
    if (!tz || !*tz || !gfx_vsync_set_hz || !gfx_vsync_hz) return;
    g_boot_hz = gfx_vsync_hz();
    gfx_vsync_set_hz((uint64_t)strtoull(tz, NULL, 10));
}

static void turbo_off(void) {
    if (g_boot_hz && gfx_vsync_set_hz) gfx_vsync_set_hz(g_boot_hz);
    g_boot_hz = 0;
}

int level_run_active(void) {
    int r = __atomic_load_n(&g_run, __ATOMIC_ACQUIRE);
    return r == RUN_LOADING || r == RUN_MEASURE;
}

void level_run_warped(const char *name, int32_t world_id, double play_time) {
    g_target_id = world_id;
    snprintf(g_target_name, sizeof g_target_name, "%s", name);
    g_t_warp = plat_now_ns();
    g_pt = play_time, g_pt_moves = 0;
    __atomic_store_n(&g_run, RUN_LOADING, __ATOMIC_RELEASE);
}

void level_run_clear(const char *source) {
    if (__atomic_load_n(&g_run, __ATOMIC_ACQUIRE) == RUN_MEASURE && !g_clear_t) {
        g_clear_frames = 0;
        __atomic_store_n(&g_clear_t, plat_now_ns(), __ATOMIC_RELEASE);
        rt_log("level: LEVEL CLEAR (%s)", source);
    }
}

// CPU time of every thread of the process (utime + stime, clock ticks) from /proc/self/task.
static int task_times(TaskTime *out, int cap) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < cap) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[64], buf[512];
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t k = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[k] = 0;
        char *l = strchr(buf, '('), *r = strrchr(buf, ')');
        if (!l || !r) continue;
        TaskTime *t = &out[n++];
        t->tid = atoi(e->d_name);
        size_t nl = (size_t)(r - l - 1) < sizeof t->name - 1 ? (size_t)(r - l - 1) : sizeof t->name - 1;
        memcpy(t->name, l + 1, nl);
        t->name[nl] = 0;
        unsigned long long ut = 0, st = 0;   // fields 14 and 15; field 3 (state) follows ") "
        sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st);
        t->ticks = ut + st;
    }
    closedir(d);
    return n;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

void level_run_flip(uint64_t t_ns) {
    if (__atomic_load_n(&g_run, __ATOMIC_ACQUIRE) != RUN_MEASURE || __atomic_load_n(&g_clear_t, __ATOMIC_ACQUIRE)) {
        g_flip_last = 0;
        return;
    }
    uint32_t n = __atomic_load_n(&g_nfv, __ATOMIC_RELAXED);
    if (g_flip_last && n < FLIP_MAX) {
        g_fv[n] = (float)((t_ns - g_flip_last) / 1e6);
        __atomic_store_n(&g_nfv, n + 1, __ATOMIC_RELEASE);
    }
    g_flip_last = t_ns;
}

static void report(const char *why) {
    __atomic_store_n(&g_run, RUN_DONE, __ATOMIC_RELEASE);
    turbo_off();
    double secs = (g_t_last - g_t0) / 1e9;
    rt_log("level: result %s WorldID %d end %s, %.1f s and %llu frames measured, load %.1f s", g_target_name,
           g_target_id, why, secs, (unsigned long long)g_frames, (g_t_load - g_t_warp) / 1e9);
    if (g_niv) {
        double sum = 0;
        int over16 = 0, over33 = 0;
        for (size_t i = 0; i < g_niv; i++) sum += g_iv[i], over16 += g_iv[i] > 16.9f, over33 += g_iv[i] > 33.4f;
        qsort(g_iv, g_niv, sizeof *g_iv, cmp_float);
        rt_log("level: frame ms mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f; over 16.9 ms %d, over 33.4 ms %d (of %zu)",
               sum / g_niv, g_iv[g_niv / 2], g_iv[g_niv * 95 / 100], g_iv[g_niv * 99 / 100], g_iv[g_niv - 1], over16,
               over33, g_niv);
    }
    size_t nf = __atomic_load_n(&g_nfv, __ATOMIC_ACQUIRE);
    if (nf) {   // the host flips of the same window (level_run_flip)
        double sum = 0;
        int over16 = 0, over33 = 0;
        for (size_t i = 0; i < nf; i++) sum += g_fv[i], over16 += g_fv[i] > 16.9f, over33 += g_fv[i] > 33.4f;
        qsort(g_fv, nf, sizeof *g_fv, cmp_float);
        rt_log("level: flip ms mean %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f; over 16.9 ms %d, over 33.4 ms %d (of %zu)",
               sum / nf, g_fv[nf / 2], g_fv[nf * 95 / 100], g_fv[nf * 99 / 100], g_fv[nf - 1], over16, over33, nf);
    }
    rt_log("level: dropped frames with control %llu (in %llu frames with control, worst %.1f ms), without control %llu "
           "(in %llu frames)", (unsigned long long)g_ctl_drops, (unsigned long long)g_ctl_frames, g_ctl_worst,
           (unsigned long long)g_noctl_drops, (unsigned long long)g_noctl_frames);
    static TaskTime now[MAX_TASKS];
    int n = task_times(now, MAX_TASKS);
    double hz = (double)sysconf(_SC_CLK_TCK), frames = g_frames ? (double)g_frames : 1;
    typedef struct { char name[16]; double ms; } Use;
    Use use[MAX_TASKS];
    int nu = 0;
    double total = 0;
    for (int i = 0; i < n; i++) {
        uint64_t before = 0;
        for (int j = 0; j < g_ntask0; j++)
            if (g_task0[j].tid == now[i].tid) before = g_task0[j].ticks;
        double ms = (double)(now[i].ticks - before) / hz * 1000.0 / frames;
        total += ms;
        if (ms >= 0.05) snprintf(use[nu].name, sizeof use[nu].name, "%s", now[i].name), use[nu++].ms = ms;
    }
    for (int i = 0; i < nu; i++)   // busiest first
        for (int j = i + 1; j < nu; j++)
            if (use[j].ms > use[i].ms) { Use t = use[i]; use[i] = use[j]; use[j] = t; }
    char line[1024];
    int k = snprintf(line, sizeof line, "level: cpu ms per frame %.2f in all threads; busiest:", total);
    for (int i = 0; i < nu && i < 10 && k < (int)sizeof line - 40; i++)
        k += snprintf(line + k, sizeof line - (size_t)k, " %s %.2f%s", use[i].name, use[i].ms, i + 1 < nu && i < 9 ? "," : "");
    rt_log("%s", line);
}

static int exit_after(void) {
    const char *e = getenv("CB_WARP_EXIT");
    return !(e && !strcmp(e, "0"));
}

int level_run_frame(uint64_t frame, double play_time, int in_level, int control, const char *(*left)(void)) {
    int run = __atomic_load_n(&g_run, __ATOMIC_ACQUIRE);
    if (run == RUN_OFF || run == RUN_DONE) return 0;
    uint64_t now = plat_now_ns();
    if (run == RUN_LOADING) {
        static uint64_t t_first;   // the first present of the current run of advancing play time
        if (play_time == g_pt || !in_level) {
            g_pt = play_time, g_pt_moves = 0;
            return 0;
        }
        g_pt = play_time;
        if (g_pt_moves++ == 0) t_first = now;
        if (g_pt_moves < PLAY_RUN_FRAMES) return 0;
        g_t0 = g_t_last = now, g_frames = 0, g_niv = 0;
        g_flip_last = 0, __atomic_store_n(&g_nfv, 0, __ATOMIC_RELEASE);
        g_t_load = t_first;
        g_clear_t = 0;
        g_ctl_frames = g_ctl_drops = g_noctl_frames = g_noctl_drops = 0, g_ctl_worst = 0;
        g_ntask0 = task_times(g_task0, MAX_TASKS);
        g_run = RUN_MEASURE;
        rt_log("level: %s playing %.1f s after the warp (frame %llu); measuring", g_target_name, (t_first - g_t_warp) / 1e9,
               (unsigned long long)frame);
        turbo_on();
        return 0;
    }
    if (g_niv == g_civ) {
        g_civ = g_civ ? 2 * g_civ : 8192;
        g_iv = realloc(g_iv, g_civ * sizeof *g_iv);
        if (!g_iv) rt_fatal("level: out of memory for frame times");
    }
    g_iv[g_niv++] = (float)((now - g_t_last) / 1e6);
    {
        float ms = g_iv[g_niv - 1];
        uint64_t drops = ms > 25.0f ? (uint64_t)(ms / 16.667f + 0.5f) - 1 : 0;   // vblanks missed at 60 Hz
        if (control > 0) {
            g_ctl_frames++, g_ctl_drops += drops;
            if (ms > g_ctl_worst) g_ctl_worst = ms;
        } else {
            g_noctl_frames++, g_noctl_drops += drops;
        }
    }
    if (rt_spike_diag_on()) rt_spike_diag(frame, (now - g_t_last) / 1e6, control);
    g_t_last = now, g_frames++;
    static int trace = -1;
    if (trace < 0) trace = getenv("CB_WARP_TRACE") && !strcmp(getenv("CB_WARP_TRACE"), "1");
    if (trace && g_frames % 60 == 0) rt_log("level: frame %llu play time %.2f s", (unsigned long long)frame, play_time);
    uint64_t clear = __atomic_load_n(&g_clear_t, __ATOMIC_ACQUIRE);
    if (clear && now - clear > 2000000000ull) {   // 2 s of the LEVEL CLEAR screen in the shots, not in the numbers
        g_frames = g_clear_frames, g_t_last = clear, g_niv = g_clear_niv;
        report("completed (LEVEL CLEAR)");
        return exit_after();
    }
    if (clear && !g_clear_frames) g_clear_frames = g_frames, g_clear_niv = g_niv;
    if (in_level) return 0;
    report(left ? left() : "another world");
    return exit_after();
}

void level_run_finish(const char *why) {
    if (__atomic_load_n(&g_run, __ATOMIC_ACQUIRE) == RUN_MEASURE) report(why);
}
