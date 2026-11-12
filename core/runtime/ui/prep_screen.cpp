// Country-Box: the blocking "Preparing shaders" screen (ui/prep_screen.h). Our own layout over a plain dark
// background: the runtime's font (Roboto, ui/menu.cpp), the menu's gold accent, text only. Sized from the swapchain
// height like the menu, so it reads the same on a 1080p monitor and the Steam Deck's 800-line panel (Game Mode opens
// the window fullscreen, gfx_window.c).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
extern "C" {
#include "platform/platform.h"
}
#include "ui/prep_screen.h"

static PlatMutex g_lock = PLAT_MUTEX_INIT;
static volatile int g_active;
static char g_game[128];
static ShaderPrepProgress g_p;
static uint64_t g_t0;                                      // plat_now_ns at begin: the spinner's clock

extern "C" void prep_screen_begin(const char *game) {
    plat_mutex_lock(&g_lock);
    snprintf(g_game, sizeof g_game, "%s", game ? game : "");
    memset(&g_p, 0, sizeof g_p);
    g_p.eta_ms = -1;
    g_t0 = plat_now_ns();
    plat_mutex_unlock(&g_lock);
    __atomic_store_n(&g_active, 1, __ATOMIC_RELEASE);
}

extern "C" void prep_screen_set(const ShaderPrepProgress *p) {
    plat_mutex_lock(&g_lock);
    g_p = *p;
    plat_mutex_unlock(&g_lock);
}

extern "C" const char *prep_screen_shot(void) {
    static char path[512];
    static int taken;
    const char *e = getenv("CB_PREP_SHOT");
    if (taken || !e || !*e) return NULL;
    snprintf(path, sizeof path, "%s", e);
    char *colon = strrchr(path, ':');
    double ms = 3000;
    if (colon) *colon = 0, ms = atof(colon + 1);
    if ((plat_now_ns() - g_t0) / 1e6 < ms) return NULL;
    taken = 1;
    return path;
}

extern "C" void prep_screen_end(void) { __atomic_store_n(&g_active, 0, __ATOMIC_RELEASE); }

extern "C" int prep_screen_active(void) { return __atomic_load_n(&g_active, __ATOMIC_ACQUIRE); }

static void clock_text(double ms, char *out, size_t cap) {
    long s = ms > 0 ? (long)(ms / 1000 + 0.5) : 0;
    if (s >= 3600) snprintf(out, cap, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, cap, "%ld:%02ld", s / 60, s % 60);
}

static void centered(ImDrawList *dl, ImFont *f, float size, float cx, float y, ImU32 col, const char *t) {
    ImVec2 sz = f->CalcTextSizeA(size, 1e9f, 0, t);
    dl->AddText(f, size, ImVec2(floorf(cx - sz.x * 0.5f), floorf(y)), col, t);
}

