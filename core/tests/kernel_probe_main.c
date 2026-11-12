// Country-Box: entry for --probe-kernel on Linux while the rest of the runtime (graphics, audio, input, IPC services,
// file system) is still being ported. It runs runtime/kernel_probe.c's probe_kernel() unchanged, on the real kernel,
// memory, loader, dispatch, SVC and host-function layers and the game's real lifted code, with the start-up of
// runtime/main.c main(). The subsystems below are not reached by the probe; each stub stops with a fatal error if it
// is, so a stub never hides a call.
//
// Build: core/tools/build_probe.sh (links this, the ported runtime files and the game's generated objects).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"

int probe_kernel(void);   // runtime/kernel_probe.c

#define NOT_IN_PROBE(ret, name, args)                                                                                  \
    ret name args { rt_fatal("kernel probe: %s reached; it is not part of the probe build", #name); }

NOT_IN_PROBE(int, ipc_svc, (Cpu *c, uint32_t imm))
NOT_IN_PROBE(void, ipc_handle_closed, (int kind, void *obj))

// Harmless when reached: no profiler, no window, no presents, no saves and no pipeline cache in a probe. The guest
// ExitProcess the probe checks (svc.c 0x07) flushes the last two on its way out.
void vk_pipeline_save(void) {}
void fs_save_flush(void) {}
void prof_write(void) {}
void prof_start(void) {}
void perf_main_thread(void) {}
void gfx_dump_state(void) {}
GfxStats gfx_stats(void) { return (GfxStats){0}; }
double gfx_window_refresh_hz(void) { return 60.0; }

int main(int argc, char **argv) {
    mem_init();   // first, as in main.c: claim the guest address space before anything else lands there
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    rt_parse_trace();
    RtConfig *c = rt_config_set();
    *c = (RtConfig){.exefs = rt_default_exefs, .romfs = rt_default_romfs, .profile = rt_default_profile};
    for (int i = 1; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "--exefs")) c->exefs = argv[i + 1];
        else if (!strcmp(argv[i], "--romfs")) c->romfs = argv[i + 1];
    rt_log_open(0);
    return probe_kernel();
}
