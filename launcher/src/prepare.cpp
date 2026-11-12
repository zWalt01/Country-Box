// Country-Box launcher: Prepare (prepare.h). The pipeline runs on the player's machine: their copy goes in, a
// ready build comes out. Every child's output lands in one log the errors can point at, progress is parsed from
// what the children already print (the lifter's "emitted N/M chunks", ninja's "[n/m]", the shader preparation's
// "done / total ... about N s left"), and cancelling kills the child's process group — the next Prepare resumes
// through ninja's incremental build and the lift's chunk-preserving rewrite.
#include "prepare.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "json.h"
#include "library.h"
#include "proc.h"
#include "sha256.h"

namespace fs = std::filesystem;

// ---- small helpers

static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string read_text(const std::string &p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static bool write_text(const std::string &p, const std::string &text) {
    std::string tmp = p + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) return false;
    }
    return !rename(tmp.c_str(), p.c_str());
}

static std::string which(const std::string &name) {
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0 ? name : "";
    const char *path = getenv("PATH");
    std::stringstream ps(path ? path : "");
    for (std::string d; std::getline(ps, d, ':');) {
        if (d.empty()) continue;
        std::string p = d + "/" + name;
        if (access(p.c_str(), X_OK) == 0) return p;
    }
    return "";
}

static double now_s() {
    return (double)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
               .count() / 1000.0;
}

static std::string local_iso() {
    char t[32];
    time_t now = time(nullptr);
    strftime(t, sizeof t, "%Y-%m-%dT%H:%M", localtime(&now));
    return t;
}

static uint64_t dir_bytes(const std::string &path) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) n += (uint64_t)it->file_size(ec);
    }
    return n;
}

// Free space of the filesystem that holds path (tests can pretend with CB_TEST_DISK_FREE_GB).
static uint64_t free_bytes(const std::string &path) {
    if (const char *t = getenv("CB_TEST_DISK_FREE_GB"); t && *t) return (uint64_t)(atof(t) * (1 << 30));
    struct statvfs st;
    std::string p = fs::weakly_canonical(path).string();
    for (;;) {
        if (statvfs(p.c_str(), &st) == 0) return (uint64_t)st.f_bavail * st.f_frsize;
        std::error_code ec;
        if (!fs::exists(p, ec)) {
            std::string parent = fs::path(p).parent_path().string();
            if (parent == p || parent.empty()) return 0;
            p = parent;
            continue;
        }
        return 0;
    }
}

static std::string human_gb(uint64_t bytes) {
    char b[32];
    snprintf(b, sizeof b, "%.1f GB", bytes / (double)(1 << 30));
    return b;
}

