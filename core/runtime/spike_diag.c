// Country-Box: frame-spike diagnosis shared by the games' tools (dkcr_tools.c for Returns HD, ov_dktf.c for Tropical
// Freeze), moved out of dkcr_tools.c; they decide when a present is measured and whether the player has control, this
// file says what the threads did. The log lines are those tools/sweep.py parses.
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"

#define SPIKE_TASKS 256
// CB_SPIKE_DIAG=1: at every present a game's tools measure (rt_spike_diag), each thread's run time and run-queue wait (schedstat, ns); a present
// over CB_SPIKE_MS (33.4) logs the threads that ran or waited most since the previous one, to tell game work (one guest thread
// busy), starvation (threads runnable but waiting for a CPU) and a wait on something else (nobody ran) apart.
// Diagnostic only: about 70 small /proc reads per frame on the presenting thread. The page faults of each thread
// (minor: a page mapped from memory, e.g. code of the 350 MB binary already in the page cache; major: read from disk)
// tell a first touch of code or data from steady work. CB_SPIKE_SHOTS=<dir> saves the spike's frame as
// <dir>/spike-<frame>.png (at most 30; the present after a shot is not judged: the shot itself makes it slow), to see
// whether a spike is gameplay or a loading screen.
typedef struct { int tid; char name[16]; uint64_t run, wait, minflt, majflt; } Sched;
static Sched g_sp_prev[SPIKE_TASKS];
static int g_sp_nprev;

static int sched_times(Sched *out, int cap) {
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < cap) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[64], buf[96];
        Sched *s = &out[n];
        s->tid = atoi(e->d_name);
        snprintf(path, sizeof path, "/proc/self/task/%s/schedstat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        unsigned long long r = 0, w = 0;
        int ok = fscanf(f, "%llu %llu", &r, &w) == 2;
        fclose(f);
        if (!ok) continue;
        s->run = r, s->wait = w, s->minflt = s->majflt = 0;
        char st[512];
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        if ((f = fopen(path, "r"))) {
            size_t k = fread(st, 1, sizeof st - 1, f);
            fclose(f);
            st[k] = 0;
            char *rp = strrchr(st, ')');   // fields after the name: state ppid pgrp session tty tpgid flags minflt cminflt majflt
            unsigned long long mn = 0, mj = 0;
            if (rp && sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %llu %*u %llu", &mn, &mj) == 2) s->minflt = mn, s->majflt = mj;
        }
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        s->name[0] = 0;
        if ((f = fopen(path, "r"))) {
            if (fgets(buf, sizeof buf, f)) snprintf(s->name, sizeof s->name, "%s", strtok(buf, "\n"));
            fclose(f);
        }
        n++;
    }
    closedir(d);
    return n;
}

static char g_spike_shot[1100];   // rt_spike_shot: the path for this present's frame, "" none
static int g_shot_skip, g_shots;

const char *rt_spike_shot(void) { return g_spike_shot[0] ? g_spike_shot : NULL; }

int rt_spike_diag_on(void) {
    static int on = -1;
    if (on < 0) on = getenv("CB_SPIKE_DIAG") && !strcmp(getenv("CB_SPIKE_DIAG"), "1");
    return on;
}

// control: 1 the player has control, 0 not (both named in the line, which tools/sweep.py parses), -1 unknown (omitted).
void rt_spike_diag(uint64_t frame, double interval_ms, int control) {
    static Sched cur[SPIKE_TASKS];
    int n = sched_times(cur, SPIKE_TASKS);
    g_spike_shot[0] = 0;
    int skip = g_shot_skip;
    g_shot_skip = 0;
    static double limit = -1;   // CB_SPIKE_MS: the trigger (default 33.4 ms, two frames at 60 fps)
    if (limit < 0) limit = getenv("CB_SPIKE_MS") ? atof(getenv("CB_SPIKE_MS")) : 33.4;
    if (interval_ms > limit && g_sp_nprev && skip) {
        rt_log("spike: frame %llu took %.1f ms (after a spike shot: not judged)", (unsigned long long)frame, interval_ms);
    } else if (interval_ms > limit && g_sp_nprev) {
        typedef struct { const char *name; double run, wait; uint64_t mn, mj; } D;
        D d[SPIKE_TASKS];
        int nd = 0;
        double run_all = 0;
        uint64_t mn_all = 0, mj_all = 0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < g_sp_nprev; j++)
                if (g_sp_prev[j].tid == cur[i].tid) {
                    d[nd] = (D){cur[i].name, (cur[i].run - g_sp_prev[j].run) / 1e6, (cur[i].wait - g_sp_prev[j].wait) / 1e6,
                                cur[i].minflt - g_sp_prev[j].minflt, cur[i].majflt - g_sp_prev[j].majflt};
                    run_all += d[nd].run, mn_all += d[nd].mn, mj_all += d[nd].mj;
                    nd++;
                }
        for (int i = 0; i < nd; i++)
            for (int j = i + 1; j < nd; j++)
                if (d[j].run + d[j].wait > d[i].run + d[i].wait) { D t = d[i]; d[i] = d[j]; d[j] = t; }
        char line[1400];
        char ctl[24] = "";
        if (control >= 0) snprintf(ctl, sizeof ctl, " (%s control)", control ? "with" : "without");
        int k = snprintf(line, sizeof line, "spike: frame %llu took %.1f ms%s; threads ran %.1f ms in all, page "
                         "faults %llu minor %llu major; most (run/wait ms, minor/major faults):", (unsigned long long)frame,
                         interval_ms, ctl, run_all, (unsigned long long)mn_all, (unsigned long long)mj_all);
        for (int i = 0; i < nd && i < 8 && k < (int)sizeof line - 64; i++)
            k += snprintf(line + k, sizeof line - (size_t)k, " %s %.1f/%.1f %llu/%llu%s", d[i].name, d[i].run, d[i].wait,
                          (unsigned long long)d[i].mn, (unsigned long long)d[i].mj, i < 7 ? "," : "");
        rt_log("%s", line);
        const char *dir = getenv("CB_SPIKE_SHOTS");
        if (dir && *dir && g_shots < 30) {
            snprintf(g_spike_shot, sizeof g_spike_shot, "%s/spike-%05llu.png", dir, (unsigned long long)frame);
            g_shots++, g_shot_skip = 1;
        }
    }
    memcpy(g_sp_prev, cur, (size_t)n * sizeof *cur);
    g_sp_nprev = n;
}
