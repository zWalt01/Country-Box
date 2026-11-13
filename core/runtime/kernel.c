// From fe3h-pc-recomp runtime/kernel.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4; MP4's main thread stack size.
// From totk-pc-recomp runtime/kernel.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), renamed to fe3h; FE3H's main thread stack size; FE3H_THREAD_DUMP.
// Adapted from MacNativeGames games/totk/runtime/threads.c @ b75be8c (MIT, Copyright (c) 2026 MacNativeGames
// contributors; itself from d2r-mobile dm/host/hle_thread.c), d2r-mobile dm/host/thread.c @ 53e7c5b (MIT, Copyright
// (c) 2026 jhaizhou) and LAS Recomp runtime/kernel.c @ da35b83 (MIT, Copyright (c) 2026 Peiikai); the preemption of
// virtual cores from MacNativeGames threads.c @ 33e8fa8 (MIT, as above).
// Threads, synchronization, virtual cores and fibers of the Horizon kernel.
//
// One guest thread = one Windows thread with a 256 MiB reserved host stack (every guest BL is a host call) and its own
// Cpu. The SDK's nn::os runs as lifted code; only the kernel under it is the host. Every wait hangs on one global lock
// G with one condition variable per thread; wake order is the kernel's: lower priority value first, then first come.
// Semantics follow the public Horizon kernel (KConditionVariable, KAddressArbiter, KSynchronization).
// Country-Box: Linux only. Guest threads are pthreads with a 256 MiB lazily committed stack; waits are futex-based
// plat_cond_wait_ns (nanosecond deadlines, where Windows rounded every wait up to a millisecond); guest fibers switch
// with an own x86-64 context switch (kern_ctx_switch below: callee-saved registers, MXCSR and the x87 control word, no
// system call, unlike swapcontext); the asynchronous preemption of virtual cores interrupts the owner with a real-time
// signal whose handler redirects the interrupted rip, in place of SuspendThread / Get/SetThreadContext. kern_ap_stub is
// rewritten for the System V ABI. The lifted code is built with -mno-red-zone, so the return slot kern_ap_stub keeps
// just below the interrupted rsp sits in the 128 bytes the kernel never puts a signal frame into, and nothing the
// interrupted function keeps there is overwritten.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "kernel.h"
#include "platform/platform.h"
#include "savestate.h"

#undef R_OK   // unistd.h access() mode; here R_OK is the Horizon result 0
#define R_OK 0u
#define R_TIMEOUT KERN_R_TIMEOUT
#define R_CANCELLED KERN_R_CANCELLED
#define R_BAD_HANDLE KERN_R_BAD_HANDLE
#define R_BAD_STATE KERN_R_BAD_STATE
#define R_INVALID_ENUM KERN_R_INVALID_ENUM
#define WAITERS_BIT KERN_WAITERS_BIT
#define H_CUR_THREAD KERN_H_CURRENT_THREAD

#define MAX_H 4096
#define NCORE 3
#define HOST_STACK (256ull << 20)        // reserved, committed on demand
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400            // "current thread object" at TLR+0x1F8 (d2r-mobile thread.c)
#define TLS_SIZE 0x2000
#define MAIN_STACK 0x100000ull           // main.npdm META+0x1C (MP4 1.0.0 and 1.1.0)
#define MAIN_PRIO 44                     // META+0xE
#define MAIN_CORE 0                      // META+0xF
#define FIBER_MAX 1024

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_SESSION = KERN_SESSION, O_TMEM = KERN_TMEM };
_Static_assert(O_EVENT_W < O_SESSION, "handle kinds");
enum { W_NONE = KERN_WAIT_NONE, W_LOCK = KERN_WAIT_LOCK, W_CONDVAR = KERN_WAIT_CONDVAR, W_ADDRESS = KERN_WAIT_ADDRESS,
       W_SYNC = KERN_WAIT_SYNC };

// Scheduler accounting states (MP4_TRACE=sched).
enum { S_OFF, S_RUN, S_CORE, S_HOST, S_SLEEP, S_SYNC, S_CV, S_LOCK, S_ADDR, S_N };
static const char *const s_name[S_N] = {"off", "run", "core", "host", "sleep", "sync", "cv", "lock", "addr"};

typedef struct Event { int signaled, refs; } Event;
typedef struct Fiber Fiber;
void kern_ctx_switch(void **save, void *load);   // fibers, below
static void **fiber_host_slot(Fiber *f);

struct RtThread {
    int preempt;               // first member: rt_dispatch reads it (host.h kern_preempt_pending)
    int host_depth;            // second member: host calls and SVCs in progress, while kern_host_track (host.h)
    uint32_t handle;
    uint64_t id;
    Cpu *cpu;                  // the thread's own context
    Cpu *cur;                  // the context running now: cpu, or the current fiber's
    uint64_t entry, arg, stack_top;
    int prio, core;
    uint64_t mask;
    int started, exited, closed, cancel, is_main;
    int susp_req, parked, in_svc;   // Country-Box: SetThreadActivity pause; parked at a safe point; inside an SVC
    PlatCond cv;
    // current wait
    int wkind, woke;
    uint64_t waddr, wkey;
    uint32_t wtag, wres;
    uint64_t wseq;             // registration order: equal priority wakes first come first
    RtThread *next;            // wait list
    RtThread *all;             // every thread
    // virtual core
    int cur_core;              // core held while running guest code, -1 while blocked
    uint64_t cseq;
    RtThread *cnext;
    uint8_t *tlr, *tls;
    jmp_buf exit_jmp;          // the thread start; ExitThread longjmps to it
    // fibers
    void *base_fiber;          // the saved stack pointer of the thread's own context while a guest fiber runs
    Fiber *fiber;              // current guest fiber, NULL on the thread's own context
    Fiber *leaving;            // fiber just switched away from, released by whoever resumes next
    int exit_pending;          // ExitThread on a fiber: unwind once back on the thread's own context
    // MP4_TRACE=sched accounting (holding G)
    uint32_t tid;              // host thread id (gettid)
    int st;                    // S_* state since st_since
    uint64_t st_since, st_ns[S_N], nblk, npre, pre_since, cpu_last;
    uint64_t wobj;             // what the current wait is on: owner handle, key, address or first handle
    uint32_t waker;            // the thread whose signal ended the last wait (0: a host thread or a timeout)
    uint64_t wake_at, grant_at;   // when a wait was signalled and a core granted, until the thread runs again
    uint64_t lw_ns, lw_n, lw_max, lg_ns, lg_n, lg_max, sl_n, sl_req, sl_act;   // those latencies; SleepThread asked/took
    uint64_t sp_n, sp_ho, sp_site;   // spin wait passes and core handoffs, the last loop head (the thread writes them)
    RtThread *target;          // as a core waiter: the owner core_kick asked to give its core up for it (holding G)
    // MP4_ASYNC_PREEMPT (holding G)
    pthread_t pth;             // the host thread, for the preemption signal (has_pth); set while it may own a core
    int has_pth;
    uint64_t ap_rip;           // the interrupted host rip kern_ap_stub returns to
    uint64_t ap_last;          // the last redirect attempt on this owner
    int ap_pending;            // redirected to kern_ap_stub, which has not reached kern_ap_yield yet
    // MP4_CORE_DIAG (holding G, except dg_host_end, which only the thread itself writes)
    uint64_t dg_since;         // as a waiter: core wait start
    int dg_cls;                // as a waiter: DG_* class at the start of the wait
    uint64_t dg_kick;          // as an owner: the preempt request
    uint64_t dg_host_end;      // as an owner: the return from the host call or SVC it was in at the request
    uint64_t dg_pc, dg_lr;     // as an owner: guest pc and lr at the checkpoint that gave the core up
    int dg_host_kick;          // as an owner: in a host call or SVC at the request
    uint32_t dg_svc;           // the thread's latest SVC (MP4_CORE_DIAG: what an owner in a host call is in)
    struct DgRel { uint64_t kick, host_end, pc, lr; int host, pre, prio; uint32_t h; } dg_rel;   // as a waiter: the
                                                                                                 // owner that released
    // Country-Box: save states (ss_poll below). These fields survive a state restore (ss_keep_fields).
    int ss_at;                 // waiting in ss_poll while a freeze is on
    int ss_cmd, ss_done;       // SS_CAPTURE / SS_RESTORE asked of the thread, and done
    uint64_t ss_hi;            // top of the host stack a state copies: thread_main's / guest_main's frame
    void *ss_sp;               // the thread's saved context (kern_ctx_switch) while its helper copies the stack
    void *ss_alt_sp;           // the helper context (ss_alt_main) on its own small stack
    uint8_t *ss_alt;
    uint8_t *ss_buf;           // SS_CAPTURE: the stack bytes from ss_sp to ss_hi
    size_t ss_n;
    void *ss_load_sp;          // SS_RESTORE: the context and stack bytes to go back to
    const uint8_t *ss_load_buf;
    size_t ss_load_n;
};

static PlatMutex G = PLAT_MUTEX_INIT;
static struct { int type; void *obj; } g_h[MAX_H];
static _Thread_local RtThread *tcur;
_Static_assert(offsetof(RtThread, preempt) == 0, "host.h kern_preempt_pending reads the first int");
_Static_assert(offsetof(RtThread, host_depth) == sizeof(int), "host.h kern_host_depth reads the second int");
static RtThread *g_all, *g_wait, *g_main;
static uint64_t g_next_id = 0x51, g_wseq, g_created;
static uint64_t g_main_stack;

static void lock(void) { plat_mutex_lock(&G); }
static void unlock(void) { plat_mutex_unlock(&G); }

// Country-Box: save states (the "save states" section at the end). Every place a guest thread waits under G calls
// ss_poll, which holds the thread there while a freeze is on.
static volatile int g_ss_phase;   // 0 off; else a freeze is on (holding G to write)
static int g_ss_keep;             // a state was captured: handle objects are not freed (they may come back)
static void ss_poll(RtThread *t);
static int ss_defer_close(int kind, void *obj, uint32_t h);
static int ss_on(void) { return __atomic_load_n(&g_ss_phase, __ATOMIC_ACQUIRE) != 0; }

uint64_t kern_now_ns(void) { return plat_now_ns(); }

// ---- MP4_CRIT=<first>-<last frame> with MP4_TRACE=sched: every state change and wake of the guest threads and
// every present over those frames, written to MP4_CRIT_OUT (default crit.bin) at the last one, for
// tools/crit_path.py, which walks the chain of waits back from each present (holding G). MP4_CRIT=w<lo>-<hi> takes
// world-clock seconds instead (frame_step_world_us): from the first present at or after lo to the first after hi, so a
// session covers the same game time at any frame rate.
typedef struct { uint64_t ts, obj; uint32_t a, b; uint16_t kind, st; uint32_t pad; } CritEv;
enum { CRIT_STATE, CRIT_WAKE, CRIT_FRAME };   // a: thread; b: its waker (STATE), the waker (WAKE); st: new S_* state
#define CRIT_N (1u << 22)                // 128 MiB, about 2000 fight frames
static CritEv *g_crit;
static uint32_t g_crit_n;
static int g_crit_on;
static uint64_t g_crit_lo, g_crit_hi;   // frames; UINT64_MAX in world mode until the world clock fixes them
static int64_t g_crit_wlo, g_crit_whi;  // world mode: the window in microseconds of world clock, else 0

static void crit_add(int kind, uint32_t a, uint32_t b, int st, uint64_t obj, uint64_t ts) {
    if (g_crit_n < CRIT_N) g_crit[g_crit_n++] = (CritEv){ts, obj, a, b, (uint16_t)kind, (uint16_t)st, 0};
}

// ---- MP4_TRACE=sched accounting (holding G)
#define RW_N 32
static RtThread *g_render;                               // the thread that presents (sched trace, core diag)
typedef struct { int st; uint32_t waker; uint64_t obj, ns; } WaitRec;
static WaitRec g_rw[RW_N], g_mw[RW_N];                   // the render and main thread's waits by object and waker
static uint64_t g_pre_req, g_pre_n, g_pre_ns, g_pre_max, g_pre_stale;  // preempt requests; releases after one and their latency
static uint64_t g_sync_calls, g_sync_blocks;              // WaitSynchronization calls and the blocks inside them

static void wait_add(WaitRec *w, int st, uint64_t obj, uint32_t waker, uint64_t ns) {
    for (int i = 0; i < RW_N; i++)
        if (!w[i].ns || (w[i].st == st && w[i].obj == obj && w[i].waker == waker)) {
            w[i].st = st;
            w[i].obj = obj;
            w[i].waker = waker;
            w[i].ns += ns;
            return;
        }
}

// Close the current state's interval and enter st.
static void sched_acct(RtThread *t, int st) {
    if (!(rt_trace & RT_TRACE_SCHED)) return;
    uint64_t now = kern_now_ns();
    if (t->st_since) {
        t->st_ns[t->st] += now - t->st_since;
        if (t->st >= S_HOST && (t == g_render || t->is_main)) {
            uint32_t waker = t->st == S_HOST || t->st == S_SLEEP ? 0 : t->waker;
            wait_add(t == g_render ? g_rw : g_mw, t->st, t->wobj, waker, now - t->st_since);
        }
    }
    if (g_crit_on) crit_add(CRIT_STATE, t->handle, t->waker, st, t->wobj, now);
    t->st = st;
    t->st_since = now;
}

static int st_of(int kind) {
    return kind == W_LOCK ? S_LOCK : kind == W_CONDVAR ? S_CV : kind == W_ADDRESS ? S_ADDR : S_SYNC;
}

// ---- handles (caller holds G)
static uint32_t h_new(int type, void *obj) {
    for (int i = 1; i < MAX_H; i++)
        if (!g_h[i].type) {
            g_h[i].type = type;
            g_h[i].obj = obj;
            return 0x1000 + i;
        }
    rt_fatal("kernel: handle table full (%d handles)", MAX_H);
}

static void *h_peek(uint32_t h, int type) {
    if (h == H_CUR_THREAD && type == O_THREAD) return tcur;
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}

static void *h_get(uint32_t h, int type) {
    void *o = h_peek(h, type);
    if (!o) rt_log("kernel: invalid handle %#x (want kind %d)", h, type);
    return o;
}

static RtThread *cur(uint32_t svc) {
    if (!tcur) rt_fatal("kernel: svc %#x on a host thread (no guest thread)", svc);
    return tcur;
}

