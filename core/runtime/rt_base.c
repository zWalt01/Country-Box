// From fe3h-pc-recomp runtime/main.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4 only; adds MP4_BREAK_AT, a log at chosen lifted function entries.
// From totk-pc-recomp runtime/main.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied with totk renamed to fe3h only.
// Country-Box: moved out of main.c (logging, tracing, SHA-256, configuration, the break hooks, rt_brk, rt_udf and the
// guest counter) so the probes and the game link the same code without main.c's entry point; Linux only.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "kernel.h"
#include "platform/platform.h"

// ---- logging, tracing, hashing
unsigned rt_trace;

// Log file. A game run whose stderr is not a file already (started from Explorer, a shortcut or a console) also
// writes its log to logs\mp4.log next to mp4.exe, so a player has one to attach to a report; run_game.py and
// `2> file` keep their own file. Each start moves the earlier logs to mp4.1.log .. mp4.4.log. A file stops at 64 MiB
// (MP4_TRACE can write that much). Lines from before log_open wait in a static buffer: mem_init runs first so that
// nothing else lands in the guest address space, and an earlier open would put the CRT's heap there.
#define LOG_KEEP 5
#define LOG_CAP (64ull << 20)
enum { LOG_EARLY, LOG_FILE, LOG_OFF };
static int g_log_state;
static PlatAppend *g_log_file;
static char g_log_path[4096];
static char g_log_early[32768];
static size_t g_log_early_n;
static uint64_t g_log_bytes;

void rt_log_raw(const char *p, size_t n) {
    fwrite(p, 1, n, stderr);
    int state = __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE);
    if (state == LOG_EARLY) {
        size_t at = __atomic_fetch_add(&g_log_early_n, n, __ATOMIC_RELAXED);
        if (at + n <= sizeof g_log_early) memcpy(g_log_early + at, p, n);
    } else if (state == LOG_FILE) {
        uint64_t at = __atomic_fetch_add(&g_log_bytes, n, __ATOMIC_RELAXED);
        if (at + n <= LOG_CAP) {
            plat_append_write(g_log_file, p, n);
        } else if (at <= LOG_CAP) {   // the first write past the cap says so, once
            static const char cut[] = "[host] log: the file reached 64 MiB, the rest of this run goes to stderr only\n";
            plat_append_write(g_log_file, cut, sizeof cut - 1);
        }
    }
}

void rt_vlogf(const char *fmt, va_list ap) {
    char buf[2048];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) rt_log_raw(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}

void rt_log(const char *fmt, ...) {
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "[host] ");
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    n = k < 0 ? n : n + k < (int)sizeof buf - 1 ? n + k : (int)sizeof buf - 2;
    buf[n++] = '\n';
    rt_log_raw(buf, n);
}

const char *rt_log_path(void) {
    return __atomic_load_n(&g_log_state, __ATOMIC_ACQUIRE) == LOG_FILE ? g_log_path : NULL;
}

// Opens the log file for a game run (game = 1) and writes the buffered lines to it; anything else drops them. Runs on
// the main thread before the boot starts other threads.
void rt_log_open(int game) {
    char dir[4096], from[4096 + 32], to[4096 + 32];
    char *cut = NULL;
    if (game && !plat_stderr_path(dir, sizeof dir) && plat_exe_path(dir, sizeof dir)) {
        cut = strrchr(dir, '/');
    }
    if (cut && (size_t)(cut - dir) + sizeof "/logs" <= sizeof dir) {
        strcpy(cut, "/logs");
        plat_file_mkdir_p(dir);
        for (int i = LOG_KEEP - 1; i > 0; i--) {   // country-box.3.log -> .4.log, ..., country-box.log -> .1.log
            if (i > 1) snprintf(from, sizeof from, "%s/country-box.%d.log", dir, i - 1);
            else snprintf(from, sizeof from, "%s/country-box.log", dir);
            snprintf(to, sizeof to, "%s/country-box.%d.log", dir, i);
            if (plat_file_exists(from)) plat_file_replace(from, to);
        }
        snprintf(g_log_path, sizeof g_log_path, "%s/country-box.log", dir);
        g_log_file = plat_append_open(g_log_path);
    }
    if (!g_log_file) {
        __atomic_store_n(&g_log_state, LOG_OFF, __ATOMIC_RELEASE);
        return;
    }
    size_t n = __atomic_load_n(&g_log_early_n, __ATOMIC_RELAXED);
    if (n > sizeof g_log_early) n = sizeof g_log_early;
    plat_append_write(g_log_file, g_log_early, n);
    g_log_bytes = n;
    __atomic_store_n(&g_log_state, LOG_FILE, __ATOMIC_RELEASE);
    rt_log("log: %s", g_log_path);
}

