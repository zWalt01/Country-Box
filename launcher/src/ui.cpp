// Country-Box launcher: the hub's screens (ui.h). Layout in units of a 1280x800 window (the Steam Deck's panel), times
// the window's scale; everything is placed by hand and drawn with draw lists, the widgets are Dear ImGui items so the
// controller's navigation and the mouse both work on them.
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <unistd.h>

#include "art.h"
#include "imgui_internal.h"
#include "proc.h"
#include "update.h"

namespace fs = std::filesystem;
using art::alpha;
using art::mix;

static const ImU32 kPanel = IM_COL32(10, 30, 19, 225);
static const ImU32 kPanelEdge = IM_COL32(120, 84, 40, 255);
static const ImU32 kButton = IM_COL32(46, 112, 66, 255);
static const ImU32 kButtonDim = IM_COL32(40, 60, 46, 255);

static ImVec2 V(float x, float y) { return ImVec2(x, y); }

static bool nav_visible() { return GImGui->NavCursorVisible && GImGui->NavHighlightItemUnderNav; }

static float luminance(ImU32 c) {
    ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
    return 0.3f * v.x + 0.59f * v.y + 0.11f * v.z;
}

static std::string human_bytes(double b) {
    char t[32];
    if (b >= 1e9) snprintf(t, sizeof t, "%.1f GB", b / 1e9);
    else if (b >= 1e6) snprintf(t, sizeof t, "%.0f MB", b / 1e6);
    else snprintf(t, sizeof t, "%.0f KB", b / 1e3);
    return t;
}

// Text helpers on the window's draw list
static void text_at(ImFont *f, ImVec2 p, ImU32 col, const std::string &s, float wrap = 0) {
    ImGui::GetWindowDrawList()->AddText(f, f->FontSize, p, col, s.c_str(), nullptr, wrap);
}
static ImVec2 text_size(ImFont *f, const std::string &s, float wrap = 0) {
    return f->CalcTextSizeA(f->FontSize, 1e9f, wrap, s.c_str());
}
static void text_center(ImFont *f, float cx, float y, ImU32 col, const std::string &s) {
    text_at(f, V(cx - text_size(f, s).x * 0.5f, y), col, s);
}
// centered with a wrap width (Prepare's outcome lines can be long)
static void text_center(ImFont *f, float cx, float y, ImU32 col, const std::string &s, float wrap) {
    text_at(f, V(cx - text_size(f, s, wrap).x * 0.5f, y), col, s, wrap);
}
static void shadow_text(ImFont *f, ImVec2 p, ImU32 col, const std::string &s, float off) {
    text_at(f, V(p.x + off, p.y + off), IM_COL32(0, 0, 0, 150), s);
    text_at(f, p, col, s);
}
static void panel(ImVec2 mn, ImVec2 mx, float s, ImU32 fill = kPanel) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(V(mn.x + 6 * s, mn.y + 8 * s), V(mx.x + 6 * s, mx.y + 8 * s), IM_COL32(0, 0, 0, 90), 22 * s);
    dl->AddRectFilled(mn, mx, fill, 22 * s);
    dl->AddRect(mn, mx, kPanelEdge, 22 * s, 0, 3 * s);
}

bool Ui::focus_take() {
    if (focus_ <= 0) return false;
    focus_ = 0;
    return true;
}

bool Ui::busy() const {
    return job_ != nullptr || prep_ != nullptr || import_thread_.joinable() || update_ != nullptr || checker_ != nullptr ||
           lib_.any_running() || t_ < toast_until_;
}

Ui::~Ui() {
    if (prep_) prep_->cancel();
    if (update_) update_->cancel();
    if (import_thread_.joinable()) import_thread_.join();
}

void Ui::toast(const std::string &text, ImU32 col) {
    toast_ = text;
    toast_col_ = col;
    toast_until_ = t_ + 4.0f;
}

bool Ui::button(const char *id, const char *label, ImVec2 pos, ImVec2 size, ImU32 col, bool enabled, ImFont *font, bool focus_here) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginDisabled(!enabled);
    bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    ImGui::EndDisabled();
    if (focus_here || (enabled && focus_take())) {
        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
        GImGui->NavHighlightItemUnderNav = true;
    }
    bool hot = enabled && (ImGui::IsItemHovered() || (ImGui::IsItemFocused() && nav_visible()));
    bool held = enabled && ImGui::IsItemActive();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = std::min(size.y * 0.5f, 18 * s_);
    ImVec2 mn = pos, mx = V(pos.x + size.x, pos.y + size.y);
    ImU32 base = enabled ? col : kButtonDim;
    if (hot) base = mix(base, IM_COL32(255, 255, 255, 255), 0.12f);
    if (held) base = mix(base, IM_COL32(0, 0, 0, 255), 0.15f);
    if (hot) {
        dl->AddRect(V(mn.x - 5 * s_, mn.y - 5 * s_), V(mx.x + 5 * s_, mx.y + 5 * s_), alpha(art::kBanana, 0.35f), r + 5 * s_, 0, 6 * s_);
        dl->AddRect(V(mn.x - 3 * s_, mn.y - 3 * s_), V(mx.x + 3 * s_, mx.y + 3 * s_), art::kBanana, r + 3 * s_, 0, 3.5f * s_);
    }
    dl->AddRectFilled(V(mn.x, mn.y + 4 * s_), V(mx.x, mx.y + 4 * s_), IM_COL32(0, 0, 0, 80), r);
    dl->AddRectFilled(mn, mx, base, r);
    dl->AddRectFilled(V(mn.x + 3 * s_, mn.y + 3 * s_), V(mx.x - 3 * s_, mn.y + size.y * 0.48f), IM_COL32(255, 255, 255, enabled ? 34 : 12), r - 2 * s_,
                      ImDrawFlags_RoundCornersTop);
    dl->AddRect(mn, mx, mix(base, IM_COL32(0, 0, 0, 255), 0.45f), r, 0, 2 * s_);
    ImFont *f = font ? font : fonts.body;
    ImU32 tc = !enabled ? IM_COL32(150, 160, 150, 255) : luminance(col) > 0.55f ? art::kInk : art::kCream;
    std::string l = label;
    ImVec2 ts = text_size(f, l);
    text_at(f, V(mn.x + (size.x - ts.x) * 0.5f, mn.y + (size.y - ts.y) * 0.5f), tc, l);
    return pressed && enabled;
}

// ---- frame

void Ui::frame(float scale, float t) {
    s_ = scale;
    t_ = t;
    if (ImGui::GetFrameCount() <= 3) request_focus();   // after Dear ImGui's own first-frame focus on the window
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 W = io.DisplaySize;
    ImGui::SetNextWindowPos(V(0, 0));
    ImGui::SetNextWindowSize(W);
    ImGui::Begin("hub", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    art::background(ImGui::GetBackgroundDrawList(), V(0, 0), W, s_, t_);
    if (lib_.poll_running()) toast("The game closed.", art::kInfo);
    if (checker_ && checker_->done()) {
        update_info_ = checker_->info();
        checker_.reset();
        checked_ = true;
        if (update_info_.available && flow_ == Flow::None && !scripted_ &&
            update_ini_value(lib_.home(), "declined") != update_info_.version)
            show_update_ask();
        else if (!update_info_.err.empty() && tab_ == Tab::About)
            toast("Update check failed: " + update_info_.err, art::kBad);
    }
    if (update_ && update_->done()) {
        update_outcome_ = {update_->ok, update_->cancelled, update_->err, update_->from_version, update_->to_version,
                           update_->installed_dir, update_->backup_dir, update_->saved, update_->need_prepare};
        if (update_->ok && !update_outcome_.to.empty()) update_ini_set(lib_.home(), "declined", update_outcome_.to);
        update_.reset();
        flow_ = Flow::UpdateResult;
        request_focus();
    }
    if (job_ && job_->done()) {
        result_ = job_->result();
        result_game_ = job_->game();
        result_path_ = job_->path();
        job_.reset();
        flow_ = Flow::Result;
        if (result_.kind == VerifyResult::Ok) {
            if (Game *g = lib_.find(result_game_)) {
                lib_.set_copy(*g, result_);
                std::string err;
                if (!lib_.save(&err)) toast(err, art::kBad);
            }
        }
        request_focus();
    }
    // B / Escape goes back
    bool back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    if (back) {
        if (flow_ == Flow::Checking && job_) job_->cancel();
        else if (flow_ == Flow::Prepare && prep_ && !prep_->done()) prep_->cancel();
        else if (flow_ == Flow::Updating && update_) update_->cancel();
        else if (flow_ == Flow::UpdateAsk) decline_update();
        else if (flow_ != Flow::None) finish_flow();
        else if (tab_ != Tab::Games) set_tab(Tab::Games);
    }
    if (flow_ == Flow::None) {
        int dir = (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_PageDown, false)) -
                  (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_PageUp, false));
        if (dir) set_tab((Tab)(((int)tab_ + dir + 3) % 3));
    }

    float header_h = 150 * s_, tabs_y = 160 * s_, content_y = 244 * s_, footer_h = 58 * s_;
    draw_header(V(0, 0), V(W.x, header_h));
    ImVec2 cmn(0, content_y), cmx(W.x, W.y - footer_h);
    if (flow_ == Flow::None) {
        draw_tabs(tabs_y);
        if (tab_ == Tab::Games) draw_games(cmn, cmx);
        else if (tab_ == Tab::Settings) draw_settings(cmn, cmx);
        else draw_about(cmn, cmx);
    } else {
        ImVec2 fmn(std::max(40 * s_, W.x * 0.5f - 600 * s_), tabs_y), fmx(std::min(W.x - 40 * s_, W.x * 0.5f + 600 * s_), W.y - footer_h - 10 * s_);
        if (flow_ == Flow::Browse) draw_browse(fmn, fmx);
        else if (flow_ == Flow::Checking) draw_checking(fmn, fmx);
        else if (flow_ == Flow::Prepare) draw_prepare(fmn, fmx);
        else if (flow_ == Flow::Import) draw_import(fmn, fmx);
        else if (flow_ == Flow::Result) draw_result(fmn, fmx);
        else if (flow_ == Flow::UpdateAsk) draw_update_ask(fmn, fmx);
        else if (flow_ == Flow::Updating) draw_updating(fmn, fmx);
        else draw_update_result(fmn, fmx);
    }
    draw_footer(V(0, W.y - footer_h), W);
    if (lib_.any_running()) draw_running(V(0, 0), W);
    draw_toast(V(0, 0), W);
    ImGui::End();
}