// ---- wait primitives (holding G)
static void wait_unlink(RtThread *t) {
    for (RtThread **p = &g_wait; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    t->next = NULL;
}

// The waiter to wake in kernel order. Lock waits match by lock address, condvar waits by key, arbiter waits by
// address.
static RtThread *pick(int kind, uint64_t a) {
    RtThread *b = NULL;
    for (RtThread *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == W_CONDVAR ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}

static void wake(RtThread *t, uint32_t res) {
    wait_unlink(t);
    t->wkind = W_NONE;
    t->woke = 1;
    t->wres = res;
    t->waker = tcur ? tcur->handle : 0;
    if (rt_trace & RT_TRACE_SCHED) t->wake_at = kern_now_ns();
    if (g_crit_on) crit_add(CRIT_WAKE, t->handle, t->waker, 0, 0, t->wake_at);
    plat_cond_signal(&t->cv);
}

static void wake_sync_waiters(void) {
    for (RtThread *w = g_wait; w;) {
        RtThread *n = w->next;
        if (w->wkind == W_SYNC) wake(w, (uint32_t)-1);
        w = n;
    }
}

// The wait may end early (a spurious wakeup): the deadline is re-checked against the monotonic clock.
static uint32_t block_inner(RtThread *t, int kind, int64_t ns) {
    t->wkind = kind;
    t->woke = 0;
    t->wseq = ++g_wseq;
    t->next = g_wait;
    g_wait = t;
    if (ns == 0) {
        wait_unlink(t);
        t->wkind = W_NONE;
        return R_TIMEOUT;
    }
    uint64_t now = kern_now_ns();
    int forever = ns < 0 || (uint64_t)ns > UINT64_MAX - now;
    uint64_t deadline = forever ? 0 : now + (uint64_t)ns;
    while (!t->woke) {
        if (ss_on()) {   // Country-Box: a save state freezes the thread here (ss_poll)
            ss_poll(t);
            continue;
        }
        if (forever) {
            plat_cond_wait(&t->cv, &G);
            continue;
        }
        now = kern_now_ns();
        if (now >= deadline) {
            wait_unlink(t);
            t->wkind = W_NONE;
            return R_TIMEOUT;
        }
        plat_cond_wait_ns(&t->cv, &G, deadline - now);
    }
    if (t->wake_at) {
        uint64_t d = kern_now_ns() - t->wake_at;
        t->lw_ns += d;
        t->lw_n++;
        if (d > t->lw_max) t->lw_max = d;
        t->wake_at = 0;
    }
    return t->wres;
}

// ---- virtual cores
// Horizon runs at most one thread per core, and TotK keys per-core data (allocators, job queues) by the current core
// number without locks. A guest thread therefore holds one of the three application cores while it runs guest code
// and gives it up only where Horizon would switch threads (waits, sleep, yield, exit, blocking host calls); a free
// core goes to the best waiting thread. A waiter better than a holder asks one holder to give its core up (core_kick),
// which it does at its next SVC or indirect call (kern_checkpoint, rt_dispatch), as Horizon runs the better thread at
// once; guest code without either keeps the core until it reaches one. MP4_FREE_CORES=1 turns the gate off.
static RtThread *g_core_owner[NCORE], *g_core_wait;
static int g_cores_on = -1, g_held_max;

static void diag_init(void);
static void ap_init(void);

static int cores_enabled(void) {
    if (g_cores_on < 0) {
        g_cores_on = !cfg_bool("MP4_FREE_CORES");   // "0" keeps the gate, like unset
        diag_init();
        ap_init();
    }
    return g_cores_on;
}

static int core_allowed(const RtThread *t, int c) { return (int)((t->mask >> c) & 1); }

// ---- asynchronous preemption (MP4_ASYNC_PREEMPT, on unless 0)
// Adapted from MacNativeGames games/totk/runtime/threads.c @ 33e8fa8 (8bf0893, PR #488; MIT, Copyright (c) 2026
// MacNativeGames contributors): Horizon gives a core to a strictly better thread the moment that thread is ready.
// Here a better waiter only marks the owner, which gives the core up at its next SVC or indirect call; an owner in a
// stretch of game code with neither keeps it (MP4_CORE_DIAG: 0.5-1.1 ms of thread-time per frame, waits to 16 ms and
// more). With it on, three parts, all of which MacNativeGames found necessary:
// - The waiter interrupts the owner (a signal to its host thread, ir_send) when it has waited AP_FIRST_NS, then every
//   millisecond: if the owner's host rip is in the lifted modules' code (g_gen_lo..g_gen_hi) and its host depth is 0
//   (not in a host call or SVC, nor in game code a host call made), it holds no host lock, so its rip is moved to
//   kern_ap_stub, which saves every volatile register, gives the core up like a checkpoint (kern_ap_yield) and jumps
//   back. Otherwise it is only marked, as before.
// - Priority inheritance (eff_prio): a thread owning a guest mutex counts at the priority of the best thread blocked on
//   it, for granting, kicking, preempting and yielding. Without it MacNativeGames' game stopped at its first
//   "Continue" (a better thread took the core of the low-priority owner of a mutex it then waited for).
// - Horizon's preemption disable: the SDK's spin locks (1.2.1 sdk f_70051242a8, f_7005124360 and siblings) count
//   TLR+0x100 (disable_count) while they try or hold the lock, and after clearing it call SVC 0x36 when TLR+0x102
//   (interrupt_flag) is set. Such an owner is not preempted, at a checkpoint either: interrupt_flag is set and the
//   switch happens at that SVC. Preempting it anyway hung MacNativeGames' game at "Continue" (the lock holder lost its
//   core to a thread that then spun on that lock).
// MP4_ASYNC_PREEMPT=0 keeps the old scheduling exactly: raw priorities, no interruption, no disable_count; =2 keeps
// inheritance and the preemption disable without interrupting owners.
#define AP_FIRST_NS 250000ull   // the waiter spins this long before the first interruption (most waits end sooner)
#define AP_GAP_NS 1000000ull    // and interrupts the same owner at most this often
static int g_ap = -1, g_ap_int;   // g_ap_int: interrupt owners (MP4_ASYNC_PREEMPT=2: inheritance and disable only)
extern const char __executable_start[];   // the linker's: where the executable is loaded (PIE included)
static uintptr_t exe_base(void) { return (uintptr_t)__executable_start; }
static int g_ap_log;              // redirects logged so far (AP_LOG_N)
#define AP_LOG_N 40
static uintptr_t g_gen_lo, g_gen_hi;   // host code of the lifted modules: first and last function start
static uint64_t g_ap_sent, g_ap_taken, g_ap_host, g_ap_pc, g_ap_deferred, g_ap_fail, g_ap_spin_ok;
void kern_ap_stub(void);

static void ap_init(void) {
    if (g_ap >= 0) return;
    const char *e = cfg_value("MP4_ASYNC_PREEMPT");
    g_ap = strcmp(e, "0") != 0;
    g_ap_int = g_ap && strcmp(e, "2") != 0;
    if (g_ap) {
        RtModule m[16];
        int n = rt_linked_modules(m, 16);
        uintptr_t lo = UINTPTR_MAX, hi = 0;
        for (int i = 0; i < n; i++)
            for (uint64_t k = 0; k < m[i].count; k++) {
                uintptr_t f = (uintptr_t)m[i].fns[k];
                if (f < lo) lo = f;
                if (f > hi) hi = f;
            }
        const uintptr_t host[] = {(uintptr_t)kern_ap_stub, (uintptr_t)kern_checkpoint, (uintptr_t)rt_svc,
                                  (uintptr_t)rt_dispatch, (uintptr_t)rt_call};
        int bad = !n || lo >= hi;
        for (unsigned i = 0; i < sizeof host / sizeof *host; i++) bad |= host[i] >= lo && host[i] <= hi;
        if (bad) {
            rt_log("kernel: asynchronous preemption off: the lifted code range %#llx-%#llx holds host code (the lifted "
                   "objects must be linked together)",
                   (unsigned long long)lo, (unsigned long long)hi);
            g_ap = 0;
        } else {
            g_gen_lo = lo;
            g_gen_hi = hi;
            kern_host_track = 1;
        }
    }
    rt_log("kernel: asynchronous preemption of virtual cores %s, priority inheritance and preemption disable %s; lifted "
           "code exe+%#llx-exe+%#llx", g_ap_int ? "on" : "off", g_ap ? "on (MP4_ASYNC_PREEMPT=0 turns all off)" : "off",
           (unsigned long long)(g_gen_lo ? g_gen_lo - exe_base() : 0),
           (unsigned long long)(g_gen_hi ? g_gen_hi - exe_base() : 0));
}

// Horizon's priority inheritance: o counts at the priority of the best thread blocked on a guest mutex it owns (the
// owner is read from the mutex word, so it follows hand-offs). Holding G.
static int eff_prio(const RtThread *o) {
    int p = o->prio;
    for (const RtThread *w = g_wait; w; w = w->next)
        if (w->wkind == W_LOCK && w->prio < p && w->waddr &&
            (__atomic_load_n((const uint32_t *)(uintptr_t)w->waddr, __ATOMIC_RELAXED) & ~WAITERS_BIT) == o->handle)
            p = w->prio;
    return p;
}

// The priority the virtual cores go by: raw with MP4_ASYNC_PREEMPT=0, as before.
static int prio_of(const RtThread *t) { return g_ap > 0 ? eff_prio(t) : t->prio; }

// TLR+0x100 disable_count, +0x102 interrupt_flag (any thread may read them; the flag is set from outside as the
// kernel does).
static volatile uint16_t *ap_tlr(const RtThread *t) { return (volatile uint16_t *)(t->tlr + 0x100); }
static int ap_disabled(const RtThread *t) { return g_ap > 0 && ap_tlr(t)[0]; }

// kern_ap_stub: entered in place of the interrupted instruction of top-level game code. It keeps every register the
// game code may have live (System V volatile GPRs: rax, rcx, rdx, rsi, rdi, r8-r11; flags, YMM0-15, MXCSR; the C calls
// preserve the rest), gives the core up in kern_ap_yield and jumps back to kern_ap_rip(), whose value it first stores
// in the return slot it reserved at entry (just below the interrupted rsp, inside the red zone the lifted code does not
// use: -mno-red-zone), so a stack walk sees the interrupted function as its caller. No ret: the jump keeps call/return
// pairs balanced. The waiter never writes the owner's stack. (Windows x64 original: see the file header.)
__asm__(".text\n"
        ".globl kern_ap_stub\n"
        ".type kern_ap_stub, @function\n"
        ".p2align 4\n"
        "kern_ap_stub:\n"
        "    leaq -8(%rsp), %rsp\n"   // the return slot; not sub: the flags are still the interrupted code's
        "    pushfq\n"
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    pushq %rsi\n"
        "    pushq %rdi\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r10\n"
        "    pushq %r11\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    andq $-32, %rsp\n"
        "    subq $544, %rsp\n"   // 0: MXCSR saved, 4: MXCSR default, 32: YMM0-15
        "    vmovdqa %ymm0, 32(%rsp)\n"
        "    vmovdqa %ymm1, 64(%rsp)\n"
        "    vmovdqa %ymm2, 96(%rsp)\n"
        "    vmovdqa %ymm3, 128(%rsp)\n"
        "    vmovdqa %ymm4, 160(%rsp)\n"
        "    vmovdqa %ymm5, 192(%rsp)\n"
        "    vmovdqa %ymm6, 224(%rsp)\n"
        "    vmovdqa %ymm7, 256(%rsp)\n"
        "    vmovdqa %ymm8, 288(%rsp)\n"
        "    vmovdqa %ymm9, 320(%rsp)\n"
        "    vmovdqa %ymm10, 352(%rsp)\n"
        "    vmovdqa %ymm11, 384(%rsp)\n"
        "    vmovdqa %ymm12, 416(%rsp)\n"
        "    vmovdqa %ymm13, 448(%rsp)\n"
        "    vmovdqa %ymm14, 480(%rsp)\n"
        "    vmovdqa %ymm15, 512(%rsp)\n"
        "    stmxcsr 0(%rsp)\n"
        "    movl $0x1f80, 4(%rsp)\n"
        "    ldmxcsr 4(%rsp)\n"
        "    cld\n"
        "    vzeroupper\n"
        "    call kern_ap_rip\n"
        "    movq %rax, 88(%rbp)\n"   // the return slot: above rbp's 10 pushed GPRs (rbp..rax) and the flags
        "    call kern_ap_yield\n"
        "    ldmxcsr 0(%rsp)\n"
        "    vmovdqa 32(%rsp), %ymm0\n"
        "    vmovdqa 64(%rsp), %ymm1\n"
        "    vmovdqa 96(%rsp), %ymm2\n"
        "    vmovdqa 128(%rsp), %ymm3\n"
        "    vmovdqa 160(%rsp), %ymm4\n"
        "    vmovdqa 192(%rsp), %ymm5\n"
        "    vmovdqa 224(%rsp), %ymm6\n"
        "    vmovdqa 256(%rsp), %ymm7\n"
        "    vmovdqa 288(%rsp), %ymm8\n"
        "    vmovdqa 320(%rsp), %ymm9\n"
        "    vmovdqa 352(%rsp), %ymm10\n"
        "    vmovdqa 384(%rsp), %ymm11\n"
        "    vmovdqa 416(%rsp), %ymm12\n"
        "    vmovdqa 448(%rsp), %ymm13\n"
        "    vmovdqa 480(%rsp), %ymm14\n"
        "    vmovdqa 512(%rsp), %ymm15\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    popq %r11\n"
        "    popq %r10\n"
        "    popq %r9\n"
        "    popq %r8\n"
        "    popq %rdi\n"
        "    popq %rsi\n"
        "    popq %rdx\n"
        "    popq %rcx\n"
        "    popq %rax\n"
        "    popfq\n"
        "    leaq 8(%rsp), %rsp\n"
        "    jmpq *-8(%rsp)\n"
        ".size kern_ap_stub, .-kern_ap_stub\n");

enum { AP_NONE, AP_RETRY, AP_SENT };

// ---- interrupting another host thread: a real-time signal whose handler runs on that thread (Linux's way to do what
// SuspendThread / Get/SetThreadContext did). One request at a time, under G: a redirect for ap_try, or a sample of the
// interrupted rip and rsp for the diagnostics. A request the handler has not answered within IR_WAIT_NS keeps the slot
// busy until it does, so a late handler never acts on a newer request.
#define IR_SIG (SIGRTMIN + 3)
#define IR_WAIT_NS 2000000ull
enum { IR_REDIRECT, IR_SAMPLE };
enum { IRR_SENT, IRR_PC, IRR_HOST, IRR_DEFERRED };
static struct {
    int kind;
    RtThread *o;            // IR_REDIRECT: the owner to redirect
    int result;             // IRR_*
    uint64_t rip, rsp;
    volatile int done, busy;
} g_ir;

static void ir_handler(int sig, siginfo_t *si, void *ucv) {
    (void)sig, (void)si;
    if (!__atomic_load_n(&g_ir.busy, __ATOMIC_ACQUIRE) || __atomic_load_n(&g_ir.done, __ATOMIC_ACQUIRE)) return;
    ucontext_t *uc = ucv;
    greg_t *g = uc->uc_mcontext.gregs;
    uint64_t rip = (uint64_t)g[REG_RIP];
    g_ir.rip = rip;
    g_ir.rsp = (uint64_t)g[REG_RSP];
    if (g_ir.kind == IR_REDIRECT) {
        RtThread *o = g_ir.o;
        volatile uint16_t *tl = ap_tlr(o);
        if (rip < g_gen_lo || rip > g_gen_hi) g_ir.result = IRR_PC;
        else if (__atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0) g_ir.result = IRR_HOST;
        else if (tl[0]) {
            tl[1] = 1;
            g_ir.result = IRR_DEFERRED;
        } else {
            o->ap_rip = rip;
            o->ap_pending = 1;
            g[REG_RIP] = (greg_t)(uintptr_t)kern_ap_stub;
            g_ir.result = IRR_SENT;
        }
    }
    __atomic_store_n(&g_ir.done, 1, __ATOMIC_RELEASE);
}

static void ap_signal_init(void) {
    static int done;
    if (done) return;   // holding G
    done = 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = ir_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(IR_SIG, &sa, NULL) != 0) rt_log("kernel: the preemption signal could not be installed");
}

// Sends request kind to thread th and waits for its handler. 1 when it answered (g_ir holds the answer). Holding G.
static int ir_send(pthread_t th, int kind, RtThread *o) {
    if (__atomic_load_n(&g_ir.busy, __ATOMIC_ACQUIRE)) {
        if (!__atomic_load_n(&g_ir.done, __ATOMIC_ACQUIRE)) return 0;   // a late answer is still due
        __atomic_store_n(&g_ir.busy, 0, __ATOMIC_RELEASE);
    }
    ap_signal_init();
    g_ir.kind = kind, g_ir.o = o, g_ir.result = IRR_PC;
    __atomic_store_n(&g_ir.done, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ir.busy, 1, __ATOMIC_RELEASE);
    if (pthread_kill(th, IR_SIG) != 0) {
        __atomic_store_n(&g_ir.busy, 0, __ATOMIC_RELEASE);
        return 0;
    }
    uint64_t end = kern_now_ns() + IR_WAIT_NS;
    while (!__atomic_load_n(&g_ir.done, __ATOMIC_ACQUIRE))
        if (kern_now_ns() > end) return 0;   // busy stays set until the handler runs
        else __builtin_ia32_pause();
    __atomic_store_n(&g_ir.busy, 0, __ATOMIC_RELEASE);
    return 1;
}

// The waiter w interrupts its target owner if that runs top-level game code (holding G; the handler takes no lock).
static int ap_try(RtThread *w) {
    RtThread *o = w->target;
    if (!o || o->cur_core < 0 || !o->preempt) return AP_NONE;
    if (!o->has_pth || o->ap_pending) return AP_RETRY;
    volatile uint16_t *tl = ap_tlr(o);
    if (__atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0) {
        g_ap_host++;
        return AP_RETRY;
    }
    if (tl[0]) {   // preemption disabled: SVC 0x36 switches
        tl[1] = 1;
        g_ap_deferred++;
        return AP_RETRY;
    }
    uint64_t now = kern_now_ns();
    if (o->ap_last && now - o->ap_last < AP_GAP_NS) return AP_RETRY;
    o->ap_last = now;
    if (!ir_send(o->pth, IR_REDIRECT, o)) {
        g_ap_fail++;
        return AP_RETRY;
    }
    int r = AP_RETRY;
    switch (g_ir.result) {
    case IRR_PC: g_ap_pc++; break;
    case IRR_HOST: g_ap_host++; break;
    case IRR_DEFERRED: g_ap_deferred++; break;
    default: g_ap_sent++; r = AP_SENT; break;
    }
    if (r == AP_SENT && g_ap_log < AP_LOG_N) {
        g_ap_log++;
        rt_log("kernel: interrupt %#x p%d at exe+%#llx rsp %#llx for %#x p%d", o->handle, o->prio,
               (unsigned long long)(o->ap_rip - exe_base()), (unsigned long long)g_ir.rsp, w->handle, w->prio);
    }
    return r;
}

// ---- MP4_CORE_DIAG=1: why ready threads wait for a virtual core, and for how long
// Each core wait is split into classes: the owner asked to give the core up ran game code (DG_GAME, the part an
// asynchronous preemption would remove), was in a host call or SVC that kept the core (DG_HOST), every owner the waiter
// may displace was of better or equal priority (DG_EQ: Horizon waits too), or the worse owners were claimed by better
// waiters (DG_NOCORE). Per 60 presents: time per frame and a wait-length histogram per class, the owners that cost
// most; a line per present over 25 ms; a line per wait with over 2 ms of DG_GAME (where the owner reached its
// checkpoint), and one host RIP sample of the owner per such wait (exe RVA, for mp4.map).
enum { DG_GAME, DG_HOST, DG_EQ, DG_NOCORE, DG_N };
static const char *const dg_name[DG_N] = {"game", "host", "eq", "nocore"};
#define DG_NB 8   // wait lengths: < 0.05, 0.25, 1, 2, 4, 8, 16 ms, longer
static const uint64_t dg_edge[DG_NB - 1] = {50000, 250000, 1000000, 2000000, 4000000, 8000000, 16000000};
#define DG_LONG_LINES 400
#define DG_SAMPLES 600
#define DG_OWN 32
int kern_host_track;
static int g_diag = -1;
typedef struct {
    uint64_t ns[DG_N], n[DG_N], hist[DG_N][DG_NB];                    // this window
    struct { uint32_t h; int prio, cls; uint64_t ns; } own[DG_OWN];   // this window: owners by class
    uint64_t long_frames, long_game_frames;
} DgWindow;
static DgWindow g_dgw;
static uint64_t g_dgf[DG_N], g_dgfr[DG_N];   // this frame: all threads, the render thread
static uint64_t g_dg_samples, g_dg_lines, g_sync36, g_sync36_flag;

static void diag_init(void) {
    if (g_diag >= 0) return;
    const char *e = cfg_get("MP4_CORE_DIAG");
    g_diag = e && *e && strcmp(e, "0");
    if (g_diag) {
        kern_host_track = 1;
        rt_log("core-diag: on (MP4_CORE_DIAG)");
    }
}

int kern_core_diag_on(void) { return g_diag > 0; }

// A tracked host call or SVC returned to depth 0 with a preempt request pending (host.h); on the thread itself.
void kern_host_left(void *thread) {
    RtThread *t = thread;
    if (g_diag > 0 && t->dg_kick && t->dg_host_kick && !t->dg_host_end) t->dg_host_end = kern_now_ns();
}

static int dg_in_host(const RtThread *o) { return __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED) > 0; }

