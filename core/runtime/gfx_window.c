// From fe3h-pc-recomp runtime/gfx_window.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4; the window class and title name this port; the present blit turns the frame over for the lower-left window origin; an sRGB frame keeps its bytes on the UNORM swapchain.
// From totk-pc-recomp runtime/gfx_window.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied with totk renamed to fe3h only. Country-Box: SDL3 window, event loop and Vulkan surface instead of Win32; pthread locks; per-monitor DPI code dropped (Linux).
// SDL3 window, Vulkan surface and swapchain, present blit, 60 Hz vsync thread, focus.
// Swapchain set-up ported from LAS Recomp, runtime/nvn/vulkan/device.c:254-330 (https://github.com/peiikai/LASRecomp @
// da35b83a9611d046a9a681aae668d7085954d37f, MIT, Copyright (c) 2026 Peiikai), whose SDL window this file has again
// (it went through Win32 in between). Focus and vsync behaviour from MacNativeGames, games/totk/runtime/frontend.m:88-97,
// 175-176, 350-352 (https://github.com/ly05010419/MacNativeGames @ b75be8cfa704df3701e86130c8ced268e79d6b11, MIT,
// Copyright (c) 2026 MacNativeGames contributors).
//
// Threads: the window thread owns the SDL video subsystem, the SDL_Window and its event loop, and never runs guest
// code. It is the thread that calls SDL_Init(SDL_INIT_VIDEO), which makes it SDL's "main thread": every SDL video call
// (window, fullscreen, relative mouse, text input, the Vulkan surface) is made on it, and other threads reach it through
// window_call (a user event it runs and signals) or a posted user event. It starts with the first
// gfx_window_vk_extensions (the Vulkan instance needs the surface extensions before the window exists); the window
// itself opens at gfx_window_start. The vsync thread signals the bound system events. Presents come from any host
// thread (the back end's present worker) and take g_pl; every queue submission takes the recursive queue lock
// (vk_queue_lock). The swapchain is rebuilt lazily: a size event only marks it dirty, the next present (or
// gfx_window_resize) rebuilds it. What the present path needs of the window (its pixel size, minimised) the window
// thread keeps in atomics, so a present makes no SDL call.
//
// The in-game menu (ui/menu.h) draws over the game image in the present's command buffer, after the blit; its keyboard
// and mouse come from the window's events (ui/menu_sdl.cpp). The window's size and fullscreen state are the settings
// MP4_WINDOW_SIZE and MP4_FULLSCREEN, applied at start and whenever a cfg_set changes them (window_settings_live).
// Under Gamescope (the Steam Deck's Game Mode) the window opens fullscreen, so the swapchain has the panel's size.
//
// Sizes are pixels (SDL_WINDOW_HIGH_PIXEL_DENSITY): MP4_WINDOW_SIZE included, and the swapchain matches the screen pixel
// for pixel on a scaled Wayland output too. gfx_window_dpi is the window's display scale as a DPI (96 = 100%), which
// gives the menu its text size floor.
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "cfg.h"
#include "flight.h"
#include "fs.h"
#include "gfx.h"
#include "input.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "vk.h"

#define FRAMES 2
#define MAX_IMAGES 16
#define VSYNC_NS 1000000000ull
#define VSYNC_HZ 60
// Country-Box: CB_VSYNC_HZ (30..1000, default 60; above the display rate it is a fast-forward for stress sweeps: the game steps a fixed 1/60 s per frame) sets the rate of the vsync events the game waits on and of the present
// pacing (nvn_api_obj.c present_period); the tick count stays 60 per second (input scripts, profiler).
static uint64_t g_vsync_hz;
uint64_t gfx_vsync_hz(void) {
    uint64_t hz = __atomic_load_n(&g_vsync_hz, __ATOMIC_RELAXED);
    if (!hz) {
        const char *e = getenv("CB_VSYNC_HZ");
        long v = e ? strtol(e, NULL, 10) : 60;
        hz = v >= 30 && v <= 1000 ? (uint64_t)v : 60;
        __atomic_store_n(&g_vsync_hz, hz, __ATOMIC_RELAXED);
        if (hz != 60) rt_log("gfx: vsync events and present pacing at %llu Hz (CB_VSYNC_HZ)", (unsigned long long)hz);
    }
    return hz;
}
// Country-Box: changes the rate while the game runs (game tools switch a sweep to fast-forward once a level plays, so
// menus and loads keep their 60 Hz timing). The vsync thread restarts its schedule at the new rate.
void gfx_vsync_set_hz(uint64_t hz) {
    if (hz < 30 || hz > 1000) return;
    gfx_vsync_hz();
    __atomic_store_n(&g_vsync_hz, hz, __ATOMIC_RELAXED);
    rt_log("gfx: vsync events and present pacing now at %llu Hz", (unsigned long long)hz);
}

// ---- locks: recursive, as the Win32 critical sections they replace
static pthread_mutex_t g_ql;                // queue lock
static pthread_mutex_t g_pl;                // swapchain and present state
static pthread_mutex_t g_start_lock;        // gfx_window_start / vsync start
__attribute__((constructor)) static void window_init_locks(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_ql, &a);
    pthread_mutex_init(&g_pl, &a);
    pthread_mutex_init(&g_start_lock, &a);
    pthread_mutexattr_destroy(&a);
}
void vk_queue_lock(void) { pthread_mutex_lock(&g_ql); }
void vk_queue_unlock(void) { pthread_mutex_unlock(&g_ql); }

// ---- window state. g_win is written once by the window thread before window_call returns to gfx_window_start.
static SDL_Window *g_win;
static VkSurfaceKHR g_surface;
static int32_t g_dirty;                     // a size or mode change seen; swapchain stale
static int32_t g_focused = 1;
static int32_t g_focus_msgs;
static int32_t g_minimized;                 // window thread: minimised or hidden; the present skips the frame
static int32_t g_px_w, g_px_h;              // window thread: the window's size in pixels
static int g_started, g_wnd_failed;
static int g_background;                    // MP4_BACKGROUND_WINDOW: the window opens without taking focus, no flashing
static int g_fs_pending;                    // window thread: MP4_FULLSCREEN waits for the first activation
static int32_t g_dpi = 96;                  // the window's display scale as a DPI, set by the window thread

// ---- swapchain and present state (g_pl)
static VkSwapchainKHR g_sc;
static VkFormat g_sc_fmt;
static VkExtent2D g_sc_ext;
static int g_scaler;                        // the last present: 0 the game image 1:1, 1 scaled by the blit, 2 by FSR
static uint32_t g_sc_n;
static VkImage g_sc_img[MAX_IMAGES];
static VkImageView g_sc_view[MAX_IMAGES];   // colour attachment views for the menu overlay
static VkSemaphore g_render_done[MAX_IMAGES];
static VkCommandPool g_pool;
static VkCommandBuffer g_cb[FRAMES];
static VkFence g_fence[FRAMES];
static VkSemaphore g_acquire[FRAMES];
static unsigned g_frame;
static int g_capture;
static VkBuffer g_cap_buf;
static VmaAllocation g_cap_alloc;
static uint8_t *g_cap_ptr;
static VkDeviceSize g_cap_size;
static int g_cap_w, g_cap_h, g_cap_valid;
static int g_flip_on;            // vk_present_blit submitted the blit of swapchain image g_flip_idx; vk_present_flip presents it
static uint32_t g_flip_idx;

// ---- MP4_FLIGHT thumbnails (flight.c; g_pl): each present also blits the swapchain image down into slot frame % g_th_n
// of a host-visible ring. A slot's tag names the frame it holds complete; recording a new frame clears the tag first,
// and the tag is set once the fence of that present has signalled.
#define TH_BYTES ((VkDeviceSize)FLIGHT_THUMB_W * FLIGHT_THUMB_H * 4)
static int g_th_n, g_th_failed;
static uint64_t g_th_frame;                // frame of the next vk_present_image call (vk_thumbs_frame)
static uint64_t *g_th_tag;                 // per ring slot
static uint64_t g_th_pending[FRAMES];      // per fence slot: the frame whose thumbnail its submission writes
static VkImage g_th_img;
static VmaAllocation g_th_img_alloc;
static VkFormat g_th_fmt;
static VkBuffer g_th_buf;
static VmaAllocation g_th_alloc;
static uint8_t *g_th_ptr;

