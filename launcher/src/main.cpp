// Country-Box launcher: entry point. With no action it opens the hub window (SDL 3 + SDL_Renderer + Dear ImGui, the
// runtime's stack); every action also exists on the command line, for scripts and tests:
//
//   country-box-launcher [--home DIR]                     the hub (fullscreen under gamescope / Steam Deck Game Mode)
//       --size WxH | --fullscreen                         window size (default 1280x800, the Deck's panel)
//       --shot-dir DIR --auto "step;step;..."             scripted run: steps below, screenshots into DIR
//   country-box-launcher --status                          every game's state as JSON
//   country-box-launcher --verify GAME FOLDER [--quick]    check a copy against games/GAME/manifest.toml (JSON; exit 0 ok, 1 not)
//   country-box-launcher --add GAME FOLDER                 check, and on success record it in the library
//   country-box-launcher --prepare GAME [--shaders]        build the game from the checked copy here (progress on
//                                                          stderr, JSON answer on stdout; --shaders: only the shader
//                                                          preparation); resumable — cancel and run it again
//   country-box-launcher --import-build GAME PATH          install a build Prepare made on another computer (folder
//                                                          or .tar/.tar.gz/.tar.zst), checked against this copy
//   country-box-launcher --set-build GAME DIR              use the game binary in DIR (DIR/country-box)
//   country-box-launcher --play GAME                       start the game and wait for it
//
// --auto steps: frames:N, tab:games|settings|about, browse:GAME:DIR, add:GAME:FOLDER, prepare:GAME[:shaders],
// import:GAME, wait-check, wait-prep, wait-stage:S[:N] (S: 2 lift, 3 compile, 4 shaders), settings-game:GAME,
// key:NAME (down|up|left|right|a|b|lb|rb), pad (show the controller cursor), nav (print the focus), shot:NAME, quit,
// update-wait (until the update check answered), update-ask (show the dialog), update-now, update-notnow.
// CB_LAUNCHER_DEBUG_NAV=1 logs Dear ImGui's navigation and focus events. CB_LAUNCHER_NO_UPDATE=1 skips the check.
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "imgui_internal.h"
#include "json.h"
#include "library.h"
#include "manifest.h"
#include "png.h"
#include "prepare.h"
#include "proc.h"
#include "sha256.h"
#include "ui.h"
#include "update.h"

static const unsigned char kRoboto[] = {
#embed "../../core/runtime/ui/fonts/Roboto-Medium.ttf"
};

static void usage() {
    fprintf(stderr,
            "usage: country-box-launcher [--home DIR] [--size WxH | --fullscreen] [--shot-dir DIR --auto STEPS]\n"
            "       country-box-launcher [--home DIR] --status | --verify GAME FOLDER [--quick] | --add GAME FOLDER |\n"
            "                            --set-build GAME DIR | --prepare GAME [--shaders] | --import-build GAME PATH |\n"
            "                            --play GAME | --check-update | --update\n"
            "games: dkcrhd (Returns HD), dkctf (Tropical Freeze)\n"
            "update channel: CB_UPDATE_API or <home>/update.ini api_url (default: this project's GitHub Releases)\n");
}

static void load_fonts(UiFonts &f, float s) {
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x0400, 0x04FF, 0x2010, 0x2027, 0x2190, 0x2193, 0};
    auto add = [&](float px) {
        ImFontConfig c;
        c.FontDataOwnedByAtlas = false;
        c.OversampleH = 2;
        return io.Fonts->AddFontFromMemoryTTF((void *)kRoboto, (int)sizeof kRoboto, px, &c, ranges);
    };
    f.body = add(24 * s);
    f.small = add(19 * s);
    f.big = add(29 * s);
    f.title = add(40 * s);
    f.logo = add(66 * s);
    io.FontDefault = f.body;
}

static void style(float s) {
    ImGuiStyle &st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowPadding = ImVec2(0, 0);
    st.ItemSpacing = ImVec2(0, 0);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);   // every widget draws its own focus
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    st.ScrollbarSize = 10 * s;
    st.ChildRounding = 14 * s;
}

static bool save_shot(SDL_Renderer *r, const std::string &path) {
    SDL_Surface *s = SDL_RenderReadPixels(r, nullptr);
    if (!s) return false;
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);   // bytes R, G, B, A
    SDL_DestroySurface(s);
    if (!c) return false;
    bool ok = png_write_rgba(path.c_str(), (const uint8_t *)c->pixels, c->w, c->h, c->pitch) == 0;
    SDL_DestroySurface(c);
    return ok;
}