static void dg_add(const RtThread *w, int cls, uint64_t ns, uint32_t oh, int oprio) {
    if (!ns) return;
    g_dgw.ns[cls] += ns;
    g_dgf[cls] += ns;
    if (w == g_render) g_dgfr[cls] += ns;
    if (!oh) return;
    int i = 0;
    for (; i < DG_OWN && g_dgw.own[i].ns; i++)
        if (g_dgw.own[i].h == oh && g_dgw.own[i].cls == cls) break;
    if (i == DG_OWN) return;
    g_dgw.own[i].h = oh;
    g_dgw.own[i].prio = oprio;
    g_dgw.own[i].cls = cls;
    g_dgw.own[i].ns += ns;
}

// The waiter's class at the start of its wait, after core_kick claimed (or did not claim) an owner for it.
static int dg_class0(const RtThread *t) {
    if (t->target) return dg_in_host(t->target) ? DG_HOST : DG_GAME;
    for (int i = 0; i < NCORE; i++)
        if (core_allowed(t, i) && g_core_owner[i] && g_core_owner[i]->prio > t->prio) return DG_NOCORE;
    return DG_EQ;
}

// The waiter got a core: split [dg_since, now] by what the owner that released it did after its preempt request.
static void dg_done(RtThread *t) {
    uint64_t now = kern_now_ns(), t0 = t->dg_since, w = now - t0;
    t->dg_since = 0;
    if (!t0) return;
    const struct DgRel *r = &t->dg_rel;
    uint64_t part[DG_N] = {0};
    uint32_t oh = t->target ? t->target->handle : r->h;
    int oprio = t->target ? t->target->prio : r->prio;
    if (r->pre && r->kick) {
        uint64_t k = r->kick > t0 ? (r->kick < now ? r->kick : now) : t0;
        part[t->dg_cls] += k - t0;
        if (r->host) {
            uint64_t he = r->host_end > k && r->host_end < now ? r->host_end : now;
            part[DG_HOST] += he - k;
            part[DG_GAME] += now - he;
        } else
            part[DG_GAME] += now - k;
        oh = r->h;
        oprio = r->prio;
    } else
        part[t->dg_cls] += w;
    int top = 0;
    for (int c = 0; c < DG_N; c++) {
        dg_add(t, c, part[c], oh, oprio);
        if (part[c] > part[top]) top = c;
    }
    int b = 0;
    while (b < DG_NB - 1 && w >= dg_edge[b]) b++;
    g_dgw.n[top]++;
    g_dgw.hist[top][b]++;
    if ((part[DG_GAME] >= 2000000 || part[DG_HOST] >= 2000000) && g_dg_lines < DG_LONG_LINES) {
        g_dg_lines++;
        char a[128], l[128];
        rt_log("core-diag: wait %.2f ms (game %.2f host %.2f): %#x p%d mask %llx; released by %#x p%d%s at %s lr %s",
               w / 1e6, part[DG_GAME] / 1e6, part[DG_HOST] / 1e6, t->handle, t->prio, (unsigned long long)t->mask, r->h,
               r->prio, r->pre ? " (preempted)" : "", r->pc ? ldr_describe(r->pc, a, sizeof a) : "-",
               r->lr ? ldr_describe(r->lr, l, sizeof l) : "-");
    }
    t->target = NULL;
    memset(&t->dg_rel, 0, sizeof t->dg_rel);
}

// Once per long DG_GAME or DG_HOST wait: where the owner's host thread runs (module and offset), its latest SVC and
// its guest lr. Holding G; the owner is resumed before anything that could take a lock it holds.
// The module and offset of a host address ("country-box+0x1234", "libvulkan_radeon.so+0x88"), into out.
static const char *host_where(uint64_t a, char *out, size_t n) {
    Dl_info di;
    if (dladdr((void *)(uintptr_t)a, &di) && di.dli_fname) {
        const char *m = strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname;
        snprintf(out, n, "%s+%#llx", m, (unsigned long long)(a - (uintptr_t)di.dli_fbase));
    } else snprintf(out, n, "%#llx", (unsigned long long)a);
    return out;
}

static void dg_sample(RtThread *t) {
    RtThread *o = t->target;
    if (!o || o->cur_core < 0 || !o->has_pth || g_dg_samples >= DG_SAMPLES) return;
    int depth = __atomic_load_n(&o->host_depth, __ATOMIC_RELAXED);
    if (!ir_send(o->pth, IR_SAMPLE, o)) return;
    g_dg_samples++;
    char where[160], lr[128];
    rt_log("core-diag: sample owner %#x p%d depth %d rip %s, last svc %#x, guest lr %s (waiter %#x p%d, %s, "
           "%.2f ms)", o->handle, o->prio, depth, host_where(g_ir.rip, where, sizeof where), o->dg_svc,
           o->cur ? ldr_describe(o->cur->x[30], lr, sizeof lr) : "-", t->handle, t->prio,
           t->dg_cls == DG_HOST ? "host" : "game", (kern_now_ns() - t->dg_since) / 1e6);
}


// ---- MP4_THREAD_DUMP=<frame>[,<frame>...]: at those presents, every guest thread with its priority, its core, what it
// waits on and a frame-pointer backtrace of its guest context, to find what a game that stopped making progress waits
// for. The backtrace reads the thread's Cpu while it is blocked in the host, where lifted code has stored its x29/x30.
static struct { uint32_t tid; pthread_t pth; const char *name; } g_host_threads[8];
static int g_nhost;

// The calling thread for a log line: a guest thread's handle and priority, a noted host thread's name, or its id.
const char *kern_thread_describe(char *o, size_t n) {
    RtThread *t = tcur;
    uint32_t tid = plat_thread_id();
    if (t) snprintf(o, n, "guest thread %#x prio %d%s (host %lu)", t->handle, t->prio, t->is_main ? " main" : "", (unsigned long)tid);
    else {
        const char *name = NULL;
        for (int i = 0; i < g_nhost; i++) if (g_host_threads[i].tid == tid) name = g_host_threads[i].name;
        snprintf(o, n, "host thread %s%s%lu", name ? name : "", name ? " " : "", (unsigned long)tid);
    }
    return o;
}

// Host threads the dump walks besides the guest ones (the present and replay threads, vk_backend.c).
void kern_host_thread_note(const char *name) {
    lock();
    if (g_nhost < 8) {
        g_host_threads[g_nhost].tid = plat_thread_id();
        g_host_threads[g_nhost].pth = pthread_self();
        g_host_threads[g_nhost++].name = name;
    }
    unlock();
}

// Where another host thread runs: its interrupted rip and rsp, read by the signal handler on that thread (holding G).
// The Windows build unwound the whole host stack; here only the top frame is shown.
static void host_stack_log(pthread_t th) {
    if (pthread_equal(th, pthread_self()) || !ir_send(th, IR_SAMPLE, NULL)) return;
    char where[160];
    rt_logf("  host: rip %s rsp %#llx\n", host_where(g_ir.rip, where, sizeof where), (unsigned long long)g_ir.rsp);
}

static void thread_dump(uint64_t frame) {
    static const char *const kinds[] = {"no wait object (running, ready, sleeping or in a host call)", "mutex", "condition variable", "address", "sync objects"};
    lock();
    rt_log("threads at frame %llu:", (unsigned long long)frame);
    gfx_dump_state();
    {   // the virtual cores: who holds each (an exited holder is a leaked core) and who waits for one
        char line[512];
        int at = snprintf(line, sizeof line, "cores:");
        for (int i = 0; i < NCORE; i++) {
            RtThread *o = g_core_owner[i];
            if (o) at += snprintf(line + at, sizeof line - (size_t)at, " %d: thread %#llx prio %d%s%s;", i, (unsigned long long)o->id,
                                  o->prio, o->started ? "" : " not started", o->exited ? " EXITED" : "");
            else at += snprintf(line + at, sizeof line - (size_t)at, " %d: free;", i);
        }
        at += snprintf(line + at, sizeof line - (size_t)at, " waiting:");
        for (RtThread *w = g_core_wait; w && at < (int)sizeof line - 40; w = w->cnext)
            at += snprintf(line + at, sizeof line - (size_t)at, " %#llx (prio %d, mask %#llx)", (unsigned long long)w->id, w->prio,
                           (unsigned long long)w->mask);
        rt_logf("%s\n", line);
    }
    for (RtThread *t = g_all; t; t = t->all) {
        if (!t->started || t->exited) continue;
        char e[160];
        rt_logf("thread %#llx (host %lu) prio %d core %d (ideal %d, mask %#llx)%s: %s on %#llx (key %#llx), entry %s\n",
                (unsigned long long)t->id, (unsigned long)t->tid, t->prio, t->cur_core, t->core, (unsigned long long)t->mask,
                t->is_main ? " main" : "",
                t->wkind >= 0 && t->wkind <= W_SYNC ? kinds[t->wkind] : "?",
                (unsigned long long)(t->wkind == W_SYNC ? t->wobj : t->waddr), (unsigned long long)t->wkey,
                ldr_describe(t->entry, e, sizeof e));
        rt_logf("  last woken by thread %#x; blocks %llu; spin passes %llu at %#llx\n", t->waker, (unsigned long long)t->nblk,
                (unsigned long long)t->sp_n, (unsigned long long)t->sp_site);
        if (t->cur && t->cur_core < 0) {
            const char *h = hostfn_name(t->cur->pc);   // in a host call: pc is that host function's slot
            char d[160];
            rt_logf("  pc %s\n", h ? h : ldr_describe(t->cur->pc, d, sizeof d));
            kern_backtrace(t->cur);
        }
        if (t->has_pth) host_stack_log(t->pth);
    }
    for (int i = 0; i < g_nhost; i++) {
        rt_logf("host thread %s (host %lu)\n", g_host_threads[i].name, (unsigned long)g_host_threads[i].tid);
        host_stack_log(g_host_threads[i].pth);
    }
    unlock();
}

// An entry written s<seconds> fires at that wall-clock time since the first present instead, from a host thread: for
// a game that stopped presenting, where a frame number never comes. The dump reads the thread list under the kernel
// lock, so the thread it runs on does not matter.
static void thread_dump_timer(void *arg) {
    uint64_t *secs = arg;
    uint64_t done = 0;
    for (;;) {
        plat_sleep_ns(1000000000ull);
        done++;
        for (int i = 0; i < 16 && secs[i]; i++)
            if (secs[i] == done) thread_dump(done * 1000000000ull);   // the "frame" printed: nanoseconds, told apart by size
    }
}

static void thread_dump_frame(uint64_t frame) {
    static int parsed;
    static uint64_t at[16], secs[17];
    static int n, ns;
    if (!parsed) {
        parsed = 1;
        const char *e = cfg_get("MP4_THREAD_DUMP");
        for (char *p = (char *)e; p && *p && n < 16 && ns < 16;) {
            char *end;
            int by_time = *p == 's';
            uint64_t v = strtoull(by_time ? p + 1 : p, &end, 10);
            if (end == p || (by_time && end == p + 1)) break;
            if (by_time) secs[ns++] = v;
            else at[n++] = v;
            p = *end == ',' ? end + 1 : end;
        }
        if (ns) {
            PlatThread *h = plat_thread_new(thread_dump_timer, secs, "thread-dump");
            if (h) plat_thread_detach(h);
        }
    }
    for (int i = 0; i < n; i++)
        if (at[i] == frame) thread_dump(frame);
}

// Stop-the-world statistics (under G), logged every 600 presents (kern_core_diag_frame): a stop runs from the first
// pause while none is in force to the resume of the last paused thread; context waits are GetThreadContext3's waits for
// its target to park.
static int g_susp_paused;                        // threads paused now
static uint64_t g_stw_t0, g_stw_n, g_stw_ns, g_stw_max, g_ctx_n, g_ctx_ns, g_ctx_max;

// perf_present, every present: a line for a present over 25 ms, the 60-present summary.
void kern_core_diag_frame(uint64_t frame, uint64_t interval_ns) {
    thread_dump_frame(frame);
    if (g_ap > 0 && frame && frame % 600 == 0) {   // MP4_ASYNC_PREEMPT counters since the start
        lock();
        uint64_t v[8] = {g_ap_sent, g_ap_taken, g_ap_host, g_ap_pc, g_ap_deferred, g_ap_fail, g_ap_spin_ok, g_sync36};
        unlock();
        rt_log("kernel: frame %llu: asynchronous preemption: %llu interrupted, %llu cores given up there; refused: "
               "%llu in a host call, %llu outside game code, %llu preemption disabled, %llu failed; %llu waits ended "
               "in the first spin; SVC 0x36 %llu",
               (unsigned long long)frame, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
               (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6],
               (unsigned long long)v[7]);
    }
    if (frame && frame % 600 == 0) {   // Unity's garbage collector stops (SetThreadActivity), since the start
        lock();
        uint64_t s[6] = {g_stw_n, g_stw_ns, g_stw_max, g_ctx_n, g_ctx_ns, g_ctx_max};
        unlock();
        if (s[0])
            rt_log("kernel: frame %llu: thread stops %llu (avg %.2f ms, max %.2f ms); context waits %llu (avg %.3f ms, "
                   "max %.2f ms)", (unsigned long long)frame, (unsigned long long)s[0], s[1] / 1e6 / s[0], s[2] / 1e6,
                   (unsigned long long)s[3], s[3] ? s[4] / 1e6 / s[3] : 0.0, s[5] / 1e6);
    }
    if (g_diag <= 0) return;
    DgWindow w;
    uint64_t f[DG_N], fr[DG_N], s36, s36f;
    lock();
    g_render = tcur;
    memcpy(f, g_dgf, sizeof f);
    memcpy(fr, g_dgfr, sizeof fr);
    memset(g_dgf, 0, sizeof g_dgf);
    memset(g_dgfr, 0, sizeof g_dgfr);
    int lng = interval_ns > 25000000ull;
    if (lng) {
        g_dgw.long_frames++;
        if (f[DG_GAME] > 1000000) g_dgw.long_game_frames++;
    }
    int rep = frame && frame % 60 == 0;
    if (rep) {
        w = g_dgw;
        memset(&g_dgw, 0, sizeof g_dgw);
    }
    s36 = g_sync36;
    s36f = g_sync36_flag;
    unlock();
    if (lng)
        rt_log("core-diag: frame %llu: %.1f ms; core wait game %.2f host %.2f eq %.2f nocore %.2f ms; render thread game "
               "%.2f host %.2f eq %.2f nocore %.2f ms",
               (unsigned long long)frame, interval_ns / 1e6, f[0] / 1e6, f[1] / 1e6, f[2] / 1e6, f[3] / 1e6,
               fr[0] / 1e6, fr[1] / 1e6, fr[2] / 1e6, fr[3] / 1e6);
    if (!rep) return;
    char s[1024];
    int o = 0;
    for (int c = 0; c < DG_N; c++) {
        o += snprintf(s + o, sizeof s - o, "%s %.3f ms/frame %llu waits [", dg_name[c], w.ns[c] / 60e6,
                      (unsigned long long)w.n[c]);
        for (int b = 0; b < DG_NB; b++)
            o += snprintf(s + o, sizeof s - o, "%s%llu", b ? " " : "", (unsigned long long)w.hist[c][b]);
        o += snprintf(s + o, sizeof s - o, "]%s", c < DG_N - 1 ? "; " : "");
    }
    rt_log("core-diag: frames %llu-%llu: %s; long frames %llu (game wait > 1 ms in %llu); SVC 0x36 %llu (flag set %llu)",
           (unsigned long long)frame - 59, (unsigned long long)frame, s, (unsigned long long)w.long_frames,
           (unsigned long long)w.long_game_frames, (unsigned long long)s36, (unsigned long long)s36f);
    o = 0;
    for (int k = 0; k < 6; k++) {
        int bi = -1;
        for (int i = 0; i < DG_OWN; i++)
            if (w.own[i].ns && (bi < 0 || w.own[i].ns > w.own[bi].ns)) bi = i;
        if (bi < 0) break;
        o += snprintf(s + o, sizeof s - o, " %#x p%d %s %.3f;", w.own[bi].h, w.own[bi].prio, dg_name[w.own[bi].cls],
                      w.own[bi].ns / 60e6);
        w.own[bi].ns = 0;
    }
    if (o) rt_log("core-diag: owners (ms/frame):%s", s);
}