void Ui::set_tab(Tab t) {
    if (t == tab_) return;
    tab_ = t;
    if (t == Tab::Settings) load_settings();
    if (t == Tab::Games) lib_.refresh();
    request_focus();
}

void Ui::finish_flow() {
    if (job_) job_->cancel(), job_.reset();
    if (prep_) prep_->cancel(), prep_.reset();
    if (import_thread_.joinable() && import_done_.load()) import_thread_.join();
    browse_import_ = false;
    if (update_) update_->cancel(), update_.reset();
    flow_ = Flow::None;
    lib_.refresh();
    request_focus();
}

// ---- header, tabs, footer

void Ui::draw_header(ImVec2 mn, ImVec2 mx) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const char *title = "COUNTRY-BOX";
    ImFont *lf = fonts.logo;
    ImVec2 ts = text_size(lf, title);
    float bh = 112 * s_, gap = 22 * s_, total = bh * 0.8f + gap + ts.x;
    float x0 = (mn.x + mx.x - total) * 0.5f, cy = mn.y + 82 * s_;
    art::barrel(dl, V(x0 + bh * 0.4f, cy), bh, lf);
    art::banana(dl, V(x0 + bh * 0.82f, cy + bh * 0.38f), bh * 0.42f, -0.5f);
    float tx = x0 + bh * 0.8f + gap, ty = cy - ts.y * 0.62f;
    text_at(lf, V(tx + 4 * s_, ty + 5 * s_), IM_COL32(60, 34, 12, 255), title);
    text_at(lf, V(tx, ty), art::kBanana, title);
    std::string sub = "Native ports from your own copy";
    text_at(fonts.small, V(tx + 4 * s_, ty + ts.y - 4 * s_), alpha(art::kCream, 0.85f), sub);
}

void Ui::draw_tabs(float y) {
    static const char *names[] = {"Games", "Settings", "About"};
    float W = ImGui::GetIO().DisplaySize.x, bw = 200 * s_, bh = 58 * s_, gap = 18 * s_;
    float x = (W - (3 * bw + 2 * gap)) * 0.5f;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    art::pad_glyph(dl, V(x - 46 * s_, y + bh * 0.5f), 17 * s_, "LB", alpha(art::kCream, 0.8f), fonts.small, 18 * s_);
    art::pad_glyph(dl, V(x + 3 * bw + 2 * gap + 46 * s_, y + bh * 0.5f), 17 * s_, "RB", alpha(art::kCream, 0.8f), fonts.small, 18 * s_);
    int keep = focus_;   // the tabs never take the first focus: the tab's own content does
    focus_ = 0;
    for (int i = 0; i < 3; i++) {
        bool sel = (int)tab_ == i;
        char id[16];
        snprintf(id, sizeof id, "##tab%d", i);
        if (button(id, names[i], V(x + i * (bw + gap), y), V(bw, bh), sel ? art::kBanana : kButton, true, fonts.big)) set_tab((Tab)i);
    }
    if (focus_ == 0) focus_ = keep;
}

void Ui::draw_footer(ImVec2 mn, ImVec2 mx) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(mn, mx, IM_COL32(4, 14, 8, 200));
    dl->AddLine(mn, V(mx.x, mn.y), alpha(kPanelEdge, 0.8f), 2 * s_);
    float cy = (mn.y + mx.y) * 0.5f, x = mn.x + 28 * s_, r = 15 * s_;
    auto hint = [&](const char *glyph, const char *what, ImU32 col) {
        art::pad_glyph(dl, V(x + r, cy), r, glyph, col, fonts.small, 19 * s_);
        x += (glyph[1] ? 3.2f : 2.4f) * r;
        text_at(fonts.small, V(x, cy - fonts.small->FontSize * 0.5f), art::kCream, what);
        x += text_size(fonts.small, what).x + 30 * s_;
    };
    hint("A", "Select", art::kGood);
    hint("B", flow_ != Flow::None ? "Back" : tab_ != Tab::Games ? "Games" : "Back", art::kBad);
    if (flow_ == Flow::None) {
        art::pad_glyph(dl, V(x + r * 1.2f, cy), r, "LB", alpha(art::kCream, 0.85f), fonts.small, 19 * s_);
        x += 3.0f * r;
        hint("RB", "Switch tab", alpha(art::kCream, 0.85f));
    }
    if (flow_ == Flow::None && tab_ == Tab::Settings) {
        art::triangle(dl, V(x + 8 * s_, cy), 9 * s_, -1, art::kCream);
        art::triangle(dl, V(x + 26 * s_, cy), 9 * s_, 1, art::kCream);
        x += 44 * s_;
        text_at(fonts.small, V(x, cy - fonts.small->FontSize * 0.5f), art::kCream, "Change");
    }
    std::string v = "Country-Box " + launcher_version();
    text_at(fonts.small, V(mx.x - 28 * s_ - text_size(fonts.small, v).x, cy - fonts.small->FontSize * 0.5f), alpha(art::kCream, 0.7f), v);
}

void Ui::draw_toast(ImVec2, ImVec2 mx) {
    if (t_ >= toast_until_ || toast_.empty()) return;
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = text_size(fonts.body, toast_, 700 * s_);
    float a = std::min(1.0f, (toast_until_ - t_) * 2);
    ImVec2 p(mx.x * 0.5f - ts.x * 0.5f - 24 * s_, mx.y - 150 * s_);
    dl->AddRectFilled(p, V(p.x + ts.x + 48 * s_, p.y + ts.y + 28 * s_), alpha(IM_COL32(20, 26, 20, 240), a), 16 * s_);
    dl->AddRect(p, V(p.x + ts.x + 48 * s_, p.y + ts.y + 28 * s_), alpha(toast_col_, a), 16 * s_, 0, 3 * s_);
    dl->AddText(fonts.body, fonts.body->FontSize, V(p.x + 24 * s_, p.y + 14 * s_), alpha(art::kCream, a), toast_.c_str(), nullptr, 700 * s_);
}

void Ui::draw_running(ImVec2 mn, ImVec2 mx) {
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(mn, mx, IM_COL32(0, 0, 0, 170));
    std::string name;
    for (auto &g : lib_.games())
        if (g.pid > 0) name = g.manifest ? g.manifest->name : g.id;
    ImVec2 c((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f);
    art::spinner(dl, V(c.x, c.y - 60 * s_), 34 * s_, t_, art::kBanana, 7 * s_);
    std::string l1 = name + " is running", l2 = "Close the game to come back here.";
    dl->AddText(fonts.big, fonts.big->FontSize, V(c.x - text_size(fonts.big, l1).x * 0.5f, c.y), art::kCream, l1.c_str());
    dl->AddText(fonts.body, fonts.body->FontSize, V(c.x - text_size(fonts.body, l2).x * 0.5f, c.y + 52 * s_), alpha(art::kCream, 0.8f), l2.c_str());
}

// ---- games

static ImU32 state_color(GameState s) {
    switch (s) {
    case GameState::Ready: return art::kGood;
    case GameState::UpdateAvailable: return art::kInfo;
    case GameState::CopyMissing: return art::kBad;
    case GameState::NotInstalled: return art::kMuted;
    default: return art::kWarn;
    }
}

void Ui::draw_games(ImVec2 mn, ImVec2 mx) {
    auto &games = lib_.games();
    float cw = 452 * s_, gap = 56 * s_, n = (float)games.size();
    float x = (mn.x + mx.x - (n * cw + (n - 1) * gap)) * 0.5f;
    for (size_t i = 0; i < games.size(); i++) draw_card(games[i], V(x + i * (cw + gap), mn.y), V(x + i * (cw + gap) + cw, mx.y - 14 * s_), i == 0);
}

void Ui::draw_card(Game &g, ImVec2 mn, ImVec2 mx, bool) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float w = mx.x - mn.x, pad = 22 * s_, r = 24 * s_;
    panel(mn, mx, s_, IM_COL32(16, 40, 26, 238));
    float art_h = std::min(196 * s_, (mx.y - mn.y) * 0.42f);
    ImVec2 amn(mn.x + 10 * s_, mn.y + 10 * s_), amx(mx.x - 10 * s_, mn.y + 10 * s_ + art_h);
    dl->PushClipRect(amn, amx, true);
    art::card_scene(dl, amn, amx, g.id.c_str(), s_, t_);
    dl->PopClipRect();
    // round the picture's corners with the panel colour
    dl->AddRect(V(amn.x - 6 * s_, amn.y - 6 * s_), V(amx.x + 6 * s_, amx.y + 6 * s_), IM_COL32(16, 40, 26, 255), r, 0, 12 * s_);
    // state chip over the picture
    std::string chip = game_state_label(g.state);
    if (g.pid > 0) chip = "Playing";
    ImU32 sc = state_color(g.state);
    ImVec2 cs = text_size(fonts.small, chip);
    ImVec2 c0(amn.x + 14 * s_, amx.y - cs.y - 26 * s_), c1(c0.x + cs.x + 44 * s_, amx.y - 12 * s_);
    dl->AddRectFilled(c0, c1, IM_COL32(12, 18, 12, 225), 40 * s_);
    dl->AddRect(c0, c1, sc, 40 * s_, 0, 2.5f * s_);
    dl->AddCircleFilled(V(c0.x + 18 * s_, (c0.y + c1.y) * 0.5f), 6 * s_, sc);
    text_at(fonts.small, V(c0.x + 32 * s_, c0.y + (c1.y - c0.y - cs.y) * 0.5f), art::kCream, chip);
    // name and detail
    float y = amx.y + 16 * s_;
    std::string name = g.manifest ? g.manifest->name : g.id;
    text_at(fonts.big, V(mn.x + pad, y), art::kCream, name, w - 2 * pad);
    y += text_size(fonts.big, name, w - 2 * pad).y + 8 * s_;
    text_at(fonts.small, V(mn.x + pad, y), alpha(art::kCream, 0.78f), g.detail, w - 2 * pad);
    // actions
    float bh = 66 * s_, bw = w - 2 * pad, by = mx.y - pad - bh;
    bool has_copy = !g.copy.empty();
    float sec_h = 46 * s_;
    if (has_copy) by -= sec_h + 12 * s_;
    std::string id = "##primary" + g.id;
    bool running = g.pid > 0;
    switch (g.state) {
    case GameState::NotInstalled:
        if (button(id.c_str(), "Add your copy", V(mn.x + pad, by), V(bw, bh), art::kBanana, !lib_.any_running(), fonts.big))
            open_browse(g.id, "");
        break;
    case GameState::CopyMissing:
    case GameState::NeedsCheck:
        if (button(id.c_str(), "Check your copy", V(mn.x + pad, by), V(bw, bh), art::kBanana, !lib_.any_running(), fonts.big))
            start_check(g.id, g.copy);
        break;
    case GameState::NeedsBuild:
    case GameState::UpdateAvailable:
        if (button(id.c_str(), "PREPARE", V(mn.x + pad, by), V(bw, bh), art::kBanana, !lib_.any_running(), fonts.title))
            start_prepare(g.id);
        break;
    default: {
        bool ok = !running && !lib_.any_running();
        if (button(id.c_str(), running ? "Playing" : "PLAY", V(mn.x + pad, by), V(bw, bh), IM_COL32(64, 168, 80, 255), ok, fonts.title)) {
            std::string err;
            if (!lib_.play(g, &err)) toast(err, art::kBad);
        }
    }
    }
    if (has_copy) {
        // the second row: what the state needs besides the main action, and changing the copy
        std::string sid = "##second" + g.id;
        bool prep_state = g.state == GameState::NeedsBuild || g.state == GameState::UpdateAvailable;
        bool shaders_state = g.state == GameState::NeedsShaders;
        float half = (bw - 12 * s_) * 0.5f;
        float sy = mx.y - pad - sec_h;
        if (prep_state) {
            if (button(sid.c_str(), "Import a build", V(mn.x + pad, sy), V(half, sec_h), IM_COL32(70, 54, 30, 255),
                       !lib_.any_running(), fonts.body))
                start_import(g.id);
            if (button(("##change" + g.id).c_str(), "Change copy", V(mn.x + pad + half + 12 * s_, sy), V(half, sec_h),
                       IM_COL32(70, 54, 30, 255), !lib_.any_running(), fonts.body))
                open_browse(g.id, fs::path(g.copy).parent_path().string());
        } else if (shaders_state) {
            if (button(sid.c_str(), "Prepare shaders", V(mn.x + pad, sy), V(half, sec_h), IM_COL32(70, 54, 30, 255),
                       !lib_.any_running(), fonts.body))
                start_prepare(g.id, true);
            if (button(("##change" + g.id).c_str(), "Change copy", V(mn.x + pad + half + 12 * s_, sy), V(half, sec_h),
                       IM_COL32(70, 54, 30, 255), !lib_.any_running(), fonts.body))
                open_browse(g.id, fs::path(g.copy).parent_path().string());
        } else if (button(sid.c_str(), "Change copy", V(mn.x + pad, sy), V(bw, sec_h), IM_COL32(70, 54, 30, 255), !lib_.any_running(),
                          fonts.body))
            open_browse(g.id, fs::path(g.copy).parent_path().string());
    }
}

