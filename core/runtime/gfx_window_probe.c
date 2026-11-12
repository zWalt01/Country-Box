// From fe3h-pc-recomp runtime/gfx_window_probe.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4; it also presents an sRGB source.
// From totk-pc-recomp runtime/gfx_window_probe.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied with totk renamed to fe3h only. Country-Box: synthetic SDL3 events through the window thread instead of Win32 messages (Linux); the exact readbacks run with MP4_ASPECT=stretch and the letterbox of MP4_ASPECT=fit is checked on its own, as a tiling compositor sizes the window.
// --probe-window: presents 300 frames of a cycling clear colour through vk_present_image, reads the swapchain image back
// and compares every pixel (a UNORM source, then an sRGB one whose bytes must reach the UNORM swapchain unchanged),
// measures the vsync rate, flips the focus state with synthetic SDL focus events (handled on the window thread,
// gfx_window_dispatch) and resizes the window. Run with MP4_VK_VALIDATION=1 to count validation errors.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "gfx.h"
#include "input.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "vk.h"

#define SRC_W 640
#define SRC_H 360
#define FRAMES 300

typedef struct {
    VkImage img;
    VmaAllocation alloc;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkFormat fmt;
} Source;

static int fail(const char *what) {
    printf("probe-window: FAILED (%s)\n", what);
    return 1;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 s0,
                    VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = s0, .srcAccessMask = a0,
                               .dstStageMask = s1, .dstAccessMask = a1, .oldLayout = from, .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

static int source_create(Source *s, VkFormat fmt) {
    VkCtx *c = vk_ctx();
    s->fmt = fmt;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt, .extent = {SRC_W, SRC_H, 1}, .mipLevels = 1, .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, &s->img, &s->alloc, NULL) != VK_SUCCESS) return 0;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = c->gfx_family};
    if (vkCreateCommandPool(c->device, &pi, NULL, &s->pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ba = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = s->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    if (vkAllocateCommandBuffers(c->device, &ba, &s->cb) != VK_SUCCESS) return 0;
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return vkCreateFence(c->device, &fi, NULL, &s->fence) == VK_SUCCESS;
}

static void source_destroy(Source *s) {
    VkCtx *c = vk_ctx();
    if (s->fence) vkDestroyFence(c->device, s->fence, NULL);
    if (s->pool) vkDestroyCommandPool(c->device, s->pool, NULL);
    if (s->img) vmaDestroyImage(c->vma, s->img, s->alloc);
}

// Clears the source to rgba and leaves it in TRANSFER_SRC_OPTIMAL; the GPU work is waited for here, which is stricter
// than the vk_present_image contract needs.
static int source_clear(Source *s, int first, const float rgba[4]) {
    VkCtx *c = vk_ctx();
    vkResetCommandBuffer(s->cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    barrier(s->cb, s->img, first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, first ? 0 : VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue cv;
    memcpy(cv.float32, rgba, sizeof cv.float32);
    VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(s->cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rng);
    barrier(s->cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT);
    vkEndCommandBuffer(s->cb);
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, s->fence);
    vk_queue_unlock();
    if (r != VK_SUCCESS) return 0;
    r = vkWaitForFences(c->device, 1, &s->fence, VK_TRUE, 5000000000ull);
    vkResetFences(c->device, 1, &s->fence);
    return r == VK_SUCCESS;
}

static uint8_t to_byte(float f) { return (uint8_t)(f * 255.0f + 0.5f); }

static float srgb_decode(float v) { return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f); }

static int bytes_close(uint32_t a, uint32_t b, int tol) {
    for (int k = 0; k < 4; k++) {
        int d = (int)((a >> (8 * k)) & 255) - (int)((b >> (8 * k)) & 255);
        if (d > tol || d < -tol) return 0;
    }
    return 1;
}

// Presents one colour and checks every pixel of the readback. Returns 0 when it matches. An sRGB source is cleared to
// the linear value of the wanted bytes (the clear encodes it); the encode may round one step either way.
static int present_and_check(Source *s, int first, int i, int *w_out, int *h_out) {
    float rgba[4] = {(float)((i * 7) & 255) / 255.0f, (float)((i * 13) & 255) / 255.0f, (float)((i * 29) & 255) / 255.0f, 1.0f};
    int srgb = s->fmt == VK_FORMAT_R8G8B8A8_SRGB;
    float clear[4] = {rgba[0], rgba[1], rgba[2], rgba[3]};
    if (srgb)
        for (int k = 0; k < 3; k++) clear[k] = srgb_decode(rgba[k]);
    if (!source_clear(s, first, clear)) return 1;
    vk_present_image(s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->fmt, SRC_W, SRC_H, NULL);
    const uint8_t *px;
    int w, h;
    VkFormat fmt;
    if (!vk_present_captured(&px, &w, &h, &fmt)) { printf("probe-window: frame %d was not presented\n", i); return 2; }
    uint8_t rgb[3] = {to_byte(rgba[0]), to_byte(rgba[1]), to_byte(rgba[2])};
    uint8_t want[4];
    if (fmt == VK_FORMAT_B8G8R8A8_UNORM) { want[0] = rgb[2]; want[1] = rgb[1]; want[2] = rgb[0]; }
    else if (fmt == VK_FORMAT_R8G8B8A8_UNORM) { want[0] = rgb[0]; want[1] = rgb[1]; want[2] = rgb[2]; }
    else { printf("probe-window: swapchain format %d is not UNORM RGBA/BGRA\n", (int)fmt); return 3; }
    want[3] = 255;
    uint32_t wv;
    memcpy(&wv, want, 4);
    const uint32_t *p = (const uint32_t *)px;
    for (long k = 0, n = (long)w * h; k < n; k++)
        if (srgb ? !bytes_close(p[k], wv, 1) : p[k] != wv) {
            printf("probe-window: frame %d pixel %ld is %08x, expected %08x\n", i, k, p[k], wv);
            return 4;
        }
    *w_out = w;
    *h_out = h;
    return 0;
}

// Country-Box: MP4_ASPECT=fit (the default) keeps the picture's shape: in a window of another shape the swapchain gets
// black bars (gfx_window.c fit_rect), and a tiling Wayland compositor (Hyprland) sizes the window to its tile, not to
// the 16:9 size asked for, so the exact readbacks above, which compare every swapchain pixel with the source colour, run
// with MP4_ASPECT=stretch (the bars were the "black presented pixel"). This checks fit itself: one centred rectangle of
// the source colour with the source's shape (within a pixel) filling the width or the height, black around it.
static int check_letterbox(Source *s, int i) {
    setenv("MP4_ASPECT", "fit", 1);
    float rgba[4] = {(float)((i * 7) & 255) / 255.0f, (float)((i * 13) & 255) / 255.0f, (float)((i * 29) & 255) / 255.0f, 1.0f};
    int ok = source_clear(s, 0, rgba);
    if (ok) vk_present_image(s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->fmt, SRC_W, SRC_H, NULL);
    const uint8_t *px;
    int w, h;
    VkFormat fmt;
    ok = ok && vk_present_captured(&px, &w, &h, &fmt) && (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_R8G8B8A8_UNORM);
    setenv("MP4_ASPECT", "stretch", 1);
    if (!ok) { printf("probe-window: letterbox frame not presented or not UNORM RGBA/BGRA\n"); return 1; }
    uint8_t rgb[3] = {to_byte(rgba[0]), to_byte(rgba[1]), to_byte(rgba[2])};
    uint8_t want[4] = {fmt == VK_FORMAT_B8G8R8A8_UNORM ? rgb[2] : rgb[0], rgb[1], fmt == VK_FORMAT_B8G8R8A8_UNORM ? rgb[0] : rgb[2], 255};
    uint32_t wv, black = 0xff000000u;
    memcpy(&wv, want, 4);
    const uint32_t *p = (const uint32_t *)px;
    int x0 = 0, x1 = w, y0 = 0, y1 = h;   // the colour's extent along the middle row and column
    while (x0 < w && p[(long)(h / 2) * w + x0] != wv) x0++;
    while (x1 > x0 && p[(long)(h / 2) * w + x1 - 1] != wv) x1--;
    while (y0 < h && p[(long)y0 * w + w / 2] != wv) y0++;
    while (y1 > y0 && p[(long)(y1 - 1) * w + w / 2] != wv) y1--;
    long bad = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) bad += p[(long)y * w + x] != (x >= x0 && x < x1 && y >= y0 && y < y1 ? wv : black);
    long shape = (long)(x1 - x0) * SRC_H - (long)(y1 - y0) * SRC_W;   // 0 for the source's shape
    int full = (x0 == 0 && x1 == w) || (y0 == 0 && y1 == h);
    int centred = abs(x0 - (w - x1)) <= 1 && abs(y0 - (h - y1)) <= 1;
    printf("probe-window: MP4_ASPECT=fit in a %dx%d swapchain: picture %dx%d at (%d,%d), %ld pixels off, %s\n", w, h,
           x1 - x0, y1 - y0, x0, y0, bad, (long)w * SRC_H == (long)h * SRC_W ? "no bars (16:9)" : "black bars");
    if (bad || !full || !centred || labs(shape) > (SRC_W > SRC_H ? SRC_W : SRC_H)) return 1;
    return 0;
}