static void break_init(void);   // below, with the hook

void rt_parse_trace(void) {
    break_init();
    const char *e = cfg_get("MP4_TRACE");   // no default: nothing is traced when unset
    if (!e) return;
    static const struct { const char *name; unsigned bit; } names[] = {
        {"svc", RT_TRACE_SVC}, {"host", RT_TRACE_HOSTCALL}, {"ipc", RT_TRACE_IPC}, {"fs", RT_TRACE_FS},
        {"gfx", RT_TRACE_GFX}, {"perf", RT_TRACE_PERF}, {"sched", RT_TRACE_SCHED}, {"audio", RT_TRACE_AUDIO},
        {"present", RT_TRACE_PRESENT}, {"all", ~0u}};
    for (const char *p = e; *p;) {
        size_t n = strcspn(p, ",");
        int known = 0;
        for (unsigned i = 0; i < sizeof names / sizeof *names; i++)
            if (strlen(names[i].name) == n && !strncmp(p, names[i].name, n)) rt_trace |= names[i].bit, known = 1;
        if (!known && n) rt_log("MP4_TRACE: unknown item '%.*s' (svc, host, ipc, fs, gfx, perf, sched, audio, present, all)", (int)n, p);
        p += n + (p[n] == ',');
    }
}

// SHA-256 (FIPS 180-4), written for Country-Box: the ExeFS check needs no crypto library in the AppImage.
static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

static uint32_t ror32(uint32_t x, int r) { return x >> r | x << (32 - r); }

static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}

void rt_sha256_begin(RtSha256 *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->bytes = 0;
}

void rt_sha256_update(RtSha256 *c, const void *data, size_t n) {
    const uint8_t *p = data;
    size_t fill = (size_t)(c->bytes & 63);
    c->bytes += n;
    if (fill) {
        size_t take = 64 - fill < n ? 64 - fill : n;
        memcpy(c->buf + fill, p, take);
        p += take, n -= take;
        if (fill + take < 64) return;
        sha_block(c->h, c->buf);
    }
    for (; n >= 64; p += 64, n -= 64) sha_block(c->h, p);
    memcpy(c->buf, p, n);
}

void rt_sha256_end(RtSha256 *c, uint8_t out[32]) {
    uint8_t tail[128] = {0};
    size_t left = (size_t)(c->bytes & 63);
    memcpy(tail, c->buf, left);
    tail[left] = 0x80;
    size_t tl = left < 56 ? 64 : 128;
    uint64_t bits = c->bytes * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha_block(c->h, tail);
    if (tl == 128) sha_block(c->h, tail + 64);
    for (int i = 0; i < 8; i++)
        out[4 * i] = (uint8_t)(c->h[i] >> 24), out[4 * i + 1] = (uint8_t)(c->h[i] >> 16), out[4 * i + 2] = (uint8_t)(c->h[i] >> 8),
        out[4 * i + 3] = (uint8_t)c->h[i];
}

void rt_sha256(const void *data, size_t n, uint8_t out[32]) {
    RtSha256 c;
    rt_sha256_begin(&c);
    rt_sha256_update(&c, data, n);
    rt_sha256_end(&c, out);
}

// ---- configuration
static RtConfig cfg;
const RtConfig *rt_config(void) { return &cfg; }
RtConfig *rt_config_set(void) { return &cfg; }

// ---- runtime services the generated code calls (cpu.h)
volatile int g_fnprof = 0;
unsigned char g_fnhook[1 << 16];   // Country-Box: cpu.h FN_ENTER's filter; a count per bucket (hooks and breaks)
static void fnhook_mark(uint64_t addr, int d) { g_fnhook[(addr >> 2) & 0xffff] += (unsigned char)d; }

// MP4_BREAK_AT=<module>+0x<offset>[,...] (at most 8): a log line at every entry of those lifted functions with the
// caller, the argument registers and the first bytes behind x0 and x1 when they point into committed memory, for
// chasing a bad argument without a debugger. The generated code calls fnprof_hit at every function entry once
// g_fnprof is set, so this costs a compare per call while it is on. The addresses resolve at the first hit, after
// the modules are loaded.
// MP4_BREAK_WATCH=<module>+0x<offset>[,...]: global pointer slots (a GOT entry, a static) whose pointee's first
// bytes are dumped at every break as well, for an object the broken function reaches through a global.
#define BREAK_MAX 8
static struct { char module[16]; uint64_t offset, addr; } g_breaks[BREAK_MAX], g_watches[BREAK_MAX];
static int g_break_count, g_watch_count;
static volatile int32_t g_break_hits;