// The sRGB present (present_image): an image of the swapchain's size in the sRGB twin of its UNORM format, which the
// bilinear blit of an sRGB frame writes and a raw copy then moves into the swapchain image.
static VkImage g_mid;
static VmaAllocation g_mid_alloc;
static VkFormat g_mid_fmt;
static VkExtent2D g_mid_ext;
static int g_mid_failed;

// ---- vsync. The Win32 thread ran at THREAD_PRIORITY_HIGHEST; a Linux process may not raise its own priority without
// CAP_SYS_NICE (the Steam Deck's user has none), so the tick thread runs at the normal priority and relies on its 1 us
// timer slack: clock_nanosleep to an absolute deadline, no spinning.
static pthread_rwlock_t g_vs_lock = PTHREAD_RWLOCK_INITIALIZER;   // shared by the tick, exclusive by the binders
static uint64_t g_vs_ev[16];
static int g_vs_nev;
static volatile long *g_vs_ctr[4];
static int g_vs_nctr;
static int64_t g_ticks;
static int64_t g_vs_start_ns;
static int g_vs_started;

static void sleep_until_ns(uint64_t due) {
    struct timespec ts = {(time_t)(due / 1000000000ull), (long)(due % 1000000000ull)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

static void vsync_main(void *arg) {
    (void)arg;
    prctl(PR_SET_TIMERSLACK, 1ul, 0ul, 0ul, 0ul);
    uint64_t n = 0, hz = gfx_vsync_hz();
    int64_t tick_base = 0;                               // 60 Hz ticks counted before the last rate change
    for (;;) {
        uint64_t h = gfx_vsync_hz();
        if (h != hz) {                                   // rate changed: a new schedule from now
            tick_base += (int64_t)(n * VSYNC_HZ / hz), n = 0, hz = h;
            __atomic_store_n(&g_vs_start_ns, (int64_t)kern_now_ns(), __ATOMIC_RELAXED);
        }
        uint64_t start = (uint64_t)__atomic_load_n(&g_vs_start_ns, __ATOMIC_RELAXED);
        uint64_t due = start + (n + 1) * VSYNC_NS / hz;
        uint64_t now = kern_now_ns();
        if (now > due + 100000000ull) {                  // the process stalled: restart the schedule, no catch-up burst
            __atomic_store_n(&g_vs_start_ns, (int64_t)(now - (n + 1) * VSYNC_NS / hz), __ATOMIC_RELAXED);
        } else if (due > now) {
            sleep_until_ns(due);   // kern_now_ns is CLOCK_MONOTONIC (plat_now_ns)
        }
        n++;
        __atomic_store_n(&g_ticks, tick_base + (int64_t)(n * VSYNC_HZ / hz), __ATOMIC_SEQ_CST);
        uint64_t ev[16];
        volatile long *ctr[4];
        int ne, nc;
        pthread_rwlock_rdlock(&g_vs_lock);
        ne = g_vs_nev;
        nc = g_vs_nctr;
        memcpy(ev, g_vs_ev, sizeof ev);
        memcpy(ctr, g_vs_ctr, sizeof ctr);
        pthread_rwlock_unlock(&g_vs_lock);
        for (int i = 0; i < nc; i++) plat_atomic_inc(ctr[i]);
        for (int i = 0; i < ne; i++) kern_signal_system_event(ev[i]);
    }
}

static void vsync_ensure(void) {
    pthread_mutex_lock(&g_start_lock);
    if (!g_vs_started) {
        g_vs_started = 1;
        __atomic_store_n(&g_vs_start_ns, (int64_t)kern_now_ns(), __ATOMIC_RELAXED);
        PlatThread *t = plat_thread_new(vsync_main, NULL, "vsync");
        if (!t) rt_fatal("gfx: cannot start the vsync thread");
        plat_thread_detach(t);
    }
    pthread_mutex_unlock(&g_start_lock);
}

void gfx_vsync_bind(uint64_t system_event) {
    pthread_rwlock_wrlock(&g_vs_lock);
    int have = 0;
    for (int i = 0; i < g_vs_nev; i++) have |= g_vs_ev[i] == system_event;
    if (!have) {
        if (g_vs_nev == 16) rt_fatal("gfx: more than 16 vsync events");
        g_vs_ev[g_vs_nev++] = system_event;
    }
    pthread_rwlock_unlock(&g_vs_lock);
    vsync_ensure();
}

void gfx_vsync_bind_counter(volatile long *counter) {
    pthread_rwlock_wrlock(&g_vs_lock);
    if (g_vs_nctr == 4) rt_fatal("gfx: more than 4 vsync counters");
    g_vs_ctr[g_vs_nctr++] = counter;
    pthread_rwlock_unlock(&g_vs_lock);
    vsync_ensure();
}

uint64_t gfx_vsync_ticks(void) { return (uint64_t)__atomic_load_n(&g_ticks, __ATOMIC_SEQ_CST); }
uint64_t gfx_vsync_time_ns(uint64_t tick) {
    return (uint64_t)__atomic_load_n(&g_vs_start_ns, __ATOMIC_RELAXED) + tick * VSYNC_NS / VSYNC_HZ;
}

// ---- focus
int gfx_window_focused(void) { return __atomic_load_n(&g_focused, __ATOMIC_RELAXED); }
unsigned gfx_window_focus_messages(void) { return (unsigned)__atomic_load_n(&g_focus_msgs, __ATOMIC_SEQ_CST); }
void *gfx_window_handle(void) { return __atomic_load_n(&g_win, __ATOMIC_ACQUIRE); }
unsigned gfx_window_dpi(void) { return (unsigned)__atomic_load_n(&g_dpi, __ATOMIC_RELAXED); }

// ---- the window thread: SDL video, the event loop, and the calls other threads hand it
enum { EV_CALL, EV_SETTINGS, EV_FULLSCREEN, EV_CURSOR, EV_CLOSE, EV_COUNT };
static uint32_t g_ev;                       // the first of our EV_COUNT registered SDL event types
static int g_video;                         // 0 not started, 1 up, -1 failed (no display): no window can open
static const char *const *g_vk_ext;         // SDL_Vulkan_GetInstanceExtensions, valid while SDL video is up
static uint32_t g_vk_ext_n;
static pthread_t g_wthread;
static PlatMutex g_call_lock = PLAT_MUTEX_INIT;
static PlatCond g_call_cv = PLAT_COND_INIT;
static PlatOnce g_video_once = PLAT_ONCE_INIT;

typedef struct { void (*fn)(void *); void *arg; int done; } WindowCall;

int gfx_window_on_thread(void) { return __atomic_load_n(&g_video, __ATOMIC_ACQUIRE) > 0 && pthread_equal(pthread_self(), g_wthread); }

static int video_up(void);

static int post(int what, void *data) {
    if (!video_up()) return 0;
    SDL_Event e;
    SDL_zero(e);
    e.type = g_ev + (uint32_t)what;
    e.user.data1 = data;
    if (SDL_PushEvent(&e)) return 1;
    rt_log("gfx: posting window request %d failed (%s)", what, SDL_GetError());
    return 0;
}

// Runs fn(arg) on the window thread and returns after it did; 0 when there is no window thread.
static int window_call(void (*fn)(void *), void *arg) {
    if (gfx_window_on_thread()) {
        fn(arg);
        return 1;
    }
    WindowCall c = {fn, arg, 0};
    if (!post(EV_CALL, &c)) return 0;
    plat_mutex_lock(&g_call_lock);
    while (!c.done) plat_cond_wait(&g_call_cv, &g_call_lock);
    plat_mutex_unlock(&g_call_lock);
    return 1;
}

// MP4_FULLSCREEN=1: a borderless window over the display (SDL's desktop fullscreen). MP4_WINDOW_SIZE=<w>x<h>: the
// window's size in pixels (the swapchain follows the window; the game image is scaled to it).
static int cfg_fullscreen(void) {
    return cfg_bool("MP4_FULLSCREEN");
}

static void cfg_window_size(int *w, int *h) {
    int x = 0, y = 0;
    if (sscanf(cfg_value("MP4_WINDOW_SIZE"), "%dx%d", &x, &y) != 2 || x < 640 || y < 360 || x > 7680 || y > 4320)
        sscanf(cfg_meta("WINDOW_SIZE")->def, "%dx%d", &x, &y);   // unparsable or out of range: the default
    *w = x;
    *h = y;
}

// Gamescope (the Steam Deck's Game Mode, or a nested session) shows one game window over the whole output and scales
// a smaller one up: the window opens fullscreen there, so the swapchain is the panel's size and nothing is rendered
// at 1080p only to be scaled down to 800 lines.
static int under_gamescope(void) {
    const char *d = getenv("XDG_CURRENT_DESKTOP");
    return getenv("GAMESCOPE_WAYLAND_DISPLAY") != NULL || (d && !strcmp(d, "gamescope"));
}

// The desktop blanks the display and suspends after its idle timeout, and gamepad input does not always count as
// activity. While the window is up and not minimised the screen saver stays off, as other games do (SDL's default
// while video is up); a minimised window lets it run. Window thread.
static void keep_awake(int on) {
    static int cur = -1;
    if (on == cur) return;
    cur = on;
    if (on) SDL_DisableScreenSaver();
    else SDL_EnableScreenSaver();
}

static int is_fullscreen(void) { return (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0; }

// The window thread's copies of what other threads read: the pixel size, the display scale.
static void window_sizes_update(void) {
    int w = 0, h = 0;
    if (SDL_GetWindowSizeInPixels(g_win, &w, &h)) {
        __atomic_store_n(&g_px_w, w, __ATOMIC_RELEASE);
        __atomic_store_n(&g_px_h, h, __ATOMIC_RELEASE);
    }
    float scale = SDL_GetWindowDisplayScale(g_win);
    int32_t dpi = scale > 0 ? (int32_t)(96.0f * scale + 0.5f) : 96;
    if (__atomic_exchange_n(&g_dpi, dpi, __ATOMIC_RELAXED) != dpi) rt_log("gfx: window display scale %d%%", dpi * 100 / 96);
}

// A size in pixels as the points SDL_SetWindowSize takes (a scaled Wayland output has more pixels than points).
static void set_size_px(int w, int h) {
    float d = SDL_GetWindowPixelDensity(g_win);
    if (!(d > 0)) d = 1;
    SDL_SetWindowSize(g_win, (int)(w / d + 0.5f), (int)(h / d + 0.5f));
}

// F11: borderless fullscreen over the display, and back to the window as it was (SDL keeps the windowed size and
// place). The swapchain follows through the size events. Not Alt+Enter: Enter is the Plus button (input_sdl.c).
static int g_pending_w, g_pending_h;   // window thread: MP4_WINDOW_SIZE changed while fullscreen; applied when it ends

static void toggle_fullscreen(void) {
    SDL_SetWindowFullscreen(g_win, !is_fullscreen());
    SDL_SyncWindow(g_win);   // is_fullscreen tells the new state to the caller (the F11 handler saves it)
    plat_atomic_xchg(&g_dirty, 1);
}

// The window size and fullscreen state last taken from the settings (window thread). apply_settings acts on a change of
// either value only, so a window resized by hand keeps its size when another setting changes.
static char g_applied_size[32];
static int g_applied_fs;

static void note_applied(void) {
    snprintf(g_applied_size, sizeof g_applied_size, "%s", cfg_value("MP4_WINDOW_SIZE"));
    g_applied_fs = cfg_fullscreen();
}

// Window thread: the window takes the size and fullscreen state of the settings when they changed. In fullscreen the
// size waits for the window's return (SDL ignores a size request then).
static void apply_settings(void) {
    int size_changed = strcmp(cfg_value("MP4_WINDOW_SIZE"), g_applied_size) != 0, fs = cfg_fullscreen(),
        fs_changed = fs != g_applied_fs;
    note_applied();
    if (size_changed) {
        int cw, ch;
        cfg_window_size(&cw, &ch);
        if (is_fullscreen()) {
            g_pending_w = cw, g_pending_h = ch;
        } else if (__atomic_load_n(&g_px_w, __ATOMIC_ACQUIRE) != cw || __atomic_load_n(&g_px_h, __ATOMIC_ACQUIRE) != ch) {
            if (SDL_GetWindowFlags(g_win) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(g_win);
            set_size_px(cw, ch);
        }
    }
    if (fs_changed && fs != is_fullscreen()) {
        input_cursor_suspend(1);
        toggle_fullscreen();
        input_cursor_suspend(0);
    }
}

void gfx_window_apply_settings(void) {
    if (gfx_window_handle()) post(EV_SETTINGS, NULL);
}

// Present path (g_pl held): a settings change (cfg_version) reaches the window thread, which applies a changed size or
// fullscreen; a changed MP4_VK_PRESENT rebuilds the swapchain at this present.
static void window_settings_live(void) {
    static unsigned seen;
    static int started;
    static char present[16];   // the MP4_VK_PRESENT the swapchain was made with
    unsigned v = cfg_version();
    if (started && v == seen) return;
    seen = v;
    const char *e = cfg_value("MP4_VK_PRESENT");
    if (!started || strncmp(e, present, sizeof present - 1)) {
        snprintf(present, sizeof present, "%s", e);
        if (started) plat_atomic_xchg(&g_dirty, 1);
    }
    if (started) gfx_window_apply_settings();
    started = 1;
}

void gfx_window_request_close(void) {
    if (gfx_window_handle()) post(EV_CLOSE, NULL);
}

// ---- refresh rate and size of the window's display (MP4_FPS_LIMIT=display, the menu's size list). The window thread
// measures them when the window opens, lands on another display or a display mode changes; readers take the stored
// values. SDL gives the exact rational rate (59.94, 143.998) when the system does.
static SDL_DisplayID g_rate_disp;
static int32_t g_rate_mhz;   // millihertz, 0 unknown
static int32_t g_mon_w, g_mon_h;

// Window thread. force: a display mode changed, measure even on the same display.
static void refresh_rate_update(int force) {
    SDL_DisplayID d = SDL_GetDisplayForWindow(g_win);
    if (!force && d == g_rate_disp) return;
    g_rate_disp = d;
    const SDL_DisplayMode *m = d ? SDL_GetCurrentDisplayMode(d) : NULL;
    double hz = 0;
    if (m && m->refresh_rate_numerator > 0 && m->refresh_rate_denominator > 0)
        hz = (double)m->refresh_rate_numerator / m->refresh_rate_denominator;
    else if (m)
        hz = m->refresh_rate;   // 0: unspecified
    if (m) {
        float pd = m->pixel_density > 0 ? m->pixel_density : 1;
        __atomic_store_n(&g_mon_w, (int32_t)(m->w * pd + 0.5f), __ATOMIC_RELAXED);
        __atomic_store_n(&g_mon_h, (int32_t)(m->h * pd + 0.5f), __ATOMIC_RELAXED);
    }
    int32_t mhz = (int32_t)(hz * 1000 + 0.5);
    if (plat_atomic_xchg(&g_rate_mhz, mhz) != mhz) rt_log("gfx: display refresh rate %.3f Hz", hz);
}

double gfx_window_refresh_hz(void) { return __atomic_load_n(&g_rate_mhz, __ATOMIC_RELAXED) / 1000.0; }

int gfx_window_monitor_size(int *w, int *h) {
    int mw = __atomic_load_n(&g_mon_w, __ATOMIC_RELAXED), mh = __atomic_load_n(&g_mon_h, __ATOMIC_RELAXED);
    if (!gfx_window_handle() || mw <= 0 || mh <= 0) return 0;
    *w = mw;
    *h = mh;
    return 1;
}

// The process ends with the window, as in MNG, once a save Commit still running in the background is done. _exit, as
// ExitProcess did: no atexit handlers (prof_write is called here instead).
static void window_close(void) {
    rt_log("gfx: window closed; exiting");
    fs_save_flush();
    vk_pipeline_save();
    prof_write();
    fflush(stdout);
    fflush(stderr);
    _exit(0);
}

static void focus_changed(int in) {
    rt_log("gfx: window %s focus at frame %llu", in ? "gains" : "loses", (unsigned long long)gfx_stats().frames);
    plat_atomic_xchg(&g_focused, in);
    oe_set_focus(in);                          // pinned to 1 by MP4_ASSUME_FOCUS inside
    oe_push_message(OE_MSG_FOCUS_STATE_CHANGED);
    plat_atomic_inc(&g_focus_msgs);
    if (in && g_fs_pending) {
        g_fs_pending = 0;
        post(EV_FULLSCREEN, NULL);
    }
}

static void window_event(const SDL_Event *e) {
    if (e->type >= g_ev && e->type < g_ev + EV_COUNT) {
        switch (e->type - g_ev) {
        case EV_CALL: {
            WindowCall *c = e->user.data1;
            c->fn(c->arg);
            plat_mutex_lock(&g_call_lock);
            c->done = 1;
            plat_cond_broadcast(&g_call_cv);
            plat_mutex_unlock(&g_call_lock);
            return;
        }
        case EV_SETTINGS:
            if (g_win) apply_settings();
            return;
        case EV_FULLSCREEN:   // the fullscreen setting, held back until the window is first activated
            if (g_win && cfg_fullscreen() && !is_fullscreen()) {
                input_cursor_suspend(1);
                toggle_fullscreen();
                input_cursor_suspend(0);
            }
            return;
        case EV_CURSOR:
            input_cursor_apply();
            return;
        case EV_CLOSE:
            window_close();
            return;
        }
    }
    if (input_window_event(e)) return;
    if (menu_window_event(e)) return;
    if (!g_win) {
        if (e->type == SDL_EVENT_QUIT) window_close();   // SIGINT / SIGTERM before the window opened
        return;
    }
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat) break;
        if (e->key.key == SDLK_F11) {
            input_cursor_suspend(1);
            toggle_fullscreen();
            input_cursor_suspend(0);
            cfg_set("MP4_FULLSCREEN", is_fullscreen() ? "1" : "0");   // the menu shows it; the next start keeps it
            cfg_save();
            note_applied();   // what cfg_get reads now (the environment may still override it)
        }
        if (e->key.key == SDLK_F8) prof_toggle();   // MP4_PROFILE_FRAMES=key
        if (e->key.key == SDLK_P) {
            rt_shot_ring_dump();   // MP4_SHOT_RING: the last presents to PNGs
            vk_dump_arm(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED) + 2);   // MP4_DUMP=when: the frame after
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        window_sizes_update();
        plat_atomic_xchg(&g_dirty, 1);
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_HIDDEN:
        __atomic_store_n(&g_minimized, 1, __ATOMIC_RELEASE);
        keep_awake(0);
        break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_SHOWN:
        __atomic_store_n(&g_minimized, 0, __ATOMIC_RELEASE);
        keep_awake(1);
        window_sizes_update();
        plat_atomic_xchg(&g_dirty, 1);
        break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
        plat_atomic_xchg(&g_dirty, 1);
        break;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        if (g_pending_w) set_size_px(g_pending_w, g_pending_h);
        g_pending_w = g_pending_h = 0;
        plat_atomic_xchg(&g_dirty, 1);
        break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:   // moved onto another display
        refresh_rate_update(0);
        window_sizes_update();
        break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        window_sizes_update();
        break;
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
    case SDL_EVENT_DISPLAY_ADDED:
    case SDL_EVENT_DISPLAY_REMOVED:
        refresh_rate_update(1);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        focus_changed(1);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        focus_changed(0);
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_QUIT:   // also SIGINT / SIGTERM (SDL's handlers): Steam's quit ends the game through the save flush
        window_close();
        break;
    }
}

static PlatMutex g_video_lock = PLAT_MUTEX_INIT;
static PlatCond g_video_cv = PLAT_COND_INIT;

static void window_main(void *arg) {
    (void)arg;
    // The gamepad keeps working while another window has the focus (a test run in the background, a second monitor),
    // as it did before SDL had a window; Steam Input on the Deck reaches the game either way.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_APP_ID, "country-box");   // Wayland app id, X11 WM_CLASS: the desktop entry's icon
    int ok = SDL_InitSubSystem(SDL_INIT_VIDEO);
    if (!ok) rt_log("gfx: SDL video init failed (%s): no window", SDL_GetError());
    else if (!SDL_Vulkan_LoadLibrary(NULL)) rt_log("gfx: SDL cannot load Vulkan (%s): no window", SDL_GetError()), ok = 0;
    else if (!(g_vk_ext = SDL_Vulkan_GetInstanceExtensions(&g_vk_ext_n)))
        rt_log("gfx: no Vulkan surface extensions from SDL (%s): no window", SDL_GetError()), ok = 0;
    else if (!(g_ev = SDL_RegisterEvents(EV_COUNT))) rt_log("gfx: no SDL user events left: no window"), ok = 0;
    if (ok)
        rt_log("gfx: SDL %d.%d.%d video driver %s%s%s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
               SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
               SDL_GetCurrentVideoDriver(), under_gamescope() ? " under Gamescope" : "",
               SDL_IsMainThread() ? "" : " (warning: SDL does not take the window thread for its main thread)");
    g_wthread = pthread_self();
    plat_mutex_lock(&g_video_lock);
    __atomic_store_n(&g_video, ok ? 1 : -1, __ATOMIC_RELEASE);
    plat_cond_broadcast(&g_video_cv);
    plat_mutex_unlock(&g_video_lock);
    if (!ok) return;
    SDL_Event e;
    for (;;)
        if (SDL_WaitEvent(&e)) window_event(&e);
}

static void video_start(void) {
    PlatThread *t = plat_thread_new(window_main, NULL, "window");
    if (!t) rt_fatal("gfx: cannot start the window thread");
    plat_thread_detach(t);
    plat_mutex_lock(&g_video_lock);
    while (!__atomic_load_n(&g_video, __ATOMIC_ACQUIRE)) plat_cond_wait(&g_video_cv, &g_video_lock);
    plat_mutex_unlock(&g_video_lock);
}

// Starts the window thread once; 1 when SDL video is up.
static int video_up(void) {
    plat_once(&g_video_once, video_start);
    return __atomic_load_n(&g_video, __ATOMIC_ACQUIRE) > 0;
}

const char *const *gfx_window_vk_extensions(uint32_t *n) {
    if (!video_up()) {
        *n = 0;
        return NULL;
    }
    *n = g_vk_ext_n;
    return g_vk_ext;
}

// Window thread (window_call from gfx_window_start).
static void window_create(void *arg) {
    (void)arg;
    int cw, ch;
    cfg_window_size(&cw, &ch);
    // The window opens without taking the focus: it shows the game's blank loading frames for a while, and must not
    // take the keyboard from what the player works on meanwhile. The game still counts itself in focus until the
    // first focus event, so loading goes on, and keyboard input waits for the focus (input_sdl.c). It asks for
    // attention (the taskbar flashes) until it is brought up, and MP4_FULLSCREEN waits for that first activation.
    // MP4_BACKGROUND_WINDOW=1 (test runs): no attention request, never fullscreen at start. Under Gamescope it opens
    // fullscreen and focused at once: Gamescope gives the game the focus itself and has no taskbar.
    int background = g_background = cfg_get("MP4_BACKGROUND_WINDOW") && atoi(cfg_get("MP4_BACKGROUND_WINDOW"));
    int gs = under_gamescope();
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, gs ? "1" : "0");
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN |
                            (gs ? SDL_WINDOW_FULLSCREEN : 0);
    SDL_Window *w = SDL_CreateWindow("Country-Box", cw, ch, flags);
    if (!w) {
        rt_log("gfx: SDL_CreateWindow failed (%s)", SDL_GetError());
        g_wnd_failed = 1;
        return;
    }
    __atomic_store_n(&g_win, w, __ATOMIC_RELEASE);
    float d = SDL_GetWindowPixelDensity(w);
    if (d > 1.001f) set_size_px(cw, ch);   // MP4_WINDOW_SIZE is pixels; the size given above was points
    SDL_ShowWindow(w);
    SDL_SyncWindow(w);   // the size and fullscreen state are in place before the first swapchain
    if (!background && !gs) {
        SDL_FlashWindow(w, SDL_FLASH_UNTIL_FOCUSED);
        g_fs_pending = cfg_fullscreen();
    }
    note_applied();
    window_sizes_update();
    refresh_rate_update(1);
    keep_awake(1);
}

typedef struct { VkInstance instance; VkResult result; } SurfaceCall;

static void surface_create(void *arg) {
    SurfaceCall *s = arg;
    s->result = SDL_Vulkan_CreateSurface(g_win, s->instance, NULL, &g_surface) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED;
    if (s->result != VK_SUCCESS) rt_log("gfx: SDL_Vulkan_CreateSurface failed (%s)", SDL_GetError());
}

static void text_input_call(void *arg) {
    if (!g_win) return;
    if (arg) SDL_StartTextInput(g_win);
    else SDL_StopTextInput(g_win);
}

void gfx_window_text_input(int on) {
    if (gfx_window_handle()) window_call(text_input_call, on ? (void *)1 : NULL);
}

void gfx_window_cursor_sync(void) {
    if (!gfx_window_handle()) return;
    if (gfx_window_on_thread()) input_cursor_apply();
    else post(EV_CURSOR, NULL);
}

static void dispatch_call(void *arg) { window_event(arg); }

void gfx_window_dispatch(const SDL_Event *e) {
    if (gfx_window_handle()) window_call(dispatch_call, (void *)e);
}

// ---- swapchain
static const char *g_present_got = "";   // the MP4_VK_PRESENT value of the mode the swapchain got; "" before one

// MP4_VK_PRESENT (default mailbox, the original's policy): mailbox when the surface has it, else fifo. The game is paced
// by the vsync thread and the frame limiter, not by the present, so mailbox only replaces a queued image with a newer
// one and adds no frame of latency; fifo (the fallback, and what Gamescope's own pacing may leave) can hold one image
// more in the queue of minImageCount + 1.
static VkPresentModeKHR pick_present_mode(VkPhysicalDevice pd) {
    const char *e = cfg_value("MP4_VK_PRESENT");
    VkPresentModeKHR want = !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, g_surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, g_surface, &n, m);
    VkPresentModeKHR got = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) got = want;
    const char *name = got == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : got == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo";
    __atomic_store_n(&g_present_got, name, __ATOMIC_RELEASE);
    static int logged = -1;   // the mode last logged: a change from the menu shows in the log
    if ((int)got != logged) {
        logged = (int)got;
        rt_log("gfx: present mode %s (MP4_VK_PRESENT=%s)", name, e);
    }
    return got;
}

