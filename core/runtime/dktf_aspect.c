// Country-Box: native 16:10 for Donkey Kong Country: Tropical Freeze (1.0.0), MP4_ASPECT=native (read at start): the
// game renders the panel's shape instead of a 16:9 picture shown with bars.
//
// How the game sizes its picture (measured, offsets main+): IGraphicsContext::BuildContext (+0x771a8) sizes the NVN
// graphics context from COsContext::GetScreenWidth/Height (+0x1f62c/+0x1f634: 1920x1080, the docked picture) and
// GetHandheldFullWidth/Height, GetHandheldContentWidth/Height (+0x1f63c..+0x1f654: 1152x648, handheld); each is a
// constant in a two-instruction getter. CGameArchitecture and CScaleformFlashPlayer::CreateTextManager read the same
// getters. The main view's projection is built in CDisplayManager::UpdateCameraNodes and GetCameraFrustumForVisibility
// with SProjectionState::BuildPerspective (+0x85788: vertical FOV, aspect, near, far), the aspect from the display
// manager (16:9); gameplay code (camera pull-back and framing rules, screen-edge teleports) reads it through
// CDisplayManager::GetAspectRatio (+0x2c9bdc).
//
// Here: the getters answer the same widths with 16:10 heights (1920x1200 docked, 1152x720 handheld); the two main-view
// projections get the 16:10 aspect and a vertical FOV widened so the horizontal FOV stays the game's (more of the
// scene above and below, the same left to right); GetAspectRatio keeps answering 16:9, so the game's camera logic
// frames the level as on the console.
//
// The HUD needs no hook of ours (measured, 2026-11-12, A/B at 1920x1200, runs hud-04/05/06): Scaleform already
// letterboxes its 16:9-authored stage centred in the taller buffer, so the HUD (CHUDIOWin: hearts, banana counter)
// renders at 16:9 size shifted +60 px; rewriting CScaleformFlashMovieInstance::SetViewport (main+0x6598f4, the one
// choke point, called with the whole (0,0,1920,1200) box from CFlashMovieInstanceManager::Draw main+0x650574 ~40
// times in play) to a centred 1920x1080 viewport changes not one visible pixel (shot diff ~1.0 vs no rewrite; the
// counter matches the 16:9 reference at scale 1.0/+60 px with diff 0.2/255). An earlier note claimed the HUD was
// stretched 11 % at 16:10 — false; that band hook was removed as dead code. Gaps that do exist and are being audited:
// full-screen backdrop movies that are not Scaleform overlays (the loading screen draws only a small corner element,
// rows ~1013-1082, everything else black). CB_ASPECT_TRACE=1 logs projections and viewports.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"

#define OFF_SCREEN_W 0x1f62cull          // COsContext::GetScreenWidth()
#define OFF_SCREEN_H 0x1f634ull          // COsContext::GetScreenHeight()
#define OFF_HH_FULL_W 0x1f63cull         // COsContext::GetHandheldFullWidth()
#define OFF_HH_FULL_H 0x1f644ull
#define OFF_HH_CONTENT_W 0x1f64cull      // COsContext::GetHandheldContentWidth()
#define OFF_HH_CONTENT_H 0x1f654ull
#define OFF_BUILD_PERSPECTIVE 0x85788ull // SProjectionState::BuildPerspective(CAbsAngle const&, float, float, float)
#define OFF_GET_ASPECT 0x2c9bdcull       // CDisplayManager::GetAspectRatio() const
#define RET_UPDATE_CAMERA 0x2c9900ull    // return addresses of the main view's two BuildPerspective calls
#define RET_VISIBILITY 0x2c9f04ull

#define GAME_ASPECT (16.0f / 9.0f)
#define NATIVE_ASPECT (16.0f / 10.0f)

static uint64_t g_main;
static int g_trace;

static int answer(Cpu *c, void *arg) {
    ret_x(c, (uint64_t)(uintptr_t)arg);
    return 1;
}

static int hook_persp(Cpu *c, void *arg) {
    (void)arg;
    uint64_t ret = c->x[30] - g_main;
    if (ret != RET_UPDATE_CAMERA && ret != RET_VISIBILITY) return 0;
    static __thread float fov;   // the widened angle, read through x0 by the body
    uint32_t w = LD32(arg_x(c, 0));
    float v, a;
    memcpy(&v, &w, 4), memcpy(&a, &c->v[0], 4);
    fov = 2.0f * atanf(tanf(v * 0.5f) * GAME_ASPECT / NATIVE_ASPECT);
    c->x[0] = (uint64_t)(uintptr_t)&fov;
    a = NATIVE_ASPECT;
    memcpy(&c->v[0], &a, 4);
    static int n;
    if (g_trace && n++ < 4)
        rt_log("aspect: main view from main+%#llx: vertical FOV %.4f -> %.4f, aspect %.4f", (unsigned long long)ret, v, fov, a);
    return 0;
}

static int hook_get_aspect(Cpu *c, void *arg) {
    (void)arg;
    float a = GAME_ASPECT;
    memset(&c->v[0], 0, sizeof c->v[0]);
    memcpy(&c->v[0], &a, 4);
    return 1;
}

// SetViewport is left alone: Scaleform letterboxes the movie's 16:9 stage by itself (see the file header).
int dktf_aspect_native(void) {
    const char *v = cfg_value("MP4_ASPECT");
    return v && !strcmp(v, "native");
}

void dktf_aspect_init(uint64_t main_base) {
    g_main = main_base;
    g_trace = getenv("CB_ASPECT_TRACE") != NULL;
    if (!dktf_aspect_native()) return;
    rt_entry_hook_set(g_main + OFF_SCREEN_W, answer, (void *)(uintptr_t)1920);
    rt_entry_hook_set(g_main + OFF_SCREEN_H, answer, (void *)(uintptr_t)1200);
    rt_entry_hook_set(g_main + OFF_HH_FULL_W, answer, (void *)(uintptr_t)1152);
    rt_entry_hook_set(g_main + OFF_HH_FULL_H, answer, (void *)(uintptr_t)720);
    rt_entry_hook_set(g_main + OFF_HH_CONTENT_W, answer, (void *)(uintptr_t)1152);
    rt_entry_hook_set(g_main + OFF_HH_CONTENT_H, answer, (void *)(uintptr_t)720);
    rt_entry_hook_set(g_main + OFF_BUILD_PERSPECTIVE, hook_persp, NULL);
    rt_entry_hook_set(g_main + OFF_GET_ASPECT, hook_get_aspect, NULL);
    rt_log("aspect: native 16:10 (MP4_ASPECT=native): 1920x1200 docked, 1152x720 handheld, horizontal FOV kept");
}