static void break_init(void) {
    const char *e = cfg_get("MP4_BREAK_AT");
    if (!e || !*e) return;
    for (const char *p = e; *p && g_break_count < BREAK_MAX;) {
        size_t n = strcspn(p, ",");
        const char *plus = memchr(p, '+', n);
        if (!plus || plus - p >= 16) rt_fatal("MP4_BREAK_AT: '%.*s' is not <module>+0x<offset>", (int)n, p);
        memcpy(g_breaks[g_break_count].module, p, (size_t)(plus - p));
        g_breaks[g_break_count].offset = strtoull(plus + 1, NULL, 16);
        g_break_count++;
        p += n + (p[n] == ',');
    }
    const char *w = cfg_get("MP4_BREAK_WATCH");
    for (const char *p = w ? w : ""; *p && g_watch_count < BREAK_MAX;) {
        size_t n = strcspn(p, ",");
        const char *plus = memchr(p, '+', n);
        if (!plus || plus - p >= 16) rt_fatal("MP4_BREAK_WATCH: '%.*s' is not <module>+0x<offset>", (int)n, p);
        memcpy(g_watches[g_watch_count].module, p, (size_t)(plus - p));
        g_watches[g_watch_count].offset = strtoull(plus + 1, NULL, 16);
        g_watch_count++;
        p += n + (p[n] == ',');
    }
    g_fnprof = 1;
}

// ---- Country-Box: entry hooks. A host function runs at the entry of a lifted guest function (FN_ENTER), before its
// body: it may change registers or memory and let the body run (returns 0), or return for it (returns 1, x0 the
// result; the lifted caller reloads its registers from the Cpu after the call). Slots are never compacted, so a
// thread that read the count sees valid entries; a removed hook's slot keeps addr 0.
#define ENTRY_HOOKS_MAX 32
static struct { volatile uint64_t addr; RtEntryHook fn; void *arg; } g_hooks[ENTRY_HOOKS_MAX];
static volatile int32_t g_nhooks;
static PlatMutex g_hook_lock = PLAT_MUTEX_INIT;

static void fnprof_update(void) {   // g_hook_lock held
    int live = 0;
    for (int i = 0; i < g_nhooks; i++) live |= g_hooks[i].addr != 0;
    g_fnprof = g_break_count > 0 || live;
}

void rt_entry_hook_set(uint64_t addr, RtEntryHook fn, void *arg) {
    plat_mutex_lock(&g_hook_lock);
    int i = 0;
    while (i < g_nhooks && g_hooks[i].addr != addr) i++;
    if (i == g_nhooks && fn) {
        for (i = 0; i < g_nhooks && g_hooks[i].addr; i++) {}
        if (i == g_nhooks) {
            if (g_nhooks == ENTRY_HOOKS_MAX) rt_fatal("entry hooks: more than %d", ENTRY_HOOKS_MAX);
            __atomic_store_n(&g_nhooks, g_nhooks + 1, __ATOMIC_RELEASE);
        }
    }
    if (i < g_nhooks) {
        int was = g_hooks[i].addr != 0;
        g_hooks[i].fn = fn, g_hooks[i].arg = arg;
        __atomic_store_n(&g_hooks[i].addr, fn ? addr : 0, __ATOMIC_RELEASE);
        if (fn && !was) fnhook_mark(addr, 1);
        if (!fn && was) fnhook_mark(addr, -1);
    }
    fnprof_update();
    plat_mutex_unlock(&g_hook_lock);
}

int rt_entry(uint64_t addr, void *cpu) {
    if (g_break_count) fnprof_hit(addr, cpu);
    int n = __atomic_load_n(&g_nhooks, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++)
        if (__atomic_load_n(&g_hooks[i].addr, __ATOMIC_ACQUIRE) == addr) {
            RtEntryHook fn = g_hooks[i].fn;
            void *arg = g_hooks[i].arg;
            return fn && __atomic_load_n(&g_hooks[i].addr, __ATOMIC_ACQUIRE) == addr ? fn((Cpu *)cpu, arg) : 0;
        }
    return 0;
}

static int guest_readable(uint64_t a, size_t n) {   // both ends copy without faulting (plat_mem_read)
    uint8_t b;
    return a && n && plat_mem_read(a, &b, 1) && plat_mem_read(a + n - 1, &b, 1);
}

