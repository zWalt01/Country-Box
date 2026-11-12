// Country-Box launcher: the library (library.h).
#include "library.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "proc.h"
#include "sha256.h"

namespace fs = std::filesystem;

static bool fail(std::string *err, const std::string &m) {
    if (err) *err = m;
    return false;
}

#ifndef CB_VERSION
#define CB_VERSION "0.0.0"
#endif

std::string launcher_version() { return CB_VERSION; }

int version_compare(const std::string &a, const std::string &b) {
    const char *p = a.c_str(), *q = b.c_str();
    while (*p || *q) {
        long x = strtol(p, (char **)&p, 10), y = strtol(q, (char **)&q, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*p == '.') p++;
        if (*q == '.') q++;
        if ((*p && !isdigit((unsigned char)*p)) || (*q && !isdigit((unsigned char)*q))) break;
    }
    return 0;
}

const char *game_state_name(GameState s) {
    switch (s) {
    case GameState::NotInstalled: return "not_installed";
    case GameState::CopyMissing: return "copy_missing";
    case GameState::NeedsCheck: return "needs_check";
    case GameState::NeedsBuild: return "needs_build";
    case GameState::NeedsShaders: return "needs_shaders";
    case GameState::Ready: return "ready";
    case GameState::UpdateAvailable: return "update_available";
    }
    return "?";
}

const char *game_state_label(GameState s) {
    switch (s) {
    case GameState::NotInstalled: return "Not installed";
    case GameState::CopyMissing: return "Copy not found";
    case GameState::NeedsCheck: return "Needs a new check";
    case GameState::NeedsBuild:
    case GameState::NeedsShaders:
    case GameState::UpdateAvailable: return "Needs preparing";   // Prepare (milestone 2) covers all three
    case GameState::Ready: return "Ready";
    }
    return "?";
}

std::string launcher_home(const std::string &override_dir) {
    if (!override_dir.empty()) return override_dir;
    const char *e = getenv("CB_LAUNCHER_HOME");
    if (e && *e) return e;
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x) return std::string(x) + "/country-box";
    return std::string(h ? h : ".") + "/.local/share/country-box";
}

Library::Library(std::string home) : home_(std::move(home)) {
    for (auto &id : manifest_games()) {
        Game g;
        g.id = id;
        g.manifest = manifest_get(id);
        games_.push_back(g);
    }
}

Game *Library::find(const std::string &id) {
    for (auto &g : games_)
        if (g.id == id) return &g;
    return nullptr;
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

void Library::load() {
    std::ifstream f(home_ + "/library.ini");
    Game *g = nullptr;
    for (std::string line; std::getline(f, line);) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            g = find(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (!g || eq == std::string::npos) continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "copy") g->copy = v;
        else if (k == "copy_checked") g->copy_checked = v;
        else if (k == "copy_manifest") g->copy_manifest = v;
        else if (k == "build") g->build_dir = v, g->build_set = true;
    }
    refresh();
}

bool Library::save(std::string *err) const {
    std::error_code ec;
    fs::create_directories(home_, ec);
    std::string tmp = home_ + "/library.ini.tmp";
    {
        std::ofstream f(tmp);
        f << "# Country-Box launcher: your games, written by the launcher (country-box-launcher --add / --set-build).\n"
             "# copy = the checked folder with exefs/ and romfs/; build = the folder of the game binary (country-box).\n";
        for (auto &g : games_) {
            if (g.copy.empty() && !g.build_set) continue;
            f << "\n[" << g.id << "]\n";
            if (!g.copy.empty())
                f << "copy = " << g.copy << "\ncopy_checked = " << g.copy_checked << "\ncopy_manifest = " << g.copy_manifest << "\n";
            if (g.build_set) f << "build = " << g.build_dir << "\n";
        }
        if (!f) {
            if (err) *err = "cannot write " + tmp;
            return false;
        }
    }
    if (rename(tmp.c_str(), (home_ + "/library.ini").c_str())) {
        if (err) *err = "cannot replace library.ini: " + std::string(strerror(errno));
        return false;
    }
    return true;
}

