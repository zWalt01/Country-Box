// Country-Box: the C++ unwinder's jump to a landing pad. libunwind (lifted, in sdk) finds the handler, sets up the
// cursor and calls unw_resume, whose Registers_arm64::jumpto loads every register and branches to the pad on the
// handler's frame. A lifted `ret` returns to the host caller, not to another guest frame, so unw_resume is a host
// hook instead (recomp/gen.py HOOK_SYMBOLS): it reads the resumed context out of the cursor with the guest's own
// unw_get_reg / unw_get_fpreg, writes it into the Cpu and marks a non-local resume (c->unw_active, unw_sp, unw_pc).
// It then returns; every lifted call site on the way out returns at once, until the frame whose sp is unw_sp
// reloads its registers and goes to the pad (recomp/gen.py, the call sites' unwind check).
#include <stdio.h>
#include <string.h>

#include "host.h"

#define UNW_REG_IP (-1)
#define UNW_REG_SP (-2)
#define UNW_ARM64_D0 64

static uint64_t g_get_reg, g_get_fpreg;
static _Thread_local uint64_t *t_slot;   // guest-addressable out value for the getters

// int unw_resume(unw_cursor_t *cursor)
static void ov_unw_resume(Cpu *c) {
    uint64_t cursor = arg_x(c, 0);
    if (!g_get_reg) {
        g_get_reg = ldr_symbol("unw_get_reg");
        g_get_fpreg = ldr_symbol("unw_get_fpreg");
        if (!g_get_reg || !g_get_fpreg) rt_fatal("unw_resume: unw_get_reg / unw_get_fpreg are not exported");
    }
    if (!t_slot) t_slot = mem_kernel_alloc(16, 16);
    uint64_t slot = (uint64_t)(uintptr_t)t_slot;
    uint64_t x[31], v[32], sp, pc;
    for (int r = 0; r < 31; r++) {
        if (rt_call(c, g_get_reg, 3, cursor, (uint64_t)r, slot)) rt_fatal("unw_resume: unw_get_reg(x%d) failed", r);
        x[r] = *t_slot;
    }
    if (rt_call(c, g_get_reg, 3, cursor, (uint64_t)(int64_t)UNW_REG_SP, slot)) rt_fatal("unw_resume: no sp");
    sp = *t_slot;
    if (rt_call(c, g_get_reg, 3, cursor, (uint64_t)(int64_t)UNW_REG_IP, slot)) rt_fatal("unw_resume: no pc");
    pc = *t_slot;
    for (int r = 0; r < 32; r++) {
        if (rt_call(c, g_get_fpreg, 3, cursor, (uint64_t)(UNW_ARM64_D0 + r), slot))
            rt_fatal("unw_resume: unw_get_fpreg(d%d) failed", r);
        v[r] = *t_slot;
    }
    memcpy(c->x, x, sizeof x);
    for (int r = 0; r < 32; r++) c->v[r] = v[r];   // d registers; the upper halves read as zero, as after jumpto
    c->sp = sp;
    c->pc = pc;
    c->unw_sp = sp;
    c->unw_pc = pc;
    c->unw_active = 1;
    if (rt_trace & RT_TRACE_SVC) {
        char d[128];
        rt_log("unwind: resume at %s sp %#llx", ldr_describe(pc, d, sizeof d), (unsigned long long)sp);
    }
}
HOST_OVERRIDE("unw_resume", ov_unw_resume);

// longjmp / _longjmp move to another guest frame too (setjmp's return point), which the lifted code cannot do yet:
// main imports them (Unity's PNG and JPEG decoders). Until the lifter marks setjmp's return points as resume
// targets, a longjmp stops with its name instead of returning into the wrong frame.
static void ov_longjmp(Cpu *c) {
    char d[128];
    rt_fatal("longjmp from %s: non-local jumps to setjmp points are not supported yet", ldr_describe(c->x[30], d, sizeof d));
}
HOST_OVERRIDE("longjmp", ov_longjmp);
HOST_OVERRIDE("_longjmp", ov_longjmp);