static void break_dump(const char *what, uint64_t a, size_t n) {
    if (!guest_readable(a, n)) return;
    char line[3 * 64 + 1];
    for (size_t off = 0; off < n; off += 32) {
        size_t k = 0;
        for (size_t i = 0; i < 32 && off + i < n; i++)
            k += (size_t)snprintf(line + k, sizeof line - k, "%02x ", *(const uint8_t *)(uintptr_t)(a + off + i));
        rt_log("break:   %s+%#zx: %s", what, off, line);
    }
}

// MP4_BREAK_FOLLOW=[x<n>:]<off>[,<off>...][@<off>[/<len>]][;...]: from x0 (or x<n>, any of x0..x30), load the pointer
// at each offset in turn and dump 64 bytes (or <len>, hex, at most 0x1000) at the end of the chain, from "@<off>" into
// the final object (an object reached through a list entry, say: its first word is its vtable). Chains separated by
// ';' are dumped in turn, for several objects at one break (a callee-saved register still holds the caller's value
// at a function's entry).
static void break_follow(const Cpu *c) {
    const char *follow = cfg_value("MP4_BREAK_FOLLOW");
    for (const char *s = follow ? follow : ""; *s;) {
        int reg = 0;
        if (s[0] == 'x' && s[1] >= '0' && s[1] <= '9') {
            char *end;
            long r = strtol(s + 1, &end, 10);
            if (*end == ':' && r >= 0 && r <= 30) reg = (int)r, s = end + 1;
        }
        uint64_t q = c->x[reg], at = 0, len = 64;
        char chain[96];
        snprintf(chain, sizeof chain, "x%d", reg);
        while (*s && *s != ';' && q) {
            char *end;
            if (*s == '@') {
                at = strtoull(s + 1, &end, 16);
                s = end;
                if (*s == '/') len = strtoull(s + 1, &end, 16), s = end;
                if (len < 1 || len > 0x1000) len = 64;
                break;
            }
            uint64_t off = strtoull(s, &end, 16);
            if (end == s) break;
            q = guest_readable(q + off, 8) ? *(const uint64_t *)(uintptr_t)(q + off) : 0;
            snprintf(chain + strlen(chain), sizeof chain - strlen(chain), "->[+%#llx]", (unsigned long long)off);
            s = *end == ',' ? end + 1 : end;
        }
        if (q) {
            snprintf(chain + strlen(chain), sizeof chain - strlen(chain), "=%#llx+%#llx", (unsigned long long)q, (unsigned long long)at);
            break_dump(chain, q + at, (size_t)len);
        }
        s += strcspn(s, ";");   // the rest of a chain that ended early (null pointer, unreadable slot)
        if (*s == ';') s++;
    }
}

// Country-Box: the break and watch addresses once the modules are loaded (boot.c, after ldr_load), with the breaks
// marked in FN_ENTER's filter (cpu.h g_fnhook); it was the first hit's job before the filter.
void rt_breaks_resolve(void) {
    for (int k = 0; k < g_break_count; k++)
        for (int i = 0; i < ldr_count(); i++)
            if (!strcmp(ldr_module(i)->name, g_breaks[k].module)) {
                g_breaks[k].addr = ldr_module(i)->base + g_breaks[k].offset;
                fnhook_mark(g_breaks[k].addr, 1);
            }
    for (int k = 0; k < g_watch_count; k++)
        for (int i = 0; i < ldr_count(); i++)
            if (!strcmp(ldr_module(i)->name, g_watches[k].module))
                g_watches[k].addr = ldr_module(i)->base + g_watches[k].offset;
}