const char *gfx_window_present_mode(void) { return __atomic_load_n(&g_present_got, __ATOMIC_ACQUIRE); }

// Queues the present of a blit vk_present_blit left waiting (g_pl held). A swapchain rebuild and the next blit flush it
// first; the device release drops it.
static void flip_locked(void) {
    if (!g_flip_on) return;
    g_flip_on = 0;
    if (!g_sc) return;
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                           .pWaitSemaphores = &g_render_done[g_flip_idx], .swapchainCount = 1, .pSwapchains = &g_sc,
                           .pImageIndices = &g_flip_idx};
    vk_queue_lock();
    VkResult r = vkQueuePresentKHR(vk_ctx()->gfx_queue, &pi);
    vk_queue_unlock();
    vk_device_fault_report(r);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) plat_atomic_xchg(&g_dirty, 1);
}

static void free_capture(void) {
    VkCtx *c = vk_ctx();
    if (g_cap_buf && c) vmaDestroyBuffer(c->vma, g_cap_buf, g_cap_alloc);
    g_cap_buf = VK_NULL_HANDLE;
    g_cap_ptr = NULL;
    g_cap_size = 0;
    g_cap_valid = 0;
}

// Returns 1 with a usable swapchain, 0 when the window has no area (minimised). Called with g_pl held.
static int create_swapchain(void) {
    flip_locked();   // the acquired image goes out before its swapchain is replaced
    VkCtx *c = vk_ctx();
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->phys, g_surface, &caps) != VK_SUCCESS) return 0;
    // UINT32_MAX: the surface takes the swapchain's size (Wayland): the window's pixel size, as the window thread saw it
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX
                         ? caps.currentExtent
                         : (VkExtent2D){(uint32_t)__atomic_load_n(&g_px_w, __ATOMIC_ACQUIRE),
                                        (uint32_t)__atomic_load_n(&g_px_h, __ATOMIC_ACQUIRE)};
    if (!ext.width || !ext.height) return 0;
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, g_surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, g_surface, &nf, fmts);
    if (!nf) return 0;
    // UNORM: the blit writes the bytes the game wrote (no sRGB encode).
    VkSurfaceFormatKHR sf = fmts[0];
    int found = 0;
    for (uint32_t i = 0; i < nf && !found; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; found = 1; }
    for (uint32_t i = 0; i < nf && !found; i++)
        if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; found = 1; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;

    vk_queue_lock();
    vkDeviceWaitIdle(c->device);
    VkSwapchainKHR old = g_sc;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,   // every surface supports it (the menu overlay draws into it)
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = pick_present_mode(c->phys), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VkResult r = vkCreateSwapchainKHR(c->device, &ci, NULL, &g_sc);
    if (r != VK_SUCCESS) {
        rt_log("gfx: vkCreateSwapchainKHR failed (%d)", (int)r);
        g_sc = old;
        vk_queue_unlock();
        return 0;
    }
    for (uint32_t i = 0; i < g_sc_n; i++) {
        vkDestroySemaphore(c->device, g_render_done[i], NULL);
        if (g_sc_view[i]) vkDestroyImageView(c->device, g_sc_view[i], NULL);
        g_sc_view[i] = VK_NULL_HANDLE;
    }
    if (old) vkDestroySwapchainKHR(c->device, old, NULL);
    g_sc_fmt = sf.format;
    g_sc_ext = ext;
    g_sc_n = MAX_IMAGES;
    vkGetSwapchainImagesKHR(c->device, g_sc, &g_sc_n, g_sc_img);
    VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < g_sc_n; i++) {
        vkCreateSemaphore(c->device, &semi, NULL, &g_render_done[i]);
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_sc_img[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(c->device, &vi, NULL, &g_sc_view[i]) != VK_SUCCESS) g_sc_view[i] = VK_NULL_HANDLE;
    }
    free_capture();
    vk_queue_unlock();
    plat_atomic_xchg(&g_dirty, 0);
    return 1;
}