// ---- add your copy: browse

void Ui::open_browse(const std::string &game, const std::string &dir) {
    flow_game_ = game;
    flow_ = Flow::Browse;
    places_.clear();
    const char *home = getenv("HOME");
    if (home) places_.push_back({"Home", home});
    std::error_code ec;
    std::string user = home ? fs::path(home).filename().string() : "";
    for (const char *root : {"/run/media", "/media"}) {
        for (auto it = fs::directory_iterator(fs::path(root) / user, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_directory(ec)) places_.push_back({"Drive: " + it->path().filename().string(), it->path().string()});
        ec.clear();
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
            if (it->is_directory(ec) && it->path().filename() != user && it->path().filename().string().find("mmcblk") == 0)
                places_.push_back({"SD card", it->path().string()});
        ec.clear();
    }
    for (auto it = fs::directory_iterator("/mnt", ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec) && !fs::is_empty(it->path(), ec)) places_.push_back({"Drive: " + it->path().filename().string(), it->path().string()});
    for (auto &g : lib_.games())
        if (!g.copy.empty()) places_.push_back({"Next to your other copy", fs::path(g.copy).parent_path().string()});
    std::string start = dir;
    if (start.empty() || !fs::is_directory(start, ec)) start = home ? home : "/";
    list_dir(start);
    request_focus();
}

void Ui::list_dir(const std::string &dir) {
    std::error_code ec;
    dir_ = fs::weakly_canonical(dir, ec).string();
    if (dir_.empty()) dir_ = dir;
    entries_.clear();
    browse_note_.clear();
    for (auto it = fs::directory_iterator(dir_, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        std::error_code e2;
        std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.' || !it->is_directory(e2)) continue;
        Entry e{name, it->path().string(), "", false};
        if (fs::is_directory(it->path() / "exefs", e2) && fs::is_directory(it->path() / "romfs", e2)) {
            e.copy = true;
            std::string tid = npdm_title_id((it->path() / "exefs").string());
            for (auto &id : manifest_games())
                if (const Manifest *m = manifest_get(id); m && m->title_id == tid) e.game = id;
        }
        e.build = fs::is_regular_file(it->path() / "country-box", e2) && fs::is_regular_file(it->path() / "build-stamp.json", e2);
        entries_.push_back(e);
        if (entries_.size() >= 400) break;
    }
    if (ec) browse_note_ = "This folder can't be opened.";
    std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
        if (a.copy != b.copy) return a.copy;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

void Ui::draw_browse(ImVec2 mn, ImVec2 mx) {
    const Manifest *m = manifest_get(flow_game_);
    panel(mn, mx, s_);
    float pad = 28 * s_, x = mn.x + pad, y = mn.y + 22 * s_;
    if (browse_import_) {
        shadow_text(fonts.title, V(x, y), art::kBanana, "Import a build", 2 * s_);
        y += fonts.title->FontSize + 2 * s_;
        text_at(fonts.body, V(x, y), art::kCream,
                std::string("of ") + (m ? m->name : flow_game_) + ", made on another of your computers. Choose its folder.",
                mx.x - x - pad);
    } else {
        shadow_text(fonts.title, V(x, y), art::kBanana, "Add your copy", 2 * s_);
        y += fonts.title->FontSize + 2 * s_;
        text_at(fonts.body, V(x, y), art::kCream, std::string("of ") + (m ? m->name : flow_game_) + ". Choose the folder that holds exefs and romfs.",
                mx.x - x - pad);
    }
    y += fonts.body->FontSize + 18 * s_;
    // places
    float pw = 270 * s_, ph = 50 * s_, py = y;
    text_at(fonts.small, V(x, py), art::kMuted, "Places");
    py += fonts.small->FontSize + 8 * s_;
    for (size_t i = 0; i < places_.size() && py + ph < mx.y - 110 * s_; i++) {
        std::string id = "##place" + std::to_string(i);
        if (button(id.c_str(), places_[i].first.c_str(), V(x, py), V(pw, ph), kButton, true, fonts.small)) list_dir(places_[i].second);
        py += ph + 10 * s_;
    }
    // the folder list
    float lx = x + pw + 24 * s_, lw = mx.x - pad - lx, ly = y, bottom = mx.y - 100 * s_;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    std::string shown = dir_;
    while (text_size(fonts.small, shown).x > lw - 20 * s_ && shown.size() > 8) shown = "…" + shown.substr(shown.find('/', 2) == std::string::npos ? 4 : shown.find('/', 2));
    text_at(fonts.small, V(lx, ly), art::kMuted, shown);
    ly += fonts.small->FontSize + 8 * s_;
    ImGui::SetCursorScreenPos(V(lx, ly));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(4, 16, 9, 200));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14 * s_);
    ImGui::BeginChild("##dirs", V(lw, bottom - ly), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
    float rh = 54 * s_, rw = lw - 16 * s_;
    std::string go;
    auto row = [&](const std::string &id, const std::string &label, const Entry *e) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        p.x += 8 * s_;
        ImGui::SetCursorScreenPos(p);
        bool pressed = ImGui::InvisibleButton(id.c_str(), V(rw, rh), ImGuiButtonFlags_EnableNav);
        if (focus_take()) ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow()), GImGui->NavHighlightItemUnderNav = true;
        bool hot = ImGui::IsItemHovered() || (ImGui::IsItemFocused() && nav_visible());
        ImDrawList *cl = ImGui::GetWindowDrawList();
        if (hot) cl->AddRectFilled(p, V(p.x + rw, p.y + rh), IM_COL32(246, 201, 69, 50), 12 * s_), cl->AddRect(p, V(p.x + rw, p.y + rh), art::kBanana, 12 * s_, 0, 3 * s_);
        art::icon_folder(cl, V(p.x + 14 * s_, p.y + 14 * s_), V(p.x + 50 * s_, p.y + rh - 14 * s_), e && (e->copy || (browse_import_ && e->build)) ? art::kBanana : IM_COL32(196, 160, 96, 255));
        cl->AddText(fonts.body, fonts.body->FontSize, V(p.x + 64 * s_, p.y + (rh - fonts.body->FontSize) * 0.5f), art::kCream, label.c_str());
        if (e && e->copy && !browse_import_) {
            const Manifest *gm = e->game.empty() ? nullptr : manifest_get(e->game);
            std::string tag = gm ? "Game copy: " + gm->name : "Game copy (not a supported game)";
            ImVec2 ts = text_size(fonts.small, tag);
            ImVec2 t0(p.x + rw - ts.x - 34 * s_, p.y + (rh - ts.y) * 0.5f - 5 * s_);
            cl->AddRectFilled(t0, V(t0.x + ts.x + 22 * s_, t0.y + ts.y + 10 * s_), gm ? IM_COL32(70, 120, 40, 255) : IM_COL32(110, 70, 30, 255), 20 * s_);
            cl->AddText(fonts.small, fonts.small->FontSize, V(t0.x + 11 * s_, t0.y + 5 * s_), art::kCream, tag.c_str());
        }
        if (e && e->build && browse_import_) {
            std::string tag = "Country-Box build";
            ImVec2 ts = text_size(fonts.small, tag);
            ImVec2 t0(p.x + rw - ts.x - 34 * s_, p.y + (rh - ts.y) * 0.5f - 5 * s_);
            cl->AddRectFilled(t0, V(t0.x + ts.x + 22 * s_, t0.y + ts.y + 10 * s_), IM_COL32(70, 120, 40, 255), 20 * s_);
            cl->AddText(fonts.small, fonts.small->FontSize, V(t0.x + 11 * s_, t0.y + 5 * s_), art::kCream, tag.c_str());
        }
        ImGui::SetCursorScreenPos(V(p.x - 8 * s_, p.y + rh + 4 * s_));
        return pressed;
    };
    ImGui::SetCursorScreenPos(V(lx, ly + 8 * s_));
    if (dir_ != "/" && row("##up", "..  (up one folder)", nullptr)) go = fs::path(dir_).parent_path().string();
    for (size_t i = 0; i < entries_.size(); i++) {
        if (row("##d" + std::to_string(i), entries_[i].name, &entries_[i])) {
            if (!browse_import_ && entries_[i].copy) start_check(flow_game_, entries_[i].path);
            else go = entries_[i].path;
        }
    }
    if (entries_.empty())
        ImGui::GetWindowDrawList()->AddText(fonts.small, fonts.small->FontSize, V(lx + 24 * s_, ImGui::GetCursorScreenPos().y + 12 * s_), art::kMuted,
                                             browse_note_.empty() ? "No folders here." : browse_note_.c_str());
    ImGui::Dummy(V(1, 8 * s_));
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!go.empty()) list_dir(go), request_focus();
    // actions
    float bh = 62 * s_, by = mx.y - pad - bh + 6 * s_;
    if (browse_import_) {
        if (button("##use", "Use this folder", V(mx.x - pad - 300 * s_, by), V(300 * s_, bh), art::kBanana, true, fonts.big))
            start_import_dir(flow_game_, dir_);
    } else if (button("##use", "Use this folder", V(mx.x - pad - 300 * s_, by), V(300 * s_, bh), art::kBanana, flow_ == Flow::Browse, fonts.big))
        start_check(flow_game_, dir_);
    if (button("##cancel", "Cancel", V(mx.x - pad - 300 * s_ - 24 * s_ - 200 * s_, by), V(200 * s_, bh), IM_COL32(90, 60, 34, 255), true, fonts.big))
        finish_flow();
    (void)dl;
}