extern "C" void prep_screen_layout(uint32_t w, uint32_t h, float scale) {
    ShaderPrepProgress p;
    char game[128];
    plat_mutex_lock(&g_lock);
    p = g_p;
    memcpy(game, g_game, sizeof game);
    double spin_s = (plat_now_ns() - g_t0) / 1e9;
    plat_mutex_unlock(&g_lock);

    const ImU32 gold = IM_COL32(237, 194, 84, 255), text = IM_COL32(236, 238, 232, 255),
                dim = IM_COL32(160, 172, 160, 255), track = IM_COL32(255, 255, 255, 28);
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    ImFont *f = ImGui::GetFont();
    float base = ImGui::GetFontSize();   // 26 px at 1080 lines
    float W = (float)w, H = (float)h, cx = W * 0.5f;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W, H), IM_COL32(14, 28, 20, 255), IM_COL32(14, 28, 20, 255),
                                IM_COL32(5, 9, 7, 255), IM_COL32(5, 9, 7, 255));

    // what the step is
    static const char *const steps[] = {"Getting ready", "Finding shaders in your game files", "Translating shaders",
                                        "Building pipelines", "Optimizing for your GPU", "Done"};
    int ph = p.phase < 0 ? 0 : p.phase >= PREP_PHASES ? PREP_PHASE_DONE : p.phase;
    int step = ph == PREP_PHASE_START ? 0 : ph == PREP_PHASE_DONE ? 4 : ph;   // 1..4 of 4
    double frac_phase = p.total > 0 ? (double)p.done / (double)p.total : 0;
    double frac = ph == PREP_PHASE_DONE ? 1 : p.eta_ms >= 0 && p.elapsed_ms > 0 ? p.elapsed_ms / (p.elapsed_ms + p.eta_ms) : 0;
    if (frac > 1) frac = 1;

    float y = H * 0.24f;
    centered(dl, f, base * 0.72f, cx, y, gold, "COUNTRY-BOX");
    y += base * 1.2f;
    centered(dl, f, base * 1.7f, cx, y, text, ph == PREP_PHASE_DONE ? (p.failed ? "Shader preparation stopped" : "Shaders ready")
                                                                   : "Preparing shaders");
    y += base * 2.1f;
    if (game[0]) centered(dl, f, base * 0.85f, cx, y, dim, game);
    y += base * 2.2f;

    // the bar: the whole preparation's share of time
    float bw = fminf(W * 0.62f, 1100 * scale), bh = fmaxf(10 * scale, 6), bx = cx - bw * 0.5f;
    dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + bw, y + bh), track, bh * 0.5f);
    if (frac > 0) dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + (float)(bw * frac), y + bh), gold, bh * 0.5f);
    else {   // no estimate yet: a moving segment
        float seg = bw * 0.18f, t = (float)fmod(spin_s * 0.6, 1.0), sx = bx + (bw - seg) * (0.5f - 0.5f * cosf(t * 6.2831853f));
        dl->AddRectFilled(ImVec2(sx, y), ImVec2(sx + seg, y + bh), gold, bh * 0.5f);
    }
    y += bh + base * 1.0f;

    char line[256];
    if (ph == PREP_PHASE_DONE) {
        snprintf(line, sizeof line, p.failed ? "The game starts anyway; levels may stutter the first time they load."
                                             : "Starting the game.");
    } else if (ph == PREP_PHASE_START) {
        snprintf(line, sizeof line, "%s", steps[0]);
    } else if (ph == PREP_PHASE_SCAN) {
        snprintf(line, sizeof line, "Step %d of 4  ·  %s  ·  %.0f / %.0f MB", step, steps[ph], p.done / 1048576.0,
                 p.total / 1048576.0);
    } else {
        snprintf(line, sizeof line, "Step %d of 4  ·  %s  ·  %lld / %lld", step, steps[ph], (long long)p.done,
                 (long long)p.total);
    }
    centered(dl, f, base, cx, y, text, line);
    y += base * 1.5f;

    // this step's own bar, thin
    if (ph != PREP_PHASE_START && ph != PREP_PHASE_DONE) {
        float tb = fmaxf(4 * scale, 3);
        dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + bw, y + tb), track, tb * 0.5f);
        dl->AddRectFilled(ImVec2(bx, y), ImVec2(bx + (float)(bw * frac_phase), y + tb), IM_COL32(237, 194, 84, 150), tb * 0.5f);
        y += tb + base * 1.0f;
    }

    char el[32], left[32];
    clock_text(p.elapsed_ms, el, sizeof el);
    if (ph == PREP_PHASE_DONE) snprintf(line, sizeof line, "Took %s", el);
    else if (p.eta_ms >= 0) clock_text(p.eta_ms, left, sizeof left), snprintf(line, sizeof line, "Elapsed %s  ·  About %s left", el, left);
    else snprintf(line, sizeof line, "Elapsed %s  ·  Estimating time left", el);
    centered(dl, f, base * 0.85f, cx, y, dim, line);

    // why, at the bottom
    float by = H - base * 4.2f;
    centered(dl, f, base * 0.72f, cx, by, dim,
             "This runs once after installing, and again when the graphics driver or the shader translator changes.");
    by += base * 1.1f;
    if (p.why && p.why[0]) {
        snprintf(line, sizeof line, "Reason: %s  ·  using %d threads  ·  the game starts by itself when it is done", p.why,
                 p.threads);
        centered(dl, f, base * 0.72f, cx, by, dim, line);
    }
}