static void sync_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 s0,
                         VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = s0, .srcAccessMask = a0,
                               .dstStageMask = s1, .dstAccessMask = a1, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static int ensure_capture_buffer(void) {
    VkCtx *c = vk_ctx();
    VkDeviceSize need = (VkDeviceSize)g_sc_ext.width * g_sc_ext.height * 4;
    if (g_cap_buf && g_cap_size == need) return 1;
    free_capture();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = need, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    if (vmaCreateBuffer(c->vma, &bi, &ai, &g_cap_buf, &g_cap_alloc, &info) != VK_SUCCESS) { g_cap_buf = VK_NULL_HANDLE; return 0; }
    g_cap_ptr = info.pMappedData;
    g_cap_size = need;
    return g_cap_ptr != NULL;
}

void vk_thumbs_enable(int n) {
    pthread_mutex_lock(&g_pl);
    if (!g_th_n && n > 0 && (g_th_tag = calloc((size_t)n, sizeof *g_th_tag))) g_th_n = n;
    pthread_mutex_unlock(&g_pl);
}

void vk_thumbs_frame(uint64_t frame) {
    pthread_mutex_lock(&g_pl);
    g_th_frame = frame;
    pthread_mutex_unlock(&g_pl);
}

static void free_thumbs(void) {
    VkCtx *c = vk_ctx();
    if (g_th_img && c) vmaDestroyImage(c->vma, g_th_img, g_th_img_alloc);
    if (g_th_buf && c) vmaDestroyBuffer(c->vma, g_th_buf, g_th_alloc);
    g_th_img = VK_NULL_HANDLE, g_th_buf = VK_NULL_HANDLE, g_th_ptr = NULL;
    if (g_th_tag) memset(g_th_tag, 0, sizeof *g_th_tag * (size_t)g_th_n);
    memset(g_th_pending, 0, sizeof g_th_pending);
}

