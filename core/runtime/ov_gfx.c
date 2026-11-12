// From fe3h-pc-recomp runtime/ov_gfx.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied unchanged.
// From totk-pc-recomp runtime/ov_gfx.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh); adds nn::vi SetLayerScalingMode, DestroyLayer, CloseDisplay and Finalize (FE3H). Country-Box: shared-mode lock is a pthread rwlock, plat_atomic_* (Linux).
// Adapted from MacNativeGames games/totk/runtime/frontend.m (nn::vi, lines 345-359) and gfx_bridge.c (nv, lines
// 74-86) @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames contributors). The nvnBootstrapLoader stop takes the place
// of MacNativeGames' NVN entry (third_party/d2r_gfx/gfx/nvn/hle_nvn.c:115-118); no code of it is copied.
// Graphics bring-up:
//   nv::*        record the game's allocator callbacks and graphics work memory (guest addresses) for the NVN layer
//   nn::vi::*    display, layer and native window identities; the vsync event is bound here
//   nvnBootstrapLoader   the end of --boot (rt_boot_event "nvn"); otherwise the synthetic address of an NVN proc (nvn_procs.c)
#include <pthread.h>
#include <stdint.h>

#include "gfx.h"
#include "platform/platform.h"
#include "host.h"
#include "ov.h"

#define VI_R_INVALID 0xf601u   // MacNativeGames' result for a call on an unknown display or layer

static OvNvState nv;
static pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;

const OvNvState *ov_nv_state(void) { return &nv; }

static void set_allocator(Cpu *c, OvNvAllocator *a) {
    pthread_rwlock_wrlock(&lock);
    *a = (OvNvAllocator){arg_x(c, 0), arg_x(c, 1), arg_x(c, 2), arg_x(c, 3)};
    pthread_rwlock_unlock(&lock);
}

// void nv::SetGraphicsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)
static void ov_graphics_allocator(Cpu *c) {
    set_allocator(c, &nv.graphics);
    // Country-Box: games whose SDK allocates through it (boot.c games nv_allocator) get the SDK's own call as well.
    if (rt_game_nv_allocator()) {
        uint64_t orig = ldr_symbol("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_");
        if (orig) rt_call(c, orig, 4, arg_x(c, 0), arg_x(c, 1), arg_x(c, 2), arg_x(c, 3));
    }
}
// void nv::SetGraphicsDevtoolsAllocator(AllocateFunction, FreeFunction, ReallocateFunction, void *user)
static void ov_devtools_allocator(Cpu *c) { set_allocator(c, &nv.devtools); }

// void nv::InitializeGraphics(void *memory, size_t size)
static void ov_initialize_graphics(Cpu *c) {
    uint64_t mem = arg_x(c, 0), size = arg_x(c, 1);
    if (!mem || !size) rt_fatal("nv::InitializeGraphics: no graphics work memory (%#llx, %#llx)", (unsigned long long)mem,
                                (unsigned long long)size);
    pthread_rwlock_wrlock(&lock);
    nv.memory = mem;
    nv.memory_size = size;
    nv.initialized = 1;
    pthread_rwlock_unlock(&lock);
    rt_log("nv: graphics work memory %#llx+%#llx recorded", (unsigned long long)mem,
           (unsigned long long)size);
}

// ---- nn::vi: the identities are small zeroed blocks in the guest-visible kernel region, in case SDK code reads them
static int vi_ready;
static void *display, *layer, *window;

// void nn::vi::Initialize()
static void ov_vi_initialize(Cpu *c) {
    (void)c;
    pthread_rwlock_wrlock(&lock);
    if (!display) {
        display = mem_kernel_alloc(64, 16);
        layer = mem_kernel_alloc(64, 16);
        window = mem_kernel_alloc(64, 16);
    }
    vi_ready = 1;
    pthread_rwlock_unlock(&lock);
}

static int vi_up(void) {
    pthread_rwlock_rdlock(&lock);
    int r = vi_ready;
    pthread_rwlock_unlock(&lock);
    return r;
}

// Result nn::vi::OpenDefaultDisplay(Display **out)
static void ov_open_display(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up()) { ret_x(c, VI_R_INVALID); return; }
    *out = display;
    ret_x(c, 0);
}

// Result nn::vi::SetLayerScalingMode(Layer *layer, ScalingMode mode). The present scales the game's image to the window
// itself (gfx_window.c), so the mode is only checked for the layer.
static void ov_layer_scaling(Cpu *c) { ret_x(c, vi_up() && arg_p(c, 0) == layer ? 0 : VI_R_INVALID); }

// void DestroyLayer(Layer *), void CloseDisplay(Display *), void Finalize(): the one display, layer and window live
// as long as the process, as the host window does.
static void ov_vi_release(Cpu *c) { (void)c; }

// Result nn::vi::CreateLayer(Layer **out, Display *display)
static void ov_create_layer(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    *out = layer;
    ret_x(c, 0);
}

// Result nn::vi::GetNativeWindow(void **out, Layer *layer): a placeholder the NVN layer recognises
static void ov_native_window(Cpu *c) {
    void **out = arg_p(c, 0);
    if (!out || !vi_up() || arg_p(c, 1) != layer) { ret_x(c, VI_R_INVALID); return; }
    *out = window;
    ret_x(c, 0);
}

// Result nn::vi::GetDisplayVsyncEvent(os::SystemEventType *event, Display *display): auto-clear system event
static void ov_vsync_event(Cpu *c) {
    uint64_t ev = arg_x(c, 0);
    if (!ev || !vi_up() || arg_p(c, 1) != display) { ret_x(c, VI_R_INVALID); return; }
    kern_bind_system_event(c, ev, 1);
    gfx_vsync_bind(ev);
    ret_x(c, 0);
}

// void *nvnBootstrapLoader(const char *name): the first NVN call
static void ov_nvn_bootstrap(Cpu *c) {
    const char *name = arg_p(c, 0);
    if (!name) name = "(null)";
    static int32_t first = 1;
    if (rt_config()->boot && plat_atomic_xchg(&first, 0)) rt_boot_event("nvn", c, name);   // ends --boot; returns
                                                                                                // when --seconds goes on
    ret_x(c, nvn_proc_address(name));
}

// The NVN procs need their synthetic addresses before hostfn_seal; the loader seals at the end of rt_boot, so the
// registration runs at start-up instead of through boot.c.
__attribute__((constructor)) static void ov_nvn_procs_register(void) { nvn_procs_register(); }

HOST_OVERRIDE("_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_graphics_allocator);
HOST_OVERRIDE("_ZN2nv18InitializeGraphicsEPvm", ov_initialize_graphics);
HOST_OVERRIDE("_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", ov_devtools_allocator);
HOST_OVERRIDE("_ZN2nn2vi10InitializeEv", ov_vi_initialize);
HOST_OVERRIDE("_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", ov_layer_scaling);
HOST_OVERRIDE("_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", ov_vi_release);
HOST_OVERRIDE("_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", ov_vi_release);
HOST_OVERRIDE("_ZN2nn2vi8FinalizeEv", ov_vi_release);
HOST_OVERRIDE("_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", ov_open_display);
HOST_OVERRIDE("_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", ov_create_layer);
HOST_OVERRIDE("_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", ov_native_window);
HOST_OVERRIDE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", ov_vsync_event);
HOST_OVERRIDE("nvnBootstrapLoader", ov_nvn_bootstrap);