// ---- add your copy: the check

void Ui::start_check(const std::string &game, const std::string &path) {
    job_ = std::make_unique<VerifyJob>(game, path);
    flow_game_ = game;
    flow_ = Flow::Checking;
    request_focus();
}

void Ui::draw_checking(ImVec2 mn, ImVec2 mx) {
    panel(mn, mx, s_);
    if (!job_) return;
    const Manifest *m = manifest_get(job_->game());
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float cx = (mn.x + mx.x) * 0.5f, y = mn.y + 40 * s_;
    text_center(fonts.title, cx, y, art::kBanana, "Checking your copy");
    y += fonts.title->FontSize + 10 * s_;
    text_center(fonts.body, cx, y, art::kCream, m ? m->name + " " + m->version : job_->game());
    y += fonts.body->FontSize + 6 * s_;
    text_center(fonts.small, cx, y, art::kMuted, job_->path());
    y += 70 * s_;
    // a rolling barrel along the bar
    int64_t done = __atomic_load_n(&job_->done_bytes, __ATOMIC_RELAXED), total = job_->total_bytes.load();
    float frac = total > 0 ? (float)done / (float)total : 0;
    float bw = std::min(820 * s_, mx.x - mn.x - 120 * s_), bh = 34 * s_, bx = cx - bw * 0.5f;
    dl->AddRectFilled(V(bx, y), V(bx + bw, y + bh), IM_COL32(0, 0, 0, 140), bh * 0.5f);
    if (frac > 0) dl->AddRectFilled(V(bx, y), V(bx + std::max(bh, bw * frac), y + bh), art::kBanana, bh * 0.5f);
    dl->AddRect(V(bx, y), V(bx + bw, y + bh), kPanelEdge, bh * 0.5f, 0, 3 * s_);
    for (float k = 0.1f; k < 1; k += 0.1f) {
        float x = bx + bw * k;
        if (x > bx + bw * frac) dl->AddCircleFilled(V(x, y + bh * 0.5f), 4 * s_, alpha(art::kBanana, 0.35f));
    }
    art::banana(dl, V(bx + bw * frac, y - 26 * s_), 52 * s_, -0.4f + 0.15f * sinf(t_ * 4));
    y += bh + 24 * s_;
    std::string line;
    if (job_->phase.load() <= VerifyJob::Listing) line = "Looking at the files…";
    else {
        char t[160];
        snprintf(t, sizeof t, "%s of %s  ·  %d of %d files", human_bytes((double)done).c_str(), human_bytes((double)total).c_str(),
                 job_->files_done.load(), job_->files_total.load());
        line = t;
    }
    text_center(fonts.body, cx, y, art::kCream, line);
    y += fonts.body->FontSize + 12 * s_;
    text_center(fonts.small, cx, y, art::kMuted, "Every file is compared with the original game. Nothing is changed or uploaded.");
    float bh2 = 62 * s_;
    if (button("##stop", "Stop", V(cx - 120 * s_, mx.y - 40 * s_ - bh2), V(240 * s_, bh2), IM_COL32(90, 60, 34, 255), true, fonts.big))
        job_->cancel();
}

// ---- prepare (milestone 2): build the game here, with progress

void Ui::start_prepare(const std::string &game, bool shaders_only) {
    Game *g = lib_.find(game);
    if (!g) return;
    prep_game_ = game;
    prep_shaders_only_ = shaders_only;
    prep_ = std::make_unique<PrepareJob>(lib_.home(), *g, shaders_only ? PrepareJob::ShadersOnly : PrepareJob::Full);
    flow_game_ = game;
    flow_ = Flow::Prepare;
    request_focus();
}

void Ui::draw_prepare(ImVec2 mn, ImVec2 mx) {
    panel(mn, mx, s_);
    if (!prep_) return;
    if (prep_->done() && !prep_->outcome().ok && prep_->outcome().kind != "cancelled") {
        draw_prep_notice(mn, mx);   // a failure shows only its notice and Done, nothing behind it
        return;
    }
    const Manifest *m = manifest_get(prep_game_);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float cx = (mn.x + mx.x) * 0.5f, pad = 40 * s_;
    float y = mn.y + 34 * s_;
    bool shaders_only = prep_shaders_only_;
    text_center(fonts.title, cx, y, art::kBanana, shaders_only ? "Preparing the shaders" : "Preparing the game");
    y += fonts.title->FontSize + 8 * s_;
    text_center(fonts.body, cx, y, art::kCream, m ? m->name : prep_game_);
    y += fonts.body->FontSize + 26 * s_;
    bool running = !prep_->done();
    // the stages, one row each: done, running (with its own progress) or pending
    struct Row {
        const char *label;
        int stage;
    };
    Row rows[] = {
        {"Checking the tools and the space", PrepareJob::Preflight},
        {"Turning your copy's code into C", PrepareJob::Lift},
        {"Compiling and linking", PrepareJob::Compile},
        {"Preparing the shaders", PrepareJob::Shaders},
    };
    int stage = prep_->stage.load();
    const PrepProfile *prof = prep_profile(prep_game_);
    double lift_w = prof ? prof->lift_s : 100, comp_w = prof ? prof->compile_s : 500;
    double weights[] = {2, lift_w, comp_w, std::max(lift_w, comp_w) * 0.15};
    double total_w = weights[0] + weights[1] + weights[2] + (shaders_only ? 0 : weights[3]);
    double done_w = 0, frac = 0;
    for (int i = 0; i < 4; i++) {
        double w = shaders_only ? (rows[i].stage == PrepareJob::Shaders ? 1 : 0) : weights[i];
        if (stage > rows[i].stage || (!running && prep_->outcome().ok)) done_w += w;
        else if (stage == rows[i].stage && running) {
            int d = prep_->stage_done.load(), t = prep_->stage_total.load();
            frac = t > 0 && d >= 0 ? std::min(1.0, (double)d / t) : 0;
            done_w += w * frac;
        }
    }
    float row_h = 56 * s_, bar_w = std::min(900 * s_, mx.x - mn.x - 2 * pad);
    for (int i = 0; i < 4; i++) {
        if (shaders_only && rows[i].stage != PrepareJob::Shaders) continue;
        float ry = y + 6 * s_ + i * row_h;   // right under the game name: four rows fit in a short window
        if (ry + row_h > mx.y - 150 * s_) break;
        bool is_done = stage > rows[i].stage || (!running && prep_->outcome().ok);
        bool is_run = stage == rows[i].stage && running;
        float x = cx - bar_w * 0.5f;
        if (is_done) art::icon_check(dl, V(x + 20 * s_, ry + row_h * 0.5f), 16 * s_, art::kGood);
        else if (is_run) art::spinner(dl, V(x + 20 * s_, ry + row_h * 0.5f), 15 * s_, t_, art::kBanana, 5 * s_);
        else dl->AddCircle(V(x + 20 * s_, ry + row_h * 0.5f), 12 * s_, alpha(art::kCream, 0.35f), 0, 3 * s_);
        text_at(fonts.big, V(x + 56 * s_, ry + (row_h - fonts.big->FontSize) * 0.5f), is_done ? art::kCream : alpha(art::kCream, is_run ? 1.0f : 0.55f),
                rows[i].label);
        if (is_run) {
            int d = prep_->stage_done.load(), t = prep_->stage_total.load();
            float f = t > 0 && d >= 0 ? std::min(1.0f, (float)d / t) : 0;
            float bw = bar_w * 0.42f, bh = 16 * s_, bx = x + bar_w - bw - 10 * s_;
            dl->AddRectFilled(V(bx, ry + row_h * 0.5f - bh * 0.5f), V(bx + bw, ry + row_h * 0.5f + bh * 0.5f), IM_COL32(0, 0, 0, 130), bh * 0.5f);
            if (f > 0) dl->AddRectFilled(V(bx, ry + row_h * 0.5f - bh * 0.5f), V(bx + std::max(bh, bw * f), ry + row_h * 0.5f + bh * 0.5f), art::kBanana, bh * 0.5f);
            char t2[64];
            if (t > 0 && d >= 0) snprintf(t2, sizeof t2, "%d / %d", d, t);
            else snprintf(t2, sizeof t2, "…");
            text_at(fonts.small, V(bx - text_size(fonts.small, t2).x - 14 * s_, ry + row_h * 0.5f - fonts.small->FontSize * 0.5f), art::kMuted, t2);
        }
    }
    y = mx.y - 190 * s_;
    // one overall bar: elapsed and the estimate from the work so far
    double overall = total_w > 0 ? done_w / total_w : 0;
    float obh = 26 * s_, obx = cx - bar_w * 0.5f;
    if (running) {   // finished: the stage rows say it all, and the outcome takes the bar's place (short windows)
        dl->AddRectFilled(V(obx, y), V(obx + bar_w, y + obh), IM_COL32(0, 0, 0, 140), obh * 0.5f);
        if (overall > 0) dl->AddRectFilled(V(obx, y), V(obx + std::max(obh, (float)(bar_w * overall)), y + obh), art::kBanana, obh * 0.5f);
        dl->AddRect(V(obx, y), V(obx + bar_w, y + obh), kPanelEdge, obh * 0.5f, 0, 2.5f * s_);
        char pcts[160];
        snprintf(pcts, sizeof pcts, "%.0f%%", overall * 100);
        text_center(fonts.big, cx, y + obh + 12 * s_, art::kCream, pcts);
    }
    if (running) {
        // elapsed mm:ss + about N left (the shaders report their own estimate)
        static double prep_t0 = 0;
        if (stage == PrepareJob::Preflight && prep_->stage_total.load() <= 1 && prep_->stage_done.load() == 0) prep_t0 = t_;
        double elapsed = std::max(0.0, t_ - prep_t0);
        double eta = prep_->eta_s.load();
        char ts[128];
        if (eta >= 0) snprintf(ts, sizeof ts, "%d:%02d elapsed  ·  about %d:%02d left", (int)elapsed / 60, (int)elapsed % 60, (int)eta / 60, (int)eta % 60);
        else snprintf(ts, sizeof ts, "%d:%02d elapsed", (int)elapsed / 60, (int)elapsed % 60);
        text_center(fonts.small, cx, y + obh + 52 * s_, art::kMuted, ts);
        std::string line = prep_->line();
        text_center(fonts.small, cx, y + obh + 78 * s_, alpha(art::kCream, 0.55f), line, bar_w);
        float bh2 = 60 * s_;
        if (button("##pcancel", "Cancel", V(cx - 120 * s_, mx.y - 34 * s_ - bh2), V(240 * s_, bh2), IM_COL32(90, 60, 34, 255), true, fonts.big))
            prep_->cancel();
        return;
    }
    // the outcome, in plain words
    const PrepareJob::Outcome &out = prep_->outcome();
    bool ok = out.ok;
    float oy = y;
    if (ok || out.kind == "cancelled") {
        const char *title = ok ? "Done — ready to play" : "Cancelled";
        if (ok) art::icon_check(dl, V(cx - text_size(fonts.big, title).x * 0.5f - 30 * s_, oy + fonts.big->FontSize * 0.5f), 18 * s_, art::kGood);
        text_center(fonts.big, cx, oy, ok ? art::kBanana : art::kCream, title);
        oy += fonts.big->FontSize + 10 * s_;
        text_center(fonts.body, cx, oy, art::kCream, ok ? std::string("The game is built on this computer.")
                                                      : std::string("Prepare again to continue where it stopped."), bar_w);
    }
    float bh2 = 62 * s_;
    if (button("##pdone", "Done", V(cx - 140 * s_, mx.y - 34 * s_ - bh2), V(280 * s_, bh2), art::kBanana, true, fonts.big)) {
        prep_.reset();
        finish_flow();
        if (Game *g = lib_.find(prep_game_)) lib_.refresh(*g);
    }
}