// A synthetic event for the game window, run through the window thread's handling before this returns.
static void send(SDL_EventType type, int arg) {
    SDL_Event e;
    SDL_zero(e);
    e.type = type;
    SDL_WindowID id = SDL_GetWindowID(gfx_window_handle());
    if (type == SDL_EVENT_KEY_DOWN || type == SDL_EVENT_KEY_UP) {
        e.key.windowID = id;
        e.key.scancode = (SDL_Scancode)arg;
        e.key.key = SDL_GetKeyFromScancode((SDL_Scancode)arg, SDL_KMOD_NONE, false);
        e.key.down = type == SDL_EVENT_KEY_DOWN;
    } else if (type == SDL_EVENT_MOUSE_BUTTON_DOWN || type == SDL_EVENT_MOUSE_BUTTON_UP) {
        e.button.windowID = id;
        e.button.button = (Uint8)arg;
        e.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.clicks = 1;
    } else {
        e.window.windowID = id;
    }
    gfx_window_dispatch(&e);
}

int probe_window(void) {
    // the readbacks compare whole images: no performance overlay in its corner, whatever the settings file says (the
    // environment wins over it, cfg.c cfg_get, and is not saved)
    setenv("MP4_PERF_OVERLAY", "off", 1);
    setenv("MP4_ASPECT", "stretch", 1);   // Country-Box: every pixel is the picture (check_letterbox has fit)
    gfx_window_start();
    VkCtx *c = vk_ctx();
    if (!c) return fail("no Vulkan device");
    vk_present_capture(1);
    Source s = {0};
    if (!source_create(&s, VK_FORMAT_R8G8B8A8_UNORM)) return fail("source image");

    int w = 0, h = 0;
    for (int i = 0; i < FRAMES; i++)
        if (present_and_check(&s, i == 0, i, &w, &h)) return fail("presented pixels differ");
    printf("probe-window: %d presents, readback %dx%d exact\n", FRAMES, w, h);

    // The game's frames are sRGB images: the swapchain must show their bytes, not their decoded values.
    Source ss = {0};
    if (!source_create(&ss, VK_FORMAT_R8G8B8A8_SRGB)) return fail("sRGB source image");
    for (int i = 0; i < 64; i++)
        if (present_and_check(&ss, i == 0, i * 4, &w, &h)) return fail("sRGB frame shown decoded");
    source_destroy(&ss);
    printf("probe-window: 64 sRGB presents keep the frame's bytes (within 1)\n");

    // Focus: synthetic focus loss and gain, handled on the window thread before send returns.
    unsigned m0 = gfx_window_focus_messages();
    send(SDL_EVENT_WINDOW_FOCUS_LOST, 0);
    int off = gfx_window_focused();
    send(SDL_EVENT_WINDOW_FOCUS_GAINED, 0);
    int on = gfx_window_focused();
    unsigned m1 = gfx_window_focus_messages();
    if (off != 0 || on != 1 || m1 - m0 != 2) {
        printf("probe-window: focus off=%d on=%d messages=%u\n", off, on, m1 - m0);
        return fail("focus");
    }
    printf("probe-window: focus off/on pushed 2 messages (15)\n");

    // Cursor release: Esc releases, a click in the window recaptures, a focus change made without a held mouse button
    // (Alt+Tab) leaves it captured. SDL reports no click on the title bar (the Win32 probe's WM_MOUSEACTIVATE case);
    // input_sdl.c takes a focus gain with a button held for one, which a synthetic event cannot hold. The probe window
    // may not have the focus, so this checks the release flag, not the relative mouse mode.
    send(SDL_EVENT_KEY_DOWN, SDL_SCANCODE_ESCAPE);
    send(SDL_EVENT_KEY_UP, SDL_SCANCODE_ESCAPE);
    int esc = input_cursor_released();
    send(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
    send(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
    int click = input_cursor_released();
    send(SDL_EVENT_WINDOW_FOCUS_LOST, 0);
    send(SDL_EVENT_WINDOW_FOCUS_GAINED, 0);
    int back = input_cursor_released();
    if (menu_is_open()) menu_toggle();   // Esc opened the menu: the presents below compare the bare game image
    if (!esc || click || back) {
        printf("probe-window: released esc=%d click=%d refocused=%d\n", esc, click, back);
        return fail("cursor release");
    }
    printf("probe-window: cursor released by Esc, recaptured by a click, kept over a focus change\n");

    // Resize: the client area changes, the swapchain follows, presenting still reads back exact.
    gfx_window_resize(1280, 720);
    int cw = 0, ch = 0;
    SDL_GetWindowSizeInPixels(gfx_window_handle(), &cw, &ch);   // a read of SDL's copy; the probe thread only reads
    for (int i = 0; i < 10; i++)
        if (present_and_check(&s, 0, FRAMES + i, &w, &h)) return fail("presents after resize");
    if (w != cw || h != ch) {
        printf("probe-window: readback %dx%d, window %dx%d pixels\n", w, h, cw, ch);
        return fail("swapchain size after resize");
    }
    printf("probe-window: resized to %dx%d, presents still exact\n", w, h);
    if (check_letterbox(&s, FRAMES + 20)) return fail("MP4_ASPECT=fit letterbox");
    vk_present_capture(0);

    // Vsync: count ticks between two tick edges about 5 s apart.
    static volatile long ticks;
    gfx_vsync_bind_counter(&ticks);
    long a = ticks;
    while (ticks == a) plat_sleep_ns(1000000);
    uint64_t t0 = plat_now_ns();
    long n0 = ticks;
    plat_sleep_ns(5000000000ull);
    long b = ticks;
    while (ticks == b) plat_sleep_ns(1000000);
    uint64_t t1 = plat_now_ns();
    long n1 = ticks;
    double rate = (double)(n1 - n0) * 1e9 / (double)(t1 - t0);
    printf("probe-window: vsync %ld ticks in %.3f s = %.3f Hz\n", n1 - n0, (t1 - t0) / 1e9, rate);
    if (rate < 60.0 * 0.995 || rate > 60.0 * 1.005) return fail("vsync rate");

    source_destroy(&s);
    vk_shutdown();
    long errors = vk_validation_errors_total();
    if (errors) {
        printf("probe-window: FAILED (%ld validation errors)\n", errors);
        return 1;
    }
    printf("probe-window: passed (%d frames, %ld validation errors, vsync %.1f Hz)\n", FRAMES, errors, rate);
    return 0;
}
