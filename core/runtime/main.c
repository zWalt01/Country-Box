// From fe3h-pc-recomp runtime/main.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4 only; adds MP4_BREAK_AT, a log at chosen lifted function entries.
// From totk-pc-recomp runtime/main.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied with totk renamed to fe3h only. Country-Box: crash report from a SIGSEGV/SIGBUS/SIGILL/SIGFPE handler on an alternate stack; --probe-dpi and the timer period gone (Linux); the selftest expects host memory Inaccessible.
// mp4 entry: reserves the guest address space, parses the command line and runs one mode.
//
//   mp4 [--exefs DIR] [--romfs DIR] [--profile DIR] <mode>
//     --selftest        layout, dispatch table and host-function registry; needs no game files
//     --probe-loader    load and relocate the modules, check them against Returns HD's measured values (tools/measure_modules.py)
//     --probe-kernel, --probe-ipc, --probe-services, --probe-fs    host probes
//     --probe-gfx-abi   NVN object layouts against nvn.h's reserved sizes; needs no game files
//     --probe-nvn, --probe-nvn-tex, --probe-window, --probe-shader, --probe-vkmem, --probe-vkexec    graphics probes
//     --probe-astc      the compute ASTC decoder against astc_decode_ldr; needs no game files
//     --bench-astc      the compute ASTC decoder timed on the uploads MP4_ASTC_DUMP wrote (astc_probe.c bench_astc)
//     --probe-movie     host VP9 movie::Decoder on one RomFS movie; skips without game data
//     --boot            run the game to the first NVN call
//     --seconds S       run the game (implies --boot's start-up) until the world clock (game time since the loaded
//                       save's first frame) passed S seconds, then exit 0 at that present
//     --shot PNG        with --seconds: save that frame; env MP4_GPU=null|vulkan selects the back end
//     (no mode)         run the game
//     --settings-dump, --settings-set key=value ...   the settings model as JSON / change it (settings.c), for the
//                       launcher; nothing else starts
//
// Also here: the runtime services the generated code calls besides rt_dispatch and rt_svc (rt_brk, rt_udf,
// rt_ticks, the function profiler hook), logging, SHA-256, and the crash reporter (a SIGSEGV/SIGBUS/SIGILL/SIGFPE
// handler on an alternate stack that names the lifted function, the guest registers and the guest frame-pointer chain).
#define _GNU_SOURCE
#include <limits.h>
#include <link.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <ucontext.h>
#include <unistd.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "settings.h"
#include "platform/platform.h"
#include "shader_prep.h"

#define MAX_MODULES 16

// Logging, tracing, SHA-256, configuration, break hooks, rt_brk/rt_udf and rt_ticks: rt_base.c (Country-Box).
#define cfg (*rt_config_set())

// ---- crash reporter
typedef struct { uintptr_t host; uint64_t guest; } FnRev;
static FnRev *fn_rev;
static uint64_t fn_rev_n;

static int by_host(const void *a, const void *b) {
    uintptr_t x = ((const FnRev *)a)->host, y = ((const FnRev *)b)->host;
    return x < y ? -1 : x > y;
}

// The lifted function that contains a host code address, by the nearest function start below it.
static uint64_t lifted_function(uintptr_t rip) {
    if (!fn_rev) {
        static RtModule mods[MAX_MODULES];
        int n = rt_linked_modules(mods, MAX_MODULES);
        uint64_t total = 0;
        for (int i = 0; i < n; i++) total += mods[i].count;
        fn_rev = malloc((total ? total : 1) * sizeof *fn_rev);
        if (!fn_rev) return 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < mods[i].count; k++)
                fn_rev[fn_rev_n++] = (FnRev){(uintptr_t)mods[i].fns[k], mods[i].addrs[k]};
        qsort(fn_rev, fn_rev_n, sizeof *fn_rev, by_host);
    }
    uint64_t lo = 0, hi = fn_rev_n;
    while (lo < hi) {
        uint64_t mid = (lo + hi) / 2;
        if (fn_rev[mid].host <= rip) lo = mid + 1;
        else hi = mid;
    }
    return lo ? fn_rev[lo - 1].guest : 0;
}