static void core_count(void) {
    int n = 0;
    for (int i = 0; i < NCORE; i++) n += g_core_owner[i] != NULL;
    if (n > g_held_max) g_held_max = n;
}

static struct DgRel g_dg_rel;   // MP4_CORE_DIAG: the owner core_put is releasing, for core_grant

static void core_grant(int c) {
    RtThread *b = NULL, **bp = NULL;
    int bpri = 0;
    for (RtThread **p = &g_core_wait; *p; p = &(*p)->cnext) {
        if (!core_allowed(*p, c)) continue;
        int pr = prio_of(*p);
        if (!b || pr < bpri || (pr == bpri && (*p)->cseq < b->cseq)) {
            b = *p;
            bp = p;
            bpri = pr;
        }
    }
    if (!b) return;
    *bp = b->cnext;
    b->cnext = NULL;
    b->cur_core = c;
    g_core_owner[c] = b;
    core_count();
    if (g_diag > 0) b->dg_rel = g_dg_rel;
    if (rt_trace & RT_TRACE_SCHED) b->grant_at = kern_now_ns();
    plat_cond_signal(&b->cv);
}

static void core_put(RtThread *t) {
    if (t->cur_core < 0) return;
    int c = t->cur_core;
    if (t->preempt && t->pre_since) {   // sched trace: time from the preempt request to this release
        uint64_t d = kern_now_ns() - t->pre_since;
        g_pre_n++;
        g_pre_ns += d;
        if (d > g_pre_max) g_pre_max = d;
        t->npre++;
    }
    t->pre_since = 0;
    if (g_diag > 0) {
        g_dg_rel = (struct DgRel){.kick = t->dg_kick, .host_end = t->dg_host_end, .pc = t->dg_pc, .lr = t->dg_lr,
                                  .host = t->dg_host_kick, .pre = t->preempt, .prio = t->prio, .h = t->handle};
        t->dg_kick = t->dg_host_end = t->dg_pc = t->dg_lr = 0;
        t->dg_host_kick = 0;
    }
    t->cur_core = -1;
    t->preempt = 0;
    g_core_owner[c] = NULL;
    core_grant(c);
}

// A free core t may run on, other than `except`: its ideal core first, then the one with the fewest threads pinned to
// it alone. FE3H pins its job workers and render thread to core 2 (mask 0x4) and leaves the main thread free to
// run anywhere; placed on core 2 while cores 0 and 1 were busy, the main thread then spun there for a worker that
// could run nowhere else. Horizon keeps a thread on its ideal core and migrates it when a core-bound thread needs
// the one it borrowed; the choice here and the migration below (core_must_move) stand in for that.
static int core_pick_free(const RtThread *t, int except) {
    int best = -1, best_pinned = 0;
    for (int i = 0; i < NCORE; i++) {
        if (i == except || !core_allowed(t, i) || g_core_owner[i]) continue;
        int pinned = 0;
        for (const RtThread *p = g_all; p; p = p->all)
            if (p != t && p->started && !p->exited && p->mask == (1ull << i)) pinned++;
        if (best < 0 || pinned < best_pinned) {
            best = i;
            best_pinned = pinned;
        }
    }
    return best;
}

// Whether t, on core c, should move to a free core for a waiter that may run on c and on no free core (which it would
// have taken): priorities aside, as Horizon's scheduler moves a thread off a core another one is bound to.
static int core_must_move(const RtThread *t, int c) {
    int wanted = 0;
    for (const RtThread *w = g_core_wait; w && !wanted; w = w->cnext) wanted = core_allowed(w, c);
    return wanted && core_pick_free(t, c) >= 0;
}

// Whether a waiter better than t may run on core c.
static int core_better_waiter(const RtThread *t, int c) {
    int tp = prio_of(t);
    for (const RtThread *w = g_core_wait; w; w = w->cnext)
        if (core_allowed(w, c) && prio_of(w) < tp) return 1;
    return 0;
}

// Asks holders to give their cores up, one holder per waiter: waiters best first, each claims the worst holder worse
// than itself on a core it may run on that no better waiter claimed (a request already pending is reused). Asking
// every worse holder made several drop their cores for one waiter; the spare cores went to worse waiters, which asked
// again.
static void core_kick(void) {
    RtThread *w[64];
    int wp[64], op[NCORE], n = 0, claimed = 0;
    for (int i = 0; i < NCORE; i++) op[i] = g_core_owner[i] ? prio_of(g_core_owner[i]) : 0;
    for (RtThread *p = g_core_wait; p && n < 64; p = p->cnext) {
        int i = n++, pp = prio_of(p);
        for (; i > 0 && (wp[i - 1] > pp || (wp[i - 1] == pp && w[i - 1]->cseq > p->cseq)); i--) {
            w[i] = w[i - 1];
            wp[i] = wp[i - 1];
        }
        w[i] = p;
        wp[i] = pp;
    }
    for (int k = 0; k < n && claimed != (1 << NCORE) - 1; k++) {
        int v = -1;
        for (int i = 0; i < NCORE; i++) {
            RtThread *o = g_core_owner[i];
            if ((claimed >> i & 1) || !core_allowed(w[k], i) || !o || op[i] <= wp[k]) continue;
            RtThread *b = v < 0 ? NULL : g_core_owner[v];
            if (!b || op[i] > op[v] || (op[i] == op[v] && o->preempt && !b->preempt)) v = i;
        }
        if (v < 0)   // no worse holder: a holder that can move to a free core gives way all the same (core_must_move)
            for (int i = 0; i < NCORE && v < 0; i++)
                if (!(claimed >> i & 1) && core_allowed(w[k], i) && g_core_owner[i] && core_pick_free(g_core_owner[i], i) >= 0) v = i;
        if (v < 0) continue;
        claimed |= 1 << v;
        RtThread *o = g_core_owner[v];
        w[k]->target = o;
        if (o->preempt) continue;
        if (rt_trace & RT_TRACE_SCHED) {
            o->pre_since = kern_now_ns();
            g_pre_req++;
        }
        if (g_diag > 0) {
            o->dg_kick = kern_now_ns();
            o->dg_host_kick = dg_in_host(o);
            o->dg_host_end = 0;
        }
        __atomic_store_n(&o->preempt, 1, __ATOMIC_RELEASE);
    }
}

static void core_take(RtThread *t) {
    if (t->cur_core >= 0) return;
    if (!cores_enabled()) {
        sched_acct(t, S_RUN);
        return;
    }
    if (!(t->mask & ((1u << NCORE) - 1)))
        rt_fatal("kernel: thread %#x core mask %#llx has no application core", t->handle, (unsigned long long)t->mask);
    int c = t->core >= 0 && t->core < NCORE && core_allowed(t, t->core) && !g_core_owner[t->core] ? t->core : -1;
    if (c < 0) c = core_pick_free(t, -1);
    if (c >= 0) {
        t->cur_core = c;
        g_core_owner[c] = t;
        core_count();
        sched_acct(t, S_RUN);
        return;
    }
    sched_acct(t, S_CORE);
    t->cseq = ++g_wseq;
    t->cnext = g_core_wait;
    g_core_wait = t;
    t->target = NULL;
    if (g_diag > 0) {
        t->dg_since = kern_now_ns();
        memset(&t->dg_rel, 0, sizeof t->dg_rel);
    }
    core_kick();
    if (g_diag > 0) t->dg_cls = dg_class0(t);
    if (g_ap_int && t->target && t->cur_core < 0) {   // most waits end within microseconds: spin before interrupting
        uint64_t end = kern_now_ns() + AP_FIRST_NS;
        unlock();
        while (__atomic_load_n(&t->cur_core, __ATOMIC_RELAXED) < 0 && kern_now_ns() < end) __builtin_ia32_pause();
        lock();
        if (t->cur_core >= 0) g_ap_spin_ok++;
    }
    int sampled = 0;
    while (t->cur_core < 0) {
        if (ss_on()) {   // Country-Box: a save state freezes the thread here (ss_poll)
            ss_poll(t);
            continue;
        }
        uint64_t ns = 0;   // 0: until woken
        if (g_ap_int && ap_try(t) != AP_NONE) ns = 1000000;   // the owner keeps its core: again in a millisecond
        int sample = g_diag > 0 && !sampled && (t->dg_cls == DG_GAME || t->dg_cls == DG_HOST);
        if (sample && !ns) ns = 2000000;
        if (ns) plat_cond_wait_ns(&t->cv, &G, ns);
        else plat_cond_wait(&t->cv, &G);
        if (sample && t->cur_core < 0 && kern_now_ns() - t->dg_since >= 2000000) {
            sampled = 1;
            dg_sample(t);
        }
    }
    if (g_diag > 0) dg_done(t);
    if (t->grant_at) {
        uint64_t d = kern_now_ns() - t->grant_at;
        t->lg_ns += d;
        t->lg_n++;
        if (d > t->lg_max) t->lg_max = d;
        t->grant_at = 0;
    }
    t->target = NULL;
    sched_acct(t, S_RUN);
}

static uint32_t block(RtThread *t, int kind, int64_t ns) {
    t->waker = 0;
    if (ns == 0) return block_inner(t, kind, ns);
    core_put(t);
    sched_acct(t, st_of(kind));
    t->nblk++;
    uint32_t r = block_inner(t, kind, ns);
    core_take(t);
    return r;
}

int kern_core_away(void) {
    RtThread *t = tcur;
    if (!t || t->cur_core < 0) return 0;
    lock();
    core_put(t);
    sched_acct(t, S_HOST);
    if (rt_trace & RT_TRACE_SCHED)   // the caller as an exe RVA (mp4.map)
        t->wobj = (uintptr_t)__builtin_return_address(0) - exe_base();
    unlock();
    return 1;
}

void kern_core_back(int *away) {
    RtThread *t = tcur;
    if (!*away || !t) return;
    lock();
    core_take(t);
    unlock();
    *away = 0;
}

// ---- spin waits (recomp/patches.py `spin` lines, found by tools/spin_waits.py)
// Adapted from MacNativeGames games/totk/runtime/threads.c runtime_spin_wait @ a038e6b and nxnative dm/host/hle_thread.c
// dm_spin_wait @ 6d34984 (MIT, Copyright (c) 2026 MacNativeGames contributors, Copyright (c) 2026 seedds).
// A pass of a guest loop that polls memory until another thread changes it. On the Switch it fills only its own core,
// where a better thread pinned there preempts it; here it holds a virtual core and a host core, and pollers that take a
// lock on every pass hand its cache line between their host cores so the thread whose item it is rarely gets it
// (nxnative found this in Breath of the Wild's job queue). Every pass pauses the host core once. A wait is a run of
// passes through the same loop at most SPIN_GAP_NS apart; once one has lasted MP4_SPIN_HANDOFF_US (default 50) and a
// thread waits for a virtual core, each pass gives the core to the waiter, whatever its priority, yields the host CPU
// and takes a core again. Shorter waits keep the core: a lock held for a moment would otherwise make the next pass wait
// for another thread's next SVC (MacNativeGames). MP4_SPIN=0 keeps the core always (the gate's own cost only).
#define SPIN_GAP_NS 20000ull
static int g_spin_on = -1;
static uint64_t g_spin_handoff_ns;
static _Thread_local uint64_t sw_head, sw_t0, sw_last;

void rt_spin_gate(Cpu *c, uint64_t head) {
    (void)c;
    __builtin_ia32_pause();
    RtThread *t = tcur;
    if (!t) return;
    if (g_spin_on < 0) {
        const char *e = cfg_get("MP4_SPIN_HANDOFF_US");
        g_spin_handoff_ns = (uint64_t)(e && *e ? atoll(e) : 50) * 1000;
        g_spin_on = !(cfg_get("MP4_SPIN") && !cfg_bool("MP4_SPIN"));
    }
    t->sp_n++;
    t->sp_site = head;
    uint64_t now = kern_now_ns();
    if (head != sw_head || now - sw_last > SPIN_GAP_NS) {   // a new wait
        sw_head = head;
        sw_t0 = now;
    }
    sw_last = now;
    if (ss_on()) {   // Country-Box: a save state's freeze also stops a spinning thread (what it waits for is frozen)
        lock();
        ss_poll(t);
        unlock();
    }
    if (!g_spin_on || t->cur_core < 0 || now - sw_t0 < g_spin_handoff_ns ||
        !__atomic_load_n(&g_core_wait, __ATOMIC_RELAXED))   // a racy look: a waiter missed now is seen at the next pass
        return;
    t->sp_ho++;
    int away = kern_core_away();
    sched_yield();
    kern_core_back(&away);
    sw_last = kern_now_ns();
}

// ---- thread suspension (Country-Box): svc 0x32 SetThreadActivity / 0x33 GetThreadContext3. Unity's garbage
// collector stops every other thread (nn::os::SuspendThread), reads its registers and scans its stack. Lifted code keeps
// guest registers in host locals between calls, so a context is only whole at a call boundary: a paused thread stops
// at its next SVC or indirect call (both checkpoints; the request also sets preempt so the indirect-call check fires)
// and parks there without its virtual core. A thread inside an SVC (a wait, mostly) has flushed its registers and
// counts as stopped at once; it parks when the SVC returns. GetThreadContext3 waits until the target is in one of
// those two states. The asynchronous preemption's stop (kern_ap_stub) is not such a point and never parks.
static PlatCond g_susp_cv = PLAT_COND_INIT;   // a thread parked or entered an SVC while paused

static void suspend_park(RtThread *t) {   // holding G
    core_put(t);
    t->parked = 1;
    plat_cond_broadcast(&g_susp_cv);
    while (t->susp_req && !t->cancel) {
        if (ss_on()) ss_poll(t);   // Country-Box: a save state freezes the thread here
        else plat_cond_wait(&t->cv, &G);
    }
    t->parked = 0;
    core_take(t);
}

void kern_svc_enter(void) {
    RtThread *t = tcur;
    if (!t) return;
    __atomic_store_n(&t->in_svc, t->in_svc + 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&t->susp_req, __ATOMIC_ACQUIRE)) {
        lock();
        plat_cond_broadcast(&g_susp_cv);
        unlock();
    }
}

void kern_svc_leave(void) {
    RtThread *t = tcur;
    if (!t) return;
    if ((__atomic_load_n(&t->susp_req, __ATOMIC_ACQUIRE) || ss_on()) && t->in_svc == 1) {
        lock();
        if (t->susp_req) suspend_park(t);
        if (ss_on()) ss_poll(t);   // Country-Box: a save state freezes the thread at the end of its SVC
        unlock();
    }
    __atomic_store_n(&t->in_svc, t->in_svc - 1, __ATOMIC_RELEASE);
}

static uint32_t set_activity(uint32_t h, uint32_t act) {
    lock();
    RtThread *t = h_get(h, O_THREAD);
    uint32_t r = !t ? R_BAD_HANDLE : act > 1 || t == tcur ? R_BAD_STATE : R_OK;
    if (!r) {
        if (act && !t->susp_req && g_susp_paused++ == 0) g_stw_t0 = kern_now_ns();
        if (!act && t->susp_req && --g_susp_paused == 0) {
            uint64_t d = kern_now_ns() - g_stw_t0;
            g_stw_n++, g_stw_ns += d;
            if (d > g_stw_max) g_stw_max = d;
        }
        t->susp_req = (int)act;
        if (act) __atomic_store_n(&t->preempt, 1, __ATOMIC_RELEASE);
        else plat_cond_broadcast(&t->cv);
    }
    unlock();
    return r;
}

// GetThreadContext3 layout (libnx ThreadContext, 0x320 bytes): x0-x28, fp, lr, sp, pc, psr (u32), 32 x 16-byte V
// registers at 0x110, fpcr and fpsr (u32) at 0x310, tpidr at 0x318.
static uint32_t get_context(uint64_t out, uint32_t h) {
    lock();
    RtThread *t = h_get(h, O_THREAD);
    if (!t || t == tcur || !t->susp_req) {
        unlock();
        return t ? R_BAD_STATE : R_BAD_HANDLE;
    }
    uint64_t t0 = kern_now_ns(), end = t0 + 5000000000ull;
    while (!t->parked && !__atomic_load_n(&t->in_svc, __ATOMIC_ACQUIRE) && !t->exited) {
        if (ss_on() && tcur) {   // Country-Box: a save state freezes the caller too; its time does not count
            ss_poll(tcur);
            end = kern_now_ns() + 5000000000ull;
            continue;
        }
        if (kern_now_ns() > end)
            rt_fatal("kernel: GetThreadContext3: thread %#x reached no SVC or indirect call within 5 s of its pause",
                     t->handle);
        plat_cond_wait_ns(&g_susp_cv, &G, 1000000);
    }
    uint64_t waited = kern_now_ns() - t0;
    g_ctx_n++, g_ctx_ns += waited;
    if (waited > g_ctx_max) g_ctx_max = waited;
    const Cpu *c = t->cur ? t->cur : t->cpu;
    uint8_t *p = (uint8_t *)(uintptr_t)out;
    memset(p, 0, 0x320);
    memcpy(p, c->x, 29 * 8);
    memcpy(p + 0xe8, &c->x[29], 8);
    memcpy(p + 0xf0, &c->x[30], 8);
    memcpy(p + 0xf8, &c->sp, 8);
    memcpy(p + 0x100, &c->pc, 8);
    uint32_t psr = (uint32_t)c->nzcv, fpcr = (uint32_t)c->fpcr, fpsr = (uint32_t)c->fpsr;
    memcpy(p + 0x108, &psr, 4);
    memcpy(p + 0x110, c->v, 32 * 16);
    memcpy(p + 0x310, &fpcr, 4);
    memcpy(p + 0x314, &fpsr, 4);
    memcpy(p + 0x318, &c->tpidr, 8);
    unlock();
    return R_OK;
}