// Counts one module's chunk files (<mod>/<mod>_NNNN.c; never <mod>_table.c).
static int chunk_count(const std::string &moddir, const std::string &mod) {
    std::error_code ec;
    if (!fs::is_directory(moddir, ec)) return 0;
    int n = 0;
    std::string prefix = mod + "_", suffix = ".c";
    for (auto &e : fs::directory_iterator(moddir, ec)) {
        std::string name = e.path().filename().string();
        if (name.size() <= prefix.size() + suffix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
        size_t d = name.size() - suffix.size();
        bool digits = name.compare(d, suffix.size(), suffix) == 0;
        for (size_t i = prefix.size(); digits && i < d; i++) digits = isdigit((unsigned char)name[i]) != 0;
        n += digits;
    }
    return n;
}

// One hash over every file of the exefs (sorted): what "the copy's code changed" means to the lift.
static std::string exefs_hash_line(const std::string &exefs) {
    std::vector<std::pair<std::string, std::string>> files;
    std::error_code ec;
    for (auto &e : fs::directory_iterator(exefs, ec)) {
        if (!e.is_regular_file(ec)) continue;
        char hex[65];
        if (!sha256_file(e.path().c_str(), hex, nullptr, nullptr)) files.push_back({e.path().filename().string(), hex});
    }
    std::sort(files.begin(), files.end());
    std::string joined;
    for (auto &f : files) joined += f.first + " " + f.second + "\n";
    Sha256 *s = sha256_new();
    if (!s) return "";
    sha256_update(s, joined.data(), joined.size());
    uint8_t d[32];
    sha256_final(s, d);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
    return hex;
}

std::string prep_payload_dir(const std::string &exe) {
    auto valid = [](const std::string &p) {
        std::error_code ec;
        return !p.empty() && fs::is_regular_file(p + "/core/recomp/gen.py", ec) &&
               fs::is_regular_file(p + "/core/tools/build_linux.py", ec);
    };
    if (const char *e = getenv("CB_PREP_SOURCES"); e && *e)
        return valid(e) ? e : "";   // an override that is not a payload is reported as no payload
    std::error_code ec;
    std::string dir = fs::path(exe).parent_path().string();
    for (const std::string &cand : {dir + "/prep", dir + "/../share/country-box/prep", dir + "/../.."})
        if (std::string p = fs::weakly_canonical(cand, ec).string(); valid(p)) return p;
    return "";
}

// ---- the measured cost of each game (docs/LAUNCHER.md keeps the raw numbers and their date)

// *Measured* 2026-11-12 on the reference PC (16 threads, nice -n 19): weights are the -j4 wall seconds of lift
// and compile+link; disk is gen + objects + binary counted after each run. Deck numbers stay *supuesto*.
static const PrepProfile kProfiles[] = {
    {"dkcrhd", 1.6, 2.6, 106, 445},   // measured 2026-11-12, Ryzen 7 5700X3D, 16 threads
    {"dkctf", 0.5, 1.1, 47, 232},
};

const PrepProfile *prep_profile(const std::string &game) {
    for (auto &p : kProfiles)
        if (game == p.game) return &p;
    return nullptr;
}

// ---- the job

PrepareJob::PrepareJob(std::string home, const Game &g, Mode mode)
    : home_(std::move(home)), game_(g.id), copy_(g.copy),
      manifest_id_(g.manifest ? g.manifest->id : ""), version_key_(g.manifest ? g.manifest->version_key : ""),
      mode_(mode) {
    build_dir_ = home_ + "/games/" + game_;
    gen_dir_ = home_ + "/gen/" + game_;
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    payload_ = prep_payload_dir(n > 0 ? std::string(exe, (size_t)n) : "");
    thread_ = std::thread([this] { run(); });
}

PrepareJob::~PrepareJob() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

std::string PrepareJob::line() {
    std::lock_guard<std::mutex> lock(line_mu_);
    return line_;
}

// Runs one child, tees everything it prints into the log, parses progress lines. False when it failed or was
// cancelled (outcome_.kind says which; "cancelled" only when we asked for it).
bool PrepareJob::child(const std::vector<std::string> &argv, const std::string &cwd, const char *parse_what) {
    if (argv.empty()) return false;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC)) return false;
    std::vector<char *> args;
    for (auto &s : argv) args.push_back(const_cast<char *>(s.c_str()));
    args.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]), close(fds[1]);
        return false;
    }
    if (pid == 0) {
        setsid();
        if (!getenv("CB_PREP_NICE")) (void)setpriority(PRIO_PROCESS, 0, 10);   // a build never fights the game for CPU
        dup2(fds[1], 1), dup2(fds[1], 2);
        close(fds[0]), close(fds[1]);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) dup2(null, 0);
        if (!cwd.empty() && chdir(cwd.c_str())) _exit(127);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(fds[1]);
    std::ofstream log(log_, std::ios::app);
    bool cancelled = false;
    double kill_at = 0;
    char buf[8192];
    std::string pend;
    auto handle = [&](const std::string &l) {
        std::string line = trim(l);
        if (line.empty()) return;
        log << line << '\n' << std::flush;
        set_line_text(line);
        int d = -1, t = -1;
        if (!strcmp(parse_what, "lift") && sscanf(line.c_str(), "emitted %d/%d chunks", &d, &t) != 2) d = -1;
        else if (!strcmp(parse_what, "compile") && sscanf(line.c_str(), "[%d/%d]", &d, &t) != 2) d = -1;
        else if (!strcmp(parse_what, "shaders")) {
            d = t = -1;
            double elapsed = -1;
            if (sscanf(line.c_str(), "prepare-shaders: %*s %d / %d  elapsed %lf", &d, &t, &elapsed) >= 3) {
                stage_done.store(d), stage_total.store(t);
                if (const char *about = strstr(line.c_str(), "about ")) {
                    double left = -1;
                    if (sscanf(about, "about %lf s left", &left) == 1 && left >= 0) eta_s.store(left);
                } else if (elapsed >= 0 && d > 0 && t > 0) {
                    eta_s.store(elapsed * (t - d) / d);
                }
                return;
            }
            d = t = -1;
        }
        if (d >= 0 && t > 0) {
            stage_done.store(d), stage_total.store(t);
            double in_stage = now_s() - stage_elapsed_base_;
            if (d > 0 && in_stage > 1) eta_s.store(in_stage * (t - d) / d);
        }
    };
    auto ended = [&]() {
        int st = 0;
        return waitpid(pid, &st, WNOHANG) == pid;
    };
    for (;;) {
        if (__atomic_load_n(&cancel_flag_, __ATOMIC_RELAXED) && !cancelled) {
            cancelled = true;
            kill(-pid, SIGTERM);
            kill_at = now_s() + 5;
        }
        if (cancelled && kill_at > 0 && now_s() > kill_at) kill(-pid, SIGKILL), kill_at = 0;
        struct pollfd p = {fds[0], POLLIN, 0};
        int r = poll(&p, 1, 200);
        if (r < 0 && errno == EINTR) continue;
        if (r > 0) {
            ssize_t n = read(fds[0], buf, sizeof buf);
            if (n <= 0) break;
            pend.append(buf, (size_t)n);
            size_t a = 0;
            for (size_t nl; (nl = pend.find('\n', a)) != std::string::npos; a = nl + 1) handle(pend.substr(a, nl - a));
            pend.erase(0, a);
            continue;
        }
        if (ended()) break;   // silent child: check it is still alive after each empty poll
    }
    for (;;) {   // drain what is left after the child (and its children) closed the pipe
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        pend.append(buf, (size_t)n);
        size_t a = 0;
        for (size_t nl; (nl = pend.find('\n', a)) != std::string::npos; a = nl + 1) handle(pend.substr(a, nl - a));
        pend.erase(0, a);
    }
    close(fds[0]);
    handle(pend);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (cancelled) {
        outcome_.kind = "cancelled";
        return false;
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

// The launcher's own Python environment: $XDG_DATA_HOME/country-box/python (default ~/.local/share/...), if made.
static std::string own_python() {
    std::string base = getenv("XDG_DATA_HOME") && *getenv("XDG_DATA_HOME") ? getenv("XDG_DATA_HOME")
                       : getenv("HOME") ? std::string(getenv("HOME")) + "/.local/share" : std::string();
    std::string p = base + "/country-box/python/bin/python";
    std::error_code ec;
    return !base.empty() && fs::exists(p, ec) ? p : std::string();
}

// A plain-words list of everything missing, or "" when the machine can build. CB_TEST_TOOLS_OK skips the check
// (the fast tests exercise the stages after it without a toolchain).
static std::string missing_tools(std::string *python) {
    std::string missing;
    if (getenv("CB_TEST_TOOLS_OK")) {
        *python = getenv("CB_PYTHON") && *getenv("CB_PYTHON") ? getenv("CB_PYTHON") : "python3";
        return "";
    }
    for (const char *n : {"clang", "clang++", "ld.lld", "ninja", "pkg-config"})
        if (which(n).empty()) missing += (missing.empty() ? std::string() : std::string(", ")) + n;
    // CB_PYTHON, else the launcher's own environment (the one the hint below creates), else the system python3:
    // a launcher opened from the desktop never sees an exported variable.
    std::string own = own_python();
    *python = which(getenv("CB_PYTHON") && *getenv("CB_PYTHON") ? getenv("CB_PYTHON")
                    : !own.empty() ? own.c_str() : "python3");
    if (python->empty()) missing += (missing.empty() ? std::string() : std::string(", ")) + "python3";
    else {
        std::string out;
        int rc = proc_run({*python, "-c", "import capstone, unicorn, lz4, numpy"}, "", {}, out, 30000);
        if (rc != 0) missing += (missing.empty() ? std::string() : std::string(", ")) + "python3 with capstone, unicorn, lz4 and numpy";
    }
    if (!which("pkg-config").empty()) {   // proc_run execv's: absolute paths only
        std::string out;
        if (proc_run({which("pkg-config"), "--exists", "sdl3", "vulkan", "opus", "vpx"}, "", {}, out, 15000) != 0)
            missing += (missing.empty() ? std::string() : std::string(", ")) + "the SDL 3, Vulkan, Opus and vpx development packages";
    }
    return missing;
}

// What to run for what is missing, in this distribution's words (from /etc/os-release). SteamOS has no compiler.
static std::string missing_tools_hint(const std::string &missing) {
    std::string os = read_text("/etc/os-release"), id;
    for (const char *key : {"\nID=", "\nID_LIKE="}) {
        size_t at = ("\n" + os).find(key);
        if (at != std::string::npos) id += " " + ("\n" + os).substr(at + strlen(key), ("\n" + os).find('\n', at + 1) - at - strlen(key));
    }
    auto has = [&](const char *d) { return id.find(d) != std::string::npos; };
    std::string tail = "\nThen press Prepare again. Already built this game on another computer? \u201cImport a build\u201d "
                       "needs no compiler here.";
    if (has("steamos"))
        return "SteamOS has no compiler. Prepare the game on a Linux PC with the same Country-Box version, copy its "
               "build folder here, and use \u201cImport a build\u201d on the game's card.";
    bool py_only = missing.rfind("python3 with", 0) == 0;
    if (py_only)
        return "Python could not set itself up (no network, or python3 without its venv module). In a terminal:\n"
               "python3 -m venv ~/.local/share/country-box/python\n"
               "~/.local/share/country-box/python/bin/pip install capstone==5.0.9 lz4==4.4.5 numpy==2.5.4 unicorn==2.1.4" + tail;
    if (has("arch"))
        return "In a terminal:\nsudo pacman -S --needed clang lld ninja pkgconf python sdl3 vulkan-headers vulkan-icd-loader "
               "opus libvpx zlib" + tail;
    if (has("fedora") || has("rhel"))
        return "In a terminal:\nsudo dnf install clang lld ninja-build pkgconf-pkg-config python3 SDL3-devel "
               "vulkan-loader-devel opus-devel libvpx-devel zlib-devel" + tail;
    if (has("debian") || has("ubuntu"))
        return "In a terminal:\nsudo apt install clang lld ninja-build pkg-config python3-venv libsdl3-dev libvulkan-dev "
               "libopus-dev libvpx-dev zlib1g-dev" + tail;
    return "Install " + missing + " with your distribution's package manager (development packages for SDL3, "
           "Vulkan, Opus, libvpx and zlib too)." + tail;
}

// The build kit at a fixed place: <home>/prep. An AppImage mounts at a new /tmp/.mount_* path every run, and that
// path ends up in every compile command, so building from it made ninja rebuild the whole game after each start or
// update. The sync writes only files whose bytes changed (an unchanged one keeps its mtime, so ninja skips what
// depends on it) and removes files the new kit no longer has. Returns the path to build from (src if the sync
// failed: the build still works, it only loses the incremental rebuild).
static std::string sync_payload(const std::string &src, const std::string &dst) {
    std::error_code ec;
    if (src.empty() || fs::weakly_canonical(src, ec) == fs::weakly_canonical(dst, ec)) return src;
    std::set<std::string> keep;
    for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::string rel = fs::relative(it->path(), src, ec).string();
        if (ec) return src;
        keep.insert(rel);
        fs::path to = fs::path(dst) / rel;
        if (it->is_directory(ec)) { fs::create_directories(to, ec); continue; }
        if (!it->is_regular_file(ec)) continue;
        std::string a = read_text(it->path().string());
        if (fs::is_regular_file(to, ec) && fs::file_size(to, ec) == a.size() && read_text(to.string()) == a) continue;
        fs::create_directories(to.parent_path(), ec);
        std::string tmp = to.string() + ".new";
        if (!write_text(tmp, a)) return src;
        fs::permissions(tmp, fs::status(it->path(), ec).permissions(), ec);
        fs::rename(tmp, to, ec);
        if (ec) return src;
    }
    if (ec) return src;
    std::vector<fs::path> gone;
    for (auto it = fs::recursive_directory_iterator(dst, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (!keep.count(fs::relative(it->path(), dst, ec).string())) gone.push_back(it->path());
    for (auto &g : gone) fs::remove_all(g, ec);
    return dst;
}

bool PrepareJob::preflight(std::string *python) {
    stage.store(Preflight), stage_done.store(0), stage_total.store(1), eta_s.store(-1);
    stage_elapsed_base_ = now_s();
    *python = "";
    if (mode_ == ShadersOnly) {   // no tools needed: the game's own binary does the work
        std::error_code ec;
        if (!fs::is_regular_file(build_dir_ + "/country-box", ec)) {
            outcome_.kind = "shaders";
            outcome_.headline = "The game is not built here yet.";
            outcome_.hint = "Prepare it first.";
            return false;
        }
        stage_done.store(1), stage_total.store(1);
        return true;
    }
    set_line_text("checking the tools and the disk");
    if (payload_.empty()) {
        outcome_.kind = "payload";
        outcome_.headline = "This launcher has no build kit with it.";
        outcome_.explain = "Prepare needs the prep/ folder that ships with Country-Box (the lifter and the runtime's "
                           "sources); it was not found next to the launcher.";
        outcome_.hint = "Reinstall Country-Box, or point CB_PREP_SOURCES at its prep/ folder.";
        return false;
    }
    set_line_text("checking the build kit");
    payload_ = sync_payload(payload_, home_ + "/prep");
    std::string missing = missing_tools(python);
    // Only the Python modules missing, and a system python3 there: make the launcher's own environment and install
    // the pinned modules into it, once (needs the network). The player never sees this as an error.
    if (missing == "python3 with capstone, unicorn, lz4 and numpy" && !getenv("CB_PYTHON") && own_python().empty()) {
        std::string sys_py = which("python3");
        std::string base = getenv("XDG_DATA_HOME") && *getenv("XDG_DATA_HOME") ? getenv("XDG_DATA_HOME")
                           : std::string(getenv("HOME") ? getenv("HOME") : "") + "/.local/share";
        std::string env = base + "/country-box/python";
        set_line_text("setting up Python for the build (first time only; downloads about 40 MB)");
        bool made = !sys_py.empty() && child({sys_py, "-m", "venv", env}, payload_, "python") &&
                    child({env + "/bin/pip", "install", "--disable-pip-version-check", "-r", payload_ + "/requirements.txt"},
                          payload_, "python");
        if (!made && outcome_.kind == "cancelled") return false;
        if (!made) {
            std::error_code ec;
            fs::remove_all(env, ec);   // a half-made environment would be found next time and fail the same way
        }
        outcome_.kind.clear();
        missing = missing_tools(python);
    }
    if (!missing.empty()) {
        outcome_.kind = "missing_tools";
        outcome_.headline = "This computer is missing the tools that build the game.";
        outcome_.explain = "Missing: " + missing + ".";
        outcome_.hint = missing_tools_hint(missing);
        return false;
    }
    std::string vout;
    proc_run({which("clang"), "--version"}, "", {}, vout, 5000);
    toolchain_ = vout.substr(0, vout.find('\n'));
    if (const PrepProfile *p = prep_profile(game_)) {
        uint64_t need = (uint64_t)((p->gen_gb + p->build_gb) * (1 << 30));
        uint64_t have = dir_bytes(gen_dir_) + dir_bytes(build_dir_);   // an interrupted Prepare already paid for some
        uint64_t want = need > have ? need - have : 0;
        want += (uint64_t)(1.2 * (1 << 30));   // link-time temp files, the log, the index
        uint64_t got = free_bytes(home_);
        if (got < want) {
            outcome_.kind = "disk";
            outcome_.headline = "Not enough free space where the game gets built.";
            outcome_.explain = "Preparing this game needs about " + human_gb(want) + " free; there are " + human_gb(got) +
                               " free for " + home_ + ".";
            outcome_.hint = "Free space there (or give Country-Box a home on a bigger drive) — or use \u201cImport a "
                            "build\u201d from another computer, which needs no compiler and less space.";
            return false;
        }
    }
    stage_done.store(1), stage_total.store(1);
    return true;
}

// The lift runs again only when the copy's exefs or the lifter itself changed, or when the last one did not
// finish; a chunk whose text did not change keeps its mtime, so the compiler keeps its objects as well.
bool PrepareJob::lift_up_to_date(std::string *why) {
    Json stamp;
    if (!json_parse(read_text(gen_dir_ + "/gen-stamp.json"), stamp) || stamp.type != Json::Object) {
        *why = "no finished lift on record";
        return false;
    }
    if (stamp["lifter"].s() != trim(read_text(payload_ + "/lifter.id"))) {
        *why = "Country-Box's lifter changed";
        return false;
    }
    if (stamp["exefs"].s() != exefs_hash_line(copy_ + "/exefs")) {
        *why = "your copy's code differs from the copy this was lifted from";
        return false;
    }
    const Json &chunks = stamp["chunks"];
    for (size_t i = 0; i < chunks.keys.size(); i++) {
        const std::string &mod = chunks.keys[i];
        if ((int)chunks.arr[i].n(-1) != chunk_count(gen_dir_ + "/" + mod, mod)) {
            *why = "the previous lift did not finish";
            return false;
        }
    }
    *why = "the lifted code is up to date";
    return true;
}

static bool layout_module(const Json &entry, std::string *mod) {
    if (entry.type == Json::Object) *mod = entry["name"].s();
    else if (entry.type == Json::Array && entry.size() > 0) *mod = entry[(size_t)0].s();
    else mod->clear();
    return !mod->empty();
}

bool PrepareJob::write_lift_stamp() {
    Json layout;
    if (!json_parse(read_text(gen_dir_ + "/layout.json"), layout) || layout.type != Json::Array) return false;
    std::string mods;
    std::string mod;
    for (size_t i = 0; i < layout.size(); i++)
        if (layout_module(layout[i], &mod))
            mods += (mods.empty() ? "" : ",") + json_quote(mod) + ":" + std::to_string(chunk_count(gen_dir_ + "/" + mod, mod));
    std::string text = std::string("{\n \"lifter\": ") + json_quote(trim(read_text(payload_ + "/lifter.id"))) +
                       ",\n \"exefs\": " + json_quote(exefs_hash_line(copy_ + "/exefs")) + ",\n \"chunks\": {" + mods +
                       "},\n \"date\": " + json_quote(local_iso()) + "\n}\n";
    return write_text(gen_dir_ + "/gen-stamp.json", text);
}

// Parallel jobs for the lift and the compile: every online CPU (CB_PREP_JOBS overrides). The compile is also bounded
// by the memory available now, at ~1.5 GB per compiler (*supuesto*: clang on the largest generated chunks), so a
// Steam Deck (16 GB shared with the GPU) does not run out of memory.
static int prep_jobs(bool compile) {
    if (const char *e = getenv("CB_PREP_JOBS"); e && atoi(e) > 0) return atoi(e);
    long hw = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
    long jobs = hw;
    if (compile) {
        long avail_kb = 0;
        if (FILE *f = fopen("/proc/meminfo", "r")) {
            char line[256];
            while (fgets(line, sizeof line, f))
                if (sscanf(line, "MemAvailable: %ld kB", &avail_kb) == 1) break;
            fclose(f);
        }
        if (avail_kb > 0) jobs = std::min(jobs, std::max(1L, avail_kb / (1536L * 1024)));
    }
    return (int)std::max(1L, jobs);
}

bool PrepareJob::do_lift(const std::string &python) {
    stage.store(Lift), stage_done.store(0), stage_total.store(-1), eta_s.store(-1);
    stage_elapsed_base_ = now_s();
    std::string why;
    if (lift_up_to_date(&why)) {
        set_line_text("lift skipped: " + why);
        return true;
    }
    set_line_text("lifting the code from your copy (" + why + ")");
    std::error_code ec;
    fs::create_directories(gen_dir_, ec);
    int jobs = prep_jobs(false);
    if (!child({python, payload_ + "/core/recomp/gen.py", "--exefs", copy_ + "/exefs", "--out", gen_dir_,
                "--jobs", std::to_string(jobs)},
               payload_ + "/core/recomp", "lift")) {
        if (outcome_.kind != "cancelled") {
            outcome_.kind = "lift";
            outcome_.headline = "Turning your copy's code into C did not finish.";
            outcome_.explain = "The lifter stopped before all of the game's code was written out.";
            outcome_.hint = "Prepare again — it picks up where it stopped. If it stops in the same place, report the "
                            "problem with the log below.";
        }
        return false;
    }
    if (!write_lift_stamp()) {
        outcome_.kind = "lift";
        outcome_.headline = "The lift's bookmark could not be written.";
        outcome_.explain = "The C was generated in " + gen_dir_ + ", but its stamp (what lets the next Prepare skip "
                           "the lift) could not be written.";
        outcome_.hint = "Check that the drive is writable, then Prepare again: the lift itself will not repeat.";
        return false;
    }
    return true;
}

bool PrepareJob::do_compile(const std::string &python) {
    stage.store(Compile), stage_done.store(0), stage_total.store(-1), eta_s.store(-1);
    stage_elapsed_base_ = now_s();
    set_line_text("compiling");
    int jobs = prep_jobs(true);
    std::vector<std::string> argv = {python,     payload_ + "/core/tools/build_linux.py", "--gen",  gen_dir_,
                                     "--exefs",  copy_ + "/exefs",                       "--romfs", copy_ + "/romfs",
                                     "--out",    build_dir_,                              "--jobs", std::to_string(jobs),
                                     "--game",   game_};
    if (!version_key_.empty()) argv.push_back("--version"), argv.push_back(version_key_);
    if (!child(argv, payload_ + "/core/tools", "compile")) {
        if (outcome_.kind != "cancelled") {
            outcome_.kind = "compile";
            outcome_.headline = "The compiler stopped with an error.";
            outcome_.explain = "Compiling or linking the game failed; everything it printed is in the log.";
            outcome_.hint = "Prepare again — finished parts are kept. If it fails in the same place, report the "
                            "problem and attach the log below.";
        }
        return false;
    }
    std::error_code ec;
    if (!fs::is_regular_file(build_dir_ + "/country-box", ec)) {
        outcome_.kind = "compile";
        outcome_.headline = "The build ended without a game binary.";
        outcome_.explain = "The build tool reported success but " + build_dir_ + "/country-box is not there.";
        outcome_.hint = "Report the problem with the log below.";
        return false;
    }
    return true;
}

bool PrepareJob::place_runtime_files() {
    // build_linux.py already copies the translator and the game's shader lists next to the binary; this makes it
    // true whatever happened around it (and after an import that came without them).
    std::error_code ec;
    fs::create_directories(build_dir_, ec);
    std::string xlate = payload_ + "/build/xlate/mp4_xlate";
    if (fs::is_regular_file(xlate, ec)) {
        fs::copy_file(xlate, build_dir_ + "/mp4_xlate", fs::copy_options::overwrite_existing, ec);
        fs::permissions(build_dir_ + "/mp4_xlate", fs::perms::owner_exec, fs::perm_options::add, ec);
    }
    if (fs::is_regular_file(xlate + ".id", ec))
        fs::copy_file(xlate + ".id", build_dir_ + "/mp4_xlate.id", fs::copy_options::overwrite_existing, ec);
    for (const char *name : {"shader-list.bin", "pipeline-list.bin", "shader-patches.bin"}) {
        // the shader lists of Tropical Freeze live in games/dktf (the runtime's name), its manifest in games/dkctf
        std::string from = payload_ + "/games/" + (game_ == "dkctf" ? std::string("dktf") : game_) + "/" + name;
        if (fs::is_regular_file(from, ec))
            fs::copy_file(from, build_dir_ + "/" + name, fs::copy_options::overwrite_existing, ec);
    }
    std::string stamp = std::string("{\n \"country-box\": ") + json_quote(launcher_version()) +
                        ",\n \"game\": " + json_quote(game_) + ",\n \"copy_manifest\": " + json_quote(manifest_id_) +
                        ",\n \"toolchain\": " + json_quote(toolchain_) + ",\n \"gen_opt\": \"-O1\",\n \"xlate\": " +
                        json_quote(trim(read_text(payload_ + "/build/xlate/mp4_xlate.id"))) +
                        ",\n \"date\": " + json_quote(local_iso()) + "\n}\n";
    if (!write_text(build_dir_ + "/build-stamp.json", stamp)) {
        outcome_.kind = "compile";
        outcome_.headline = "The build's stamp could not be written.";
        outcome_.explain = "The game is built, but build-stamp.json — what \u201cImport a build\u201d checks on another "
                           "computer — could not be written next to it.";
        outcome_.hint = "Check the drive, then Prepare again: nothing will be rebuilt.";
        return false;
    }
    return true;
}

bool PrepareJob::do_shaders() {
    std::error_code ec;
    if (!fs::is_regular_file(build_dir_ + "/shader-list.bin", ec)) return true;   // this game has no list (yet)
    stage.store(Shaders), stage_done.store(0), stage_total.store(-1), eta_s.store(-1);
    stage_elapsed_base_ = now_s();
    set_line_text("preparing the shaders");
    if (!child({build_dir_ + "/country-box", "--prepare-shaders", "--exefs", copy_ + "/exefs", "--romfs",
                copy_ + "/romfs"},
               build_dir_, "shaders")) {
        if (outcome_.kind == "cancelled") return false;
        outcome_.kind = "shaders";
        outcome_.headline = "The game is built; its shaders did not prepare now.";
        outcome_.explain = "The shader preparation stopped early. Nothing is lost.";
        outcome_.hint = "Press PLAY: the game prepares them on its own \u201cPreparing shaders\u201d screen (about a "
                        "minute, once). If that fails too, report the problem with the log below.";
        return false;
    }
    return true;
}

void PrepareJob::run() {
    double t0 = now_s();
    std::error_code ec;
    fs::create_directories(home_ + "/logs", ec);
    char t[32];
    time_t now = time(nullptr);
    strftime(t, sizeof t, "%Y%m%d-%H%M%S", localtime(&now));
    log_ = home_ + "/logs/prepare-" + game_ + "-" + t + ".log";
    std::ofstream(log_, std::ios::app) << "# prepare " << game_ << " mode " << mode_ << " payload " << payload_ << '\n';
    std::string python;
    bool ok = preflight(&python);
    if (ok && mode_ == Full) ok = do_lift(python);
    if (ok && mode_ == Full) ok = do_compile(python);
    if (ok && mode_ == Full) ok = place_runtime_files();
    if (ok) ok = do_shaders();
    if (outcome_.kind == "cancelled") {
        outcome_.headline = "Cancelled.";
        outcome_.explain = "Preparing stopped.";
        outcome_.hint = "Next Prepare picks up where this one stopped.";
    }
    outcome_.ok = ok;
    outcome_.log = log_;
    outcome_.elapsed_s = now_s() - t0;
    stage.store(Finished);
}

PrepareJob::Outcome PrepareJob::run_sync(const std::string &home, const Game &g, Mode mode, bool (*progress)(void *),
                                         void *ctx) {
    PrepareJob job(home, g, mode);
    while (!job.done()) {
        if (progress && progress(ctx)) job.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return job.outcome();
}

// ---- import a build made on another machine

static bool copy_tree_keep_player(const fs::path &from, const fs::path &to, int64_t *bytes_done, int64_t *bytes_total,
                                  std::string *err) {
    std::error_code ec;
    if (bytes_total && *bytes_total == 0) {
        for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) *bytes_total += (int64_t)it->file_size(ec);
        }
    }
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const fs::path &p = it->path();
        if (p == from) continue;
        std::string rel = fs::relative(p, from, ec).string();
        if (ec) break;
        // the other machine's saves, settings and shader marker never travel: those are per player
        if (rel == "profile" || rel == "mp4.ini" || rel == "shader-prepared.id" || rel.rfind("profile/", 0) == 0) {
            if (it->is_directory(ec)) it.disable_recursion_pending();
            continue;
        }
        fs::path q = to / rel;
        if (it->is_directory(ec)) {
            fs::create_directories(q, ec);
            continue;
        }
        if (!it->is_regular_file(ec)) continue;
        fs::copy_file(p, q, fs::copy_options::overwrite_existing | fs::copy_options::copy_symlinks, ec);
        if (ec) {
            *err = ec.message();
            return false;
        }
        if (bytes_done) *bytes_done += (int64_t)it->file_size(ec);
    }
    return true;
}