static int readable(uint64_t a, uint64_t n) {
    uint8_t probe[16];   // a frame record; plat_mem_read fails on unmapped or PROT_NONE pages instead of faulting
    return n && n <= sizeof probe && plat_mem_read(a, probe, (size_t)n);
}

// The executable's own mapped image (its PT_LOAD segments), found once at start-up: a fault inside it is ours to report,
// one in a driver or system library is left to the default action (a core dump for the system's crash collector).
static uintptr_t image_lo, image_hi;

static int find_image(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size, (void)data;
    for (int i = 0; i < info->dlpi_phnum; i++) {   // the first object listed is the main program
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD) continue;
        uintptr_t lo = info->dlpi_addr + p->p_vaddr, hi = lo + p->p_memsz;
        if (!image_lo || lo < image_lo) image_lo = lo;
        if (hi > image_hi) image_hi = hi;
    }
    return 1;
}

static struct sigaction old_action[NSIG];   // what was installed before crash_install, for faults that are not ours

static void crash_handler(int sig, siginfo_t *si, void *ucv) {
    static volatile int32_t busy;
    uintptr_t rip = (uintptr_t)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP];
    if (si->si_code <= 0 || rip < image_lo || rip >= image_hi) {   // raised by kill/raise, or not our code
        const struct sigaction *o = &old_action[sig];
        if (o->sa_flags & SA_SIGINFO) {   // a handler that was there first (the memory layer's, if it has one)
            o->sa_sigaction(sig, si, ucv);
            return;
        }
        if (o->sa_handler != SIG_DFL && o->sa_handler != SIG_IGN) {
            o->sa_handler(sig);
            return;
        }
        signal(sig, SIG_DFL);   // a hardware fault runs the instruction again and the default action takes it
        if (si->si_code <= 0) raise(sig);   // one sent by kill/raise would not come back by itself
        return;
    }
    if (plat_atomic_xchg(&busy, 1)) _exit(71);
    greg_t *g = ((ucontext_t *)ucv)->uc_mcontext.gregs;
    char d[160];
    const char *name = sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : sig == SIGILL ? "SIGILL" : "SIGFPE";
    rt_logf("FATAL: signal %d (%s) at host %p (exe+%#llx)", sig, name, (void *)rip,
            (unsigned long long)(rip - image_lo));
    if (sig == SIGSEGV || sig == SIGBUS) {
        uint64_t err = (uint64_t)g[REG_ERR];   // x86 page-fault error code: bit 1 write, bit 4 instruction fetch
        rt_logf(", %s address %#llx", err & 16 ? "executing" : err & 2 ? "writing" : "reading",
                (unsigned long long)(uintptr_t)si->si_addr);
    }
    rt_logf(", host rsp %#llx\n", (unsigned long long)g[REG_RSP]);
    uint64_t f = lifted_function(rip);
    if (f) rt_logf("  in lifted function f_%llx = %s\n", (unsigned long long)f, ldr_describe(f, d, sizeof d));
    Cpu *c = kern_cpu();
    if (c) {
        for (int i = 0; i < 31; i++)
            rt_logf("  x%-2d %016llx%s", i, (unsigned long long)c->x[i], i % 4 == 3 ? "\n" : "");
        const char *hn = hostfn_name(c->pc);   // a host function the guest called: its name (the lifted function above is then the nearest symbol only)
        rt_logf("  sp  %016llx\n  pc  %016llx%s%s  lr %s\n", (unsigned long long)c->sp, (unsigned long long)c->pc, hn ? " = " : "",
                hn ? hn : "", ldr_describe(c->x[30], d, sizeof d));
        uint64_t fp = c->x[29];
        for (int n = 0; n < 32 && fp && !(fp & 7) && readable(fp, 16); n++) {
            uint64_t next = LD64(fp), lr = LD64(fp + 8);
            rt_logf("  guest frame %2d: %s\n", n, ldr_describe(lr, d, sizeof d));
            if (next <= fp || next - fp > (64u << 20)) break;
            fp = next;
        }
    } else {
        rt_logf("  not on a guest thread\n");
    }
    fflush(stderr);
    _exit(71);
}