// Prepare failed: a notice window — title, what happened, the commands to run in a selectable box (copy them), the log
// — with a scroll bar when it is long, and the Done button under it.
void Ui::draw_prep_notice(ImVec2 mn, ImVec2 mx) {
    const PrepareJob::Outcome &out = prep_->outcome();
    float cx = (mn.x + mx.x) * 0.5f;
        // A failure is a notice: a window over the screen, its text wrapped inside, the commands in a selectable box
    // (copy them), a scroll bar when it is long. Nothing spills over the rest of the screen.
    float nw = std::min(mx.x - mn.x - 80 * s_, 1000 * s_);
    float top = mn.y + 40 * s_, bottom = mx.y - 34 * s_ - 62 * s_ - 24 * s_;
    ImGui::SetNextWindowPos(V(cx - nw * 0.5f, top));
    ImGui::SetNextWindowSize(V(nw, bottom - top));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(28, 17, 8, 248));
    ImGui::PushStyleColor(ImGuiCol_Border, art::kBanana);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14 * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.5f * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, V(24 * s_, 20 * s_));
    ImGui::Begin("##prep_notice", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());   // drawn over the screen, focus stays on Done
    ImGui::PushFont(fonts.big);
    ImGui::TextColored(ImColor(art::kBanana), "Preparing stopped");
    ImGui::PopFont();
    ImGui::PushFont(fonts.body);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(out.headline.c_str());
    ImGui::PopFont();
    ImGui::PushFont(fonts.small);
    if (!out.explain.empty()) ImGui::TextColored(ImColor(art::kMuted), "%s", out.explain.c_str());
    ImGui::PopTextWrapPos();
    if (!out.hint.empty()) {
        ImGui::Spacing();
        static std::string hint_buf;
        hint_buf = out.hint;
        float lines = 1.0f;
        for (char ch : hint_buf) lines += ch == '\n';
        float boxh = std::min(ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 2.5f,
                              (lines + 2.5f) * ImGui::GetTextLineHeightWithSpacing());
        ImGui::InputTextMultiline("##hint", hint_buf.data(), hint_buf.size() + 1, V(-1, std::max(boxh, 60 * s_)),
                                  ImGuiInputTextFlags_ReadOnly);
    }
    if (!out.log.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImColor(alpha(art::kCream, 0.55f)), "Log: %s", out.log.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    float bh2 = 62 * s_;
    if (button("##pndone", "Done", V(cx - 140 * s_, mx.y - 34 * s_ - bh2), V(280 * s_, bh2), art::kBanana, true, fonts.big)) {
        prep_.reset();
        finish_flow();
        if (Game *g = lib_.find(prep_game_)) lib_.refresh(*g);
    }
}

// ---- import a build made on another computer

void Ui::start_import(const std::string &game, const std::string &dir) {
    flow_game_ = game;
    browse_import_ = true;
    open_browse(game, dir);
    flow_ = Flow::Browse;
}

void Ui::start_import_dir(const std::string &game, const std::string &path) {
    Game *g = lib_.find(game);
    if (!g || import_thread_.joinable()) return;
    flow_game_ = game;
    import_done_.store(false);
    import_bytes_.store(0);
    import_total_.store(0);
    std::string home = lib_.home(), id = g->id;
    flow_ = Flow::Import;
    request_focus();
    // the copy runs in the background; the outcome lands in import_out_ before import_done_ turns true
    import_thread_ = std::thread([this, home, id, path, g] {
        ImportOutcome r = import_build(home, *g, path, nullptr, nullptr);
        import_out_ = r;
        import_done_.store(true);
    });
    (void)id;
}

void Ui::draw_import(ImVec2 mn, ImVec2 mx) {
    panel(mn, mx, s_);
    const Manifest *m = manifest_get(flow_game_);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float cx = (mn.x + mx.x) * 0.5f, y = mn.y + 40 * s_;
    text_center(fonts.title, cx, y, art::kBanana, "Importing a build");
    y += fonts.title->FontSize + 10 * s_;
    text_center(fonts.body, cx, y, art::kCream, m ? m->name : flow_game_);
    y += 60 * s_;
    bool done = import_done_.load();
    if (!done) {
        art::spinner(dl, V(cx, y), 30 * s_, t_, art::kBanana, 6 * s_);
        y += 70 * s_;
        text_center(fonts.body, cx, y, art::kCream, "Copying the build into this computer…");
        y += fonts.body->FontSize + 16 * s_;
        text_center(fonts.small, cx, y, art::kMuted, "Your saves and settings here are not touched.");
        return;
    }
    const ImportOutcome &r = import_out_;
    bool ok = r.ok;
    if (ok) art::icon_check(dl, V(cx - 30 * s_, y + 20 * s_), 22 * s_, art::kGood);
    text_center(fonts.big, cx, y, ok ? art::kBanana : art::kCream, ok ? "The build is in" : r.headline);
    y += fonts.big->FontSize + 12 * s_;
    if (ok) {
        text_center(fonts.body, cx, y, art::kCream, r.build);
        y += fonts.body->FontSize + 10 * s_;
        text_center(fonts.small, cx, y, art::kMuted, "Checked against your copy of the game. Saves and settings stayed as they were.");
    } else {
        if (!r.explain.empty()) text_center(fonts.small, cx, y, art::kMuted, r.explain, mx.x - mn.x - 120 * s_), y += fonts.small->FontSize + 10 * s_;
        if (!r.hint.empty()) text_center(fonts.small, cx, y, art::kBanana, r.hint, mx.x - mn.x - 120 * s_);
    }
    float bh = 62 * s_;
    if (button("##idone", "Done", V(cx - 140 * s_, mx.y - 34 * s_ - bh), V(280 * s_, bh), art::kBanana, true, fonts.big)) {
        if (import_thread_.joinable()) import_thread_.join();
        finish_flow();
        if (Game *g = lib_.find(flow_game_)) lib_.refresh(*g);
    }
}