// A pending preempt request, at an SVC or an indirect call. The core goes over only if a better thread still waits
// for it, so core_put grants it to a waiter better than t; a stale request (that waiter got another core) is dropped.
void kern_checkpoint(void) {
    RtThread *t = tcur;
    if (!t || !__atomic_load_n(&t->preempt, __ATOMIC_RELAXED)) return;
    lock();
    if (t->susp_req) suspend_park(t);   // a pause (SetThreadActivity) waits here, at a whole context
    if (ss_on()) ss_poll(t);            // Country-Box: so does a save state's freeze
    if (g_diag > 0 && t->cur) {
        t->dg_pc = t->cur->pc;
        t->dg_lr = t->cur->x[30];
    }
    int better = t->cur_core >= 0 && (core_better_waiter(t, t->cur_core) || core_must_move(t, t->cur_core));
    if (better && ap_disabled(t)) {   // preemption disabled: the switch comes at SVC 0x36
        ap_tlr(t)[1] = 1;
        t->preempt = 0;
        g_ap_deferred++;
    } else if (better) {
        core_put(t);   // clears preempt (after the sched trace reads it)
        core_take(t);
    } else {
        t->preempt = 0;
        t->pre_since = 0;
        t->dg_kick = t->dg_host_end = 0;
        t->dg_host_kick = 0;
        g_pre_stale++;
    }
    unlock();
}

// kern_ap_stub, on the interrupted owner: where to jump back to, then the core given up as at a checkpoint.
uint64_t kern_ap_rip(void) { return tcur->ap_rip; }

void kern_ap_yield(void) {
    RtThread *t = tcur;
    lock();
    t->ap_pending = 0;
    if (t->preempt && t->cur_core >= 0 && !ap_disabled(t) &&
        (core_better_waiter(t, t->cur_core) || core_must_move(t, t->cur_core))) {
        if (g_diag > 0) {
            t->dg_pc = t->cur->pc;
            t->dg_lr = t->cur->x[30];
        }
        g_ap_taken++;
        core_put(t);
        core_take(t);
    }
    unlock();
}

// SVC 0x36 SynchronizePreemptionState: a preemption deferred by disable_count happens here.
static void ap_sync(RtThread *t) {
    lock();
    if (g_ap > 0 && t->cur_core >= 0 && (core_better_waiter(t, t->cur_core) || core_must_move(t, t->cur_core))) {
        core_put(t);
        core_take(t);
    }
    unlock();
}

void kern_core_stats(int *held_now, int *held_max) {
    lock();
    int n = 0;
    for (int i = 0; i < NCORE; i++) n += g_core_owner[i] != NULL;
    *held_now = n;
    *held_max = g_held_max;
    unlock();
}

void kern_ap_info(KernApInfo *out) {
    lock();
    cores_enabled();   // runs ap_init the first time
    *out = (KernApInfo){.on = g_ap_int, .lo = g_gen_lo, .hi = g_gen_hi, .sent = g_ap_sent, .taken = g_ap_taken,
                        .host = g_ap_host, .pc = g_ap_pc, .deferred = g_ap_deferred, .fail = g_ap_fail};
    unlock();
}

void kern_core_stats_reset(void) {
    lock();
    g_held_max = 0;
    core_count();
    unlock();
}

// MP4_TRACE=sched: every 120 presents, the time split of the busiest threads (running, waiting for a core, host
// calls) and of the render thread, the render and the main thread's longest waits by object and by the thread whose
// signal ended them, and the preemption latency.
#define REP_N 8
// At each present (holding G): starts recording at MP4_CRIT's first frame, stops and writes the file at its last.
// File: "CRIT", thread count, event count (u32 each); per thread handle, priority, flags (1 main, 2 presents), host
// tid (u32 each); then the CritEv records.
static void crit_frame(RtThread *self, uint64_t frame) {
    static int parsed;
    if (!parsed) {
        parsed = 1;
        const char *e = cfg_get("MP4_CRIT");
        unsigned long long lo, hi;
        double wl, wh;
        if (e && e[0] == 'w' && sscanf(e + 1, "%lf-%lf", &wl, &wh) == 2 && wl >= 0 && wl < wh &&
            (g_crit = malloc(CRIT_N * sizeof *g_crit))) {
            g_crit_wlo = (int64_t)(wl * 1e6), g_crit_whi = (int64_t)(wh * 1e6);
            g_crit_lo = g_crit_hi = UINT64_MAX;
            rt_log("crit: recording world seconds %g-%g", wl, wh);
        } else if (e && sscanf(e, "%llu-%llu", &lo, &hi) == 2 && lo < hi && (g_crit = malloc(CRIT_N * sizeof *g_crit))) {
            g_crit_lo = lo;
            g_crit_hi = hi;
            rt_log("crit: recording frames %llu-%llu", lo, hi);
        }
    }
    if (g_crit && g_crit_whi && g_crit_hi == UINT64_MAX) {   // world mode: fix the frames as the clock passes lo and hi
        int64_t w = frame_step_world_us();
        if (g_crit_lo == UINT64_MAX) {
            if (w < g_crit_wlo) return;
            g_crit_lo = frame;
        }
        if (w > g_crit_whi) g_crit_hi = frame;
    }
    if (!g_crit || frame < g_crit_lo || frame > g_crit_hi) return;
    g_crit_on = 1;
    crit_add(CRIT_FRAME, self ? self->handle : 0, 0, 0, frame, kern_now_ns());
    if (frame < g_crit_hi) return;
    g_crit_on = 0;
    const char *path = cfg_get("MP4_CRIT_OUT");
    FILE *f = fopen(path && *path ? path : "crit.bin", "wb");
    if (!f) {
        rt_log("crit: cannot write %s", path && *path ? path : "crit.bin");
        return;
    }
    uint32_t nt = 0;
    for (RtThread *t = g_all; t; t = t->all) nt++;
    fwrite("CRIT", 1, 4, f);
    fwrite(&nt, 4, 1, f);
    fwrite(&g_crit_n, 4, 1, f);
    for (RtThread *t = g_all; t; t = t->all) {
        uint32_t row[4] = {t->handle, (uint32_t)t->prio, (uint32_t)(t->is_main | (t == g_render) << 1), t->tid};
        fwrite(row, 4, 4, f);
    }
    fwrite(g_crit, sizeof *g_crit, g_crit_n, f);
    fclose(f);
    rt_log("crit: %u events, %u threads, frames %llu-%llu%s", g_crit_n, nt, (unsigned long long)g_crit_lo,
           (unsigned long long)g_crit_hi, g_crit_n >= CRIT_N ? " (buffer full: the last frames are incomplete)" : "");
}

void kern_sched_report(uint64_t frame) {
    static uint64_t last;
    typedef struct {
        uint32_t h;
        int prio, render;
        uint64_t mask, entry, ns[S_N], nblk, npre, cpu, lw_ns, lw_n, lw_max, lg_ns, lg_n, lg_max, sl_n, sl_req, sl_act;
        uint64_t sp_n, sp_ho, sp_site;
    } Row;
    Row rows[REP_N + 1];
    WaitRec rw[2][4] = {0};   // the render and the main thread's longest waits
    int nr = 0;
    RtThread *self = tcur;
    lock();
    if (g_render != self) {
        g_render = self;
        memset(g_rw, 0, sizeof g_rw);
    }
    crit_frame(self, frame);
    if (frame % 120) {
        unlock();
        return;
    }
    uint64_t now = kern_now_ns(), span = last ? now - last : 0;
    last = now;
    RtThread *top[REP_N + 1] = {0};
    uint64_t busy[REP_N + 1] = {0}, all_run = 0, all_core = 0;
    for (RtThread *t = g_all; t; t = t->all) {
        sched_acct(t, t->st);   // close the open interval
        uint64_t b = t->st_ns[S_RUN] + t->st_ns[S_CORE] + t->st_ns[S_HOST];
        all_run += t->st_ns[S_RUN];
        all_core += t->st_ns[S_CORE];
        if (t == self) continue;
        for (int i = 0; i < REP_N; i++)
            if (!top[i] || b > busy[i]) {
                memmove(top + i + 1, top + i, (REP_N - 1 - i) * sizeof *top);
                memmove(busy + i + 1, busy + i, (REP_N - 1 - i) * sizeof *busy);
                top[i] = t;
                busy[i] = b;
                break;
            }
    }
    if (self) {   // the render thread first
        memmove(top + 1, top, REP_N * sizeof *top);
        top[0] = self;
    }
    for (int i = 0; i <= REP_N && top[i]; i++) {
        RtThread *t = top[i];
        Row *r = &rows[nr++];
        r->h = t->handle;
        r->prio = t->prio;
        r->render = t == self;
        r->mask = t->mask;
        r->entry = t->entry;
        memcpy(r->ns, t->st_ns, sizeof r->ns);
        r->nblk = t->nblk;
        r->npre = t->npre;
        r->lw_ns = t->lw_ns, r->lw_n = t->lw_n, r->lw_max = t->lw_max;
        r->lg_ns = t->lg_ns, r->lg_n = t->lg_n, r->lg_max = t->lg_max;
        r->sl_n = t->sl_n, r->sl_req = t->sl_req, r->sl_act = t->sl_act;
        r->sp_n = t->sp_n, r->sp_ho = t->sp_ho, r->sp_site = t->sp_site;
        r->cpu = 0;
        clockid_t cid;   // the thread's CPU time; pthread_t is only kept for threads that may be preempted
        struct timespec ts;
        if (t->has_pth && pthread_getcpuclockid(t->pth, &cid) == 0 && clock_gettime(cid, &ts) == 0) {
            uint64_t c = (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
            r->cpu = t->cpu_last ? c - t->cpu_last : 0;
            t->cpu_last = c;
        }
    }
    for (int k = 0; k < 2; k++) {
        const WaitRec *src = k ? g_mw : g_rw;
        for (int i = 0; i < RW_N && src[i].ns; i++)
            for (int j = 0; j < 4; j++)
                if (src[i].ns > rw[k][j].ns) {
                    memmove(rw[k] + j + 1, rw[k] + j, (3 - j) * sizeof **rw);
                    rw[k][j] = src[i];
                    break;
                }
    }
    uint64_t pre_stale = g_pre_stale, pre_req = g_pre_req, pre_n = g_pre_n, pre_ns = g_pre_ns, pre_max = g_pre_max;
    uint64_t sc = g_sync_calls, sb = g_sync_blocks;
    for (RtThread *t = g_all; t; t = t->all) {
        memset(t->st_ns, 0, sizeof t->st_ns);
        t->nblk = t->npre = 0;
        t->lw_ns = t->lw_n = t->lw_max = t->lg_ns = t->lg_n = t->lg_max = t->sl_n = t->sl_req = t->sl_act = 0;
        t->sp_n = t->sp_ho = 0;   // racy against the thread's own increments: a few passes may be lost
    }
    memset(g_rw, 0, sizeof g_rw);
    memset(g_mw, 0, sizeof g_mw);
    g_pre_req = g_pre_n = g_pre_ns = g_pre_max = g_pre_stale = 0;
    g_sync_calls = g_sync_blocks = 0;
    unlock();
    if (!span) return;
    rt_log("sched: frame %llu: %.2f s, cores busy %.2f of %d, core wait %.2f thread-s, preempt req %llu released %llu "
           "avg %.2f ms max %.2f ms, WaitSynchronization %llu calls %llu blocks, stale preempt %llu%s",
           (unsigned long long)frame, span / 1e9, (double)all_run / span, NCORE, all_core / 1e9,
           (unsigned long long)pre_req, (unsigned long long)pre_n, pre_n ? pre_ns / 1e6 / pre_n : 0.0, pre_max / 1e6,
           (unsigned long long)sc, (unsigned long long)sb, (unsigned long long)pre_stale, self ? "" : " (render thread is not a guest thread)");
    for (int i = 0; i < nr; i++) {
        Row *r = &rows[i];
        char d[128], s[256];
        int o = 0;
        for (int k = S_RUN; k < S_N; k++)
            o += snprintf(s + o, sizeof s - o, " %s %.0f%%", s_name[k], 100.0 * r->ns[k] / span);
        ldr_describe(r->entry, d, sizeof d);
        rt_log("sched: %s%#x p%d m%llx%s cpu %.0f%% blocks %llu preempted %llu (%s)", r->render ? "render " : "",
               r->h, r->prio, (unsigned long long)r->mask, s, 100.0 * r->cpu / span, (unsigned long long)r->nblk,
               (unsigned long long)r->npre, r->entry ? d : "main");
        rt_log("sched:   %#x wake to run avg %.1f us max %.0f us (%llu), core grant to run avg %.1f us max %.0f us "
               "(%llu), sleep %llu asked avg %.1f us took avg %.1f us", r->h, r->lw_n ? r->lw_ns / 1e3 / r->lw_n : 0.0,
               r->lw_max / 1e3, (unsigned long long)r->lw_n, r->lg_n ? r->lg_ns / 1e3 / r->lg_n : 0.0, r->lg_max / 1e3,
               (unsigned long long)r->lg_n, (unsigned long long)r->sl_n, r->sl_n ? r->sl_req / 1e3 / r->sl_n : 0.0,
               r->sl_n ? r->sl_act / 1e3 / r->sl_n : 0.0);
        if (r->sp_n) {
            ldr_describe(r->sp_site, d, sizeof d);
            rt_log("sched:   %#x spin wait passes %llu, core handoffs %llu, last at %s", r->h,
                   (unsigned long long)r->sp_n, (unsigned long long)r->sp_ho, d);
        }
    }
    for (int k = 0; k < 2; k++)
        for (int j = 0; j < 4 && rw[k][j].ns; j++) {
            char by[32] = "";
            if (rw[k][j].waker) snprintf(by, sizeof by, ", woken by %#x", rw[k][j].waker);
            rt_log("sched: %s wait %d: %s on %s%#llx%s: %.1f%%", k ? "main" : "render", j + 1, s_name[rw[k][j].st],
                   rw[k][j].st == S_HOST ? "exe+" : "", (unsigned long long)rw[k][j].obj, by, 100.0 * rw[k][j].ns / span);
        }
}

// ---- locks: tag word = owner handle | WAITERS_BIT
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

// Release the lock at addr: hand it to the best waiter, or clear it.
static void lock_handoff(uint64_t addr) {
    RtThread *w = pick(W_LOCK, addr);
    int more = 0;
    if (!w) {
        st32(addr, 0);
        return;
    }
    for (RtThread *t = g_wait; t; t = t->next)
        if (t != w && t->wkind == W_LOCK && t->waddr == addr) {
            more = 1;
            break;
        }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}

// A condvar waiter takes its lock: directly if free (and wakes), else it becomes a lock waiter behind the others.
static void lock_acquire_for(RtThread *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) {
            if (cas32(addr, &v, tag)) {
                wake(t, 0);
                return;
            }
            continue;
        }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    sched_acct(t, S_LOCK);
    t->wobj = v & ~WAITERS_BIT;
    t->wkind = W_LOCK;
    t->waddr = addr;
    t->wtag = tag;
    t->wseq = ++g_wseq;
}

// ---- threads
static RtThread *thr_new(int prio, int core, uint64_t mask) {
    RtThread *t = calloc(1, sizeof *t);
    Cpu *c = aligned_alloc(64, (sizeof(Cpu) + 63) & ~(size_t)63);
    if (!t || !c) rt_fatal("kernel: thread allocation");
    memset(c, 0, sizeof *c);
    plat_cond_init(&t->cv);
    t->prio = prio;
    t->core = core;
    t->mask = mask;
    t->cur_core = -1;
    // TLR (0x200) and the "current thread object" the SDK reads through TLR+0x1F8, in one guest-visible block.
    t->tlr = mem_kernel_alloc(TLR_SIZE + THREAD_OBJ_SIZE, TLR_SIZE);
    *(uint64_t *)(t->tlr + 0x1F8) = (uint64_t)(uintptr_t)(t->tlr + TLR_SIZE);
    t->tls = mem_kernel_alloc(TLS_SIZE, 16);
    c->tpidrro = (uint64_t)(uintptr_t)t->tlr;
    c->tpidr = (uint64_t)(uintptr_t)t->tls;
    c->excl_addr = ~0ull;
    c->thread = t;
    t->cpu = t->cur = c;
    return t;
}

static void thr_link(RtThread *t) {   // holding G
    t->handle = h_new(O_THREAD, t);
    t->id = g_next_id++;
    t->all = g_all;
    g_all = t;
    g_created++;
}

static void thr_free(RtThread *t) {   // holding G; t has exited and its handle is closed
    for (RtThread **p = &g_all; *p; p = &(*p)->all)
        if (*p == t) {
            *p = t->all;
            break;
        }
    mem_kernel_free(t->tlr, TLR_SIZE + THREAD_OBJ_SIZE);
    mem_kernel_free(t->tls, TLS_SIZE);
    free(t->cpu);
    free(t);
}

// The end of every guest thread, on its own host context after the unwind: give the core up, signal the thread
// object, free it if its handle is already closed. t is not touched after the unlock.
static void thread_finish(RtThread *t) {
    t->base_fiber = NULL;
    tcur = NULL;
    lock();
    core_put(t);
    sched_acct(t, S_OFF);
    t->has_pth = 0;   // no waiter interrupts a thread without a core
    t->exited = 1;
    wake_sync_waiters();
    if (t->closed) thr_free(t);
    unlock();
}