enum { ALT_STACK_SIZE = 64 << 10 };

// Gives the calling thread an alternate signal stack, so the report can run after a stack overflow. Every thread has its
// own; the process-wide handler below is installed with SA_ONSTACK and uses it. Idempotent.
void rt_crash_thread_init(void) {
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE)) return;
    stack_t st = {.ss_sp = malloc(ALT_STACK_SIZE), .ss_size = ALT_STACK_SIZE};
    if (st.ss_sp) sigaltstack(&st, NULL);   // a thread without one overflows into a plain SIGSEGV death
}

// The crash reporter's signals. SIGRTMIN+3 (kernel.c's preemption) is neither blocked nor touched; SA_NODEFER lets a fault
// inside the report reach the busy check in the handler instead of killing the process on the spot.
static void crash_install(void) {
    dl_iterate_phdr(find_image, NULL);
    rt_crash_thread_init();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, &old_action[sigs[i]]);
}

// ---- self-test: layout, dispatch table, host-function registry, without game files
static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static double now_s(void) { return (double)plat_now_ns() / 1e9; }

// The mapping that holds addr in /proc/self/maps: its start and permissions ("---p"); 0 when none does.
static int mapping_of(uint64_t addr, uint64_t *start, char perms[5]) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int found = 0;
    while (f && !found && fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        char p[8];
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, p) == 3 && addr >= lo && addr < hi)
            *start = lo, memcpy(perms, p, 4), perms[4] = 0, found = 1;
    }
    if (f) fclose(f);
    return found;
}

static volatile int host_probe_hits;
static void host_probe(Cpu *c) { host_probe_hits++; c->x[0] = 42; }

static void check_cpu(void) {
    __builtin_cpu_init();
    int ok = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
    printf("cpu: x86-64-v3 (AVX2, FMA, BMI2) %s\n", ok ? "present" : "MISSING");
    CHECK(ok, "the lifted code is built for x86-64-v3");
}

// Layout: ascending, no overlap, below 2^39, and inside the window mem_init reserved.
static void check_layout(void) {
    const RtLayoutEntry *lay = rt_layout;
    int n = rt_layout_count;
    printf("layout: %d modules\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %-8s base %#llx  size %#10llx  end %#llx\n", lay[i].name, (unsigned long long)lay[i].base,
               (unsigned long long)lay[i].size, (unsigned long long)(lay[i].base + lay[i].size));
        CHECK(lay[i].size > 0, "%s has size 0", lay[i].name);
        CHECK(lay[i].base % 0x10000 == 0, "%s base is not 64 KiB aligned", lay[i].name);
        CHECK(lay[i].base + lay[i].size <= RT_KERNEL_BASE, "%s reaches into the kernel region", lay[i].name);
        if (i) {
            CHECK(lay[i].base > lay[i - 1].base, "%s base is not above %s", lay[i].name, lay[i - 1].name);
            CHECK(lay[i - 1].base + lay[i - 1].size <= lay[i].base, "%s overlaps %s", lay[i - 1].name, lay[i].name);
        }
        uint64_t map_start;
        char perms[5];
        int ours = mapping_of(lay[i].base, &map_start, perms) && !strncmp(perms, "---", 3) && map_start <= lay[i].base;
        CHECK(ours, "%s is not inside the reserved module window", lay[i].name);
    }
    const RtGuestLayout *g = mem_layout();
    printf("guest memory: aslr %#llx+%#llx stack %#llx heap %#llx alias %#llx+%#llx kernel %#llx host functions %#llx\n",
           (unsigned long long)g->aslr.base, (unsigned long long)g->aslr.size, (unsigned long long)g->stack.base,
           (unsigned long long)g->heap.base, (unsigned long long)g->alias.base, (unsigned long long)g->alias.size,
           (unsigned long long)g->kernel.base, (unsigned long long)g->hostfn.base);
    CHECK(g->alias.base + g->alias.size <= lay[0].base && g->heap.base + g->heap.size <= g->alias.base,
          "guest regions overlap");
}