void Ui::draw_result(ImVec2 mn, ImVec2 mx) {
    panel(mn, mx, s_);
    const VerifyResult &r = result_;
    bool ok = r.kind == VerifyResult::Ok;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float pad = 40 * s_, x = mn.x + pad, y = mn.y + 36 * s_, wrap = mx.x - pad - (x + 110 * s_);
    ImU32 col = ok ? art::kGood : r.kind == VerifyResult::OtherGame || r.kind == VerifyResult::Cancelled ? art::kWarn : art::kBad;
    if (ok) art::icon_check(dl, V(x + 44 * s_, y + 44 * s_), 44 * s_, col);
    else art::icon_alert(dl, V(x + 44 * s_, y + 44 * s_), 44 * s_, col);
    float tx = x + 110 * s_;
    text_at(fonts.title, V(tx, y), ok ? art::kBanana : art::kCream, r.headline, wrap);
    y += text_size(fonts.title, r.headline, wrap).y + 12 * s_;
    if (!r.explain.empty() && !ok) {
        text_at(fonts.body, V(tx, y), alpha(art::kCream, 0.9f), r.explain, wrap);
        y += text_size(fonts.body, r.explain, wrap).y + 12 * s_;
    }
    if (!r.note.empty()) {
        text_at(fonts.small, V(tx, y), art::kMuted, r.note, wrap);
        y += text_size(fonts.small, r.note, wrap).y + 10 * s_;
    }
    if (ok) {   // what was checked, one line each
        const Manifest *m = manifest_get(result_game_);
        char t[160];
        snprintf(t, sizeof t, "All %d files present and identical to the original (%s, checked in %.0f s)", m ? (int)m->files.size() : 0,
                 human_bytes(m ? (double)m->bytes : 0).c_str(), std::max(1.0, r.seconds));
        std::string lines[] = {
            "It is " + (m ? m->name : result_game_) + " (title " + r.title_id + ")",
            "Version " + (m ? m->version : std::string("?")) + ": the base game, no update applied",
            t,
            "Nothing was changed: Country-Box only reads your copy",
        };
        y += 6 * s_;
        for (auto &l : lines) {
            art::icon_check(dl, V(tx + 16 * s_, y + fonts.body->FontSize * 0.5f), 13 * s_, art::kGood);
            text_at(fonts.body, V(tx + 44 * s_, y), art::kCream, l, wrap - 44 * s_);
            y += text_size(fonts.body, l, wrap - 44 * s_).y + 14 * s_;
        }
        y += 4 * s_;
    }
    std::string where = "Folder: " + (r.root.empty() ? result_path_ : r.root);
    text_at(fonts.small, V(tx, y), art::kMuted, where, wrap);
    y += text_size(fonts.small, where, wrap).y + 16 * s_;
    if (!r.details.empty()) {
        float bx0 = tx, bx1 = mx.x - pad, lh = fonts.small->FontSize + 6 * s_;
        size_t shown = std::min<size_t>(r.details.size(), (size_t)std::max(1.0f, (mx.y - 220 * s_ - y) / lh));
        float by1 = y + 20 * s_ + shown * lh;
        dl->AddRectFilled(V(bx0, y), V(bx1, by1), IM_COL32(0, 0, 0, 110), 12 * s_);
        for (size_t i = 0; i < shown; i++) {
            std::string d = (i + 1 == shown && shown < r.details.size()) ? "… and more" : r.details[i];
            dl->AddCircleFilled(V(bx0 + 22 * s_, y + 10 * s_ + i * lh + lh * 0.45f), 4 * s_, col);
            text_at(fonts.small, V(bx0 + 38 * s_, y + 10 * s_ + i * lh), art::kCream, d);
        }
        y = by1 + 16 * s_;
    }
    if (!r.hint.empty()) {
        float hy0 = y, hx1 = mx.x - pad;
        ImVec2 hs = text_size(fonts.body, r.hint, hx1 - tx - 160 * s_);
        dl->AddRectFilled(V(tx, hy0), V(hx1, hy0 + hs.y + 28 * s_), IM_COL32(246, 201, 69, 36), 14 * s_);
        dl->AddRectFilled(V(tx, hy0), V(tx + 8 * s_, hy0 + hs.y + 28 * s_), art::kBanana, 4 * s_);
        text_at(fonts.body, V(tx + 26 * s_, hy0 + 14 * s_), art::kBanana, "What to do:");
        text_at(fonts.body, V(tx + 160 * s_, hy0 + 14 * s_), art::kCream, r.hint, hx1 - tx - 180 * s_);
    }
    // actions
    float bh = 64 * s_, by = mx.y - 36 * s_ - bh;
    if (ok) {
        if (button("##done", "Done", V(mx.x - pad - 280 * s_, by), V(280 * s_, bh), art::kBanana, true, fonts.big)) finish_flow();
        if (Game *g = lib_.find(result_game_); g && !g->exe.empty())
            text_at(fonts.body, V(tx, by + 16 * s_), art::kGood, "Ready to play.");
        else if (g)
            text_at(fonts.body, V(tx, by + 16 * s_), alpha(art::kCream, 0.85f), "Next: the game gets built for this computer.");
        return;
    }
    float bx = mx.x - pad;
    if (r.kind == VerifyResult::OtherGame) {
        const Manifest *o = manifest_get(r.other_game);
        std::string l = "Add to " + std::string(r.other_game == "dkctf" ? "Tropical Freeze" : "Returns HD");
        bx -= 380 * s_;
        if (button("##other", l.c_str(), V(bx, by), V(380 * s_, bh), art::kBanana, true, fonts.big)) {
            std::string other = r.other_game, path = r.root;
            start_check(other, path);
            return;
        }
        (void)o;
        bx -= 24 * s_;
    }
    bx -= 340 * s_;
    if (button("##again", "Choose another folder", V(bx, by), V(340 * s_, bh), r.kind == VerifyResult::OtherGame ? kButton : art::kBanana, true, fonts.big))
        open_browse(result_game_, fs::path(r.root.empty() ? result_path_ : r.root).parent_path().string());
    bx -= 24 * s_ + 200 * s_;
    if (button("##close", "Cancel", V(bx, by), V(200 * s_, bh), IM_COL32(90, 60, 34, 255), true, fonts.big)) finish_flow();
}

// ---- the update channel

void Ui::start_update_check() {
    if (getenv("CB_LAUNCHER_NO_UPDATE")) return;
    checked_ = false;
    checker_ = std::make_unique<UpdateChecker>(lib_.home());
}

void Ui::show_update_ask() {
    flow_ = Flow::UpdateAsk;
    request_focus();
}

void Ui::start_update() {
    if (!update_info_.available || update_) return;
    std::vector<std::pair<std::string, std::string>> builds;
    for (auto &g : lib_.games())
        if (!g.build_dir.empty()) builds.push_back({g.id, g.build_dir});
    update_ = std::make_unique<UpdateJob>(update_info_, lib_.home(), builds);
    flow_ = Flow::Updating;
    request_focus();
}

void Ui::decline_update() {
    if (!update_info_.version.empty()) update_ini_set(lib_.home(), "declined", update_info_.version);
    finish_flow();
}

void Ui::restart_launcher() {
    std::string exe = lib_.home() + "/app/current/country-box-launcher";
    if (access(exe.c_str(), X_OK) != 0) {
        toast("The new launcher is not at " + exe, art::kBad);
        return;
    }
    std::error_code ec;
    fs::create_directories(lib_.home() + "/logs", ec);
    pid_t pid = proc_spawn({exe, "--home", lib_.home()}, fs::path(exe).parent_path().string(), lib_.home() + "/logs/launcher.log");
    if (pid > 0) quit_ = true;
    else toast("The new launcher could not be started (" + exe + ").", art::kBad);
}

void Ui::draw_update_ask(ImVec2 mn, ImVec2 mx) {
    panel(mn, mx, s_);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float cx = (mn.x + mx.x) * 0.5f, pad = 40 * s_;
    float y = mn.y + 36 * s_;
    art::icon_alert(dl, V(cx - text_size(fonts.title, "Update available").x * 0.5f - 46 * s_, y + 8 * s_), 34 * s_, art::kBanana);
    text_center(fonts.title, cx, y, art::kBanana, "Update available");
    y += fonts.title->FontSize + 12 * s_;
    char l[160];
    snprintf(l, sizeof l, "Country-Box %s  ·  %s", update_info_.version.c_str(), human_bytes((double)update_info_.asset_size).c_str());
    text_center(fonts.body, cx, y, art::kCream, l);
    y += fonts.body->FontSize + 14 * s_;
    float wrap = mx.x - mn.x - 2 * pad;
    if (!update_info_.notes.empty()) {
        // "What changed": the release notes, the first lines of them
        std::string notes = "What changed:\n" + update_info_.notes;
        ImVec2 ns = text_size(fonts.small, notes, wrap);
        dl->PushClipRect(V(mn.x + pad, y), V(mx.x - pad, y + std::max(ns.y, 60.f * s_)), true);
        text_at(fonts.small, V(mn.x + pad, y), alpha(art::kCream, 0.9f), notes, wrap);
        dl->PopClipRect();
        y += std::min(ns.y, std::max(90.f * s_, (mx.y - mn.y) - (y - mn.y) - 250 * s_)) + 12 * s_;
    }
    text_center(fonts.small, cx, std::min(y, mx.y - 190 * s_), art::kMuted,
                "Saves and settings are never touched. Each game is rebuilt for the new version.");
    float bh = 66 * s_, by = mx.y - 36 * s_ - bh;
    if (button("##updnow", "Update now", V(mx.x - pad - 340 * s_, by), V(340 * s_, bh), art::kBanana, true, fonts.big))
        start_update();
    if (button("##updnot", "Not now", V(mn.x + pad, by), V(280 * s_, bh), IM_COL32(70, 54, 30, 255), true, fonts.big, false))
        decline_update();
}

void Ui::draw_updating(ImVec2 mn, ImVec2 mx) {
    panel(mn, mx, s_);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float cx = (mn.x + mx.x) * 0.5f, y = mn.y + 40 * s_;
    text_center(fonts.title, cx, y, art::kBanana, "Updating Country-Box");
    y += fonts.title->FontSize + 24 * s_;
    const char *phase_text = "Downloading the new version…";
    int64_t done = update_ ? update_->done_bytes.load() : 0, total = update_ ? update_->total_bytes.load() : 0;
    if (update_ && update_->phase.load() >= UpdateJob::Backup) phase_text = "Backing up your saves and installing…";
    else if (update_ && update_->phase.load() == UpdateJob::Verify) phase_text = "Verifying the download…";
    float frac = total > 0 && update_ && update_->phase.load() <= UpdateJob::Verify ? (float)done / (float)total : 1;
    float bw = std::min(760 * s_, mx.x - mn.x - 120 * s_), bh = 34 * s_, bx = cx - bw * 0.5f;
    dl->AddRectFilled(V(bx, y), V(bx + bw, y + bh), IM_COL32(0, 0, 0, 140), bh * 0.5f);
    if (frac > 0) dl->AddRectFilled(V(bx, y), V(bx + std::max(bh, bw * frac), y + bh), art::kBanana, bh * 0.5f);
    dl->AddRect(V(bx, y), V(bx + bw, y + bh), kPanelEdge, bh * 0.5f, 0, 3 * s_);
    art::banana(dl, V(bx + bw * frac, y - 26 * s_), 52 * s_, -0.4f + 0.15f * sinf(t_ * 4));
    y += bh + 24 * s_;
    char l[160];
    snprintf(l, sizeof l, "%s  ·  %s of %s", phase_text, human_bytes((double)done).c_str(), human_bytes((double)total).c_str());
    text_center(fonts.body, cx, y, art::kCream, l);
    y += fonts.body->FontSize + 12 * s_;
    text_center(fonts.small, cx, y, art::kMuted, "Your games, saves and settings are never touched by an update.");
    float bh2 = 62 * s_;
    if (update_ && update_->phase.load() <= UpdateJob::Verify &&
        button("##updstop", "Stop", V(cx - 120 * s_, mx.y - 40 * s_ - bh2), V(240 * s_, bh2), IM_COL32(90, 60, 34, 255), true, fonts.big))
        update_->cancel();
}

