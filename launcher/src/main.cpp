// Country-Box launcher: entry point. With no action it opens the hub window (SDL 3 + SDL_Renderer + Dear ImGui, the
// runtime's stack); every action also exists on the command line, for scripts and tests:
//
//   country-box-launcher [--data-dir DIR]                 the hub (fullscreen under gamescope / Steam Deck Game Mode);
//                                                          --home DIR is the same. Default: Country-Box-data next to
//                                                          the AppImage / extracted folder (relocate.h)
//       --resolution WxH | --fullscreen | --windowed      window size (default 1280x800, the Deck's panel); with
//                                                          --play / --skip they set the game's window for that run
//       --no-update                                       no update check this run (CB_LAUNCHER_NO_UPDATE=1)
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
//   country-box-launcher --skip [GAME]                     straight into the game without the hub: GAME, else the one
//                                                          played last, else the one ready; the hub opens instead only
//                                                          when no game is ready to play
//   country-box-launcher --play GAME                       start the game and wait for it, without the hub (Steam
//                                                          launch options, shortcuts); --returns / --freeze for short.
//                                                          Without a terminal, a game that can't start says why in
//                                                          a window
//   country-box-launcher --move-library DIR                move the library (builds, saves, logs) to DIR, or to
//                                                          DIR/Country-Box when DIR has files; later runs use it
//   country-box-launcher --library-choice keep|here|DIR    answer the 1.0.7 first-start question without the window
//
// --auto steps: frames:N, tab:games|settings|about, browse:GAME:DIR, add:GAME:FOLDER, prepare:GAME[:shaders],
// import:GAME, wait-check, wait-prep, wait-stage:S[:N] (S: 2 lift, 3 compile, 4 shaders), settings-game:GAME,
// key:NAME (down|up|left|right|a|b|lb|rb), pad (show the controller cursor), nav (print the focus), shot:NAME, quit,
// update-wait (until the update check answered), update-ask (show the dialog), update-now, update-notnow,
// go:PATH (the browser's path field), system-picker, library-browse, move-here, wait-move, choose:keep|here|folder|later, wheel:X:Y:NOTCHES, mouse:X:Y:0|1 (X, Y: fractions of the window).
// CB_LAUNCHER_DEBUG_NAV=1 logs Dear ImGui's navigation and focus events. CB_LAUNCHER_NO_UPDATE=1 skips the check.
// Started by RecompLauncher on Linux: RECOMPLAUNCHER_DATA is the data folder (relocate.h), and with RECOMPLAUNCHER=1
// there is no update check and no self-update (the hub's and --update): RecompLauncher installs every release.
#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
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
#include "relocate.h"
#include "sha256.h"
#include "ui.h"
#include "update.h"

static const unsigned char kRoboto[] = {
#embed "../../core/runtime/ui/fonts/Roboto-Medium.ttf"
};

