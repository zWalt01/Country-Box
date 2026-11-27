// Country-Box launcher: moving the library (relocate.h).
#include "relocate.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

#include "library.h"
#include "manifest.h"

namespace fs = std::filesystem;

// Kept in place by a player's move: tools (python/, app/), the record of where the library is, import staging,
// temporary files and caches. A migration moves the tools too; what is only temporary is dropped instead.
static const std::set<std::string> kStay = {"python", "app", "library-location", "library-choice", "import", "tmp", "cache"};
static const std::set<std::string> kStayMigration = {"library-location", "library-choice", "import", "tmp", "cache"};

static const char *kDataFolder = "Country-Box-data";

std::string legacy_launcher_home() {
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && *appdata) return std::string(appdata) + "/Country-Box";
    const char *userp = getenv("USERPROFILE");
    if (userp && *userp) return std::string(userp) + "/AppData/Roaming/Country-Box";
#endif
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x) return std::string(x) + "/country-box";
    return std::string(h ? h : ".") + "/.local/share/country-box";
}

std::string recomplauncher_data_dir() {
#ifdef _WIN32
    return "";
#else
    const char *d = getenv("RECOMPLAUNCHER_DATA");
    if (!d || d[0] != '/') return "";   // only an absolute path: never a folder relative to wherever this started
    std::string s = d;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return fs::path(s).lexically_normal().string();
#endif
}

bool under_recomplauncher() {
#ifdef _WIN32
    return false;
#else
    const char *r = getenv("RECOMPLAUNCHER");
    return r && strcmp(r, "1") == 0;
#endif
}

// The portable rule of 1.0.7: Country-Box-data next to the package AppRun names; "" for a bare binary.
static std::string beside_package_dir() {
    // AppRun: the .AppImage file ($APPIMAGE), or the extracted folder (tarball, --appimage-extract) it runs from
    const char *e = getenv("CB_PORTABLE_EXE");
    if (!e || !*e) return "";
    std::string anchor = e;
    while (anchor.size() > 1 && anchor.back() == '/') anchor.pop_back();
    fs::path parent = fs::path(anchor).parent_path();
    if (parent.empty()) parent = ".";
    std::error_code ec;
    fs::path abs = fs::absolute(parent, ec);
    return ((ec ? parent : abs).lexically_normal() / kDataFolder).string();
}

std::string portable_data_dir() {
    if (std::string d = recomplauncher_data_dir(); !d.empty()) return d;   // RecompLauncher's {data} wins (relocate.h)
    return beside_package_dir();
}

static bool runs_programs(const fs::path &dir, std::string &why);

static std::string g_base, g_base_note;
static bool g_base_decided = false;

// The data folder when it can hold the library, else the legacy place with the reason.
static void decide_base() {
    if (g_base_decided) return;
    g_base_decided = true;
    std::string p = portable_data_dir();
    if (p.empty()) {
        g_base = legacy_launcher_home();
        return;
    }
    std::error_code ec;
    fs::path dir = p, parent = dir.parent_path();
    std::string why;
    if (fs::is_directory(dir, ec)) {
        if (access(dir.c_str(), W_OK) != 0) why = "it can't be written";
    } else if (!fs::is_directory(parent, ec)) why = "its folder is not there";
    else if (access(parent.c_str(), W_OK) != 0) why = "that folder can't be written";
    else runs_programs(parent, why);
    if (why.empty()) {
        g_base = p;
        return;
    }
    g_base = legacy_launcher_home();
    g_base_note = "Country-Box keeps its data in " + g_base + " because " + p + " can't be used (" + why + ").";
}

std::string default_launcher_home() {
    decide_base();
    return g_base;
}

std::string base_note() {
    decide_base();
    return g_base_note;
}

void set_base_home(const std::string &dir) {
    decide_base();
    g_base = dir;
}

std::string library_location_file() { return default_launcher_home() + "/library-location"; }

void contain_temp_files(const std::string &base) {
    if (base.empty()) return;
    std::error_code ec;
    fs::create_directories(base + "/tmp", ec);
    fs::create_directories(base + "/cache", ec);
    setenv("TMPDIR", (base + "/tmp").c_str(), 1);
    setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);   // pip, Mesa's cache for the launcher's own window
}

static std::string human(double b) {
    char t[64];
    if (b >= 1e9) snprintf(t, sizeof t, "%.1f GB", b / 1e9);
    else snprintf(t, sizeof t, "%.0f MB", b / 1e6);
    return t;
}