// Every function start dispatches to its own function. The word after a start (unless that is a start too), a
// misaligned address and the word below the module dispatch to nothing.
static void check_modules(const RtModule *mods, int n) {
    uint64_t total = 0;
    for (int i = 0; i < n; i++) {
        const RtModule *m = &mods[i];
        const RtLayoutEntry *l = NULL;
        for (int k = 0; k < rt_layout_count; k++)
            if (!strcmp(rt_layout[k].name, m->name)) l = &rt_layout[k];
        CHECK(l && l->base == m->base && l->size == m->size, "%s: linked base/size differ from the layout", m->name);

        uint64_t bad = 0, bad_neg = 0;
        for (uint64_t k = 0; k < m->count; k++) {
            uint64_t a = m->addrs[k];
            if (rt_dispatch_lookup(a) != m->fns[k] && bad++ < 5)
                printf("  FAIL: %s_fn_addrs[%llu] %#llx does not dispatch to its function\n", m->name,
                       (unsigned long long)k, (unsigned long long)a);
            int next_is_start = k + 1 < m->count && m->addrs[k + 1] == a + 4;
            if ((!next_is_start && rt_dispatch_lookup(a + 4)) || rt_dispatch_lookup(a + 2)) bad_neg++;
        }
        if (rt_dispatch_lookup(m->base - 4)) bad_neg++;
        failures += bad > 0;
        CHECK(!bad_neg, "%s: %llu addresses that are not function starts dispatch to a function", m->name,
              (unsigned long long)bad_neg);
        uint64_t lo = m->count ? m->addrs[0] : 0, hi = m->count ? m->addrs[m->count - 1] : 0;
        printf("  %-8s %8llu functions, first %#llx, last %#llx: %s\n", m->name, (unsigned long long)m->count,
               (unsigned long long)lo, (unsigned long long)hi, bad || bad_neg ? "FAIL" : "ok");
        total += m->count;
    }
    printf("  total    %8llu functions\n", (unsigned long long)total);
}

static void bench(const RtModule *mods, int n) {
    uint64_t total = 0, sink = 0;
    double t0 = now_s();
    for (int i = 0; i < n; i++) {
        for (uint64_t k = 0; k < mods[i].count; k++) sink += (uintptr_t)rt_dispatch_lookup(mods[i].addrs[k]);
        total += mods[i].count;
    }
    double dt = now_s() - t0;
    __asm__ volatile("" ::"r"(sink));
    if (total) printf("lookup: %.1f ns per function start (address order)\n", dt * 1e9 / (double)total);
}