ImportOutcome import_build(const std::string &home, const Game &g, const std::string &path, int64_t *bytes_done,
                           int64_t *bytes_total) {
    ImportOutcome o;
    double t0 = now_s();
    std::error_code ec;
    fs::path src = fs::weakly_canonical(path, ec);
    if (path.empty() || !fs::exists(src, ec)) {
        o.kind = "not_found";
        o.headline = "That build was not found.";
        o.explain = "There is nothing at " + path + ".";
        o.hint = "Pick the folder (or .tar) that Prepare made on your other computer.";
        return o;
    }
    std::string low = path;
    for (auto &c : low) c = (char)tolower((unsigned char)c);
    if (low.size() > 4 && low.rfind(".tar") != std::string::npos) {
        fs::path staging = fs::path(home) / "import" / (g.id + ".tmp");
        fs::remove_all(staging, ec);
        fs::create_directories(staging, ec);
        fs::create_directories(fs::path(home) / "logs", ec);
        std::string tar = which("tar");
        pid_t pid = -1;
        if (!tar.empty())
            pid = proc_spawn({tar, "-xf", src.string(), "-C", staging.string()}, "", home + "/logs/import-" + g.id + ".log");
        int st = 0;
        bool ok = pid > 0;
        while (ok && proc_poll(pid, &st) == 1) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!ok || st != 0) {
            o.kind = "extract";
            o.headline = "That archive could not be unpacked.";
            o.explain = std::string(tar.empty() ? "tar is not on this computer." : "tar stopped with an error.");
            o.hint = "Bring the folder itself, or a .tar / .tar.gz of it.";
            return o;
        }
        src = staging;
        // an archive usually carries one folder on top; step into it (or into the folder that is the build)
        std::error_code e3;
        for (int depth = 0; depth < 2 && !fs::is_regular_file(src / "build-stamp.json", e3); depth++) {
            fs::path inner;
            for (auto &e : fs::directory_iterator(src, e3)) {
                if (!e.is_directory(e3)) continue;
                if (fs::is_regular_file(e.path() / "build-stamp.json", e3) || inner.empty()) inner = e.path();
                if (fs::is_regular_file(e.path() / "build-stamp.json", e3)) break;
            }
            if (inner.empty()) break;
            src = inner;
        }
    }
    Json stamp;
    if (!json_parse(read_text((src / "build-stamp.json").string()), stamp) || stamp.type != Json::Object) {
        o.kind = "not_build";
        o.headline = "That folder is not a Country-Box build.";
        o.explain = "A build that Prepare made carries build-stamp.json; " + src.string() + " has none.";
        o.hint = "Prepare the game on your other computer, then bring that folder (or a .tar of it) here.";
        return o;
    }
    if (stamp["game"].s() != g.id) {
        o.kind = "wrong_game";
        o.headline = "That build is of another game.";
        o.explain = "Its stamp says it is " + (stamp["game"].s().empty() ? "an unknown game" : stamp["game"].s()) +
                    "; this card is " + g.id + ".";
        return o;
    }
    if (g.copy.empty() || !g.manifest) {
        o.kind = "wrong_copy";
        o.headline = "Add your copy of the game first.";
        o.explain = "A build is only ever played against your own checked copy of the game.";
        o.hint = "Use \u201cAdd your copy\u201d on this card, then import the build again.";
        return o;
    }
    if (stamp["copy_manifest"].s().empty()) {
        o.kind = "wrong_copy";
        o.headline = "That build does not say which copy it was built from.";
        o.explain = "Its stamp has no copy check in it; Country-Box cannot tell it matches yours.";
        o.hint = "Build it again on the other computer with this Country-Box version.";
        return o;
    }
    if (stamp["copy_manifest"].s() != g.manifest->id) {
        o.kind = "wrong_copy";
        o.headline = "That build was made from another version of the game.";
        o.explain = "It was built from a copy whose check (" + stamp["copy_manifest"].s().substr(0, 8) +
                    "\u2026) differs from yours (" + g.manifest->id.substr(0, 8) + "\u2026).";
        o.hint = "Build it again on the other computer from a copy like yours, or add that copy here instead.";
        return o;
    }
    if (!fs::is_regular_file(src / "country-box", ec)) {
        o.kind = "not_build";
        o.headline = "That build has no game binary in it.";
        o.explain = src.string() + " has the stamp but no country-box.";
        return o;
    }
    fs::path to = fs::path(home) / "games" / g.id;
    fs::create_directories(to, ec);
    std::string err;
    if (!copy_tree_keep_player(src, to, bytes_done, bytes_total, &err)) {
        o.kind = "copy";
        o.headline = "The build could not be copied in.";
        o.explain = "Copying into " + to.string() + " failed (" + err + ").";
        o.hint = "Check the free space and permissions there, then import again.";
        return o;
    }
    for (const char *name : {"country-box", "mp4_xlate"})
        if (fs::is_regular_file(to / name, ec)) fs::permissions(to / name, fs::perms::owner_exec, fs::perm_options::add, ec);
    fs::remove_all(fs::path(home) / "import" / (g.id + ".tmp"), ec);
    o.ok = true;
    o.build = to.string();
    o.elapsed_s = now_s() - t0;
    return o;
}