static bool is_empty_dir(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec) && fs::is_empty(p, ec);
}

std::string library_target(const std::string &picked) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(picked, ec);
    if (p.empty()) p = picked;
    if (p == fs::weakly_canonical(default_launcher_home(), ec)) return p.string();   // back home, next to the tools
    if (!fs::exists(p, ec) || is_empty_dir(p)) return p.string();
    return (p / "Country-Box").string();
}

static int64_t tree_bytes(const fs::path &p) {
    std::error_code ec;
    if (fs::is_symlink(p, ec)) return 0;
    if (fs::is_regular_file(p, ec)) return (int64_t)fs::file_size(p, ec);
    int64_t n = 0;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_symlink(e2) && it->is_regular_file(e2)) n += (int64_t)it->file_size(e2);
    }
    return n;
}

// A copy that keeps symlinks as symlinks and file modes; done counts bytes.
static bool copy_tree(const fs::path &src, const fs::path &dst, std::atomic<int64_t> &done, std::string &err) {
    std::error_code ec;
    auto one = [&](const fs::path &s, const fs::path &d) -> bool {
        std::error_code e;
        auto st = fs::symlink_status(s, e);
        if (fs::is_symlink(st)) fs::copy_symlink(s, d, e);
        else if (fs::is_directory(st)) {
            fs::create_directory(d, e);
            std::error_code ignore;   // a drive without Unix modes may refuse; the folder is still usable
            if (!e) fs::permissions(d, st.permissions(), ignore);
        } else if (fs::is_regular_file(st)) {
            fs::copy_file(s, d, fs::copy_options::none, e);
            if (!e) done += (int64_t)fs::file_size(s, e);
        }
        if (e) err = d.string() + ": " + e.message();
        return !e;
    };
    if (!one(src, dst)) return false;
    if (!fs::is_directory(fs::symlink_status(src, ec))) return true;
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) return err = src.string() + ": " + ec.message(), false;
        if (!one(it->path(), dst / it->path().lexically_relative(src))) return false;
    }
    return true;
}

static bool same_bytes(const fs::path &a, const fs::path &b) {
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    char p[65536], q[65536];
    for (;;) {
        x.read(p, sizeof p), y.read(q, sizeof q);
        if (x.gcount() != y.gcount() || memcmp(p, q, (size_t)x.gcount())) return false;
        if (!x || !y) return x.eof() && y.eof();
    }
}

// What a save or a setting looks like: read back byte for byte after a copy (the rest is compared by size: the
// builds and the generated C are many gigabytes and can be made again).
static bool precious(const fs::path &rel) {
    std::string s = rel.string(), ext = rel.extension().string();
    return s.find("profile/") != std::string::npos || s.rfind("profile", 0) == 0 || ext == ".ini" || ext == ".json" ||
           rel.filename() == "library-location";
}

// The copy at dst holds every regular file of src with its size (and the same bytes for precious ones).
static bool same_tree(const fs::path &src, const fs::path &dst, std::string &err) {
    std::error_code ec;
    auto one = [&](const fs::path &s, const fs::path &d, const fs::path &rel) {
        std::error_code e;
        if (fs::is_symlink(fs::symlink_status(s, e))) return fs::is_symlink(fs::symlink_status(d, e)) || (err = d.string() + ": missing", false);
        if (!fs::is_regular_file(s, e)) return true;
        if (!fs::is_regular_file(d, e) || fs::file_size(s, e) != fs::file_size(d, e)) return err = d.string() + ": size differs", false;
        if (precious(rel) && !same_bytes(s, d)) return err = d.string() + ": contents differ", false;
        return true;
    };
    if (!fs::is_directory(fs::symlink_status(src, ec))) return one(src, dst, src.filename());
    for (auto it = fs::recursive_directory_iterator(src, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) return err = src.string() + ": " + ec.message(), false;
        fs::path rel = it->path().lexically_relative(src);
        if (!one(it->path(), dst / rel, src.filename() / rel)) return false;
    }
    return true;
}

// ---- saves first: a copy on the Desktop before any move (relocate.h)