// Memory SVCs without modules: heap, physical memory in the alias region, a stack move, QueryMemory, GetInfo.
static void check_memory(void) {
    const RtGuestLayout *g = mem_layout();
    RtMemoryInfo q;
    uint64_t heap = 0, v = 0;
    int before = failures;
    CHECK(mem_set_heap_size(0x400000, &heap) == 0 && heap == g->heap.base, "SetHeapSize 4 MiB");
    memset((void *)(uintptr_t)heap, 0xAB, 0x400000);
    mem_query(heap + 0x1000, &q);
    CHECK(q.base == heap && q.size == 0x400000 && q.state == 5 && q.perm == 3, "QueryMemory on the heap");
    CHECK(mem_set_heap_size(0x300000, &heap) != 0, "SetHeapSize accepts a size that is not a 2 MiB multiple");

    uint64_t a = g->alias.base + 0x100000;
    CHECK(mem_map_physical(a, 0x10000) == 0, "MapPhysicalMemory");
    ST64(a + 0x8000, 0x1234);
    CHECK(mem_map_physical(a - 0x4000, 0x20000) == 0 && LD64(a + 0x8000) == 0x1234,
          "MapPhysicalMemory over a mapped range lost its contents");
    mem_query(a, &q);
    CHECK(q.base == a - 0x4000 && q.size == 0x20000 && q.state == 5, "QueryMemory on mapped alias pages");
    mem_info(7, &v);
    CHECK(v == 0x400000 + 0x20000, "GetInfo 7 (used memory) %#llx", (unsigned long long)v);

    uint64_t stack = g->stack.base + 0x100000;
    ST64(a + 8, 0x99);
    CHECK(mem_map(stack, a, 0x4000) == 0 && LD64(stack + 8) == 0x99, "MapMemory alias -> stack did not copy");
    ST64(stack + 0x10, 0x5678);
    mem_query(stack, &q);
    CHECK(q.base == stack && q.size == 0x4000 && q.state == 0xB, "QueryMemory on a moved stack");
    mem_query(a, &q);
    CHECK(q.base == a && q.state == 5 && q.perm == 0, "QueryMemory on a moved stack's source");
    CHECK(mem_unmap_physical(a, 0x4000) != 0, "UnmapPhysicalMemory under a moved stack");
    CHECK(mem_unmap(stack, a, 0x4000) == 0 && LD64(a + 0x10) == 0x5678, "UnmapMemory did not copy the stack back");

    CHECK(mem_unmap_physical(a - 0x4000, 0x20000) == 0, "UnmapPhysicalMemory");
    mem_query(a, &q);
    CHECK(q.state == 0 && q.base == g->alias.base && q.size == g->alias.size, "QueryMemory on the free alias region");
    CHECK(mem_map_physical(a, 0x1000) == 0 && LD64(a + 0x10) == 0 && mem_unmap_physical(a, 0x1000) == 0,
          "remapped physical memory is not zeroed");
    CHECK(mem_map_physical(a, 0x100000000ull) == 0x10801, "MapPhysicalMemory beyond the pool is not LimitReached");
    mem_query(RT_HOSTFN_BASE + 0x40, &q);
    CHECK(q.state == 0x10 && q.base == RT_HOSTFN_BASE, "QueryMemory on the host-function window");
    mem_query(g->alias.base + g->alias.size + 0x1000, &q);
    // Country-Box: host memory is Inaccessible (0x10) with no access, no longer Code (3): memory.c mem_query, f9e98f8
    // (the SDK's exception unwinder walked Code RX regions out of the modules into it)
    CHECK(q.state == 0x10 && q.perm == 0 && q.base == g->alias.base + g->alias.size,
          "QueryMemory on host memory: state %#x perm %u base %#llx", q.state, q.perm, (unsigned long long)q.base);
    CHECK(mem_set_heap_size(0, &heap) == 0, "SetHeapSize 0");
    printf("memory: heap, physical memory, stack move, QueryMemory, GetInfo: %s\n", failures > before ? "FAIL" : "ok");
}