void Ui::draw_update_result(ImVec2 mn, ImVec2 mx) {
    panel(mn, mx, s_);
    auto &r = update_outcome_;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float pad = 40 * s_, x = mn.x + pad, y = mn.y + 36 * s_, wrap = mx.x - pad - (x + 110 * s_);
    if (r.ok) art::icon_check(dl, V(x + 44 * s_, y + 44 * s_), 44 * s_, art::kGood);
    else art::icon_alert(dl, V(x + 44 * s_, y + 44 * s_), 44 * s_, r.cancelled ? art::kWarn : art::kBad);
    float tx = x + 110 * s_;
    text_at(fonts.title, V(tx, y), r.ok ? art::kBanana : art::kCream,
            r.ok ? "Country-Box " + r.to + " installed" : r.cancelled ? "Update stopped" : "Update failed", wrap);
    y += text_size(fonts.title, "Country-Box installed", wrap).y + 12 * s_;
    if (!r.ok) {
        if (!r.err.empty()) {
            text_at(fonts.body, V(tx, y), alpha(art::kCream, 0.9f), r.cancelled ? "Nothing was changed." : r.err, wrap);
            y += text_size(fonts.body, r.cancelled ? "Nothing was changed." : r.err, wrap).y + 10 * s_;
        }
        text_at(fonts.small, V(tx, y), art::kMuted, "You can try again from the About tab.");
    } else {
        std::vector<std::string> lines;
        lines.push_back("The previous version (" + r.from + ") stays in app/ for a rollback.");
        lines.push_back(r.saved.empty() ? "No saves found to back up (nothing had been played yet)."
                                        : "Saves backed up to " + r.backup + ": " + r.saved[0] +
                                              (r.saved.size() > 1 ? " and " + std::to_string(r.saved.size() - 1) + " more" : ""));
        lines.push_back("Your games, copies and settings were not touched.");
        for (auto &t : lines) {
            art::icon_check(dl, V(tx + 16 * s_, y + fonts.body->FontSize * 0.5f), 13 * s_, art::kGood);
            text_at(fonts.body, V(tx + 44 * s_, y), art::kCream, t, wrap - 44 * s_);
            y += text_size(fonts.body, t, wrap - 44 * s_).y + 14 * s_;
        }
        if (!r.need_prepare.empty()) {
            std::string prep = "Each game now needs preparing once for the new version (its card says so after the restart): " +
                               r.need_prepare[0] + (r.need_prepare.size() > 1 ? ", " + r.need_prepare[1] : "") + ".";
            text_at(fonts.body, V(tx, y), art::kBanana, prep, wrap);
            y += text_size(fonts.body, prep, wrap).y + 12 * s_;
        }
    }
    float bh = 64 * s_, by = mx.y - 36 * s_ - bh;
    if (r.ok) {
        if (button("##updrestart", "Restart now", V(mx.x - pad - 340 * s_, by), V(340 * s_, bh), art::kBanana, true, fonts.big))
            restart_launcher();
        if (button("##updlater", "Later", V(tx, by), V(240 * s_, bh), IM_COL32(70, 54, 30, 255), true, fonts.big, false)) finish_flow();
    } else {
        if (button("##updok", "OK", V(mx.x - pad - 240 * s_, by), V(240 * s_, bh), art::kBanana, true, fonts.big)) finish_flow();
    }
}

// ---- settings

void Ui::select_settings_game(const std::string &game) {
    settings_game_ = game;
    load_settings();
}

void Ui::load_settings() {
    dump_ = Json();
    settings_err_.clear();
    lib_.refresh();
    Game *g = lib_.find(settings_game_);
    if (!g || g->exe.empty()) {
        g = nullptr;
        for (auto &x : lib_.games())
            if (!x.exe.empty()) {
                g = &x;
                break;
            }
    }
    if (!g) {
        settings_game_.clear();
        return;
    }
    settings_game_ = g->id;
    if (!lib_.settings_dump(*g, dump_, &settings_err_)) dump_ = Json();
    if (group_ >= (int)dump_["groups"].size()) group_ = 0;
}

void Ui::set_setting(const std::string &key, const std::string &value) {
    Game *g = lib_.find(settings_game_);
    if (!g) return;
    Json ans;
    std::string err;
    if (!lib_.settings_set(*g, {key + "=" + value}, ans, &err)) {
        toast(err, art::kBad);
        return;
    }
    if (!ans["ok"].truthy()) {
        toast(ans["errors"][(size_t)0].s("The game refused that value."), art::kBad);
        return;
    }
    if (ans["restart"].truthy()) toast("Saved. It applies the next time the game starts.", art::kInfo);
    std::string keep;
    lib_.settings_dump(*g, dump_, &keep);
}

void Ui::step_setting(const Json &row, int dir) {
    if (!row["unavailable"].is_null() || row["set_by_env"].truthy()) return;
    std::string key = row["key"].s();
    if (row["type"].s() == "range") {
        double v = atof(row["value"].s().c_str()), st = row["step"].n(1), lo = row["min"].n(), hi = row["max"].n();
        double nv = std::clamp(std::round((v + dir * st) / st) * st, lo, hi);
        if (nv == v) return;
        char b[32];
        snprintf(b, sizeof b, "%g", nv);
        set_setting(key, b);
        return;
    }
    const Json &opts = row["options"];
    int n = (int)opts.size(), cur = 0;
    for (int i = 0; i < n; i++)
        if (opts[i]["value"].s() == row["value"].s()) cur = i;
    for (int k = 1; k < n; k++) {
        int i = ((cur + dir * k) % n + n) % n;
        if (opts[i]["unavailable"].is_null()) {
            set_setting(key, opts[i]["value"].s());
            return;
        }
    }
}