static bool exists_dir(const std::string &p) {
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

static std::string read_text(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// The line "<name> <value>" of the shader marker (shader_prep.c marker_text).
static std::string marker_line(const std::string &marker, const char *name) {
    std::istringstream s(marker);
    for (std::string line; std::getline(s, line);)
        if (line.rfind(std::string(name) + " ", 0) == 0) return line.substr(strlen(name) + 1);
    return "";
}

void Library::refresh(Game &g) {
    if (!g.build_set) g.build_dir = home_ + "/games/" + g.id;
    g.exe.clear();
    g.build_version.clear();
    g.build_game.clear();
    g.detail.clear();
    std::string exe = g.build_dir + "/country-box";
    if (access(exe.c_str(), X_OK) == 0) {
        g.exe = exe;
        Json d;
        if (settings_dump(g, d, nullptr)) g.build_version = d["version"].s(), g.build_game = d["game"].s();
    }
    if (g.copy.empty()) {
        g.state = GameState::NotInstalled;
        g.detail = "Add your own extracted copy of the game to start.";
        return;
    }
    if (!exists_dir(g.copy + "/exefs") || !exists_dir(g.copy + "/romfs")) {
        g.state = GameState::CopyMissing;
        g.detail = "Your copy isn't at " + g.copy + " anymore. Connect its drive or add it again.";
        return;
    }
    if (!g.manifest || g.copy_manifest != g.manifest->id) {
        g.state = GameState::NeedsCheck;
        g.detail = "Country-Box changed what it checks. Check your copy again (it takes a minute).";
        return;
    }
    if (g.exe.empty()) {
        g.state = GameState::NeedsBuild;
        g.detail = "Your copy is checked. The game still has to be built for this computer.";
        return;
    }
    if (!g.build_version.empty() && version_compare(g.build_version, launcher_version()) < 0) {
        g.state = GameState::UpdateAvailable;
        g.detail = "Country-Box " + launcher_version() + " is here. Prepare again — your saves and settings stay.";
        return;
    }
    // Shaders: the game decides at start (its marker also names the GPU driver, which only it can read); here only
    // the parts on disk: no marker, or a marker for other shader lists.
    std::string list = g.build_dir + "/shader-list.bin", marker = read_text(g.build_dir + "/shader-prepared.id");
    if (access(list.c_str(), R_OK) == 0) {
        std::string why;
        if (marker.empty()) why = "first start";
        else {
            char hex[65];
            if (!sha256_file(list.c_str(), hex, nullptr, nullptr) && marker_line(marker, "shader-list") != hex)
                why = "the shader lists changed";
        }
        if (!why.empty()) {
            g.state = GameState::NeedsShaders;
            g.detail = "Shaders get prepared when you press PLAY (" + why + "; about a minute, only once).";
            return;
        }
    }
    g.state = GameState::Ready;
    g.detail = "Version " + (g.manifest ? g.manifest->version : std::string("?")) + " checked" +
               (g.copy_checked.empty() ? "" : " on " + g.copy_checked.substr(0, 10)) + ".";
}

void Library::refresh() {
    for (auto &g : games_) refresh(g);
}

void Library::set_copy(Game &g, const VerifyResult &r) {
    g.copy = r.root;
    g.copy_manifest = g.manifest ? g.manifest->id : "";
    char t[32];
    time_t now = time(nullptr);
    strftime(t, sizeof t, "%Y-%m-%dT%H:%M", localtime(&now));
    g.copy_checked = t;
    refresh(g);
}

void Library::forget_copy(Game &g) {
    g.copy.clear(), g.copy_checked.clear(), g.copy_manifest.clear();
    refresh(g);
}

void Library::set_build(Game &g, const std::string &dir) {
    std::error_code ec;
    g.build_dir = fs::absolute(dir, ec).lexically_normal().string();
    if (!g.build_dir.empty() && g.build_dir.back() == '/') g.build_dir.pop_back();
    g.build_set = true;
    refresh(g);
}

bool Library::play(Game &g, std::string *err) {
    refresh(g);
    if (g.pid > 0) return fail(err, "The game is already running.");
    if (g.exe.empty() || g.copy.empty() || g.state == GameState::CopyMissing || g.state == GameState::NeedsCheck)
        return fail(err, g.detail);
    std::error_code ec;
    fs::create_directories(home_ + "/logs", ec);
    char t[32];
    time_t now = time(nullptr);
    strftime(t, sizeof t, "%Y%m%d-%H%M%S", localtime(&now));
    g.log = home_ + "/logs/" + g.id + "-" + t + ".log";
    g.pid = proc_spawn({g.exe, "--exefs", g.copy + "/exefs", "--romfs", g.copy + "/romfs", "--profile",
                        g.build_dir + "/profile"},
                       g.build_dir, g.log);
    if (g.pid < 0) return fail(err, "The game could not be started.");
    return true;
}

bool Library::poll_running() {
    bool ended = false;
    for (auto &g : games_) {
        if (g.pid <= 0) continue;
        int st = 0;
        if (proc_poll(g.pid, &st) != 1) {
            g.pid = -1, g.last_exit = st, ended = true;
            refresh(g);
        }
    }
    return ended;
}

bool Library::any_running() const {
    for (auto &g : games_)
        if (g.pid > 0) return true;
    return false;
}

bool Library::settings_dump(const Game &g, Json &out, std::string *err) const {
    if (g.exe.empty()) return fail(err, "The game isn't built yet.");
    std::string text;
    int rc = proc_run({g.exe, "--settings-dump"}, g.build_dir, {}, text);
    std::string perr;
    if (rc != 0 || !json_parse(text, out, &perr) || out["groups"].type != Json::Array) {
        if (err) *err = rc != 0 ? "The game's settings could not be read (exit " + std::to_string(rc) + ")."
                                : "The game answered something unexpected (" + perr + ").";
        return false;
    }
    return true;
}

bool Library::settings_set(const Game &g, const std::vector<std::string> &pairs, Json &out, std::string *err) const {
    if (g.exe.empty()) return fail(err, "The game isn't built yet.");
    std::vector<std::string> argv = {g.exe, "--settings-set"};
    argv.insert(argv.end(), pairs.begin(), pairs.end());
    std::string text;
    int rc = proc_run(argv, g.build_dir, {}, text);
    if ((rc != 0 && rc != 2) || !json_parse(text, out)) {
        if (err) *err = "The setting could not be saved (exit " + std::to_string(rc) + ").";
        return false;
    }
    return true;
}

std::string Library::status_json() {
    std::string o = "{\"version\": " + json_quote(launcher_version()) + ", \"home\": " + json_quote(home_) + ", \"games\": [";
    bool first = true;
    for (auto &g : games_) {
        o += std::string(first ? "\n" : ",\n") + "  {\"id\": " + json_quote(g.id) +
             ", \"name\": " + json_quote(g.manifest ? g.manifest->name : g.id) + ", \"state\": " + json_quote(game_state_name(g.state)) +
             ", \"detail\": " + json_quote(g.detail) + ", \"copy\": " + json_quote(g.copy) + ", \"build\": " + json_quote(g.build_dir) +
             ", \"build_version\": " + json_quote(g.build_version) + ", \"build_game\": " + json_quote(g.build_game) + "}";
        first = false;
    }
    return o + "\n]}";
}