static int format_srgb(VkFormat f) {
    return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

// The sRGB format with the same bytes as a UNORM one, for the formats create_swapchain picks; else UNDEFINED.
static VkFormat srgb_twin(VkFormat f) {
    if (f == VK_FORMAT_B8G8R8A8_UNORM) return VK_FORMAT_B8G8R8A8_SRGB;
    if (f == VK_FORMAT_R8G8B8A8_UNORM) return VK_FORMAT_R8G8B8A8_SRGB;
    if (f == VK_FORMAT_A8B8G8R8_UNORM_PACK32) return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    return VK_FORMAT_UNDEFINED;
}

static VkFormat unorm_twin(VkFormat f) {
    if (f == VK_FORMAT_B8G8R8A8_SRGB) return VK_FORMAT_B8G8R8A8_UNORM;
    if (f == VK_FORMAT_R8G8B8A8_SRGB) return VK_FORMAT_R8G8B8A8_UNORM;
    if (f == VK_FORMAT_A8B8G8R8_SRGB_PACK32) return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    return f;
}

static void free_mid(void) {
    VkCtx *c = vk_ctx();
    if (g_mid && c) vmaDestroyImage(c->vma, g_mid, g_mid_alloc);
    g_mid = VK_NULL_HANDLE;
}

// The sRGB present's intermediate at the swapchain's size and format; made again when either changes. A failure
// (no blit-destination support for the format, no memory) leaves the plain blit for the session, logged once.
static int ensure_mid(void) {
    VkFormat f = srgb_twin(g_sc_fmt);
    if (g_mid_failed || f == VK_FORMAT_UNDEFINED) return 0;
    if (g_mid && g_mid_fmt == f && g_mid_ext.width == g_sc_ext.width && g_mid_ext.height == g_sc_ext.height) return 1;
    VkCtx *c = vk_ctx();
    if (g_mid) {   // a swapchain change: earlier presents may still read it
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        free_mid();
    }
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(c->phys, f, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {g_sc_ext.width, g_sc_ext.height, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if ((fp.optimalTilingFeatures & need) != need ||
        vmaCreateImage(c->vma, &ci, &ai, &g_mid, &g_mid_alloc, NULL) != VK_SUCCESS) {
        g_mid = VK_NULL_HANDLE;
        g_mid_failed = 1;
        rt_log("gfx: no %ux%u sRGB present image (format %d): sRGB frames are shown decoded", g_sc_ext.width,
               g_sc_ext.height, (int)f);
        return 0;
    }
    g_mid_fmt = f;
    g_mid_ext = g_sc_ext;
    return 1;
}

// The ring and the blit target in the swapchain's format; allocated on the first present, the image again when the
// swapchain format changes. A failure turns the thumbnails off for the session.
static int ensure_thumbs(void) {
    if (!g_th_n || g_th_failed || !g_th_frame) return 0;
    VkCtx *c = vk_ctx();
    if (!g_th_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = TH_BYTES * (VkDeviceSize)g_th_n,
                                 .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(c->vma, &bi, &ai, &g_th_buf, &g_th_alloc, &info) != VK_SUCCESS || !info.pMappedData) {
            if (g_th_buf) vmaDestroyBuffer(c->vma, g_th_buf, g_th_alloc);
            g_th_buf = VK_NULL_HANDLE;
            g_th_failed = 1;
            rt_log("gfx: flight thumbnails off: no host-visible ring of %llu MB", (unsigned long long)(bi.size >> 20));
            return 0;
        }
        g_th_ptr = info.pMappedData;
    }
    if (g_th_img && g_th_fmt != g_sc_fmt) {
        vmaDestroyImage(c->vma, g_th_img, g_th_img_alloc);   // the previous present's fence was waited for: idle
        g_th_img = VK_NULL_HANDLE;
    }
    if (!g_th_img) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = g_sc_fmt,
                                .extent = {FLIGHT_THUMB_W, FLIGHT_THUMB_H, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &g_th_img, &g_th_img_alloc, NULL) != VK_SUCCESS) {
            g_th_img = VK_NULL_HANDLE;
            g_th_failed = 1;
            rt_log("gfx: flight thumbnails off: no %ux%u image", FLIGHT_THUMB_W, FLIGHT_THUMB_H);
            return 0;
        }
        g_th_fmt = g_sc_fmt;
    }
    return 1;
}