// ExitThread: back to the thread start with longjmp (glibc's does not unwind: no destructors run in lifted frames).
static __attribute__((noreturn)) void exit_now(RtThread *t) { longjmp(t->exit_jmp, 1); }

static void ap_signal_init(void);

// The calling host thread, for its CPU time and, with MP4_ASYNC_PREEMPT, for ap_try's signal. Holding G.
static void ap_self(RtThread *t) {
    t->pth = pthread_self();
    t->has_pth = 1;
    if (cores_enabled() && g_ap_int) ap_signal_init();
}

// A host thread for guest code: HOST_STACK of address space, committed as it is touched (every guest BL is a host call).
static void host_thread(void *(*fn)(void *), void *arg, pthread_t *out) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, HOST_STACK);
    pthread_t th;
    int e = pthread_create(&th, &a, fn, arg);
    pthread_attr_destroy(&a);
    if (e) rt_fatal("kernel: pthread_create for a guest thread failed (%d)", e);
    if (out) *out = th;
    else pthread_detach(th);
}

static void *thread_main(void *p) {
    RtThread *t = p;
    _Alignas(16) char alt[PLAT_SIGNAL_STACK];   // the crash report's stack (platform.h)
    plat_signal_stack(alt, sizeof alt);
    tcur = t;
    t->tid = plat_thread_id();
    char nm[16];   // Country-Box: "g<handle> p<priority>" in /proc and debuggers (dkcr_tools.c per-thread CPU time)
    snprintf(nm, sizeof nm, "g%x p%d", t->handle, t->prio);
    pthread_setname_np(pthread_self(), nm);
    t->ss_hi = (uint64_t)(uintptr_t)__builtin_frame_address(0);   // Country-Box: a save state copies the stack below
    lock();
    ap_self(t);
    while (!t->started) {
        if (ss_on()) ss_poll(t);   // Country-Box: a save state freezes a thread not started yet here
        else plat_cond_wait(&t->cv, &G);
    }
    core_take(t);
    unlock();
    Cpu *c = t->cpu;
    c->sp = t->stack_top & ~15ull;
    if (!setjmp(t->exit_jmp)) {
        rt_call(c, t->entry, 1, t->arg);
        char d[128];
        rt_fatal("guest thread %#x: entry %s returned (Horizon threads end with ExitThread)", t->handle,
                 ldr_describe(t->entry, d, sizeof d));
    }
    thread_finish(tcur);
    return NULL;
}

uint64_t kern_threads_created(void) {
    lock();
    uint64_t n = g_created;
    unlock();
    return n;
}

Cpu *kern_cpu(void) { return tcur ? tcur->cur : NULL; }

uint32_t kern_main_thread(Cpu **cpu) {
    if (tcur) {
        if (!tcur->is_main) rt_fatal("kern_main_thread on guest thread %#x, not the main thread", tcur->handle);
        *cpu = tcur->cpu;
        return tcur->handle;
    }
    if (g_main) rt_fatal("kern_main_thread: the main thread already runs on another host thread");
    const RtGuestLayout *L = mem_layout();
    uint64_t stack = L->stack.base + L->stack.size - MAIN_STACK;
    if (!plat_mem_commit(stack, MAIN_STACK))
        rt_fatal("kernel: commit the main thread stack at %#llx failed", (unsigned long long)stack);
    RtThread *t = thr_new(MAIN_PRIO, MAIN_CORE, 7);
    t->started = 1;
    t->is_main = 1;
    t->stack_top = stack + MAIN_STACK;
    t->cpu->sp = t->stack_top;
    t->tid = plat_thread_id();
    perf_main_thread();
    lock();
    ap_self(t);
    thr_link(t);
    g_main = t;
    g_main_stack = stack;
    core_take(t);
    unlock();
    tcur = t;
    *cpu = t->cpu;
    return t->handle;
}

void kern_main_stack(uint64_t *base, uint64_t *size) {
    uint64_t b = __atomic_load_n(&g_main_stack, __ATOMIC_ACQUIRE);
    *base = b;
    *size = b ? MAIN_STACK : 0;
}

static void *guest_main(void *p) {
    void (*fn)(Cpu *) = (void (*)(Cpu *))p;
    _Alignas(16) char alt[PLAT_SIGNAL_STACK];
    plat_signal_stack(alt, sizeof alt);
    Cpu *c;
    kern_main_thread(&c);
    tcur->ss_hi = (uint64_t)(uintptr_t)__builtin_frame_address(0);   // Country-Box: a save state copies the stack below
    pthread_setname_np(pthread_self(), "guest-main");   // Country-Box: nnMain's thread (Unity's main thread)
    if (!setjmp(tcur->exit_jmp)) fn(c);
    thread_finish(tcur);
    return NULL;
}

void kern_run_on_guest_main(void (*fn)(Cpu *c)) {
    pthread_t th;
    host_thread(guest_main, (void *)fn, &th);
    pthread_join(th, NULL);
}

// On Windows the waitable timer ended a wait 0.3-0.5 ms late at best, so sleeps below 1 ms spun.
// Country-Box: on Linux the guest's sleeps never spin (plat_sleep_lax_ns: a 1 us timer slack, a wake some
// microseconds late). Returns HD polls with short SleepThread calls without end: one job thread spun 16.4 ms of every
// 16.7 ms frame in the old 60 us spin (measured with eu-stack on level 1-3), a whole core the Steam Deck's four
// need. CB_SLEEP_SPIN_NS=<n> spins sleeps up to n ns again, for comparison.
static void sleep_ns(RtThread *t, int64_t ns) {
    (void)t;
    static int64_t spin = -1;
    if (spin < 0) {
        const char *e = getenv("CB_SLEEP_SPIN_NS");
        spin = e && *e ? strtoll(e, NULL, 10) : 0;
    }
    if (ns <= spin) {
        uint64_t end = kern_now_ns() + (uint64_t)ns;
        while (kern_now_ns() < end) __builtin_ia32_pause();
        return;
    }
    plat_sleep_lax_ns((uint64_t)ns);
}