std::string desktop_dir() {
    if (const char *o = getenv("CB_DESKTOP_DIR"); o && *o) return o;
    std::error_code ec;
#ifdef _WIN32
    if (const char *u = getenv("USERPROFILE"); u && *u && fs::is_directory(std::string(u) + "/Desktop", ec))
        return std::string(u) + "/Desktop";
#endif
    const char *h = getenv("HOME");
    std::string home = h && *h ? h : ".";
    const char *xc = getenv("XDG_CONFIG_HOME");
    std::ifstream f((xc && *xc ? std::string(xc) : home + "/.config") + "/user-dirs.dirs");
    for (std::string line; std::getline(f, line);) {
        if (line.rfind("XDG_DESKTOP_DIR=", 0) != 0) continue;
        std::string v = line.substr(16);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        if (v.rfind("$HOME", 0) == 0) v = home + v.substr(5);
        while (v.size() > 1 && v.back() == '/') v.pop_back();
        if (!v.empty() && v != home && fs::is_directory(v, ec)) return v;
    }
    if (fs::is_directory(home + "/Desktop", ec)) return home + "/Desktop";
    return home;
}

static std::string now_text(const char *fmt) {
    time_t t = time(nullptr);
    struct tm tmv {};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[64];
    strftime(b, sizeof b, fmt, &tmv);
    return b;
}