// dst (TRANSFER_SRC, the swapchain image just written) -> thumbnail image -> ring slot of g_th_frame.
static void record_thumb(VkCommandBuffer cb, VkImage dst, unsigned fence_slot) {
    uint64_t f = g_th_frame;
    unsigned ring = (unsigned)(f % (uint64_t)g_th_n);
    g_th_tag[ring] = 0;   // under g_pl: vk_thumbs_read no longer hands this slot out
    sync_barrier(cb, g_th_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)g_sc_ext.width, (int)g_sc_ext.height, 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {FLIGHT_THUMB_W, FLIGHT_THUMB_H, 1}}};
    vkCmdBlitImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_th_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                   VK_FILTER_LINEAR);
    sync_barrier(cb, g_th_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy bc = {.bufferOffset = TH_BYTES * ring, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {FLIGHT_THUMB_W, FLIGHT_THUMB_H, 1}};
    vkCmdCopyImageToBuffer(cb, g_th_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_th_buf, 1, &bc);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &d);
    g_th_pending[fence_slot] = f;
    g_th_frame = 0;
}

int vk_thumbs_read(uint64_t frame, uint8_t *rgba) {
    int ok = 0;
    pthread_mutex_lock(&g_pl);
    if (g_th_n && g_th_ptr && frame) {
        unsigned ring = (unsigned)(frame % (uint64_t)g_th_n);
        if (g_th_tag[ring] == frame) {
            VkCtx *c = vk_ctx();
            vmaInvalidateAllocation(c->vma, g_th_alloc, TH_BYTES * ring, TH_BYTES);
            memcpy(rgba, g_th_ptr + TH_BYTES * ring, TH_BYTES);
            ok = g_th_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_th_fmt == VK_FORMAT_B8G8R8A8_SRGB ? 2 : 1;
        }
    }
    pthread_mutex_unlock(&g_pl);
    if (ok == 2)
        for (size_t i = 0; i < TH_BYTES; i += 4) {
            uint8_t t = rgba[i];
            rgba[i] = rgba[i + 2], rgba[i + 2] = t;
        }
    return ok != 0;
}

void vk_present_capture(int on) {
    pthread_mutex_lock(&g_pl);
    g_capture = on;
    if (!on) g_cap_valid = 0;
    pthread_mutex_unlock(&g_pl);
}