// ---- synchronization objects
static int sig_obj(uint32_t h, int *bad) {   // 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) {
        *bad = 1;
        return 0;
    }
    switch (g_h[i].type) {
    case O_THREAD: return ((RtThread *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((Event *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}

int kern_thread_state(uint32_t h) {
    lock();
    RtThread *t = h_peek(h, O_THREAD);
    int r = !t ? -1 : t->exited ? KERN_THREAD_EXITED : t->wkind;
    unlock();
    return r;
}

// CloseHandle: an event goes with its last end; a thread object once it has also exited.
static uint32_t handle_close(uint32_t h) {
    unsigned i = h - 0x1000;
    int ok = 0, kind = 0;
    void *ipc = NULL;
    lock();
    if (i > 0 && i < MAX_H) {
        int type = g_h[i].type;
        void *o = g_h[i].obj;
        if (type == O_EVENT_R || type == O_EVENT_W) {
            Event *e = o;
            if (--e->refs == 0 && !g_ss_keep) free(e);   // Country-Box: kept once a save state may bring it back
            ok = 1;
        } else if (type == O_SESSION || type == O_TMEM) {
            kind = type;
            ipc = o;
            ok = 1;
            if (g_ss_keep && ss_defer_close(kind, ipc, h)) ipc = NULL;   // Country-Box: closed once no state has it
        } else if (type == O_THREAD) {
            RtThread *t = o;
            t->closed = 1;
            if (t->exited) thr_free(t);
            ok = 1;
        }
        if (ok) {
            g_h[i].type = O_FREE;
            g_h[i].obj = NULL;
        }
    }
    unlock();
    if (ipc) ipc_handle_closed(kind, ipc);
    return ok ? R_OK : R_BAD_HANDLE;
}

void kern_handle_close(uint32_t h) {
    if (handle_close(h)) rt_fatal("kern_handle_close(%#x): bad handle", h);
}

// Hooks of the probe's condvar negative control.
volatile int kern_test_cv_old_order;
void (*volatile kern_test_cv_hook)(void);

static void cv_hook(void) {
    void (*h)(void) = kern_test_cv_hook;
    if (h) h();
}

int kern_svc(Cpu *c, uint32_t imm) {
    uint64_t *x = c->x;
    if (g_diag > 0 && tcur) tcur->dg_svc = imm;
    switch (imm) {
    case 0x08: {   // CreateThread(-> w1 handle; x1 entry, x2 arg, x3 stack top, w4 priority, w5 core)
        int prio = (int)x[4], core = (int)x[5];
        if (!x[1] || !x[3] || prio < 28 || prio > 59 || core < -3 || core > 2) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        RtThread *t = thr_new(prio, core >= 0 ? core : MAIN_CORE, core >= 0 ? 1ull << core : 7);
        t->entry = x[1];
        t->arg = x[2];
        t->stack_top = x[3] & ~15ull;
        lock();
        thr_link(t);
        unlock();
        host_thread(thread_main, t, NULL);
        if (rt_trace & RT_TRACE_SVC) {
            char d[128];
            rt_log("CreateThread %#x entry %s stack %#llx prio %d core %d", t->handle,
                   ldr_describe(t->entry, d, sizeof d), (unsigned long long)t->stack_top, prio, core);
        }
        x[0] = R_OK;
        x[1] = t->handle;
        return 1;
    }
    case 0x09: {   // StartThread(w0)
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->started = 1;
            plat_cond_signal(&t->cv);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x0A: {   // ExitThread
        RtThread *t = cur(imm);
        if (t->fiber) {   // on a guest fiber: unwind from the thread's own context (kern_fiber_switch resumes it)
            t->exit_pending = 1;
            t->leaving = t->fiber;
            t->fiber = NULL;
            t->cur = t->cpu;
            kern_ctx_switch(fiber_host_slot(t->leaving), t->base_fiber);
            rt_fatal("kernel: ExitThread on a fiber came back");
        }
        exit_now(t);
    }
    case 0x0B: {   // SleepThread(x0 ns); 0, -1, -2 yield
        int64_t ns = (int64_t)x[0];
        RtThread *t = cur(imm);
        if (ns > 0) {
            lock();
            core_put(t);
            sched_acct(t, S_SLEEP);
            t->wobj = (uint64_t)ns;
            t->nblk++;
            unlock();
            uint64_t t0 = kern_now_ns();
            sleep_ns(t, ns);
            uint64_t took = kern_now_ns() - t0;
            lock();
            t->sl_n++;
            t->sl_req += (uint64_t)ns;
            t->sl_act += took;
            core_take(t);
            unlock();
            return 1;
        }
        // Yield: hand the core to a waiting thread of equal or better priority.
        lock();
        int give = 0;
        int tp = prio_of(t);
        for (RtThread *w = g_core_wait; w && !give; w = w->cnext)
            give = t->cur_core >= 0 && core_allowed(w, t->cur_core) && prio_of(w) <= tp;
        if (give) {
            core_put(t);
            core_take(t);
        }
        unlock();
        if (!give) sched_yield();
        return 1;
    }
    case 0x0C: {   // GetThreadPriority(w1 handle -> w1)
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = (uint64_t)t->prio;
        unlock();
        return 1;
    }
    case 0x0D: {   // SetThreadPriority(w0 handle, w1 priority)
        if ((uint32_t)x[1] < 28 || (uint32_t)x[1] > 59) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) t->prio = (int)x[1];
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x0E: {   // GetThreadCoreMask(w2 handle -> w1 core, x2 mask)
        lock();
        RtThread *t = h_get((uint32_t)x[2], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) {
            x[1] = (uint64_t)(uint32_t)t->core;
            x[2] = t->mask;
        }
        unlock();
        return 1;
    }
    case 0x0F: {   // SetThreadCoreMask(w0 handle, w1 ideal core, x2 mask): -1 process default, -2/-3 keep the ideal core
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            int core = (int)x[1];
            if (core >= 0) t->core = core;
            else if (core == -1) t->core = MAIN_CORE;
            if (x[2]) t->mask = x[2];
            if (t == tcur && t->cur_core >= 0 && !core_allowed(t, t->cur_core)) {   // move off a forbidden core
                core_put(t);
                core_take(t);
            }
        }
        x[0] = t ? R_OK : R_BAD_HANDLE;
        unlock();
        return 1;
    }
    case 0x10: {   // GetCurrentProcessorNumber: the virtual core held
        RtThread *t = cur(imm);
        x[0] = (uint64_t)(uint32_t)(t->cur_core >= 0 ? t->cur_core : t->core);
        return 1;
    }
    case 0x36: {   // SynchronizePreemptionState: the SDK's spin locks call it after clearing TLR+0x100 (disable_count)
                   // when TLR+0x102 (interrupt_flag) is set; rt_svc's checkpoint has already honoured a pending preempt
        RtThread *t = cur(imm);
        volatile uint16_t *flag = (volatile uint16_t *)(t->tlr + 0x102);
        __atomic_fetch_add(&g_sync36, 1, __ATOMIC_RELAXED);
        if (*flag) __atomic_fetch_add(&g_sync36_flag, 1, __ATOMIC_RELAXED);
        *flag = 0;
        ap_sync(t);
        x[0] = R_OK;
        return 1;
    }
    case 0x32:   // SetThreadActivity(w0 handle, w1 0 runnable / 1 paused) (Country-Box)
        x[0] = set_activity((uint32_t)x[0], (uint32_t)x[1]);
        return 1;
    case 0x33:   // GetThreadContext3(x0 context out, w1 handle) (Country-Box)
        x[0] = get_context(x[0], (uint32_t)x[1]);
        return 1;
    case 0x25: {   // GetThreadId(w1 handle -> x1)
        lock();
        RtThread *t = h_get((uint32_t)x[1], O_THREAD);
        x[0] = t ? R_OK : R_BAD_HANDLE;
        if (t) x[1] = t->id;
        unlock();
        return 1;
    }

    case 0x1A: {   // ArbitrateLock(w0 owner handle, x1 address, w2 tag)
        RtThread *t = cur(imm);
        uint64_t addr = x[1];
        uint32_t r = R_OK;
        lock();
        if (ld32(addr) == ((uint32_t)x[0] | WAITERS_BIT)) {
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            t->wobj = (uint32_t)x[0];
            r = block(t, W_LOCK, -1);
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1B:   // ArbitrateUnlock(x0 address)
        lock();
        lock_handoff(x[0]);
        unlock();
        x[0] = R_OK;
        return 1;
    case 0x1C: {   // WaitProcessWideKeyAtomic(x0 lock, x1 key, w2 tag, x3 timeout)
        // The key is set before the lock is released. The SDK's Signal reads the key in user mode and skips svc 0x1D
        // when it is 0, and user mode takes the lock without G: releasing first lets another thread take the lock,
        // change the condition, read key 0 and wake nobody, after which this thread waits forever (MNG threads.c
        // 439-446). Linking into the wait list happens in block, still holding G, so svc 0x1D cannot come between.
        RtThread *t = cur(imm);
        uint64_t addr = x[0], key = x[1];
        lock();
        if (!kern_test_cv_old_order) {
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
            lock_handoff(addr);
            cv_hook();
        } else {   // the probe's negative control: MNG's pre-fix order
            lock_handoff(addr);
            cv_hook();
            st32(key, 1);
            t->wkey = key;
            t->waddr = addr;
            t->wtag = (uint32_t)x[2];
        }
        t->wobj = key;
        uint32_t r = block(t, W_CONDVAR, (int64_t)x[3]);   // on timeout user mode takes the lock itself
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x1D: {   // SignalProcessWideKey(x0 key, w1 count; <= 0 all)
        uint64_t key = x[0];
        int cnt = (int)x[1], n = 0;
        lock();
        for (RtThread *w; (cnt <= 0 || n < cnt) && (w = pick(W_CONDVAR, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);
        int left = 0;
        for (RtThread *w = g_wait; w && !left; w = w->next) left = w->wkind == W_CONDVAR && w->wkey == key;
        if (!left) st32(key, 0);
        unlock();
        x[0] = R_OK;
        return 1;
    }

    case 0x34: {   // WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 if less, 1 decrement if less, 2 if equal
        RtThread *t = cur(imm);
        uint64_t addr = x[0];
        int type = (int)x[1];
        int32_t val = (int32_t)x[2];
        if (type < 0 || type > 2) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        lock();
        uint32_t r, v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1)
            while (!cas32(addr, &v, v - 1))
                if (!((int32_t)v < val)) {
                    ok = 0;
                    break;
                }
        if (!ok) r = R_BAD_STATE;
        else {
            t->waddr = addr;
            t->wobj = addr;
            r = block(t, W_ADDRESS, (int64_t)x[3]);
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x35: {   // SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal,
                   // 2 if equal modify by the number of waiters
        uint64_t addr = x[0];
        int type = (int)x[1], cnt = (int)x[3];
        int32_t val = (int32_t)x[2];
        if (type < 0 || type > 2) {
            x[0] = R_INVALID_ENUM;
            return 1;
        }
        lock();
        int nw = 0;
        for (RtThread *w = g_wait; w; w = w->next) nw += w->wkind == W_ADDRESS && w->waddr == addr;
        uint32_t r = R_OK;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                if (cnt <= 0) nv = nw > 0 ? val - 1 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r)
            for (int n = 0; cnt <= 0 || n < cnt; n++) {
                RtThread *w = pick(W_ADDRESS, addr);
                if (!w) break;
                wake(w, 0);
            }
        unlock();
        x[0] = r;
        return 1;
    }

    case 0x45: {   // CreateEvent(-> w1 writable, w2 readable)
        Event *e = calloc(1, sizeof *e);
        if (!e) rt_fatal("kernel: event allocation");
        e->refs = 2;
        lock();
        uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e);
        unlock();
        x[0] = R_OK;
        x[1] = hw;
        x[2] = hr;
        return 1;
    }
    case 0x11: case 0x12: case 0x17: {   // SignalEvent(writable), ClearEvent(either end), ResetSignal(readable)
        uint32_t h = (uint32_t)x[0];
        uint32_t r = R_OK;
        lock();
        Event *e = imm == 0x11 ? h_peek(h, O_EVENT_W) : imm == 0x17 ? h_peek(h, O_EVENT_R)
                 : h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R);
        if (!e) {
            r = imm == 0x17 && h_peek(h, O_THREAD) ? R_BAD_STATE : R_BAD_HANDLE;
            if (r == R_BAD_HANDLE) rt_log("kernel: event svc %#x: invalid handle %#x", imm, h);
        } else if (imm == 0x11) {
            e->signaled = 1;
            wake_sync_waiters();
        } else if (imm == 0x17 && !e->signaled) {
            r = R_BAD_STATE;
        } else {
            e->signaled = 0;
        }
        unlock();
        x[0] = r;
        return 1;
    }
    case 0x18: {   // WaitSynchronization(-> w1 index; x1 handles, w2 count, x3 timeout)
        RtThread *t = cur(imm);
        const uint32_t *hs = (const uint32_t *)(uintptr_t)x[1];
        int n = (int)x[2];
        int64_t ns = (int64_t)x[3];
        if (n < 0 || n > 64 || (!hs && n)) {
            x[0] = R_BAD_STATE;
            return 1;
        }
        uint64_t start = kern_now_ns();
        uint32_t r;
        int idx = -1;
        lock();
        g_sync_calls++;
        for (;;) {
            if (t->cancel) {
                t->cancel = 0;
                r = R_CANCELLED;
                break;
            }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++)
                if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) {
                r = R_BAD_HANDLE;
                break;
            }
            if (idx >= 0) {
                r = R_OK;
                break;
            }
            int64_t left = ns;
            if (ns > 0) {
                uint64_t spent = kern_now_ns() - start;
                if (spent >= (uint64_t)ns) {
                    r = R_TIMEOUT;
                    break;
                }
                left = ns - (int64_t)spent;
            }
            t->wobj = n ? hs[0] | (uint64_t)n << 32 : 0;
            g_sync_blocks++;
            if (block(t, W_SYNC, left) == R_TIMEOUT) {
                r = R_TIMEOUT;
                break;
            }
        }
        unlock();
        x[0] = r;
        x[1] = idx >= 0 ? (uint64_t)idx : 0;
        return 1;
    }
    case 0x19: {   // CancelSynchronization(w0 thread)
        lock();
        RtThread *t = h_get((uint32_t)x[0], O_THREAD);
        if (t) {
            t->cancel = 1;
            if (t->wkind == W_SYNC) wake(t, (uint32_t)-1);
        }
        unlock();
        x[0] = t ? R_OK : R_BAD_HANDLE;
        return 1;
    }
    case 0x16:   // CloseHandle(w0)
        x[0] = handle_close((uint32_t)x[0]);
        return 1;
    }
    return 0;
}

// ---- handles and events for host services (ipc.c and the service overrides)
uint32_t kern_handle_new(int kind, void *obj) {
    if (kind != KERN_SESSION && kind != KERN_TMEM) rt_fatal("kern_handle_new: kind %d", kind);
    lock();
    uint32_t h = h_new(kind, obj);
    unlock();
    return h;
}

void *kern_handle_get(uint32_t h, int kind) {
    lock();
    void *o = h_peek(h, kind);
    unlock();
    return o;
}

uint32_t kern_event_new(void) {
    Event *e = calloc(1, sizeof *e);
    if (!e) rt_fatal("kernel: event allocation");
    e->refs = 1;
    lock();
    uint32_t hw = h_new(O_EVENT_W, e);
    unlock();
    return hw;
}

// Copy-handle transfer of the readable end: each call gives the client its own handle to the same event.
uint32_t kern_event_readable(uint32_t hw) {
    lock();
    Event *e = h_peek(hw, O_EVENT_W);
    if (!e) rt_fatal("kern_event_readable: %#x is not a writable event handle", hw);
    e->refs++;
    uint32_t hr = h_new(O_EVENT_R, e);
    unlock();
    return hr;
}

void kern_event_signal(uint32_t hw) {
    lock();
    Event *e = h_peek(hw, O_EVENT_W);
    if (!e) rt_fatal("kern_event_signal: %#x is not a writable event handle", hw);
    e->signaled = 1;
    wake_sync_waiters();
    unlock();
}

// System events of host services: the SDK's own nn::os::CreateSystemEvent creates the kernel event (SVC 0x45) on a
// guest thread; the writable handle is kept, so any thread (host audio tick included) can signal it later.
static struct { uint64_t ev; uint32_t wh; } g_se[512];
static int g_nse;
static PlatMutex g_se_lock = PLAT_MUTEX_INIT;

static uint32_t se_find(uint64_t ev) {
    plat_mutex_lock(&g_se_lock);
    uint32_t wh = 0;
    for (int i = 0; i < g_nse && !wh; i++)
        if (g_se[i].ev == ev) wh = g_se[i].wh;
    plat_mutex_unlock(&g_se_lock);
    return wh;
}

void kern_bind_system_event(Cpu *c, uint64_t ev, int autoclear) {
    if (se_find(ev)) return;
    if (!tcur || !c) rt_fatal("kern_bind_system_event(%#llx) on a host thread", (unsigned long long)ev);
    static uint64_t create, getw;
    if (!create) {
        create = ldr_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
        getw = ldr_symbol("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
        if (!create || !getw) rt_fatal("kern_bind_system_event: the SDK's system event functions are not loaded");
    }
    uint32_t r = (uint32_t)rt_call(c, create, 3, ev, (uint64_t)(autoclear ? 1 : 0), (uint64_t)1);
    if (r) rt_fatal("kern_bind_system_event: CreateSystemEvent(%#llx) failed %#x", (unsigned long long)ev, r);
    uint32_t wh = (uint32_t)rt_call(c, getw, 1, ev);
    plat_mutex_lock(&g_se_lock);
    if (g_nse == (int)(sizeof g_se / sizeof *g_se)) rt_fatal("kern_bind_system_event: table full");
    g_se[g_nse].ev = ev;
    g_se[g_nse++].wh = wh;
    plat_mutex_unlock(&g_se_lock);
}

// An event not bound yet is bound on the spot with manual clear, which needs a guest thread.
void kern_signal_system_event(uint64_t ev) {
    uint32_t wh = se_find(ev);
    if (!wh) {
        kern_bind_system_event(kern_cpu(), ev, 0);
        wh = se_find(ev);
    }
    kern_event_signal(wh);
}

// ---- fibers
// The SDK switches fibers by swapping register context, which breaks "RET = C return". Each guest fiber gets a host
// fiber (own 256 MiB reserved host stack) and its own Cpu, so a suspended fiber's registers and host frames stay
// untouched while others run, and a fiber may resume on another thread. A switch carries the thread state (TLR, TLS
// pointer, FPCR/FPSR) over to the target's Cpu and clears the exclusive monitor.
enum { F_NEW, F_STARTED, F_DONE };
struct Fiber {
    uint64_t addr, func, arg, stack, size;
    Cpu *cpu;
    void *host;                // the saved host stack pointer while the fiber is switched away (kern_ctx_switch)
    uint8_t *host_stack;       // HOST_STACK of address space, committed as it is touched, a guard page at the bottom
    int state;
    volatile int running;
};

// The host side of a fiber switch, System V x86-64: pushes the callee-saved registers (rbp, rbx, r12-r15), the x87
// control word and MXCSR (what Windows' FIBER_FLAG_FLOAT_SWITCH kept), stores rsp to *save, loads load and pops the
// same from there. A fiber's first switch "returns" into kern_fiber_start, which calls fiber_main(r12).
void kern_fiber_start(void);
__asm__(".text\n"
        ".globl kern_ctx_switch\n"
        ".type kern_ctx_switch, @function\n"
        ".p2align 4\n"
        "kern_ctx_switch:\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $16, %rsp\n"
        "    fnstcw 0(%rsp)\n"
        "    stmxcsr 8(%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    fldcw 0(%rsp)\n"
        "    ldmxcsr 8(%rsp)\n"
        "    addq $16, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        ".size kern_ctx_switch, .-kern_ctx_switch\n"
        ".globl kern_fiber_start\n"
        ".type kern_fiber_start, @function\n"
        ".p2align 4\n"
        "kern_fiber_start:\n"
        "    movq %r12, %rdi\n"
        "    call kern_fiber_main\n"
        "    ud2\n"
        ".size kern_fiber_start, .-kern_fiber_start\n");

// A fresh host stack whose saved context kern_ctx_switch pops into kern_fiber_start with r12 = f; the slot layout
// matches the pushes above (rsp is 16-aligned again after the ret, as at a call site).
static void *fiber_host_new(Fiber *f) {
    uint8_t *m = mmap(NULL, HOST_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK,
                      -1, 0);
    if (m == MAP_FAILED) rt_fatal("kernel: fiber host stack");
    mprotect(m, 0x1000, PROT_NONE);   // guard page
    f->host_stack = m;
    uint64_t *top = (uint64_t *)(m + HOST_STACK), *sp = top - 9;
    sp[0] = 0x037F | (uint64_t)0x1F80 << 32;   // x87 control word, then MXCSR at +8 (low half of sp[1]); defaults
    sp[1] = 0x1F80;
    sp[2] = 0;                                  // r15
    sp[3] = 0;                                  // r14
    sp[4] = 0;                                  // r13
    sp[5] = (uint64_t)(uintptr_t)f;             // r12
    sp[6] = 0;                                  // rbx
    sp[7] = 0;                                  // rbp
    sp[8] = (uint64_t)(uintptr_t)kern_fiber_start;   // ret
    return sp;
}

static void **fiber_host_slot(Fiber *f) { return &f->host; }

static void fiber_host_free(Fiber *f) {
    if (f->host_stack) munmap(f->host_stack, HOST_STACK);
    f->host_stack = NULL;
    f->host = NULL;
}
static Fiber *g_fibers[FIBER_MAX];
static PlatMutex g_fiber_lock = PLAT_MUTEX_INIT;

static Fiber *fiber_find(uint64_t addr) {
    Fiber *f = NULL;
    plat_mutex_lock(&g_fiber_lock);
    for (int i = 0; i < FIBER_MAX && !f; i++)
        if (g_fibers[i] && g_fibers[i]->addr == addr) f = g_fibers[i];
    plat_mutex_unlock(&g_fiber_lock);
    return f;
}

static void fiber_switch_to(RtThread *t, const Cpu *from, Fiber *to);

// After a host fiber switch: release the context left behind, and finish an ExitThread issued on a fiber.
static void fiber_resumed(void) {
    RtThread *t = tcur;
    rt_fpcr_sync(t->cur);   // the host fiber resumes with its own MXCSR (FIBER_FLAG_FLOAT_SWITCH)
    if (t->leaving) {
        __atomic_store_n(&t->leaving->running, 0, __ATOMIC_RELEASE);
        t->leaving = NULL;
    }
    if (t->exit_pending && !t->fiber) exit_now(t);
}

__attribute__((used)) void kern_fiber_main(void *p);
void kern_fiber_main(void *p) {
    Fiber *f = p;
    fiber_resumed();
    Cpu *c = f->cpu;
    tcur->host_depth = 0;   // a new fiber starts at the top level; each context restores its own depth on resume
    uint64_t next = rt_call(c, f->func, 1, f->arg);
    f->state = F_DONE;
    Fiber *n = NULL;
    if (next && !(n = fiber_find(next)))
        rt_fatal("fiber %#llx returned %#llx, which is not an initialized fiber", (unsigned long long)f->addr,
                 (unsigned long long)next);
    fiber_switch_to(tcur, c, n);   // NULL: back to the thread's own context
    rt_fatal("fiber %#llx resumed after it finished", (unsigned long long)f->addr);
}

static void fiber_switch_to(RtThread *t, const Cpu *from, Fiber *to) {
    Cpu *dst = to ? to->cpu : t->cpu;
    if (to) {
        if (to->state == F_DONE) rt_fatal("SwitchToFiber(%#llx): the fiber has finished", (unsigned long long)to->addr);
        int idle = 0;
        if (!__atomic_compare_exchange_n(&to->running, &idle, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            rt_fatal("SwitchToFiber(%#llx): the fiber is running", (unsigned long long)to->addr);
        if (to->state == F_NEW) {
            dst->sp = (to->stack + to->size) & ~15ull;
            to->state = F_STARTED;
        }
    }
    dst->tpidr = from->tpidr;
    dst->tpidrro = from->tpidrro;
    dst->fpcr = from->fpcr;
    dst->fpsr = from->fpsr;
    dst->excl_addr = ~0ull;
    dst->thread = t;
    t->leaving = t->fiber;
    t->fiber = to;
    t->cur = dst;
    int depth = t->host_depth;
    void **save = t->leaving ? &t->leaving->host : &t->base_fiber;
    kern_ctx_switch(save, to ? to->host : t->base_fiber);
    t = tcur;   // the fiber may have been resumed by another host thread: its tcur, read again after the switch
    t->host_depth = depth;
    fiber_resumed();
}

// InitializeFiber(x0 FiberType*, x1 function, x2 argument, x3 stack, x4 stack size, w5 flags)
void kern_fiber_initialize(Cpu *c) {
    uint64_t addr = c->x[0];
    Fiber *f = fiber_find(addr);
    if (f && f->running) rt_fatal("InitializeFiber(%#llx): the fiber is running", (unsigned long long)addr);
    if (!f) {
        f = calloc(1, sizeof *f);
        Cpu *fc = aligned_alloc(64, (sizeof(Cpu) + 63) & ~(size_t)63);
        if (!f || !fc) rt_fatal("kernel: fiber allocation");
        f->cpu = fc;
        plat_mutex_lock(&g_fiber_lock);
        int i = 0;
        while (i < FIBER_MAX && g_fibers[i]) i++;
        if (i == FIBER_MAX) rt_fatal("kernel: more than %d fibers", FIBER_MAX);
        g_fibers[i] = f;
        plat_mutex_unlock(&g_fiber_lock);
    } else if (f->host_stack) {
        fiber_host_free(f);
    }
    memset(f->cpu, 0, sizeof *f->cpu);
    f->cpu->excl_addr = ~0ull;
    f->addr = addr;
    f->func = c->x[1];
    f->arg = c->x[2];
    f->stack = c->x[3];
    f->size = c->x[4];
    f->state = F_NEW;
    f->running = 0;
    f->host = fiber_host_new(f);
}

// FinalizeFiber(x0 FiberType*)
void kern_fiber_finalize(Cpu *c) {
    uint64_t addr = c->x[0];
    plat_mutex_lock(&g_fiber_lock);
    Fiber *f = NULL;
    for (int i = 0; i < FIBER_MAX && !f; i++)
        if (g_fibers[i] && g_fibers[i]->addr == addr) {
            f = g_fibers[i];
            g_fibers[i] = NULL;
        }
    plat_mutex_unlock(&g_fiber_lock);
    if (!f) rt_fatal("FinalizeFiber(%#llx): not an initialized fiber", (unsigned long long)addr);
    if (f->running) rt_fatal("FinalizeFiber(%#llx): the fiber is running", (unsigned long long)addr);
    fiber_host_free(f);
    free(f->cpu);
    free(f);
}

// SwitchToFiber(x0 FiberType*)
void kern_fiber_switch(Cpu *c) {
    RtThread *t = tcur;
    if (!t) rt_fatal("SwitchToFiber on a host thread");
    Fiber *to = fiber_find(c->x[0]);
    if (!to) rt_fatal("SwitchToFiber(%#llx): not an initialized fiber", (unsigned long long)c->x[0]);
    if (to == t->fiber) return;
    fiber_switch_to(t, c, to);
}

// GetCurrentFiber() -> x0, NULL on the thread's own context
void kern_fiber_current(Cpu *c) {
    RtThread *t = tcur;
    c->x[0] = t && t->fiber ? t->fiber->addr : 0;
}

HOST_OVERRIDE(KERN_SYM_INITIALIZE_FIBER, kern_fiber_initialize);
HOST_OVERRIDE(KERN_SYM_FINALIZE_FIBER, kern_fiber_finalize);
HOST_OVERRIDE(KERN_SYM_SWITCH_TO_FIBER, kern_fiber_switch);
HOST_OVERRIDE(KERN_SYM_GET_CURRENT_FIBER, kern_fiber_current);

// ---- Country-Box: save states (savestate.h, savestate.c)
// A frozen guest thread waits in ss_poll, holding nothing but its place in a wait under G: its guest registers are in
// its Cpu or in the lifted frames of its host stack, and every kernel fact about it (wait, core, wake result) is in
// its RtThread and the tables below. So a thread's state is its host stack from the frozen point up to its start
// frame (ss_hi), plus those structures. To copy the stack out or back the thread switches (kern_ctx_switch, which
// keeps the callee-saved registers, x87 control word and MXCSR on the stack) to a helper context on a small stack of
// its own; the helper copies, then switches back to the saved context: after a restore that is the context the
// capture left, so the thread goes on from the point it was frozen at when the state was saved. Everything under G.
enum { SS_NONE, SS_CAPTURE, SS_RESTORE };
#define SS_ALT_STACK (256u << 10)
static PlatCond g_ss_cv = PLAT_COND_INIT;   // a thread reached ss_poll or finished a copy

typedef struct { int type; void *obj; } SsHandle;
static SsHandle *g_ss_tab[SS_SLOTS];        // the handle table of each kept state: its objects are not released
#define SS_DEFER_MAX 4096
static struct { int kind; void *obj; } g_ss_def[SS_DEFER_MAX];   // sessions and transfer memory closed while kept
static int g_ss_ndef;

void kern_ss_alt_start(void);
void kern_ss_alt_main(RtThread *t);
__asm__(".text\n"
        ".globl kern_ss_alt_start\n"
        ".type kern_ss_alt_start, @function\n"
        ".p2align 4\n"
        "kern_ss_alt_start:\n"
        "    movq %r12, %rdi\n"
        "    call kern_ss_alt_main\n"
        "    ud2\n"
        ".size kern_ss_alt_start, .-kern_ss_alt_start\n");

// The helper context: copies the thread's stack out (SS_CAPTURE) or the captured one back in (SS_RESTORE) and returns
// to the thread's context, or to the captured one.
__attribute__((noreturn)) void kern_ss_alt_main(RtThread *t) {
    for (;;) {
        void *back;
        if (t->ss_cmd == SS_RESTORE) {
            memcpy(t->ss_load_sp, t->ss_load_buf, t->ss_load_n);
            back = t->ss_load_sp;
        } else {
            size_t n = (size_t)(t->ss_hi - (uint64_t)(uintptr_t)t->ss_sp);
            t->ss_buf = malloc(n);
            if (t->ss_buf) memcpy(t->ss_buf, t->ss_sp, n);
            t->ss_n = t->ss_buf ? n : 0;
            back = t->ss_sp;
        }
        kern_ctx_switch(&t->ss_alt_sp, back);
    }
}

static void *ss_alt(RtThread *t) {   // the helper context, made at the first use (same slot layout as fiber_host_new)
    if (!t->ss_alt_sp) {
        if (!t->ss_alt && !(t->ss_alt = aligned_alloc(64, SS_ALT_STACK))) rt_fatal("kernel: save state helper stack");
        uint64_t *sp = (uint64_t *)(t->ss_alt + SS_ALT_STACK) - 9;
        sp[0] = 0x037F | (uint64_t)0x1F80 << 32;
        sp[1] = 0x1F80;
        sp[2] = sp[3] = sp[4] = sp[6] = sp[7] = 0;
        sp[5] = (uint64_t)(uintptr_t)t;                 // r12
        sp[8] = (uint64_t)(uintptr_t)kern_ss_alt_start;  // ret
        t->ss_alt_sp = sp;
    }
    return t->ss_alt_sp;
}

static void ss_poll(RtThread *t) {   // holding G
    if (!ss_on()) return;
    t->ss_at = 1;
    plat_cond_broadcast(&g_ss_cv);
    while (ss_on()) {
        if (t->ss_cmd != SS_NONE) {
            kern_ctx_switch(&t->ss_sp, ss_alt(t));
            // Here after the copy out, or, after a restore, in the frame the capture of the loaded state left.
            t->ss_cmd = SS_NONE;
            t->ss_done = 1;
            plat_cond_broadcast(&g_ss_cv);
            continue;
        }
        plat_cond_wait(&t->cv, &G);
    }
    t->ss_at = 0;
}

static void ss_wake_all(void) {   // holding G
    for (RtThread *t = g_all; t; t = t->all) plat_cond_broadcast(&t->cv);
}

int kern_ss_freeze(uint64_t timeout_ns, char *why, size_t wn) {
    for (int i = 0; i < FIBER_MAX; i++)
        if (__atomic_load_n(&g_fibers[i], __ATOMIC_ACQUIRE)) {
            snprintf(why, wn, "the game uses guest fibers, which save states do not cover yet");
            return -1;
        }
    lock();
    if (ss_on()) {
        unlock();
        snprintf(why, wn, "a save or load is already running");
        return -1;
    }
    __atomic_store_n(&g_ss_phase, 1, __ATOMIC_RELEASE);
    uint64_t end = kern_now_ns() + timeout_ns;
    for (;;) {
        RtThread *late = NULL;
        for (RtThread *t = g_all; t; t = t->all) {
            if (t->exited || t->ss_at) continue;
            late = t;
            __atomic_store_n(&t->preempt, 1, __ATOMIC_RELEASE);   // stops at its next SVC or indirect call
            plat_cond_broadcast(&t->cv);
        }
        if (!late) break;
        if (kern_now_ns() > end) {
            snprintf(why, wn, "guest thread %#x (priority %d, in an SVC %d, host calls %d) reached no stop point within "
                     "%llu ms", late->handle, late->prio, late->in_svc, late->host_depth,
                     (unsigned long long)(timeout_ns / 1000000));
            if (getenv("CB_SS_DIAG")) {   // where it is (development: eu-stack of that host thread)
                char cmd[256];
                snprintf(cmd, sizeof cmd, "eu-stack -p %d 2>&1 | sed -n '/^TID %u:/,/^TID/p' | head -40 >&2", getpid(),
                         late->tid);
                unlock();
                prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
                if (system(cmd)) {}
                lock();
            }
            __atomic_store_n(&g_ss_phase, 0, __ATOMIC_RELEASE);
            ss_wake_all();
            unlock();
            return -1;
        }
        plat_cond_wait_ns(&g_ss_cv, &G, 1000000);
    }
    unlock();
    return 0;
}

int kern_ss_host_wait(void *m) {
    RtThread *t = tcur;
    if (!t || !ss_on()) return 0;
    plat_mutex_unlock(m);
    lock();
    ss_poll(t);
    unlock();
    plat_mutex_lock(m);
    return 1;
}

void kern_ss_thaw(void) {
    lock();
    __atomic_store_n(&g_ss_phase, 0, __ATOMIC_RELEASE);
    ss_wake_all();
    unlock();
}

// Asks every live thread for cmd and waits until each did it (holding G).
static void ss_command(int cmd) {
    for (RtThread *t = g_all; t; t = t->all) {
        if (t->exited) continue;
        t->ss_done = 0;
        t->ss_cmd = cmd;
        plat_cond_broadcast(&t->cv);
    }
    for (;;) {
        int all = 1;
        for (RtThread *t = g_all; t && all; t = t->all)
            if (!t->exited && !t->ss_done) all = 0;
        if (all) return;
        plat_cond_wait(&g_ss_cv, &G);
    }
}

typedef struct { uint64_t ptr, id, sp, n; uint32_t handle, pad; } SsThreadRec;
typedef struct {
    RtThread *wait, *main_t, *core_wait, *core_owner[NCORE];
    uint64_t next_id, wseq, created;
    int susp_paused, nse;
} SsGlobals;

static int ss_tab_has(const void *obj) {   // a handle of the live table or of a kept state names obj (holding G)
    for (int i = 1; i < MAX_H; i++)
        if (g_h[i].obj == obj) return 1;
    for (int s = 0; s < SS_SLOTS; s++)
        if (g_ss_tab[s])
            for (int i = 1; i < MAX_H; i++)
                if (g_ss_tab[s][i].obj == obj) return 1;
    return 0;
}

static int ss_defer_close(int kind, void *obj, uint32_t h) {   // holding G
    (void)h;
    for (int i = 0; i < g_ss_ndef; i++)
        if (g_ss_def[i].obj == obj) return 1;   // closed again after a load brought it back: once is enough
    if (g_ss_ndef == SS_DEFER_MAX) return 0;   // full: closed now (a later load of a state naming it gets an error)
    g_ss_def[g_ss_ndef].kind = kind;
    g_ss_def[g_ss_ndef++].obj = obj;
    return 1;
}

// The deferred closes no handle names any more: taken off the list into out (holding G); closed after the unlock.
static int ss_defer_collect(int *kinds, void **objs) {
    int n = 0;
    for (int i = 0; i < g_ss_ndef;) {
        if (ss_tab_has(g_ss_def[i].obj)) {
            i++;
            continue;
        }
        kinds[n] = g_ss_def[i].kind, objs[n++] = g_ss_def[i].obj;
        g_ss_def[i] = g_ss_def[--g_ss_ndef];
    }
    return n;
}

static void ss_defer_release(void) {   // not holding G
    static int kinds[SS_DEFER_MAX];
    static void *objs[SS_DEFER_MAX];
    static PlatMutex m = PLAT_MUTEX_INIT;
    plat_mutex_lock(&m);
    lock();
    int n = ss_defer_collect(kinds, objs);
    unlock();
    for (int i = 0; i < n; i++) ipc_handle_closed(kinds[i], objs[i]);
    plat_mutex_unlock(&m);
}

int kern_ss_capture(SsBuf *out, int slot, char *why, size_t wn) {
    lock();
    for (RtThread *t = g_all; t; t = t->all)
        if (!t->exited && !t->ss_hi) {
            unlock();
            snprintf(why, wn, "guest thread %#x has not reached its start frame", t->handle);
            return -1;
        }
    ss_command(SS_CAPTURE);
    uint32_t n = 0;
    for (RtThread *t = g_all; t; t = t->all)
        if (!t->exited) {
            if (!t->ss_buf) {
                for (RtThread *u = g_all; u; u = u->all) free(u->ss_buf), u->ss_buf = NULL;
                unlock();
                snprintf(why, wn, "out of memory copying the stack of thread %#x", t->handle);
                return -1;
            }
            n++;
        }
    ss_buf_put(out, &n, sizeof n);
    for (RtThread *t = g_all; t; t = t->all) {
        if (t->exited) continue;
        SsThreadRec r = {(uint64_t)(uintptr_t)t, t->id, (uint64_t)(uintptr_t)t->ss_sp, t->ss_n, t->handle, 0};
        ss_buf_put(out, &r, sizeof r);
        ss_buf_put(out, t, sizeof *t);
        ss_buf_put(out, t->cpu, sizeof *t->cpu);
        ss_buf_put(out, t->ss_buf, t->ss_n);
        free(t->ss_buf);
        t->ss_buf = NULL;
    }
    ss_buf_put(out, g_h, sizeof g_h);
    uint32_t nev = 0;
    for (int i = 1; i < MAX_H; i++) nev += g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W;
    ss_buf_put(out, &nev, sizeof nev);
    for (int i = 1; i < MAX_H; i++)
        if (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W) {
            uint32_t at = (uint32_t)i;
            ss_buf_put(out, &at, sizeof at);
            ss_buf_put(out, g_h[i].obj, sizeof(Event));
        }
    SsGlobals g = {g_wait, g_main, g_core_wait, {0}, g_next_id, g_wseq, g_created, g_susp_paused, 0};
    memcpy(g.core_owner, g_core_owner, sizeof g_core_owner);
    plat_mutex_lock(&g_se_lock);
    g.nse = g_nse;
    ss_buf_put(out, &g, sizeof g);
    ss_buf_put(out, g_se, sizeof g_se);
    plat_mutex_unlock(&g_se_lock);
    if (slot >= 0 && slot < SS_SLOTS) {
        if (!g_ss_tab[slot] && !(g_ss_tab[slot] = malloc(sizeof g_h))) rt_fatal("kernel: save state handle table");
        memcpy(g_ss_tab[slot], g_h, sizeof g_h);
    }
    g_ss_keep = 1;
    unlock();
    ss_defer_release();
    return 0;
}

// Walks a capture: calls fn for each thread record with its saved structures and stack.
typedef struct { SsThreadRec r; const RtThread *t; const Cpu *cpu; const uint8_t *stack; } SsThreadView;
static int ss_threads(SsIn *in, SsThreadView *v, uint32_t max, uint32_t *n) {
    if (!ss_in_get(in, n, sizeof *n) || *n > max) return 0;
    for (uint32_t i = 0; i < *n; i++) {
        const SsThreadRec *r = ss_in_take(in, sizeof *r);
        if (!r) return 0;
        v[i].r = *r;
        if (!(v[i].t = ss_in_take(in, sizeof(RtThread))) || !(v[i].cpu = ss_in_take(in, sizeof(Cpu))) ||
            !(v[i].stack = ss_in_take(in, (size_t)r->n)))
            return 0;
    }
    return 1;
}

#define SS_MAX_THREADS 512
int kern_ss_check(SsIn in, char *why, size_t wn) {
    static SsThreadView v[SS_MAX_THREADS];
    uint32_t n;
    if (!ss_threads(&in, v, SS_MAX_THREADS, &n)) {
        snprintf(why, wn, "the state's thread records are damaged");
        return 0;
    }
    lock();
    uint32_t live = 0;
    for (RtThread *t = g_all; t; t = t->all) live += !t->exited;
    int ok = 1;
    for (uint32_t i = 0; i < n && ok; i++) {
        RtThread *t = NULL;
        for (RtThread *u = g_all; u && !t; u = u->all)
            if ((uint64_t)(uintptr_t)u == v[i].r.ptr) t = u;
        if (!t || t->id != v[i].r.id || t->handle != v[i].r.handle || t->exited) {
            snprintf(why, wn, "guest thread %#x ended since this state was saved", v[i].r.handle);
            ok = 0;
        } else if (t->ss_hi != v[i].t->ss_hi) {
            snprintf(why, wn, "guest thread %#x runs on another host stack", v[i].r.handle);
            ok = 0;
        }
    }
    if (ok && live != n) {
        snprintf(why, wn, "the game started %u thread(s) since this state was saved", live > n ? live - n : 0);
        ok = 0;
    }
    unlock();
    return ok;
}

// The saved structure, except what belongs to the live host thread: its condition variable, pthread and host thread
// id, the save state bookkeeping and the list of every thread (the same threads; the wait lists come back saved).
static void ss_thread_put_back(RtThread *t, const RtThread *s) {
    RtThread keep;
    memcpy(&keep, t, sizeof keep);
    size_t cv0 = offsetof(RtThread, cv), cv1 = cv0 + sizeof(PlatCond);
    memcpy(t, s, cv0);
    memcpy((uint8_t *)t + cv1, (const uint8_t *)s + cv1, sizeof *t - cv1);
    t->all = keep.all;
    t->pth = keep.pth, t->has_pth = keep.has_pth, t->tid = keep.tid;
    t->ss_at = keep.ss_at, t->ss_cmd = keep.ss_cmd, t->ss_done = keep.ss_done, t->ss_hi = keep.ss_hi;
    t->ss_sp = keep.ss_sp, t->ss_alt_sp = keep.ss_alt_sp, t->ss_alt = keep.ss_alt, t->ss_buf = keep.ss_buf;
    t->ss_n = keep.ss_n;
    t->ss_load_sp = keep.ss_load_sp, t->ss_load_buf = keep.ss_load_buf, t->ss_load_n = keep.ss_load_n;
}

void kern_ss_restore(SsIn in) {
    static SsThreadView v[SS_MAX_THREADS];
    uint32_t n;
    if (!ss_threads(&in, v, SS_MAX_THREADS, &n)) rt_fatal("kernel: save state: thread records damaged after the check");
    const void *h = ss_in_take(&in, sizeof g_h);
    uint32_t nev;
    if (!h || !ss_in_get(&in, &nev, sizeof nev)) rt_fatal("kernel: save state: tables damaged after the check");
    lock();
    memcpy(g_h, h, sizeof g_h);
    for (uint32_t i = 0; i < nev; i++) {
        uint32_t at;
        const Event *e;
        if (!ss_in_get(&in, &at, sizeof at) || !(e = ss_in_take(&in, sizeof *e)) || at >= MAX_H)
            rt_fatal("kernel: save state: event records damaged");
        memcpy(g_h[at].obj, e, sizeof *e);   // still allocated: events are kept while a state may come back
    }
    SsGlobals g;
    if (!ss_in_get(&in, &g, sizeof g)) rt_fatal("kernel: save state: globals damaged");
    g_wait = g.wait, g_main = g.main_t, g_core_wait = g.core_wait;
    memcpy(g_core_owner, g.core_owner, sizeof g_core_owner);
    g_next_id = g.next_id, g_wseq = g.wseq, g_created = g.created, g_susp_paused = g.susp_paused;
    const void *se = ss_in_take(&in, sizeof g_se);
    if (se) {
        plat_mutex_lock(&g_se_lock);
        memcpy(g_se, se, sizeof g_se);
        g_nse = g.nse;
        plat_mutex_unlock(&g_se_lock);
    }
    for (uint32_t i = 0; i < n; i++) {
        RtThread *t = (RtThread *)(uintptr_t)v[i].r.ptr;
        ss_thread_put_back(t, v[i].t);
        memcpy(t->cpu, v[i].cpu, sizeof *t->cpu);
        t->ss_load_sp = (void *)(uintptr_t)v[i].r.sp;
        t->ss_load_buf = v[i].stack;
        t->ss_load_n = (size_t)v[i].r.n;
    }
    ss_command(SS_RESTORE);
    for (RtThread *t = g_all; t; t = t->all) t->ss_load_buf = NULL;
    unlock();
    ss_defer_release();   // a deferred session the live table names again stays; the others may go
}

void kern_ss_drop(int slot) {
    if (slot < 0 || slot >= SS_SLOTS) return;
    lock();
    free(g_ss_tab[slot]);
    g_ss_tab[slot] = NULL;
    unlock();
    ss_defer_release();
}