static int selftest(void) {
    static RtModule mods[MAX_MODULES];
    int n = rt_linked_modules(mods, MAX_MODULES);

    check_cpu();
    check_layout();
    uint64_t probe = hostfn_register("<selftest probe>", host_probe);

    double t0 = now_s();
    hostfn_seal();
    double dt = now_s() - t0;
    RtDispatchStats s = rt_dispatch_stats();
    printf("dispatch: %d modules linked, table covers %#llx-%#llx: %llu slots, %.1f MiB reserved, built in %.0f ms\n",
           n, (unsigned long long)s.lo, (unsigned long long)s.hi, (unsigned long long)s.slots,
           (double)s.table_bytes / (1 << 20), dt * 1e3);
    check_modules(mods, n);
    CHECK(s.functions > 0, "no functions linked");
    Cpu *c = aligned_alloc(64, ((sizeof(Cpu)) + (64) - 1) & ~(size_t)((64) - 1));
    memset(c, 0, sizeof *c);
    uint64_t r = rt_call(c, probe, 1, (uint64_t)7);
    CHECK(rt_dispatch_lookup(probe) && r == 42 && host_probe_hits == 1 && c->x[30] == 0,
          "a registered host function does not dispatch through rt_call");
    CHECK(!rt_dispatch_lookup(probe + 16), "an unregistered host-function address dispatches");
    CHECK(rt_dispatch_lookup(RT_RET_HOST), "the rt_call return sentinel is not registered");
    free(c);
    bench(mods, n);

    void *k1 = mem_kernel_alloc(0x200, 16), *k2 = mem_kernel_alloc(0x2000, 0x1000);
    CHECK((uintptr_t)k1 - RT_KERNEL_BASE < RT_KERNEL_SIZE && !((uintptr_t)k2 & 0xFFF) && k1 != k2,
          "kernel-region allocation");
    mem_kernel_free(k1, 0x200);
    CHECK(mem_kernel_alloc(0x200, 16) == k1, "a freed kernel-region block is not reused");
    check_memory();

    FILE *status = fopen("/proc/self/status", "r");   // VmHWM: the peak resident set, kB
    char line[256];
    while (status && fgets(line, sizeof line, status)) {
        unsigned long long kb;
        if (sscanf(line, "VmHWM: %llu kB", &kb) == 1) printf("process: peak working set %.1f MiB\n", (double)kb / 1024);
    }
    if (status) fclose(status);
    printf("selftest: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}

static int usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--exefs DIR] [--romfs DIR] [--profile DIR] [--selftest | --probe-loader | "
                    "--probe-cfg | --probe-kernel | --probe-ipc | --probe-services | --probe-fs | --probe-gfx-abi | "
                    "--probe-nvn | --probe-nvn-tex | --probe-window | --probe-shader | --probe-vkmem | "
                    "--probe-vkexec | --probe-astc | --bench-astc | --probe-movie | --prepare-shaders | --shader-status | --warm-shaders | --boot |--seconds S [--shot PNG]]\n"
                    "       %s [--settings-lang en|ru] --settings-dump | --settings-set key=value ...\n", argv0, argv0);
    return 2;
}

// Paths for a copied folder (tools/package_build.py). Per path: MP4_EXEFS, MP4_ROMFS, MP4_PROFILE_DIR, else the
// folder next to the executable (exefs, romfs, profile) if it exists, else the path baked in at build time. A relative
// value is relative to the executable's folder.
static char pp_exefs[PATH_MAX * 2], pp_romfs[PATH_MAX * 2], pp_profile[PATH_MAX * 2];

static const char *portable_path(const char *dir, const char *var, const char *sub, const char *must, char *out, size_t cap,
                                 const char *fallback) {
    const char *e = cfg_get(var);
    if (e && *e) {
        if (e[0] == '/') snprintf(out, cap, "%s", e);
        else snprintf(out, cap, "%s/%s", dir, e);
        return out;
    }
    char probe[PATH_MAX * 3];
    snprintf(out, cap, "%s/%s", dir, sub);
    snprintf(probe, sizeof probe, "%s/%s", out, must);
    return access(probe, F_OK) == 0 ? out : fallback;
}

static void portable_paths(RtConfig *c) {
    char exe[PATH_MAX * 2];
    char *cut = plat_exe_path(exe, sizeof exe) ? strrchr(exe, '/') : NULL;
    if (!cut) return;
    *cut = 0;
    c->exefs = portable_path(exe, "MP4_EXEFS", "exefs", "main.npdm", pp_exefs, sizeof pp_exefs, c->exefs);
    c->romfs = portable_path(exe, "MP4_ROMFS", "romfs", ".", pp_romfs, sizeof pp_romfs, c->romfs);   // Country-Box: an extracted RomFS folder (romfs.c ROMFS_DIRECTORY) or one holding romfs.bin
    c->profile = portable_path(exe, "MP4_PROFILE_DIR", "profile", ".", pp_profile, sizeof pp_profile, c->profile);
}