int vk_present_captured(const uint8_t **px, int *w, int *h, VkFormat *fmt) {
    pthread_mutex_lock(&g_pl);
    int ok = g_cap_valid;
    if (ok) { *px = g_cap_ptr; *w = g_cap_w; *h = g_cap_h; *fmt = g_sc_fmt; }
    pthread_mutex_unlock(&g_pl);
    return ok;
}

// The captured swapchain image as a PNG (the menu's test hook), RGBA with opaque alpha.
static void save_capture(const char *path) {
    size_t n = (size_t)g_cap_w * (size_t)g_cap_h * 4;
    uint8_t *px = malloc(n);
    if (!px) return;
    int bgra = g_sc_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_sc_fmt == VK_FORMAT_B8G8R8A8_SRGB;
    for (size_t i = 0; i < n; i += 4) {
        px[i] = g_cap_ptr[i + (bgra ? 2 : 0)];
        px[i + 1] = g_cap_ptr[i + 1];
        px[i + 2] = g_cap_ptr[i + (bgra ? 0 : 2)];
        px[i + 3] = 255;
    }
    rt_write_png(path, px, (uint32_t)g_cap_w, (uint32_t)g_cap_h);
    free(px);
    rt_log("gfx: menu test shot %s (%dx%d)", path, g_cap_w, g_cap_h);
}

// Where the game's picture (cw x ch) goes on the sw x sh swapchain: MP4_ASPECT=fit (default) keeps its shape and centres
// it, with black bars at the sides (a 16:9 picture in a 21:9 window) or above and below; stretch fills the swapchain.
// Bars under a pixel are not drawn. Returns 1 when the rectangle leaves bars to clear.
static int fit_rect(int cw, int ch, int sw, int sh, int out[4]) {
    out[0] = 0, out[1] = 0, out[2] = sw, out[3] = sh;
    const char *mode = cfg_value("MP4_ASPECT");   // native: the game draws the panel's shape; it keeps its shape too
    if (cw <= 0 || ch <= 0 || (strcmp(mode, "fit") && strcmp(mode, "native"))) return 0;
    long long a = (long long)cw * sh, b = (long long)ch * sw;
    if (a < b) {   // narrower than the window: bars at the sides
        int fw = (int)((a + ch / 2) / ch);
        if (fw >= sw - 1) return 0;
        out[0] = (sw - fw) / 2, out[2] = fw;
    } else if (a > b) {   // wider: bars above and below
        int fh = (int)(((long long)ch * sw + cw / 2) / cw);
        if (fh >= sh - 1) return 0;
        out[1] = (sh - fh) / 2, out[3] = fh;
    } else {
        return 0;
    }
    return 1;
}

static void present_image(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4], int defer) {
    int general = layout == VK_IMAGE_LAYOUT_GENERAL;
    gfx_window_start();
    VkCtx *c = vk_ctx();
    pthread_mutex_lock(&g_pl);
    flip_locked();
    int minimized = __atomic_load_n(&g_minimized, __ATOMIC_ACQUIRE);
    if (!g_sc || __atomic_load_n(&g_dirty, __ATOMIC_ACQUIRE)) {
        if (!minimized && !create_swapchain() && !g_sc) goto out;
    }
    if (minimized || !g_sc) goto out;

    unsigned slot = g_frame % FRAMES;
    if (vkWaitForFences(c->device, 1, &g_fence[slot], VK_TRUE, 5000000000ull) != VK_SUCCESS) {
        rt_log("gfx: present fence wait timed out");
        goto out;
    }
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(c->device, g_sc, 1000000000ull, g_acquire[slot], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        plat_atomic_xchg(&g_dirty, 1);
        if (!create_swapchain()) goto out;
        r = vkAcquireNextImageKHR(c->device, g_sc, 1000000000ull, g_acquire[slot], VK_NULL_HANDLE, &idx);
    }
    vk_device_fault_report(r);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) goto out;     // lost, not ready: skip the frame
    if (r == VK_SUBOPTIMAL_KHR) plat_atomic_xchg(&g_dirty, 1);
    vkResetFences(c->device, 1, &g_fence[slot]);
    if (g_th_pending[slot]) {   // that fence has signalled: the thumbnail its submission wrote is complete
        g_th_tag[g_th_pending[slot] % (uint64_t)g_th_n] = g_th_pending[slot];
        g_th_pending[slot] = 0;
    }

    int cx = 0, cy = 0, cw = w, ch = h;
    if (crop && crop[2] > 0 && crop[3] > 0) { cx = crop[0]; cy = crop[1]; cw = crop[2]; ch = crop[3]; }
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > w) cw = w - cx;
    if (cy + ch > h) ch = h - cy;
    window_settings_live();
    // The menu overlay: laid out before recording (its first frame starts the ImGui back end and uploads the font)
    const char *menu_shot = NULL;
    MenuTarget mt = {c->instance, c->phys, c->device, c->gfx_family, c->gfx_queue, g_sc_fmt, g_sc_ext.width,
                     g_sc_ext.height, cw, ch, g_scaler};
    int overlay = g_sc_view[idx] &&
                  menu_overlay_frame(__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED), &mt, &menu_shot);
    int cap = (g_capture || menu_shot) && ensure_capture_buffer();
    int thumb = g_th_n && ensure_thumbs();

    VkCommandBuffer cb = g_cb[slot];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    uint64_t crumb = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    vk_crumb(cb, VK_CRUMB_PRESENT, crumb);
    VkImage dst = g_sc_img[idx];
    sync_barrier(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                 VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const int rect[4] = {cx, cy, cw, ch};
    int to[4];
    if (fit_rect(cw, ch, (int)g_sc_ext.width, (int)g_sc_ext.height, to)) {
        // black bars: clear the whole image, then the picture's writes below wait for the clear
        VkClearColorValue black = {{0, 0, 0, 1}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
        sync_barrier(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    // An sRGB frame on a UNORM swapchain (create_swapchain picks UNORM so the bytes the game wrote reach the screen):
    // the blit decodes an sRGB source to linear and a UNORM destination stores that unencoded, which would show the
    // frame decoded twice (too dark, crushed blacks). FSR reads it through a UNORM view instead (the game's colour
    // images are MUTABLE_FORMAT, vk_texture.c), so EASU and RCAS work on the encoded values AMD tunes them for; the
    // blit writes the sRGB intermediate (decode, filter, encode) and a raw copy moves its bytes into the swapchain.
    if (!src) {   // Country-Box: a screen without a game image (vk_present_screen): a plain clear, the overlay draws on it
        VkClearColorValue bg = {{0.02f, 0.035f, 0.027f, 1}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &bg, 1, &all);
        g_scaler = 0;
        goto picture_done;
    }
    int srgb_frame = format_srgb(fmt) && !format_srgb(g_sc_fmt);
    // FSR (MP4_UPSCALER) when the picture's place on the swapchain is larger than the crop; else the bilinear blit
    int fsr = general && vk_fsr_record(cb, slot, src, srgb_frame ? unorm_twin(fmt) : fmt, w, h, rect, dst, to);
    g_scaler = fsr ? 2 : cw != to[2] || ch != to[3];
    if (!fsr) {
        int mid = srgb_frame && ensure_mid();
        VkImage bdst = mid ? g_mid : dst;
        if (mid)   // the previous present's copy out of it is done before this blit writes it (one queue)
            sync_barrier(cb, g_mid, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (general)   // a GENERAL source (vk_texture.c images): earlier submissions' writes become visible to the blit here
            sync_barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        // NVN's lower-left window origin (nvn_api_core.c): the frame sits bottom-up in memory and the display turns
        // it over, which the blit does by reading the rows in reverse (a negative source extent).
        int y0 = cy, y1 = cy + ch;
        if (nvn_origin_lower_left()) y0 = cy + ch, y1 = cy;
        VkImageBlit region = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{cx, y0, 0}, {cx + cw, y1, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{to[0], to[1], 0}, {to[0] + to[2], to[1] + to[3], 1}}};
        vkCmdBlitImage(cb, src, general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bdst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
        if (general)   // later submissions that write the source wait for the blit's read
            sync_barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0);
        if (mid) {   // same texel size: a raw copy, the encoded bytes unchanged
            sync_barrier(cb, g_mid, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT);
            VkImageCopy cp = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffset = {to[0], to[1], 0},
                              .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffset = {to[0], to[1], 0},
                              .extent = {(uint32_t)to[2], (uint32_t)to[3], 1}};
            vkCmdCopyImage(cb, g_mid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        }
    }
picture_done:;
    // last: the image's layout, and the stage and access of its last write, as each step leaves it
    VkImageLayout last = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkPipelineStageFlags2 last_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    VkAccessFlags2 last_write = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    if (overlay) {
        sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, last_stage, last_write,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_sc_view[idx],
                                        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_sc_ext}, .layerCount = 1,
                              .colorAttachmentCount = 1, .pColorAttachments = &ca};
        vkCmdBeginRendering(cb, &ri);
        menu_overlay_draw(cb);
        vkCmdEndRendering(cb);
        last = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        last_write = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    }
    if (cap) {
        sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, last_stage, last_write,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {g_sc_ext.width, g_sc_ext.height, 1}};
        vkCmdCopyImageToBuffer(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_cap_buf, 1, &bc);
        last = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        last_write = 0;
    }
    if (thumb) {
        if (last != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, last_stage, last_write,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        last = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        last_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        last_write = 0;
        record_thumb(cb, dst, slot);
    }
    sync_barrier(cb, dst, last, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, last_stage, last_write, VK_PIPELINE_STAGE_2_NONE, 0);
    vk_crumb(cb, VK_CRUMB_PRESENT_END, crumb);
    vkEndCommandBuffer(cb);

    VkSemaphoreSubmitInfo wait = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_acquire[slot],
                                  .stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_render_done[idx],
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &wait,
                        .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi, .signalSemaphoreInfoCount = 1,
                        .pSignalSemaphoreInfos = &sig};
    vk_queue_lock();
    r = vkQueueSubmit2(c->gfx_queue, 1, &si, g_fence[slot]);
    vk_queue_unlock();
    if (r == VK_SUCCESS) {
        g_flip_on = 1;
        g_flip_idx = idx;
        if (!defer) flip_locked();
    } else {
        vk_device_fault_report(r);
        rt_log("gfx: present submit failed (%d)", (int)r);
    }
    g_frame++;
    if (cap) {
        vkWaitForFences(c->device, 1, &g_fence[slot], VK_TRUE, 5000000000ull);
        vmaInvalidateAllocation(c->vma, g_cap_alloc, 0, VK_WHOLE_SIZE);
        g_cap_w = (int)g_sc_ext.width;
        g_cap_h = (int)g_sc_ext.height;
        g_cap_valid = 1;
        if (menu_shot) save_capture(menu_shot);
    }
out:
    pthread_mutex_unlock(&g_pl);
}

