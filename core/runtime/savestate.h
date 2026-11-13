// Country-Box: save states (savestate.c). F5 / the menu's "Save state" freezes every guest thread at a point where
// its context is whole, writes the guest memory, each thread's host stack and context, and the kernel's tables to a
// file in <profile>/savestates; F6 / "Load state" freezes again and puts all of it back. The lifted code keeps guest
// registers in host frames, so a thread is its host stack: kernel.c copies it out and back with kern_ctx_switch.
// A state is only good inside the process that wrote it (host heap objects, GPU objects and stack addresses belong to
// it); a new process deletes the old files. Not restored: the GPU's own memory (the Vulkan back end uploads the
// restored guest memory again), audio voices, open files' host side.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SS_SLOTS 3   // the newest states kept: a bad save (just before a fall) is undone by loading the one before

// A growing byte buffer (the kernel's part of a state) and a cursor over one read back.
typedef struct { uint8_t *p; size_t n, cap; } SsBuf;
void ss_buf_put(SsBuf *b, const void *p, size_t n);
void ss_buf_free(SsBuf *b);
typedef struct { const uint8_t *p; size_t n, at; } SsIn;
const void *ss_in_take(SsIn *in, size_t n);   // NULL when fewer than n bytes are left
int ss_in_get(SsIn *in, void *out, size_t n);  // 1: copied

// ---- kernel.c
// Every guest thread goes to its save-state point and waits there (G is released when this returns). 0: frozen;
// else why not (a thread that reached no point within timeout_ns, guest fibers) in why, and nothing stays frozen.
int kern_ss_freeze(uint64_t timeout_ns, char *why, size_t wn);
void kern_ss_thaw(void);
// Frozen: each thread copies its host stack, then the threads, contexts and kernel tables go to out. slot: which of
// the SS_SLOTS states this is (handle objects a kept state names are not released).
int kern_ss_capture(SsBuf *out, int slot, char *why, size_t wn);   // 0: captured
// Frozen: 1 when the live threads are the captured ones (same objects, none started or ended since), else why.
int kern_ss_check(SsIn in, char *why, size_t wn);
// Frozen, after kern_ss_check: the tables and contexts go back, then each thread switches to its captured stack.
void kern_ss_restore(SsIn in);
void kern_ss_drop(int slot);   // the slot's state was deleted
// A guest thread waiting in a host function for something that may not come while the guest is frozen (an applet
// message): with a freeze on, it releases m (a PlatMutex), waits at its save-state point and takes m again. 1: it was
// frozen, re-check the wait's condition. The wait itself must time out now and then: nothing wakes it for the freeze.
int kern_ss_host_wait(void *m);
#define KERN_SS_HOST_WAIT_NS 20000000ull

// ---- memory.c
typedef struct { uint64_t addr, len; } SsRun;
// Every guest range that holds data now (read-write mappings below RT_ADDRESS_LIMIT; memfd holes and untouched
// anonymous pages left out), sorted; malloc'd, *out freed by the caller.
size_t mem_ss_runs(SsRun **out);
// The guest's memory bookkeeping: what must not change (heap size, stack maps, pool shadows) and the alias region's
// mapped pages, which a load puts back.
void mem_ss_layout(SsBuf *out);
// Frozen: 1 when the runs of a state can go back (the strict part is the same, the runs outside the alias region are
// read-write now), else why. Changes nothing.
int mem_ss_layout_check(SsIn in, const SsRun *runs, size_t nruns, char *why, size_t wn);
void mem_ss_layout_apply(SsIn in);   // frozen, after the check: the alias region's mapped pages as saved

// ---- savestate.c
void ss_init(void);                // at start: deletes the states an earlier process left
void ss_shutdown(void);            // the process ends: its states can never load again, so they go
void ss_request_save(void);        // any thread (key handler, menu): the save runs on the save-state thread
void ss_request_load(int back);    // back: 0 the newest state, 1 the one before, ...
int ss_busy(void);                 // a save or load is running
// The last result, for the menu and the on-screen note: text and when it was set (plat_now_ns), ok or not.
void ss_status(char *out, size_t n, uint64_t *at_ns, int *ok);
int ss_count(void);                // states on disk now

#ifdef __cplusplus
}
#endif