bool backup_saves(const std::string &library, const std::string &moving_to, std::string &dir, std::string &err) {
    dir.clear();
    std::error_code ec;
    fs::path games = fs::path(library) / "games";
    std::vector<std::pair<std::string, fs::path>> found;   // (game id, its folder) with something to keep
    for (auto it = fs::directory_iterator(games, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e;
        if (!it->is_directory(e)) continue;
        if (fs::exists(it->path() / "profile", e) || fs::exists(it->path() / "mp4.ini", e))
            found.push_back({it->path().filename().string(), it->path()});
    }
    if (found.empty()) return true;
    fs::path desk = desktop_dir();
    std::string base = "Country-Box saves backup " + now_text("%Y-%m-%d %H-%M");
    fs::path out = desk / base;
    for (int n = 2; fs::exists(out, ec); n++) out = desk / (base + " (" + std::to_string(n) + ")");
    fs::create_directories(out, ec);
    if (ec) return err = out.string() + ": " + ec.message(), false;
    std::atomic<int64_t> done{0};
    std::string lines;
    for (auto &g : found) {
        const Manifest *m = manifest_get(g.first);
        std::string name = m && !m->name.empty() ? m->name : g.first;
        for (size_t at; (at = name.find(": ")) != std::string::npos;) name.replace(at, 2, " - ");   // "Country: Tropical"
        for (char &c : name)
            if (strchr("/\\:*?\"<>|", c)) c = '-';
        fs::path to = out / name;
        fs::create_directories(to, ec);
        for (const char *item : {"profile", "mp4.ini"}) {
            fs::path src = g.second / item;
            if (!fs::exists(src, ec)) continue;
            if (!copy_tree(src, to / item, done, err)) return false;
            // read back: every file, byte for byte
            if (fs::is_directory(src, ec)) {
                for (auto it = fs::recursive_directory_iterator(src, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
                    std::error_code e;
                    if (!it->is_regular_file(e)) continue;
                    fs::path d = to / item / it->path().lexically_relative(src);
                    if (!same_bytes(it->path(), d)) return err = d.string() + ": the copy does not match", false;
                }
                if (ec) return err = src.string() + ": " + ec.message(), false;
            } else if (!same_bytes(src, to / item)) return err = (to / item).string() + ": the copy does not match", false;
        }
        lines += "  " + name + "/   (game id " + g.first + ")\n";
    }
    std::ofstream r(out / "README.txt");
    r << "Country-Box: a copy of your saves\n\n"
      << "Made on " << now_text("%Y-%m-%d at %H:%M") << ", just before Country-Box moved your library\n"
      << "  from " << library << "\n  to   " << moving_to << "\n\n"
      << "Your saves moved with your games, so you don't need to do anything: this folder is an extra copy, in case.\n\n"
      << lines << "\n"
      << "Each game's folder holds its profile/ (the saves) and mp4.ini (its settings). To put one back, close the game\n"
      << "and copy them into that game's folder in your library (games/<game id>/).\n\n"
      << "You can delete this folder whenever you like.\n";
    if (!r) return err = (out / "README.txt").string() + ": cannot write", false;
    dir = out.string();
    return true;
}

// library.ini's "build = " lines that point inside the old folder follow it.
static void rewrite_builds(const fs::path &ini, const std::string &from, const std::string &to) {
    std::ifstream f(ini);
    if (!f) return;
    std::stringstream out;
    bool changed = false;
    for (std::string line; std::getline(f, line);) {
        size_t eq = line.find('=');
        std::string key = eq == std::string::npos ? "" : line.substr(0, eq);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        if (key == "build") {
            std::string v = line.substr(eq + 1);
            size_t a = v.find_first_not_of(' ');
            v = a == std::string::npos ? "" : v.substr(a);
            if (v == from || v.rfind(from + "/", 0) == 0) line = "build = " + to + v.substr(from.size()), changed = true;
        }
        out << line << "\n";
    }
    f.close();
    if (changed) std::ofstream(ini, std::ios::trunc) << out.str();
}

static bool write_location(const std::string &to, std::string &err) {
    std::error_code ec;
    fs::path def = fs::weakly_canonical(default_launcher_home(), ec), file = library_location_file();
    if (fs::weakly_canonical(to, ec) == def) {   // back at the default place: nothing to remember
        fs::remove(file, ec);
        return true;
    }
    fs::create_directories(def, ec);
    std::string tmp = file.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << to << "\n";
        if (!f) return err = "cannot write " + tmp, false;
    }
    if (rename(tmp.c_str(), file.string().c_str())) return err = "cannot write " + file.string() + ": " + strerror(errno), false;
    return true;
}

// The library holds programs (each game's country-box): the drive must keep the executable bit and allow running
// them. exFAT/NTFS/FAT drives (often SD cards and USB drives) and noexec mounts don't; checked by trying, not by
// filesystem name, so it holds on any distribution and mount setup.
static bool runs_programs(const fs::path &dir, std::string &why) {
#ifdef _WIN32
    (void)dir;
    (void)why;
    return true;
#else
    struct statvfs v {};
    if (statvfs(dir.c_str(), &v) == 0 && (v.f_flag & ST_NOEXEC)) return why = "it is mounted without permission to run programs (noexec)", false;
    fs::path t = dir / ".country-box-probe";
    std::error_code ec;
    { std::ofstream(t) << "#!/bin/sh\n"; }
    bool ok = chmod(t.c_str(), 0755) == 0;
    struct stat st {};
    ok = ok && stat(t.c_str(), &st) == 0 && (st.st_mode & S_IXUSR) && access(t.c_str(), X_OK) == 0;
    fs::remove(t, ec);
    if (!ok) why = "its format can't keep programs runnable (exFAT, NTFS or FAT)";
    return ok;
#endif
}

// The move itself; the caller sets m.finished (a migration has steps after it).
static void move_impl(const std::string &from_in, const std::string &picked, LibraryMove &m) {
    auto fail = [&](const std::string &h, const std::string &e) { m.ok = false, m.headline = h, m.explain = e; };
    std::error_code ec;
    fs::path from = fs::weakly_canonical(from_in, ec);
    fs::path to = library_target(picked);
    m.from = from.string(), m.to = to.string();
    if (picked.empty()) return fail("No folder was chosen.", "");
    if (to == from) return fail("Your library is already there.", m.to);
    if (to.string().rfind(from.string() + "/", 0) == 0)
        return fail("That folder is inside your library.", "Pick a folder outside " + m.from + ".");
    const std::set<std::string> &stay = m.everything ? kStayMigration : kStay;
    bool to_default = to == fs::weakly_canonical(default_launcher_home(), ec);
    auto only_tools = [&] {
        std::error_code e;
        for (auto &d : fs::directory_iterator(to, e))
            if (!kStay.count(d.path().filename().string())) return false;
        return true;
    };
    if (fs::exists(to, ec) && !is_empty_dir(to) && !(to_default && only_tools()))
        return fail("That folder is not empty.", m.to + " already holds files. Pick an empty folder, or the folder that should hold it.");
    // what moves
    std::vector<fs::path> items;
    for (auto it = fs::directory_iterator(from, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (!stay.count(it->path().filename().string())) items.push_back(it->path());
    if (ec && fs::exists(from)) return fail("Your library can't be read.", m.from + ": " + ec.message());
    bool made_to = !fs::exists(to, ec);
    fs::create_directories(to, ec);
    if (ec) return fail("That folder can't be used.", m.to + ": " + ec.message());
    if (std::string why; !runs_programs(to, why)) {
        std::error_code e;
        if (made_to) fs::remove_all(to, e);
        return fail("Games can't run from that drive.", m.to + ": " + why + ". Pick a folder on a Linux-formatted drive (ext4, btrfs, xfs…).");
    }
    if (m.backup_saves) {   // saves first: a checked copy on the Desktop, or nothing moves (relocate.h)
        std::string err;
        if (!backup_saves(m.from, m.to, m.backup, err)) {
            std::error_code e;
            if (made_to) fs::remove_all(to, e);
            if (!m.backup.empty()) fs::remove_all(m.backup, e);
            m.backup.clear();
            return fail("Your saves could not be copied to the Desktop first, so nothing was moved.",
                        err + ". Your library is still in " + m.from + ".");
        }
    }
    auto undo_dir = [&] {
        std::error_code e;
        if (made_to) fs::remove_all(to, e);
        else
            for (auto &d : fs::directory_iterator(to, e))
                if (!kStay.count(d.path().filename().string())) fs::remove_all(d.path(), e);
    };
    struct stat a {}, b {};
    bool same_drive = stat(from.string().c_str(), &a) == 0 && stat(to.string().c_str(), &b) == 0 && a.st_dev == b.st_dev;
    if (same_drive) {
        std::vector<fs::path> moved;
        for (auto &p : items) {
            fs::rename(p, to / p.filename(), ec);
            if (ec) {
                std::string why = p.string() + ": " + ec.message();
                for (auto &q : moved) fs::rename(to / q.filename(), q, ec);
                undo_dir();
                return fail("The library could not be moved.", why);
            }
            moved.push_back(p);
        }
        rewrite_builds(to / "library.ini", m.from, m.to);
        std::string err;
        if (!m.everything && !m.no_record && !write_location(m.to, err)) {   // a migration records its place itself
            for (auto &q : moved) fs::rename(to / q.filename(), q, ec);
            undo_dir();
            return fail("The new place could not be recorded.", err);
        }
    } else {
        int64_t total = 0;
        for (auto &p : items) total += tree_bytes(p);
        m.total.store(total);
        auto sp = fs::space(to, ec);
        if (!ec && (int64_t)sp.available < total + (64 << 20)) {
            undo_dir();
            return fail("There is not enough free space there.",
                        "Your library needs " + human((double)total) + "; " + m.to + " has " + human((double)sp.available) + " free.");
        }
        std::string err;
        for (auto &p : items)
            if (!copy_tree(p, to / p.filename(), m.done, err)) {
                undo_dir();
                return fail("The library could not be copied.", err + ". Nothing was changed; it is still in " + m.from + ".");
            }
        for (auto &p : items)   // read back before anything old goes: sizes everywhere, the bytes of saves and settings
            if (!same_tree(p, to / p.filename(), err)) {
                undo_dir();
                return fail("The copy did not match the library.", err + ". Nothing was changed; it is still in " + m.from + ".");
            }
        rewrite_builds(to / "library.ini", m.from, m.to);
        if (!m.everything && !m.no_record && !write_location(m.to, err)) {
            undo_dir();
            return fail("The new place could not be recorded.", err);
        }
        for (auto &p : items) fs::remove_all(p, ec);   // only now: the copy is complete and recorded
    }
    // the old folder goes too when nothing is left in it and it is not the default place (which keeps tools)
    if (fs::weakly_canonical(default_launcher_home(), ec) != from && is_empty_dir(from)) fs::remove(from, ec);
    m.ok = true;
}

void move_library(const std::string &from, const std::string &picked, LibraryMove &m) {
    move_impl(from, picked, m);
    m.finished.store(true);
}

// ---- first-run migration (relocate.h)

static bool holds_library(const fs::path &d) {
    std::error_code ec;
    return fs::is_regular_file(d / "library.ini", ec) || fs::is_directory(d / "games", ec);
}

// The nearest folder that exists at or above p (the data folder may not exist yet: its drive is its parent's).
static fs::path existing_ancestor(fs::path p) {
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec) && p != p.parent_path()) p = p.parent_path();
    return p;
}

// A process (an older launcher's game, a Prepare) running a program from inside dir, or working in it.
static bool something_runs_from(const fs::path &dir) {
    std::error_code ec;
    std::string root = fs::weakly_canonical(dir, ec).string() + "/";
    std::string self = std::to_string(getpid());
    for (auto &e : fs::directory_iterator("/proc", ec)) {
        std::string pid = e.path().filename().string();
        if (pid.empty() || !isdigit((unsigned char)pid[0]) || pid == self) continue;
        for (const char *what : {"exe", "cwd"}) {
            char buf[4096];
            ssize_t n = readlink(("/proc/" + pid + "/" + what).c_str(), buf, sizeof buf - 1);
            if (n > 0 && std::string(buf, (size_t)n).rfind(root, 0) == 0) return true;
        }
    }
    return false;
}

Migration plan_migration() {
    Migration m;
    std::string p = portable_data_dir();
    if (p.empty() || !base_note().empty()) return m;   // a bare binary, or the data folder can't be used
    std::error_code ec;
    fs::path from = legacy_launcher_home(), to = p;
    if (!fs::is_directory(from, ec) || fs::is_empty(from, ec) || holds_library(to)) return m;
    if (fs::weakly_canonical(from, ec) == fs::weakly_canonical(to, ec)) return m;
    if (fs::exists(to, ec) && !fs::is_empty(to, ec)) return m;   // something else is there: never mixed in
    m.from = from.string(), m.to = to.string(), m.library = m.from;
    {   // "keep it where it is" (keep_library_here): the old place stays, and nobody asks again
        std::ifstream c(from / "library-choice");
        std::string choice;
        std::getline(c, choice);
        if (choice.rfind("keep", 0) == 0) {
            m.stay = true;
            return m;
        }
    }
    std::ifstream f(from / "library-location");   // a library the player moved elsewhere stays there
    std::string moved;
    std::getline(f, moved);
    while (!moved.empty() && strchr(" \t\r\n", moved.back())) moved.pop_back();
    if (!moved.empty() && fs::is_directory(moved, ec)) m.library = moved;
    struct stat a {}, b {};
    fs::path anc = existing_ancestor(to);
    m.same_drive = stat(from.c_str(), &a) == 0 && stat(anc.c_str(), &b) == 0 && a.st_dev == b.st_dev;
    m.busy = something_runs_from(from);
    m.needed = true;
    return m;
}

bool keep_library_here(const Migration &plan, std::string &err) {
    std::string file = (fs::path(plan.from) / "library-choice").string(), tmp = file + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << "keep (" << now_text("%Y-%m-%d %H:%M") << "): the library stays here; the launcher does not ask again\n";
        if (!f) return err = "cannot write " + tmp, false;
    }
    if (rename(tmp.c_str(), file.c_str())) return err = "cannot write " + file + ": " + strerror(errno), false;
    return true;
}