void vk_present_image(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]) {
    present_image(src, layout, fmt, w, h, crop, 0);
}

void vk_present_blit(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]) {
    present_image(src, layout, fmt, w, h, crop, 1);
}

// Country-Box: a present with no game image, only the overlay over a dark clear (the shader preparation screen,
// ui/prep_screen.cpp). Starts the window like any present.
void vk_present_screen(void) {
    gfx_window_start();
    pthread_mutex_lock(&g_pl);
    VkExtent2D e = g_sc_ext;
    pthread_mutex_unlock(&g_pl);
    present_image(VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_FORMAT_UNDEFINED, e.width ? (int)e.width : 1,
                  e.height ? (int)e.height : 1, NULL, 0);
}

void vk_present_flip(void) {
    pthread_mutex_lock(&g_pl);
    flip_locked();
    pthread_mutex_unlock(&g_pl);
}

typedef struct { int w, h; } ResizeCall;

static void resize_call(void *arg) {
    ResizeCall *r = arg;
    if (is_fullscreen()) SDL_SetWindowFullscreen(g_win, false);
    if (SDL_GetWindowFlags(g_win) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(g_win);
    set_size_px(r->w, r->h);
    SDL_SyncWindow(g_win);   // returns once the window system applied it (or SDL's timeout)
    window_sizes_update();
}

void gfx_window_resize(int cw, int chh) {
    gfx_window_start();
    ResizeCall r = {cw, chh};
    window_call(resize_call, &r);
    pthread_mutex_lock(&g_pl);
    create_swapchain();
    pthread_mutex_unlock(&g_pl);
}

// Called by vk_shutdown before the device goes away; the window and its thread stay.
void gfx_window_release_vk(void) {
    VkCtx *c = vk_ctx();
    pthread_mutex_lock(&g_pl);
    if (c && g_started) {
        vk_queue_lock();
        vkDeviceWaitIdle(c->device);
        vk_queue_unlock();
        g_flip_on = 0;   // its blit is done (device idle); the swapchain goes away unpresented
        menu_vk_release();
        vk_fsr_release();
        free_mid();
        free_capture();
        free_thumbs();
        for (int i = 0; i < FRAMES; i++) {
            vkDestroySemaphore(c->device, g_acquire[i], NULL);
            vkDestroyFence(c->device, g_fence[i], NULL);
        }
        for (uint32_t i = 0; i < g_sc_n; i++) {
            vkDestroySemaphore(c->device, g_render_done[i], NULL);
            if (g_sc_view[i]) vkDestroyImageView(c->device, g_sc_view[i], NULL);
            g_sc_view[i] = VK_NULL_HANDLE;
        }
        if (g_pool) vkDestroyCommandPool(c->device, g_pool, NULL);
        if (g_sc) vkDestroySwapchainKHR(c->device, g_sc, NULL);
        if (g_surface) vkDestroySurfaceKHR(c->instance, g_surface, NULL);
        g_sc = VK_NULL_HANDLE; g_surface = VK_NULL_HANDLE; g_pool = VK_NULL_HANDLE; g_sc_n = 0;
        g_started = 0;     // the window and its thread stay; the next gfx_window_start makes a new surface
    }
    pthread_mutex_unlock(&g_pl);
}

void gfx_window_start(void) {
    pthread_mutex_lock(&g_start_lock);
    if (!g_started) {
        if (!vk_init()) rt_fatal("gfx: no usable Vulkan device (see the vk: lines above)");
        VkCtx *c = vk_ctx();
        if (!gfx_window_handle()) {
            if (!video_up()) rt_fatal("gfx: no SDL video (see the gfx: lines above): no window to present to");
            g_wnd_failed = 0;
            if (!window_call(window_create, NULL) || g_wnd_failed) rt_fatal("gfx: window creation failed");
        }
        SurfaceCall sc = {c->instance, VK_ERROR_INITIALIZATION_FAILED};
        window_call(surface_create, &sc);
        if (sc.result != VK_SUCCESS) rt_fatal("gfx: no Vulkan surface for the window");

        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
        vkCreateCommandPool(c->device, &pci, NULL, &g_pool);
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = FRAMES};
        vkAllocateCommandBuffers(c->device, &ai, g_cb);
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkSemaphoreCreateInfo semi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (int i = 0; i < FRAMES; i++) {
            vkCreateFence(c->device, &fi, NULL, &g_fence[i]);
            vkCreateSemaphore(c->device, &semi, NULL, &g_acquire[i]);
        }
        g_frame = 0;
        g_started = 1;
        pthread_mutex_lock(&g_pl);
        create_swapchain();
        pthread_mutex_unlock(&g_pl);
        rt_log("gfx: window %dx%d pixels at %u%% display scale, swapchain %ux%u format %d",
               __atomic_load_n(&g_px_w, __ATOMIC_ACQUIRE), __atomic_load_n(&g_px_h, __ATOMIC_ACQUIRE),
               gfx_window_dpi() * 100u / 96u, g_sc_ext.width, g_sc_ext.height, (int)g_sc_fmt);
    }
    pthread_mutex_unlock(&g_start_lock);
    vsync_ensure();
}