static void usage(FILE *to = stderr) {
    fprintf(to,
            "Country-Box %s\n"
            "usage: country-box [OPTIONS] [ACTION]\n"
            "\n"
            "With no action the launcher window opens. Start the game straight away, without the launcher:\n"
            "  --skip [GAME]            the game played last (or GAME, or the one that is ready); opens the launcher\n"
            "                           only when no game is ready to play\n"
            "  --play GAME              that game; says why in a window when it can't start yet\n"
            "  --returns, --freeze      the same as --play returns / --play freeze\n"
            "\n"
            "Window (the launcher's, or the game's for this run with --skip / --play):\n"
            "  --fullscreen             full screen\n"
            "  --windowed               a window\n"
            "  --resolution WxH         window size, e.g. 1920x1080 (--size WxH is the same)\n"
            "\n"
            "Folders:\n"
            "  --data-dir DIR           keep everything (builds, saves, settings, logs, caches) in DIR for this run\n"
            "                           (--home DIR and CB_LAUNCHER_HOME=DIR are the same). Default: the folder\n"
            "                           Country-Box-data next to the AppImage or the extracted Country-Box folder\n"
            "  --add [GAME] FOLDER      check your extracted copy (the folder with exefs/ and romfs/) and keep it;\n"
            "                           without GAME the copy says which game it is\n"
            "  --verify [GAME] FOLDER [--quick]  only check a copy (JSON; exit 0 when it is good)\n"
            "  --set-build GAME DIR     play the build in DIR (DIR/country-box) instead of the data folder's\n"
            "  --move-library DIR       move the library to DIR (or DIR/Country-Box when DIR has files)\n"
            "  --library-choice keep|here|DIR  first start of 1.0.7 with a library in ~/.local/share: keep it there,\n"
            "                           move it next to Country-Box, or to DIR, without asking (the window asks)\n"
            "\n"
            "Building and updating:\n"
            "  --prepare GAME [--shaders]    build the game from your checked copy (resumable)\n"
            "  --import-build GAME PATH      install a build made on another computer (folder or .tar)\n"
            "  --setup GAME=FOLDER...        check and prepare in one go (GAME: dkcrhd, dkctf or auto)\n"
            "  --check-update | --update     look for / install a new Country-Box (asks nothing)\n"
            "  --no-update                   don't look for updates this run (CB_LAUNCHER_NO_UPDATE=1)\n"
            "\n"
            "Other: --status (JSON), --version, --help. Scripted screenshots: --shot-dir DIR --auto STEPS.\n"
            "Games: returns or dkcrhd (Returns HD), freeze or dkctf (Tropical Freeze).\n"
            "Update channel: CB_UPDATE_API or <data>/update.ini api_url (default: this project's GitHub Releases).\n",
            launcher_version().c_str());
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

static int run_hub(Library &lib, int w, int h, bool fullscreen, const std::string &shot_dir, const std::string &auto_steps,
                   const Migration *migrate = nullptr, const std::string &notice = "") {
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
    if (migrate) ui.ask_migration(*migrate);   // the 1.0.7 first-start question (relocate.h)
    if (!notice.empty()) ui.notice(notice);
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
            if (op == "wait-move" && !ui.moving_finished()) break;
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
            else if (op == "system-picker") ui.open_system_picker();
            else if (op == "library-browse") ui.start_library_browse();
            else if (op == "move-here") ui.start_library_move(ui.browse_dir());
            else if (op == "choose" && parts.size() > 1) ui.migration_choice(parts[1]);
            else if (op == "go") ui.browse_go(st.substr(op.size() + 1));   // the browser's path field
            else if (op == "wheel" && parts.size() > 3)   // wheel:X:Y:NOTCHES (X, Y: fractions of the window; + scrolls down)
                io.AddMousePosEvent(pw * (float)atof(parts[1].c_str()), ph * (float)atof(parts[2].c_str())),
                io.AddMouseWheelEvent(0, -(float)atof(parts[3].c_str())), wait_frames = 3;
            else if (op == "mouse" && parts.size() > 3) {   // mouse:X:Y:DOWN (0/1): moves there, then presses or lets go
                io.AddMousePosEvent(pw * (float)atof(parts[1].c_str()), ph * (float)atof(parts[2].c_str()));
                io.AddMouseButtonEvent(0, parts[3] == "1");
                wait_frames = 2;
                break;
            }
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
        if (ui.wants_quit()) running = false;   // B on the Games tab, or "Restart now" after an update started the new one
    }
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

// --setup GAME=FOLDER... (GAME may be "auto": the copy says which game it is): check each copy, keep it in the library and prepare it, without a window. For launchers
// such as RecompLauncher (recomplauncher.json, "prepare"). GAME= with no folder is skipped (a game the player does
// not have). stdout carries only "progress <0..1> <text>" lines, the
// errors go to stderr. Exit 0 ready; 2 usage; 4 a copy was refused (wrong game, version or files); 1 Prepare failed.
// The same steps as the hub's Add and Prepare, with the hub's stage weights for the overall fraction.
static int run_setup(Library &lib, const std::vector<std::string> &args) {
    std::vector<std::pair<Game *, std::string>> todo;
    for (auto &a : args) {
        size_t eq = a.find('=');
        Game *g = eq == std::string::npos ? nullptr : lib.find(a.substr(0, eq));
        if (eq != std::string::npos && a.compare(0, eq, "auto") == 0 && eq + 1 < a.size()) {
            // auto=FOLDER: the game is the one the copy's title id names (main.npdm), as the hub's Add tells them apart
            const std::string &first = lib.games().front().id;
            VerifyResult q = verify_copy(first, a.substr(eq + 1), false);
            g = q.kind == VerifyResult::Ok ? lib.find(first) : q.kind == VerifyResult::OtherGame ? lib.find(q.other_game) : nullptr;
            if (!g) {
                fprintf(stderr, "launcher: %s %s %s\n", q.headline.c_str(), q.explain.c_str(), q.hint.c_str());
                return 4;
            }
        }
        // a game the player did not give: an empty folder, or a launcher placeholder left as written ({input:...})
        if (g && (eq + 1 >= a.size() || a.compare(eq + 1, 7, "{input:") == 0)) continue;
        if (!g) {
            fprintf(stderr, "launcher: --setup takes GAME=FOLDER, not '%s'\n", a.c_str());
            return usage(), 2;
        }
        todo.push_back({g, a.substr(eq + 1)});
    }
    if (todo.empty()) return fprintf(stderr, "launcher: --setup got no game folder\n"), usage(), 2;
    const double n = (double)todo.size();
    auto say = [](double f, const std::string &text) {
        printf("progress %.3f %s\n", std::min(1.0, std::max(0.0, f)), text.c_str());
        fflush(stdout);
    };
    for (size_t i = 0; i < todo.size(); i++) {
        Game *g = todo[i].first;
        std::string name = g->manifest ? g->manifest->name : g->id;
        say(i / n, "Checking your copy of " + name);
        VerifyResult r = verify_copy(g->id, todo[i].second, true);
        if (r.kind != VerifyResult::Ok) {
            fprintf(stderr, "launcher: %s: %s %s %s\n", name.c_str(), r.headline.c_str(), r.explain.c_str(), r.hint.c_str());
            return 4;
        }
        lib.set_copy(*g, r);
        std::string err;
        if (!lib.save(&err)) return fprintf(stderr, "launcher: %s\n", err.c_str()), 1;
        const PrepProfile *prof = prep_profile(g->id);
        double lift_w = prof ? prof->lift_s : 100, comp_w = prof ? prof->compile_s : 500;
        double w[] = {2, lift_w, comp_w, std::max(lift_w, comp_w) * 0.15}, total = w[0] + w[1] + w[2] + w[3];
        const char *what[] = {"checking the tools", "turning its code into C", "compiling", "preparing the shaders"};
        PrepareJob job(lib.home(), *g, PrepareJob::Full);
        std::string last;
        while (!job.done()) {
            int st = job.stage.load(), d = job.stage_done.load(), t = job.stage_total.load();
            if (st >= PrepareJob::Preflight && st <= PrepareJob::Shaders) {
                double done = 0;
                for (int k = 0; k < st - PrepareJob::Preflight; k++) done += w[k];
                done += w[st - PrepareJob::Preflight] * (t > 0 && d >= 0 ? std::min(1.0, (double)d / t) : 0);
                std::string text = name + ": " + what[st - PrepareJob::Preflight];
                if (t > 0 && d >= 0) text += " " + std::to_string(d) + "/" + std::to_string(t);
                if (text != last) say((i + done / total) / n, text), last = text;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        const PrepareJob::Outcome &out = job.outcome();
        lib.refresh(*g);
        if (!out.ok && out.kind == "missing_tools" && !g->exe.empty() &&
            (g->state == GameState::Ready || g->state == GameState::NeedsShaders || g->state == GameState::UpdateAvailable)) {
            // a machine without compilers (Steam Deck) keeps playing the build it has; the update waits for Prepare
            fprintf(stderr, "launcher: %s: %s Playing the build already here (Country-Box %s).\n", name.c_str(),
                    out.headline.c_str(), g->build_version.c_str());
            continue;
        }
        if (!out.ok) {
            fprintf(stderr, "launcher: %s: %s %s %s (log: %s)\n", name.c_str(), out.headline.c_str(), out.explain.c_str(),
                    out.hint.c_str(), out.log.c_str());
            return 1;
        }
    }
    say(1, "Ready to play");
    return 0;
}

// Why --play could not start a game. It is mostly run from a Steam shortcut or a desktop icon, where nobody reads
// stderr, so without a terminal the reason also comes up in a window.
static void tell_player(const std::string &title, const std::string &text) {
    fprintf(stderr, "launcher: %s\n", text.c_str());
    if (!isatty(STDERR_FILENO)) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), text.c_str(), nullptr);
}

int main(int argc, char **argv) {
    // Started by the AppImage's AppRun: our SDL is loaded already; the programs started from here (Python, clang,
    // ninja, the game built against the system's SDL) get the player's own LD_LIBRARY_PATH back.
    if (getenv("CB_APPRUN")) {
        const char *orig = getenv("CB_APPRUN_LD_LIBRARY_PATH");
        if (orig && *orig) setenv("LD_LIBRARY_PATH", orig, 1);
        else unsetenv("LD_LIBRARY_PATH");
        unsetenv("CB_APPRUN");
        unsetenv("CB_APPRUN_LD_LIBRARY_PATH");
    }
    std::string home, shot_dir = ".", auto_steps, action, game_size, library_choice;
    std::vector<std::string> args;
    int w = 1280, h = 800, game_fullscreen = -1;   // -1: the game's own setting (mp4.ini)
    bool fullscreen = getenv("GAMESCOPE_WAYLAND_DISPLAY") || getenv("SteamDeck"), quick = false, shaders_only = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--home" || a == "--data-dir") {
            home = next();
            if (home.empty()) return fprintf(stderr, "launcher: %s needs a folder\n", a.c_str()), usage(), 2;
        } else if (a == "--size" || a == "--resolution") {
            std::string v = next();
            if (sscanf(v.c_str(), "%dx%d", &w, &h) != 2 || w < 320 || h < 200)
                return fprintf(stderr, "launcher: %s takes WIDTHxHEIGHT, e.g. 1920x1080\n", a.c_str()), usage(), 2;
            fullscreen = false, game_size = std::to_string(w) + "x" + std::to_string(h);
        } else if (a == "--fullscreen") fullscreen = true, game_fullscreen = 1;
        else if (a == "--windowed") fullscreen = false, game_fullscreen = 0;
        else if (a == "--no-update") setenv("CB_LAUNCHER_NO_UPDATE", "1", 1);
        else if (a == "--shot-dir") shot_dir = next();
        else if (a == "--auto") auto_steps = next();
        else if (a == "--library-choice") {
            library_choice = next();
            if (library_choice.empty()) return fprintf(stderr, "launcher: --library-choice takes keep, here or a folder\n"), usage(), 2;
        }
        else if (a == "--quick") quick = true;
        else if (a == "--shaders") shaders_only = true;
        else if (a == "--status" || a == "--verify" || a == "--add" || a == "--set-build" || a == "--play" ||
                 a == "--prepare" || a == "--import-build" || a == "--check-update" || a == "--update" || a == "--setup" ||
                 a == "--move-library" || a == "--skip")
            action = a;
        else if (a == "--returns" || a == "--freeze")   // the short launch options: --play returns / --play freeze
            action = "--play", args.insert(args.begin(), a.substr(2));
        else if (a == "--help" || a == "-h") return usage(stdout), 0;
        else if (a == "--version") return printf("%s\n", launcher_version().c_str()), 0;
        else if (a[0] == '-') return fprintf(stderr, "launcher: unknown option %s\n", a.c_str()), usage(), 2;
        else args.push_back(a);
    }
    bool starts_game = action == "--play" || action == "--skip";
    // --fullscreen / --windowed / --resolution for the game: its settings for this run only (the environment wins
    // over mp4.ini, cfg.c); mp4.ini keeps what the player chose in the menus
    if (starts_game && game_fullscreen >= 0) setenv("MP4_FULLSCREEN", game_fullscreen ? "1" : "0", 1);
    if (starts_game && !game_size.empty()) setenv("MP4_WINDOW_SIZE", game_size.c_str(), 1);

    // The data folder (relocate.h). First start of a portable Country-Box with a library in ~/.local/share: the
    // launcher window asks where it goes (keep it there / next to Country-Box / a folder the player picks); nothing
    // moves without that answer (or --library-choice). Starting a game straight away (a Steam shortcut) just uses the
    // old place. A library the player had already moved elsewhere only brings its record and tools over, unasked.
    std::string notice;
    const Migration *hub_migration = nullptr;   // the hub asks
    Migration mig;
    bool fixed = !home.empty() || (getenv("CB_LAUNCHER_HOME") && *getenv("CB_LAUNCHER_HOME"));
    // RecompLauncher's {data} (relocate.h): the first start copies the old data folder in; until that has worked,
    // the old folder stays this run's, as before
    bool kept_old = false;
    if (!fixed) {
        OldDataCopy od = plan_old_data_copy();
        if (od.needed && od.busy) {
            set_base_home(od.from), kept_old = true;
            notice = "Your data stays in " + od.from + " for now: a game is running from there. It is copied into " +
                     od.to + " the next time you start.";
        } else if (od.needed) {
            LibraryMove m;
            std::thread t([&] { copy_old_data(od, m); });
            while (!m.finished.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (m.total.load() > 0)
                    fprintf(stderr, "progress %.3f copying your Country-Box data into %s\n", (double)m.done.load() / (double)m.total.load(), od.to.c_str());
            }
            t.join();
            if (m.ok) fprintf(stderr, "launcher: your data was copied from %s to %s (the old folder was left as it was)\n", od.from.c_str(), od.to.c_str());
            else {
                set_base_home(od.from), kept_old = true;
                notice = "Your data stays in " + od.from + " for now: copying it into " + od.to + " failed. " + m.headline + " " +
                         m.explain + " The next start tries again.";
            }
        }
        if (kept_old) fprintf(stderr, "launcher: %s\n", notice.c_str());
    }
    if (!fixed && !kept_old) mig = plan_migration();
    if (!library_choice.empty() && !mig.ask())
        fprintf(stderr, "launcher: --library-choice: nothing to choose (%s)\n",
                mig.stay ? "already kept where it is" : mig.busy ? "a game is running from the old place" : "no old library to move");
    if (mig.stay) set_base_home(mig.from);
    else if (mig.needed) {
        set_base_home(mig.from);   // until it has moved, the old place is this run's
        bool run_now = !mig.ask() ? !mig.busy : !library_choice.empty();
        if (mig.busy)
            notice = "Your library stays in " + mig.from + " for now: a game is running from there. Open Country-Box again "
                     "afterwards to choose where it goes.";
        else if (mig.ask() && library_choice == "keep") {
            std::string err;
            if (keep_library_here(mig, err)) fprintf(stderr, "launcher: your library stays in %s\n", mig.from.c_str());
            else notice = "Your library stays in " + mig.from + ", but that choice could not be recorded: " + err;
            run_now = false;
        } else if (!run_now && action.empty()) hub_migration = &mig;
        else if (!run_now)
            notice = "Your library is still in " + mig.from + ". Open Country-Box to choose where it goes (new in 1.0.7).";
        if (run_now) {
            std::string picked = !mig.ask() || library_choice == "here" ? "" : library_choice;
            LibraryMove m;
            std::thread t([&] { migrate_library(mig, picked, m); });
            while (!m.finished.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (m.total.load() > 0)
                    fprintf(stderr, "progress %.3f moving your library to %s\n", (double)m.done.load() / (double)m.total.load(), m.to.c_str());
            }
            t.join();
            if (m.ok) {
                fprintf(stderr, "launcher: your library moved from %s to %s\n", mig.from.c_str(), m.to.c_str());
                if (!m.backup.empty()) fprintf(stderr, "launcher: a copy of your saves is on your Desktop: %s\n", m.backup.c_str());
            } else
                notice = "Your library stays in " + mig.from + ": moving it failed. " + m.headline + " " + m.explain;
        }
        if (!notice.empty()) fprintf(stderr, "launcher: %s\n", notice.c_str());
    } else if (!fixed && !kept_old && !base_note().empty()) {
        notice = base_note();
        fprintf(stderr, "launcher: %s\n", notice.c_str());
    }
    Library lib(launcher_home(home));
    // temporary files and caches inside the one folder too: the --data-dir one, else the data folder (portable)
    if (fixed) contain_temp_files(lib.home());
    else if (!portable_data_dir().empty()) contain_temp_files(default_launcher_home());
    lib.load();
    if (action == "--skip") {
        // GAME, else the one played last, else the first that can play; none ready: the launcher shows what to do
        Game *g = args.empty() ? lib.last_played() : lib.find(args[0]);
        if (!args.empty() && !g) return fprintf(stderr, "launcher: unknown game '%s'\n", args[0].c_str()), usage(), 2;
        if (!g || !Library::playable(*g)) g = nullptr;
        for (auto &c : lib.games())
            if (!g && args.empty() && Library::playable(c)) g = &c;
        if (!g) {
            fprintf(stderr, "launcher: no game is ready to play yet; opening Country-Box\n");
            return run_hub(lib, w, h, fullscreen, shot_dir, auto_steps, hub_migration, notice);
        }
        action = "--play";
        args = {g->id};
    }
    if (action.empty()) return run_hub(lib, w, h, fullscreen, shot_dir, auto_steps, hub_migration, notice);
    if (action == "--update" && under_recomplauncher()) {   // RecompLauncher installs every release itself
        fprintf(stderr, "launcher: started by RecompLauncher, which installs Country-Box's updates; nothing was changed\n");
        return 1;
    }
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
        if (job.ok && ((getenv("APPIMAGE") && *getenv("APPIMAGE")) || job.installed_dir.find("/app/") == std::string::npos))
            fprintf(stderr, "launcher: %s installed over %s (the previous one is kept as %s.old); start it again\n",
                    job.to_version.c_str(), job.installed_dir.c_str(), job.installed_dir.c_str());
        else if (job.ok)
            fprintf(stderr, "launcher: %s installed (%s); previous kept at app/%s; restart the launcher to run it\n",
                    job.to_version.c_str(), job.installed_dir.c_str(), job.from_version.c_str());
        return job.ok ? 0 : 1;
    }
    if (action == "--setup") return run_setup(lib, args);
    if (action == "--move-library") {
        if (args.size() != 1) return usage(), 2;
        if (launcher_home_fixed()) {
            fprintf(stderr, "launcher: the library folder is set by --home or CB_LAUNCHER_HOME for this run; it can't be moved\n");
            return 1;
        }
        LibraryMove m;
        std::thread t([&] { move_library(lib.home(), args[0], m); });
        while (!m.finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (m.total.load() > 0) fprintf(stderr, "progress %.3f moving the library\n", (double)m.done.load() / (double)m.total.load());
        }
        t.join();
        printf("{\"ok\": %s, \"from\": %s, \"to\": %s, \"backup\": %s, \"headline\": %s, \"explain\": %s}\n",
               m.ok ? "true" : "false", json_quote(m.from).c_str(), json_quote(m.to).c_str(), json_quote(m.backup).c_str(),
               json_quote(m.headline).c_str(), json_quote(m.explain).c_str());
        return m.ok ? 0 : 1;
    }
    if ((action == "--verify" || action == "--add") && args.size() == 1) {   // the copy says which game it is
        const std::string &first = lib.games().front().id;
        VerifyResult q = verify_copy(first, args[0], false);
        std::string id = q.kind == VerifyResult::Ok ? first : q.kind == VerifyResult::OtherGame ? q.other_game : "";
        if (id.empty()) {
            printf("%s\n", verify_json(first, q).c_str());
            return 1;
        }
        args.insert(args.begin(), id);
    }
    Game *g = args.empty() ? nullptr : lib.find(args[0]);
    if (action != "--status" && !g) {
        if (action == "--play")
            tell_player("Country-Box", "Unknown game '" + (args.empty() ? std::string() : args[0]) +
                                           "'. Use --play returns or --play freeze.");
        else fprintf(stderr, "launcher: unknown game '%s'\n", args.empty() ? "" : args[0].c_str());
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
        if (!lib.play(*g, &err)) {
            std::string name = g->manifest ? g->manifest->name : g->id;
            return tell_player(name, name + " can't start yet: " + err + "\nOpen Country-Box to finish setting it up."), 1;
        }
        fprintf(stderr, "launcher: %s started (pid %d), log %s\n", g->id.c_str(), (int)g->pid, g->log.c_str());
        int st = 0;
        while (proc_poll(g->pid, &st) == 1) SDL_Delay(200);
        return st;
    }
    return usage(), 2;
}