void fnprof_hit(uint64_t addr, void *cpu) {
    const Cpu *c = cpu;
    for (int k = 0; k < g_break_count; k++) {
        if (addr != g_breaks[k].addr) continue;
        // MP4_BREAK_BRIEF=1: one line per hit with x0..x3 and 32 bytes at x0+0x30, up to 400,000 hits, for a value's
        // trajectory over a run; otherwise the full dump, 4,000 hits.
        // MP4_BREAK_COUNT=1: no line per hit; hits are counted per caller, and every 10 s the callers with the most
        // hits since the last summary are logged (for finding which call sites of a common function run per frame).
        static int count = -1;
        if (count < 0) { const char *e = cfg_get("MP4_BREAK_COUNT"); count = e && !strcmp(e, "1"); }
        if (count) {
            enum { NC = 1024 };
            static struct { uint64_t lr; volatile int32_t n; } cs[NC];
            static PlatMutex cl = PLAT_MUTEX_INIT;
            static uint64_t t_last;
            uint64_t lr = c->x[30] - 4;
            plat_mutex_lock(&cl);
            uint32_t h = (uint32_t)((lr >> 2) * 0x9e3779b1u) & (NC - 1);
            while (cs[h].lr && cs[h].lr != lr) h = (h + 1) & (NC - 1);
            cs[h].lr = lr, cs[h].n++;
            uint64_t now = (plat_now_ns() / 1000000ull);
            if (!t_last) t_last = now;
            if (now - t_last >= 10000) {
                t_last = now;
                rt_log("break count: frame %llu, callers of %s:", (unsigned long long)gfx_stats().frames,
                       ldr_describe(addr, (char[128]){0}, 128));
                for (int r = 0; r < 60; r++) {
                    int best = -1;
                    for (int i = 0; i < NC; i++)
                        if (cs[i].lr && cs[i].n > 0 && (best < 0 || cs[i].n > cs[best].n)) best = i;
                    if (best < 0) break;
                    char d[128];
                    rt_log("  %8ld from %s", (long)cs[best].n, ldr_describe(cs[best].lr, d, sizeof d));
                    cs[best].n = -cs[best].n;   // listed; restored below
                }
                for (int i = 0; i < NC; i++) cs[i].n = 0;
            }
            plat_mutex_unlock(&cl);
            continue;
        }
        static int brief = -1;
        if (brief < 0) brief = !strcmp(cfg_value("MP4_BREAK_BRIEF"), "1");
        int32_t hits = plat_atomic_inc(&g_break_hits);
        if (hits > (brief ? 400000 : 4000)) return;
        char at[128], lr[128];
        if (brief) {
            char peek[3 * 32 + 1] = "";
            if (guest_readable(c->x[0] + 0x30, 32))
                for (int i = 0; i < 32; i++) snprintf(peek + 3 * i, 4, "%02x ", *(const uint8_t *)(uintptr_t)(c->x[0] + 0x30 + i));
            float s0, s1;   // the first float arguments
            memcpy(&s0, &c->v[0], 4), memcpy(&s1, &c->v[1], 4);
            rt_log("break: %s from %s: x0 %#llx x1 %#llx x2 %#llx x3 %#llx s0 %g s1 %g; x0+0x30: %s", ldr_describe(addr, at, sizeof at),
                   ldr_describe(c->x[30] - 4, lr, sizeof lr), (unsigned long long)c->x[0], (unsigned long long)c->x[1],
                   (unsigned long long)c->x[2], (unsigned long long)c->x[3], s0, s1, peek);
            break_follow(c);
            continue;
        }
        rt_log("break: %s from %s: x0 %#llx x1 %#llx x2 %#llx x3 %#llx x4 %#llx x8 %#llx sp %#llx", ldr_describe(addr, at, sizeof at),
               ldr_describe(c->x[30] - 4, lr, sizeof lr), (unsigned long long)c->x[0], (unsigned long long)c->x[1],
               (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[4],
               (unsigned long long)c->x[8], (unsigned long long)c->sp);
        break_dump("x0", c->x[0], 192);
        break_dump("x1", c->x[1], 64);
        // MP4_BREAK_FOLLOW=<off>[,<off>...]: from x0, load the pointer at each offset in turn and dump 64 bytes at
        // the end of the chain (an object reached through a list entry, say: its first word is its vtable)
        break_follow(c);
        for (int w = 0; w < g_watch_count; w++) {
            if (!guest_readable(g_watches[w].addr, 8)) continue;
            uint64_t obj = *(const uint64_t *)(uintptr_t)g_watches[w].addr;
            char name[40];
            snprintf(name, sizeof name, "*%s+%#llx=%#llx", g_watches[w].module, (unsigned long long)g_watches[w].offset,
                     (unsigned long long)obj);
            break_dump(name, obj, 128);
        }
    }
}

void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) {
    char at[128], lr[128];
    kern_backtrace(c);   // Country-Box: the guest frames that led to the trap
    rt_fatal("brk #%#x at %s (lr %s)", imm, ldr_describe(pc, at, sizeof at), ldr_describe(c->x[30], lr, sizeof lr));
}

void rt_udf(Cpu *c, uint64_t pc) {
    char at[128], lr[128];
    kern_backtrace(c);
    rt_fatal("undefined instruction at %s (lr %s)", ldr_describe(pc, at, sizeof at),
             ldr_describe(c->x[30], lr, sizeof lr));
}

// CNTPCT_EL0: the Switch counter runs at 19.2 MHz. The scale is set before main, so no thread reads it half written.
static PlatScale tick_scale;

__attribute__((constructor)) static void tick_scale_init(void) { tick_scale = plat_scale(19200000u, 1000000000u); }

uint64_t rt_ticks(void) { return plat_scaled(tick_scale, plat_now_ns()); }