// What a migration leaves behind in the old place: temporary files, caches, records. It goes once empty.
static void tidy_old_place(const fs::path &from) {
    std::error_code ec;
    for (const char *gone : {"tmp", "cache", "import", "library-location", "library-choice"}) fs::remove_all(from / gone, ec);
    if (is_empty_dir(from)) fs::remove(from, ec);
}

void migrate_library(const Migration &plan, const std::string &picked, LibraryMove &mv) {
    auto done = [&] { mv.finished.store(true); };
    std::error_code ec;
    std::string err;
    fs::path from = plan.from;
    bool to_data = picked.empty() || fs::weakly_canonical(picked, ec) == fs::weakly_canonical(plan.to, ec);
    if (to_data) {   // everything, tools included, into the data folder
        mv.everything = true;
        move_impl(plan.from, plan.to, mv);
        if (!mv.ok) return done();
        set_base_home(plan.to);
        if (plan.library != plan.from && !write_location(plan.library, err)) {
            // the library itself did not move; without its record the next start would not find it: say so
            mv.ok = false;
            mv.headline = "The data folder could not record where your library is.";
            mv.explain = err + ". Your library is still in " + plan.library + ".";
            set_base_home(plan.from);
            return done();
        }
        tidy_old_place(from);
        return done();
    }
    // the folder the player picked: the library goes there; the tools and the record of that place go to the data
    // folder, which stays the base (it is what a portable Country-Box looks at first)
    mv.no_record = true;
    move_impl(plan.from, picked, mv);   // python/ and app/ stay behind (kStay)
    if (!mv.ok) return done();
    LibraryMove tools;
    tools.everything = true, tools.backup_saves = false;
    move_impl(plan.from, plan.to, tools);
    if (tools.ok) set_base_home(plan.to);
    if (!write_location(mv.to, err)) {
        // the data folder can't hold the record: the old place keeps it (and the tools, if they did not move), so
        // the next start finds the library and brings the rest over
        set_base_home(plan.from);
        if (!write_location(mv.to, err)) {
            mv.ok = false;
            mv.headline = "Your library moved, but its new place could not be recorded.";
            mv.explain = "It is in " + mv.to + " now (" + err + "). Choose it with Move library on the About tab.";
        }
        return done();
    }
    if (tools.ok) tidy_old_place(from);
    done();
}