static std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

static int run_hub(Library &lib, int w, int h, bool fullscreen, const std::string &shot_dir, const std::string &auto_steps) {
    SDL_SetHint(SDL_HINT_APP_NAME, "Country-Box");
    SDL_SetHint(SDL_HINT_APP_ID, "country-box-launcher");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "launcher: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("Country-Box", w, h, SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!win) {
        fprintf(stderr, "launcher: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer *ren = SDL_CreateRenderer(win, nullptr);
    if (!ren) {
        fprintf(stderr, "launcher: SDL_CreateRenderer: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCursorVisibleAuto = true;
    if (getenv("CB_LAUNCHER_DEBUG_NAV")) GImGui->DebugLogFlags |= ImGuiDebugLogFlags_EventNav | ImGuiDebugLogFlags_EventFocus | ImGuiDebugLogFlags_OutputToTTY;
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    Ui ui(lib);
    ui.set_scripted(!auto_steps.empty());
    ui.start_update_check();
    float scale = -1;
    std::vector<std::string> steps = auto_steps.empty() ? std::vector<std::string>() : split(auto_steps, ';');
    size_t step = 0;
    int wait_frames = 0, frame = 0;
    ImGuiKey key_up = ImGuiKey_None;
    bool running = true;
    Uint64 t0 = SDL_GetTicksNS();
    while (running) {
        // Idle: wait for input instead of drawing at the display's rate; animate while something moves.
        SDL_Event e;
        bool scripted = step < steps.size();
        if (!scripted && !ui.busy() && frame > 2) {
            if (SDL_WaitEventTimeout(&e, lib.any_running() ? 500 : 50)) {
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type == SDL_EVENT_QUIT) running = false;
            }
        }
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT) running = false;
        }
        int pw, ph;
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
        float s = std::min(pw / 1280.0f, ph / 800.0f);
        if (fabsf(s - scale) > 0.01f) {
            scale = s;
            load_fonts(ui.fonts, s);
            style(s);
            ImGui_ImplSDLRenderer3_DestroyFontsTexture();
            ImGui_ImplSDLRenderer3_CreateFontsTexture();
        }
        if (key_up != ImGuiKey_None) io.AddKeyEvent(key_up, false), key_up = ImGuiKey_None;
        // scripted steps
        std::string shot;
        while (step < steps.size() && wait_frames <= 0) {
            std::string st = steps[step];
            auto parts = split(st, ':');
            const std::string &op = parts[0];
            if (op == "wait-check" && ui.checking()) break;
            if (op == "wait-prep" && !ui.prep_finished()) break;
            if (op == "wait-stage" && parts.size() > 1 &&
                !ui.prep_reached(atoi(parts[1].c_str()), parts.size() > 2 ? atoi(parts[2].c_str()) : 0))
                break;
            if (op == "update-wait" && (ui.update_checking() || !ui.update_known())) break;
            step++;
            if (op == "frames" && parts.size() > 1) wait_frames = atoi(parts[1].c_str());
            else if (op == "tab" && parts.size() > 1)
                ui.set_tab(parts[1] == "settings" ? Ui::Tab::Settings : parts[1] == "about" ? Ui::Tab::About : Ui::Tab::Games);
            else if (op == "browse" && parts.size() > 2) ui.open_browse(parts[1], st.substr(op.size() + parts[1].size() + 2));
            else if (op == "add" && parts.size() > 2) ui.start_check(parts[1], st.substr(op.size() + parts[1].size() + 2));
            else if (op == "prepare" && parts.size() > 1) ui.start_prepare(parts[1], parts.size() > 2 && parts[2] == "shaders");
            else if (op == "import" && parts.size() > 1)
                ui.start_import(parts[1], parts.size() > 2 ? st.substr(op.size() + parts[1].size() + 2) : "");
            else if (op == "use-folder") ui.start_import_dir(ui.flow_game(), ui.browse_dir());
            else if (op == "settings-game" && parts.size() > 1) ui.select_settings_game(parts[1]);
            else if (op == "update-ask") ui.show_update_ask();
            else if (op == "update-now") ui.start_update();
            else if (op == "update-notnow") ui.decline_update();
            else if (op == "pad") GImGui->NavCursorVisible = true, GImGui->NavHighlightItemUnderNav = true;
            else if (op == "key" && parts.size() > 1) {
                const std::string &k = parts[1];
                // keyboard keys: without a real controller the backend reports no gamepad, and Dear ImGui's
                // gamepad navigation ignores gamepad keys then; the hub treats both the same
                ImGuiKey key = k == "down" ? ImGuiKey_DownArrow : k == "up" ? ImGuiKey_UpArrow : k == "left" ? ImGuiKey_LeftArrow
                               : k == "right" ? ImGuiKey_RightArrow : k == "a" ? ImGuiKey_Space : k == "b" ? ImGuiKey_Escape
                               : k == "lb" ? ImGuiKey_PageUp : k == "rb" ? ImGuiKey_PageDown : ImGuiKey_None;
                if (key != ImGuiKey_None) io.AddKeyEvent(key, true), key_up = key, wait_frames = 3;
                break;
            } else if (op == "shot" && parts.size() > 1) shot = shot_dir + "/" + parts[1] + ".png", wait_frames = 1;
            else if (op == "nav")
                printf("nav: id %08x window %s cursor %d\n", GImGui->NavId, GImGui->NavWindow ? GImGui->NavWindow->Name : "-",
                       (int)GImGui->NavCursorVisible);
            else if (op == "quit") running = false;
            if (!shot.empty()) break;
        }
        if (wait_frames > 0) wait_frames--;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        float t = (float)((SDL_GetTicksNS() - t0) / 1e9);
        ui.frame(scale, t);
        ImGui::Render();
        SDL_SetRenderDrawColor(ren, 6, 22, 14, 255);
        SDL_RenderClear(ren);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        if (!shot.empty()) {
            if (save_shot(ren, shot)) printf("shot %s (%dx%d)\n", shot.c_str(), pw, ph);
            else fprintf(stderr, "launcher: screenshot %s failed: %s\n", shot.c_str(), SDL_GetError());
        }
        SDL_RenderPresent(ren);
        frame++;
        if (!steps.empty() && step >= steps.size() && wait_frames <= 0) running = false;
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv) {
    std::string home, shot_dir = ".", auto_steps, action;
    std::vector<std::string> args;
    int w = 1280, h = 800;
    bool fullscreen = getenv("GAMESCOPE_WAYLAND_DISPLAY") || getenv("SteamDeck"), quick = false, shaders_only = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--home") home = next();
        else if (a == "--size") {
            std::string v = next();
            if (sscanf(v.c_str(), "%dx%d", &w, &h) != 2) return usage(), 2;
            fullscreen = false;
        } else if (a == "--fullscreen") fullscreen = true;
        else if (a == "--shot-dir") shot_dir = next();
        else if (a == "--auto") auto_steps = next();
        else if (a == "--quick") quick = true;
        else if (a == "--shaders") shaders_only = true;
        else if (a == "--status" || a == "--verify" || a == "--add" || a == "--set-build" || a == "--play" ||
                 a == "--prepare" || a == "--import-build" || a == "--check-update" || a == "--update")
            action = a;
        else if (a == "--help" || a == "-h") return usage(), 0;
        else if (a[0] == '-') return usage(), 2;
        else args.push_back(a);
    }
    Library lib(launcher_home(home));
    lib.load();
    if (action.empty()) return run_hub(lib, w, h, fullscreen, shot_dir, auto_steps);
    if (action == "--check-update" || action == "--update") {
        std::string err;
        UpdateInfo i = update_check(lib.home(), &err);
        if (action == "--check-update" || !i.err.empty() || !i.available) {
            printf("%s\n", update_check_json(i).c_str());
            return i.err.empty() ? 0 : 1;
        }
        std::vector<std::pair<std::string, std::string>> builds;
        for (auto &g : lib.games())
            if (!g.build_dir.empty()) builds.push_back({g.id, g.build_dir});
        UpdateJob job(i, lib.home(), builds);
        while (!job.done()) SDL_Delay(20);
        printf("%s\n", update_result_json(job).c_str());
        if (job.ok && getenv("APPIMAGE") && *getenv("APPIMAGE"))
            fprintf(stderr, "launcher: %s installed over %s (the previous one is kept as %s.old); start it again\n",
                    job.to_version.c_str(), job.installed_dir.c_str(), job.installed_dir.c_str());
        else if (job.ok)
            fprintf(stderr, "launcher: %s installed (%s); previous kept at app/%s; restart the launcher to run it\n",
                    job.to_version.c_str(), job.installed_dir.c_str(), job.from_version.c_str());
        return job.ok ? 0 : 1;
    }
    Game *g = args.empty() ? nullptr : lib.find(args[0]);
    if (action != "--status" && !g) {
        fprintf(stderr, "launcher: unknown game '%s'\n", args.empty() ? "" : args[0].c_str());
        return usage(), 2;
    }
    if (action == "--status") {
        printf("%s\n", lib.status_json().c_str());
        return 0;
    }
    if (action == "--verify" || action == "--add") {
        if (args.size() < 2) return usage(), 2;
        VerifyResult r = verify_copy(g->id, args[1], !quick);
        printf("%s\n", verify_json(g->id, r).c_str());
        if (r.kind != VerifyResult::Ok) return 1;
        if (action == "--add") {
            lib.set_copy(*g, r);
            std::string err;
            if (!lib.save(&err)) return fprintf(stderr, "launcher: %s\n", err.c_str()), 1;
        }
        return 0;
    }
    if (action == "--set-build") {
        if (args.size() < 2) return usage(), 2;
        lib.set_build(*g, args[1]);
        std::string err;
        if (!lib.save(&err)) return fprintf(stderr, "launcher: %s\n", err.c_str()), 1;
        printf("%s\n", lib.status_json().c_str());
        return g->exe.empty() ? 1 : 0;
    }
    if (action == "--prepare") {
        if (g->copy.empty() || g->state == GameState::CopyMissing || g->state == GameState::NeedsCheck) {
            fprintf(stderr, "launcher: %s\n", g->detail.c_str());
            return 1;
        }
        // progress lines on stderr (tests read the JSON on stdout); Ctrl-C cancels and exits 3
        struct Ctx {
            PrepareJob *job;
            int last_stage = -1, last_done = -1;
        } ctx;
        auto progress = +[](void *p) -> bool {
            Ctx *c = (Ctx *)p;
            const char *names[] = {"starting", "checking", "lifting", "compiling", "shaders", "done"};
            int st = c->job->stage.load(), d = c->job->stage_done.load(), t = c->job->stage_total.load();
            if (st != c->last_stage || d != c->last_done) {
                c->last_stage = st, c->last_done = d;
                fprintf(stderr, "prepare: %s %d/%d  %s\n", names[st], d, t, c->job->line().c_str());
            }
            return false;
        };
        PrepareJob::Outcome out;
        {
            PrepareJob job(lib.home(), *g, shaders_only ? PrepareJob::ShadersOnly : PrepareJob::Full);
            ctx.job = &job;
            while (!job.done()) {
                if (progress(&ctx)) job.cancel();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            out = job.outcome();
        }
        printf("{\"ok\": %s, \"game\": %s, \"kind\": %s, \"headline\": %s, \"explain\": %s, \"hint\": %s, "
               "\"log\": %s, \"elapsed_s\": %.1f}\n",
               out.ok ? "true" : "false", json_quote(g->id).c_str(), json_quote(out.kind).c_str(),
               json_quote(out.headline).c_str(), json_quote(out.explain).c_str(), json_quote(out.hint).c_str(),
               json_quote(out.log).c_str(), out.elapsed_s);
        return out.ok ? 0 : (out.kind == "shaders" ? 2 : 1);
    }
    if (action == "--import-build") {
        if (args.size() < 2) return usage(), 2;
        ImportOutcome r = import_build(lib.home(), *g, args[1]);
        printf("{\"ok\": %s, \"game\": %s, \"kind\": %s, \"headline\": %s, \"explain\": %s, \"hint\": %s, "
               "\"build\": %s, \"elapsed_s\": %.1f}\n",
               r.ok ? "true" : "false", json_quote(g->id).c_str(), json_quote(r.kind).c_str(),
               json_quote(r.headline).c_str(), json_quote(r.explain).c_str(), json_quote(r.hint).c_str(),
               json_quote(r.build).c_str(), r.elapsed_s);
        return r.ok ? 0 : 1;
    }
    if (action == "--play") {
        std::string err;
        if (!lib.play(*g, &err)) return fprintf(stderr, "launcher: %s\n", err.c_str()), 1;
        fprintf(stderr, "launcher: %s started (pid %d), log %s\n", g->id.c_str(), (int)g->pid, g->log.c_str());
        int st = 0;
        while (proc_poll(g->pid, &st) == 1) SDL_Delay(200);
        return st;
    }
    return usage(), 2;
}