int main(int argc, char **argv) {
    // Country-Box: the launcher's settings commands (settings.c) touch only the settings file: no guest memory, no log
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--settings-dump") || !strcmp(argv[i], "--settings-set")) return settings_cli(argc, argv);
    mem_init();   // first: claim the guest address space before anything else can land there; reads mp4.ini
    // Country-Box: CB_ALLOW_PTRACE=1 lets any process of the user attach (eu-stack, gdb -p) under Yama's
    // ptrace_scope 1, for sampling where the game's threads run (lifted functions are named by guest address).
    const char *pt = getenv("CB_ALLOW_PTRACE");
    if (pt && !strcmp(pt, "1")) prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    crash_install();
    rt_parse_trace();
    prof_start();
    cfg = (RtConfig){.exefs = rt_default_exefs, .romfs = rt_default_romfs, .profile = rt_default_profile,
                     .gpu = cfg_get("MP4_GPU")};
    portable_paths(&cfg);   // command-line paths below still win
    if (cfg.gpu && strcmp(cfg.gpu, "null") && strcmp(cfg.gpu, "vulkan")) {
        rt_logf("MP4_GPU must be null or vulkan, not '%s'\n", cfg.gpu);
        return 2;
    }
    const char *mode = NULL;
    for (int i = 1; i < argc; i++) {
        const char **path = !strcmp(argv[i], "--exefs") ? &cfg.exefs : !strcmp(argv[i], "--romfs") ? &cfg.romfs
                          : !strcmp(argv[i], "--profile") ? &cfg.profile : NULL;
        if (path) {
            if (++i == argc) return usage(argv[0]);
            *path = argv[i];
        } else if (!strcmp(argv[i], "--seconds")) {
            char *end;
            if (++i == argc) return usage(argv[0]);
            double s = strtod(argv[i], &end);
            if (*end || !(s > 0) || s > 1e6) return usage(argv[0]);
            cfg.seconds = s;
        } else if (!strcmp(argv[i], "--shot")) {
            if (++i == argc) return usage(argv[0]);
            cfg.shot = argv[i];
        } else if (!mode && !strncmp(argv[i], "--", 2)) {
            mode = argv[i];
        } else {
            return usage(argv[0]);
        }
    }
    if (cfg.shot && !cfg.seconds) return usage(argv[0]);
    if (cfg.seconds && mode) return usage(argv[0]);
    rt_log_open(!mode);
    if (!mode) {
        cfg.boot = cfg.seconds > 0;   // --seconds starts the game like --boot, with its checkpoints
        rt_boot();
        return 0;
    }
    if (!strcmp(mode, "--selftest")) return selftest();
    if (!strcmp(mode, "--probe-loader")) return probe_loader();
    if (!strcmp(mode, "--probe-cfg")) return probe_cfg();
    if (!strcmp(mode, "--probe-kernel")) return probe_kernel();
    if (!strcmp(mode, "--probe-ipc")) return probe_ipc();
    if (!strcmp(mode, "--probe-services")) return probe_services();
    if (!strcmp(mode, "--probe-fs")) return probe_fs();
    if (!strcmp(mode, "--probe-gfx-abi")) return probe_gfx_abi();
    if (!strcmp(mode, "--probe-nvn")) return probe_nvn();
    if (!strcmp(mode, "--probe-nvn-tex")) return probe_nvn_tex();
    if (!strcmp(mode, "--probe-window")) return probe_window();
    if (!strcmp(mode, "--probe-shader")) return probe_shader();
    if (!strcmp(mode, "--probe-vkmem")) return probe_vkmem();
    if (!strcmp(mode, "--probe-vkexec")) return probe_vkexec();
    if (!strcmp(mode, "--probe-astc")) return probe_astc();
    if (!strcmp(mode, "--bench-astc")) return bench_astc();
    if (!strcmp(mode, "--probe-vkdevice")) return probe_vkdevice();
    if (!strcmp(mode, "--probe-movie")) return probe_movie();
    if (!strcmp(mode, "--prepare-shaders")) return shader_prep_cli();   // Country-Box: shader_prep.c
    if (!strcmp(mode, "--shader-status")) return shader_prep_status_cli();   // Country-Box: for the launcher
    if (!strcmp(mode, "--warm-shaders")) return vk_pipeline_warm_shaders();
    if (!strcmp(mode, "--boot")) { cfg.boot = 1; rt_boot(); return 0; }
    return usage(argv[0]);
}