// ---- RecompLauncher's {data}: the first start copies the old data folder in (relocate.h)

static const char *kCopiedMarker = ".country-box-data-copied";
static const std::string kCopying = ".copying";
// Left out: temporary files, caches and import staging (made again when needed), the build kit's link (it only
// exists while a game is being prepared) and the hub's own installs from its updates (RecompLauncher updates it).
static const std::set<std::string> kNotCopied = {"tmp", "cache", "import", "toolkit", "app"};

static bool is_copying(const fs::path &p) {
    std::string n = p.filename().string();
    return n.size() > kCopying.size() && n.compare(n.size() - kCopying.size(), kCopying.size(), kCopying) == 0;
}

OldDataCopy plan_old_data_copy() {
    OldDataCopy c;
    std::string to = recomplauncher_data_dir(), from = beside_package_dir();
    if (to.empty() || from.empty()) return c;
    std::error_code ec;
    if (fs::exists(fs::path(to) / kCopiedMarker, ec)) return c;   // copied once already: never again
    if (!fs::is_directory(from, ec) || fs::weakly_canonical(from, ec) == fs::weakly_canonical(to, ec)) return c;
    bool any = false;
    for (auto it = fs::directory_iterator(from, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (!kNotCopied.count(it->path().filename().string()) && !is_copying(it->path())) any = true;
    if (!any) return c;   // nothing but temporary files: nothing to bring over
    c.from = from, c.to = to, c.needed = true;
    c.busy = something_runs_from(from);
    return c;
}

// A game's settings file (games/<game>/mp4.ini): on the start that adopts the old data, the old one replaces one the
// launcher may already have written into {data} (it writes the profile's settings before the game starts)
static bool is_game_settings(const fs::path &rel) {
    auto p = rel.parent_path();
    return rel.filename() == "mp4.ini" && !p.parent_path().empty() && p.parent_path() == "games";
}

struct CopyStep {
    fs::path src, dst;   // dst missing, or a game's settings file to replace
};

// What to bring from src (a folder of the old data) into dst (the same folder in {data}): what dst does not have yet,
// file by file inside folders it already has (the launcher can make {data}/games/<game>/mp4.ini before the first
// start), plus the games' settings files (above). What an earlier start left half copied in dst goes first.
static bool plan_merge(const fs::path &src, const fs::path &dst, const fs::path &rel, std::vector<CopyStep> &out,
                       std::string &err) {
    std::error_code ec;
    std::vector<fs::path> stale;
    for (auto it = fs::directory_iterator(dst, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (is_copying(it->path())) stale.push_back(it->path());
    if (ec) return err = dst.string() + ": " + ec.message(), false;
    for (auto &p : stale)
        if (fs::remove_all(p, ec), ec) return err = p.string() + ": a copy cut short earlier could not be removed: " + ec.message(), false;
    std::vector<fs::path> entries;
    for (auto it = fs::directory_iterator(src, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) entries.push_back(it->path());
    if (ec) return err = src.string() + ": " + ec.message(), false;
    for (auto &s : entries) {
        std::string name = s.filename().string();
        if ((rel.empty() && kNotCopied.count(name)) || is_copying(s)) continue;
        fs::path d = dst / name, r = rel / name;
        std::error_code e;
        auto ds = fs::symlink_status(d, e), ss = fs::symlink_status(s, e);
        if (!fs::exists(ds)) out.push_back({s, d});
        else if (fs::is_directory(ss) && fs::is_directory(ds)) {
            if (!plan_merge(s, d, r, out, err)) return false;
        } else if (is_game_settings(r) && fs::is_regular_file(ss) && fs::is_regular_file(ds))
            out.push_back({s, d});
    }
    return true;
}

// Every item a start renames into {data} before the marker is written goes in this journal first (its path inside
// {data}, one per line). A start that fails or is cut short leaves items older than the saves the old folder gets
// next (that start plays from it), so a failure, and the next start, take back exactly those items and copy them
// again; nothing else in {data} is touched, and the originals never. The journal goes once the marker is there.
static const char *kJournal = ".country-box-copying-items";

static bool journal_add(const fs::path &to, const fs::path &rel, std::string &err) {
    std::string line = rel.generic_string(), file = (to / kJournal).string();
    if (line.find('\n') != std::string::npos) return err = rel.string() + ": a name with a line break can't be copied", false;
    FILE *f = fopen(file.c_str(), "a");
    if (!f) return err = "cannot write " + file + ": " + strerror(errno), false;
    bool ok = fputs((line + "\n").c_str(), f) >= 0 && fflush(f) == 0;
#ifndef _WIN32
    ok = ok && fsync(fileno(f)) == 0;
#endif
    if (fclose(f) != 0) ok = false;
    if (!ok) err = "cannot write " + file;
    return ok;
}

static bool journal_undo(const fs::path &to, std::string &err) {
    fs::path file = to / kJournal;
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(file, ec))) return true;
    std::ifstream f(file);
    if (!f) return err = "cannot read " + file.string(), false;
    for (std::string line; std::getline(f, line);) {
        fs::path rel = fs::path(line).lexically_normal();
        bool plain = !line.empty() && rel.is_relative() && !rel.has_root_name();
        for (const fs::path &c : rel) plain = plain && c != ".." && c != ".";
        if (!plain) continue;   // never anything outside {data}
        if (fs::remove_all(to / rel, ec), ec)
            return err = (to / rel).string() + ": a copy cut short earlier could not be removed: " + ec.message(), false;
    }
    f.close();
    if (fs::remove(file, ec), ec) return err = "cannot remove " + file.string() + ": " + ec.message(), false;
    return true;
}

void copy_old_data(const OldDataCopy &plan, LibraryMove &m) {
    auto fail = [&](const std::string &h, const std::string &e) {
        m.ok = false, m.headline = h, m.explain = e;
        m.finished.store(true);
    };
    std::error_code ec;
    fs::path from = plan.from, to = plan.to;
    m.from = plan.from, m.to = plan.to;
    fs::create_directories(to, ec);
    if (ec) return fail("The game's data folder can't be used.", m.to + ": " + ec.message());
    std::string err;
    // what an earlier start put in place without finishing goes first: it is copied again, from the old folder as it
    // is now (journal above)
    if (!journal_undo(to, err)) return fail("Your data can't be copied.", err + ". It is still in " + m.from + ".");
    // a failure from here on takes back what this start put in place
    auto undo_fail = [&](const std::string &h, const std::string &e) {
        std::string why;
        if (!journal_undo(to, why)) fprintf(stderr, "launcher: %s (the next start tries again)\n", why.c_str());
        fail(h, e);
    };
    // what to copy: everything not copied yet (an item is renamed into place only once it is complete)
    std::vector<CopyStep> items;
    if (!plan_merge(from, to, "", items, err)) return fail("Your data can't be copied.", err + ". It is still in " + m.from + ".");
    int64_t total = 0;
    for (auto &p : items) total += tree_bytes(p.src);
    m.total.store(total);
    auto sp = fs::space(to, ec);
    if (!ec && (int64_t)sp.available < total + (64 << 20))
        return fail("There is not enough free space to copy your data.",
                    "It needs " + human((double)total) + "; " + m.to + " has " + human((double)sp.available) + " free.");
    for (auto &step : items) {
        const fs::path &p = step.src, &dst = step.dst;
        fs::path part = dst.parent_path() / (dst.filename().string() + kCopying);
        std::error_code e;
        if (!copy_tree(p, part, m.done, err) || !same_tree(p, part, err) || !journal_add(to, dst.lexically_relative(to), err)) {
            fs::remove_all(part, e);
            return undo_fail("Your data could not be copied.", err + ". It is still in " + m.from + ".");
        }
        if (fs::rename(part, dst, e), e) {
            std::string why = dst.string() + ": " + e.message();
            fs::remove_all(part, e);
            return undo_fail("Your data could not be copied.", why + ". It is still in " + m.from + ".");
        }
    }
    rewrite_builds(to / "library.ini", m.from, m.to);   // builds recorded inside the old folder: their copies here
    // the marker last: the one sign that everything is here
    std::string file = (to / kCopiedMarker).string(), tmp = file + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << "copied from " << m.from << " on " << now_text("%Y-%m-%d %H:%M")
          << "; that folder was left as it was, and is not copied again\n";
        if (!f) return undo_fail("The copy could not be recorded.", "cannot write " + tmp);
    }
    if (rename(tmp.c_str(), file.c_str())) return undo_fail("The copy could not be recorded.", file + ": " + strerror(errno));
    fs::remove(to / kJournal, ec);   // done: nothing to take back (a journal left over is never read again)
    m.ok = true;
    m.finished.store(true);
}