void Ui::draw_settings(ImVec2 mn, ImVec2 mx) {
    float W = mx.x - mn.x, pw = std::min(1100 * s_, W - 80 * s_);
    ImVec2 pmn((mn.x + mx.x - pw) * 0.5f, mn.y), pmx(pmn.x + pw, mx.y - 14 * s_);
    panel(pmn, pmx, s_);
    float pad = 26 * s_, x = pmn.x + pad, y = pmn.y + 20 * s_;
    if (settings_game_.empty() || dump_["groups"].size() == 0) {
        float cx = (pmn.x + pmx.x) * 0.5f;
        art::icon_gear(ImGui::GetWindowDrawList(), V(cx, pmn.y + 120 * s_), 50 * s_, alpha(art::kCream, 0.5f), kPanel);
        text_center(fonts.big, cx, pmn.y + 200 * s_, art::kCream, settings_game_.empty() ? "Settings appear once a game is ready." : "The game's settings could not be read.");
        text_center(fonts.body, cx, pmn.y + 250 * s_, art::kMuted, settings_game_.empty() ? "They are the game's own options, the same ones as PC OPTIONS inside the game." : settings_err_);
        if (button("##togames", "Go to Games", V(cx - 150 * s_, pmn.y + 310 * s_), V(300 * s_, 60 * s_), art::kBanana, true, fonts.big)) set_tab(Tab::Games);
        return;
    }
    // which game
    std::vector<Game *> built;
    for (auto &g : lib_.games())
        if (!g.exe.empty()) built.push_back(&g);
    float gx = x;
    for (Game *g : built) {
        std::string label = g->id == "dkctf" ? "Tropical Freeze" : "Returns HD", id = "##sg" + g->id;
        float w = text_size(fonts.body, label).x + 50 * s_;
        if (button(id.c_str(), label.c_str(), V(gx, y), V(w, 48 * s_), g->id == settings_game_ ? art::kBanana : kButton, true, fonts.body))
            select_settings_game(g->id);
        gx += w + 14 * s_;
    }
    // groups on the right of the same line
    const Json &groups = dump_["groups"];
    float gw = 0;
    for (size_t i = 0; i < groups.size(); i++) gw += text_size(fonts.body, groups[i]["label"].s()).x + 50 * s_ + 12 * s_;
    float qx = pmx.x - pad - gw + 12 * s_;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false)) group_ = (group_ + (int)groups.size() - 1) % (int)groups.size();
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false)) group_ = (group_ + 1) % (int)groups.size();
    for (size_t i = 0; i < groups.size(); i++) {
        std::string label = groups[i]["label"].s(), id = "##grp" + std::to_string(i);
        float w = text_size(fonts.body, label).x + 50 * s_;
        if (button(id.c_str(), label.c_str(), V(qx, y), V(w, 48 * s_), (int)i == group_ ? art::kBanana : kButton, true, fonts.body)) group_ = (int)i;
        qx += w + 12 * s_;
    }
    y += 64 * s_;
    // rows
    float desc_h = 84 * s_, list_h = pmx.y - pad - desc_h - y;
    ImGui::SetCursorScreenPos(V(x, y));
    ImGui::BeginChild("##rows", V(pw - 2 * pad, list_h), ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar);
    const Json &rows = groups[group_]["rows"];
    float rw = pw - 2 * pad, rh = 56 * s_, vw = 400 * s_;
    const Json *focused = nullptr;
    for (size_t i = 0; i < rows.size(); i++) {
        const Json &row = rows[i];
        ImVec2 p = ImGui::GetCursorScreenPos();
        std::string id = "##row" + row["key"].s();
        bool pressed = ImGui::InvisibleButton(id.c_str(), V(rw, rh), ImGuiButtonFlags_EnableNav);
        ImGuiID iid = ImGui::GetItemID();
        if (focus_take()) ImGui::SetFocusID(iid, ImGui::GetCurrentWindow()), GImGui->NavHighlightItemUnderNav = true;
        bool foc = ImGui::IsItemFocused(), hov = ImGui::IsItemHovered();
        bool locked = !row["unavailable"].is_null() || row["set_by_env"].truthy();
        bool range = row["type"].s() == "range";
        ImDrawList *dl = ImGui::GetWindowDrawList();
        if (foc || hov) focused = &row;
        if ((foc && nav_visible()) || hov) {
            dl->AddRectFilled(p, V(p.x + rw, p.y + rh), IM_COL32(246, 201, 69, 40), 12 * s_);
            dl->AddRect(p, V(p.x + rw, p.y + rh), art::kBanana, 12 * s_, 0, 3 * s_);
        } else if (i % 2 == 0) dl->AddRectFilled(p, V(p.x + rw, p.y + rh), IM_COL32(255, 255, 255, 8), 12 * s_);
        ImU32 tc = locked ? art::kMuted : art::kCream;
        float ty = p.y + (rh - fonts.body->FontSize) * 0.5f;
        dl->AddText(fonts.body, fonts.body->FontSize, V(p.x + 20 * s_, ty), tc, row["label"].s().c_str());
        float lx = p.x + 20 * s_ + text_size(fonts.body, row["label"].s()).x + 14 * s_;
        auto chip = [&](const char *t, ImU32 c) {
            ImVec2 ts = text_size(fonts.small, t);
            dl->AddRect(V(lx, ty - 1 * s_), V(lx + ts.x + 18 * s_, ty + ts.y + 3 * s_), c, 20 * s_, 0, 2 * s_);
            dl->AddText(fonts.small, fonts.small->FontSize, V(lx + 9 * s_, ty + 1 * s_), c, t);
            lx += ts.x + 28 * s_;
        };
        if (row["restart"].truthy()) chip("next start", art::kInfo);
        if (row["set_by_env"].truthy()) chip("set outside", art::kWarn);
        // the value box
        ImVec2 v0(p.x + rw - vw - 14 * s_, p.y + 9 * s_), v1(p.x + rw - 14 * s_, p.y + rh - 9 * s_);
        float vcy = (v0.y + v1.y) * 0.5f;
        std::string shown;
        if (range) {
            double v = atof(row["value"].s().c_str()), lo = row["min"].n(), hi = row["max"].n();
            float f = hi > lo ? (float)((v - lo) / (hi - lo)) : 0;
            char b[48];
            snprintf(b, sizeof b, "%g%s", v, row["unit"].s().c_str());
            shown = b;
            float bx0 = v0.x + 40 * s_, bx1 = v1.x - 110 * s_;
            dl->AddRectFilled(V(bx0, vcy - 6 * s_), V(bx1, vcy + 6 * s_), IM_COL32(0, 0, 0, 140), 6 * s_);
            dl->AddRectFilled(V(bx0, vcy - 6 * s_), V(bx0 + (bx1 - bx0) * f, vcy + 6 * s_), locked ? art::kMuted : art::kBanana, 6 * s_);
            dl->AddCircleFilled(V(bx0 + (bx1 - bx0) * f, vcy), 12 * s_, locked ? art::kMuted : art::kCream);
            dl->AddText(fonts.body, fonts.body->FontSize, V(v1.x - 40 * s_ - text_size(fonts.body, shown).x, vcy - fonts.body->FontSize * 0.5f), tc, shown.c_str());
        } else {
            const Json &opts = row["options"];
            shown = row["value"].s();
            for (size_t k = 0; k < opts.size(); k++)
                if (opts[k]["value"].s() == row["value"].s()) shown = opts[k]["label"].s();
            if (!row["unavailable"].is_null()) shown = "Not available yet";
            ImVec2 ts = text_size(fonts.body, shown);
            dl->AddRectFilled(v0, v1, IM_COL32(0, 0, 0, 110), 10 * s_);
            dl->AddText(fonts.body, fonts.body->FontSize, V((v0.x + v1.x - ts.x) * 0.5f, vcy - ts.y * 0.5f), tc, shown.c_str());
        }
        if (!locked) {
            art::triangle(dl, V(v0.x + 18 * s_, vcy), 9 * s_, -1, (foc || hov) ? art::kBanana : alpha(art::kCream, 0.6f));
            art::triangle(dl, V(v1.x - 18 * s_, vcy), 9 * s_, 1, (foc || hov) ? art::kBanana : alpha(art::kCream, 0.6f));
        }
        // input: left/right on the focused row, clicks on the arrows (or the bar), A cycles a choice
        int dir = 0;
        if (foc) {
            static const ImGuiKey left[] = {ImGuiKey_LeftArrow, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft};
            static const ImGuiKey right[] = {ImGuiKey_RightArrow, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight};
            for (ImGuiKey k : left) ImGui::SetKeyOwner(k, iid), dir -= ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, iid);
            for (ImGuiKey k : right) ImGui::SetKeyOwner(k, iid), dir += ImGui::IsKeyPressed(k, ImGuiInputFlags_Repeat, iid);
        }
        if (pressed) {
            float mx_ = ImGui::GetIO().MousePos.x;
            bool by_mouse = ImGui::IsMouseReleased(0);
            if (by_mouse && mx_ >= v0.x && mx_ < v0.x + 50 * s_) dir = -1;
            else if (by_mouse && mx_ > v1.x - 50 * s_ && mx_ <= v1.x) dir = 1;
            else if (range && by_mouse && mx_ > v0.x + 40 * s_ && mx_ < v1.x - 110 * s_ && !locked) {
                float bx0 = v0.x + 40 * s_, bx1 = v1.x - 110 * s_;
                double lo = row["min"].n(), hi = row["max"].n(), st = row["step"].n(1);
                double nv = std::clamp(std::round((lo + (hi - lo) * (mx_ - bx0) / (bx1 - bx0)) / st) * st, lo, hi);
                char b[32];
                snprintf(b, sizeof b, "%g", nv);
                set_setting(row["key"].s(), b);
            } else if (!range) dir = 1;
        }
        if (dir) {
            Json copy = row;   // the dump is replaced by the change
            step_setting(copy, dir > 0 ? 1 : -1);
            ImGui::EndChild();
            return;
        }
        ImGui::SetCursorScreenPos(V(p.x, p.y + rh + 4 * s_));
    }
    ImGui::Dummy(V(1, 4 * s_));
    ImGui::EndChild();
    // description of the row under the cursor
    float dy = pmx.y - pad - desc_h + 10 * s_;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(V(x, dy), V(pmx.x - pad, pmx.y - pad), IM_COL32(0, 0, 0, 90), 12 * s_);
    if (focused) {
        const Json &row = *focused;
        std::string d = row["description"].s();
        std::string why = row["unavailable"].s();
        std::map<std::string, std::string> reasons;   // reason -> the options it covers
        for (size_t k = 0; k < row["options"].size(); k++) {
            const Json &o = row["options"][k];
            if (!o["unavailable"].is_null()) {
                std::string &v = reasons[o["unavailable"].s()];
                v += (v.empty() ? "" : ", ") + o["label"].s();
            }
        }
        float tw = pmx.x - pad - x - 32 * s_;
        text_at(fonts.small, V(x + 16 * s_, dy + 10 * s_), art::kCream, d, tw);
        float yy = dy + 14 * s_ + text_size(fonts.small, d, tw).y;
        std::string extra = !why.empty() ? "Not available yet: " + why : "";
        if (extra.empty() && !reasons.empty()) extra = reasons.begin()->second + " not available yet: " + reasons.begin()->first;
        if (!extra.empty()) text_at(fonts.small, V(x + 16 * s_, yy), art::kWarn, extra, tw);
    } else {
        text_at(fonts.small, V(x + 16 * s_, dy + 10 * s_), art::kMuted,
                "These are the game's own settings, saved where the game reads them. Change them here or in PC OPTIONS inside the game.",
                pmx.x - pad - x - 32 * s_);
    }
}

// ---- about

void Ui::draw_about(ImVec2 mn, ImVec2 mx) {
    float W = mx.x - mn.x, pw = std::min(1100 * s_, W - 80 * s_);
    ImVec2 pmn((mn.x + mx.x - pw) * 0.5f, mn.y), pmx(pmn.x + pw, mx.y - 14 * s_);
    panel(pmn, pmx, s_);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float bh = std::min(220 * s_, (pmx.y - pmn.y) * 0.5f);
    art::barrel(dl, V(pmn.x + 60 * s_ + bh * 0.4f, pmn.y + 40 * s_ + bh * 0.55f), bh, fonts.logo);
    art::banana(dl, V(pmn.x + 50 * s_ + bh * 0.05f, pmn.y + 40 * s_ + bh * 1.02f), bh * 0.4f, 0.35f);
    float x = pmn.x + 100 * s_ + bh * 0.8f, y = pmn.y + 30 * s_, wrap = pmx.x - 30 * s_ - x;
    shadow_text(fonts.title, V(x, y), art::kBanana, "Country-Box " + launcher_version(), 2 * s_);
    y += fonts.title->FontSize + 10 * s_;
    auto para = [&](ImFont *f, ImU32 c, const std::string &t) {
        text_at(f, V(x, y), c, t, wrap);
        y += text_size(f, t, wrap).y + 12 * s_;
    };
    para(fonts.body, art::kCream,
         "Plays Donkey Kong Country Returns HD and Donkey Kong Country: Tropical Freeze natively on Linux and the Steam Deck, "
         "built on your computer from your own copy of the game.");
    para(fonts.small, alpha(art::kCream, 0.85f),
         "Country-Box contains no game files and never downloads, unpacks or decrypts any. Your saves and settings stay with your game "
         "and are never touched by an update.");
    y += 6 * s_;
    text_at(fonts.big, V(x, y), art::kBanana, "Updates");
    y += fonts.big->FontSize + 6 * s_;
    para(fonts.body, art::kCream,
         "Updates come from this project's GitHub Releases and are checked when the launcher opens. The download is "
         "verified against its published SHA-256; saves and settings are backed up and never touched.");
    if (checker_) {
        para(fonts.body, art::kMuted, "Checking for updates…");
        button("##upd", "Check for updates", V(x, y), V(320 * s_, 54 * s_), kButton, false, fonts.body);
    } else if (checked_) {
        if (update_info_.available) {
            para(fonts.body, art::kCream, "Country-Box " + update_info_.version + " is available.");
            if (button("##updgo", "Update now", V(x, y), V(320 * s_, 54 * s_), art::kBanana, true, fonts.body)) start_update();
        } else {
            para(fonts.body, art::kCream,
                 update_info_.err.empty() ? "Country-Box is up to date." : "The check failed: " + update_info_.err);
            if (button("##upd", "Check again", V(x, y), V(320 * s_, 54 * s_), kButton, true, fonts.body)) start_update_check();
        }
    } else {
        if (button("##upd", "Check for updates", V(x, y), V(320 * s_, 54 * s_), kButton, true, fonts.body)) start_update_check();
    }
    y += 54 * s_ + 22 * s_;
    para(fonts.small, art::kMuted, "Library: " + lib_.home() + "    Logs: " + lib_.home() + "/logs");
    para(fonts.small, art::kMuted,
         "Built on Metroid Prime 4 Recomp (Jeff Aigner), fe3h-pc-recomp, totk-pc-recomp and LAS Recomp (MIT); SDL 3 (zlib); "
         "Dear ImGui (MIT); Roboto (Apache-2.0). Game names belong to their owners; Country-Box is not affiliated with them.");
    (void)focus_take();
}
