// Country-Box launcher: moving the library (relocate.h).
#include "relocate.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

#include "library.h"

namespace fs = std::filesystem;

// Kept in place by a move: tools (python/, app/), the record of where the library is, and import staging.
static const std::set<std::string> kStay = {"python", "app", "library-location", "import"};

std::string default_launcher_home() {
    const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (x && *x) return std::string(x) + "/country-box";
    return std::string(h ? h : ".") + "/.local/share/country-box";
}

std::string library_location_file() { return default_launcher_home() + "/library-location"; }

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
    if (rename(tmp.c_str(), file.c_str())) return err = "cannot write " + file.string() + ": " + strerror(errno), false;
    return true;
}

// The library holds programs (each game's country-box): the drive must keep the executable bit and allow running
// them. exFAT/NTFS/FAT drives (often SD cards and USB drives) and noexec mounts don't; checked by trying, not by
// filesystem name, so it holds on any distribution and mount setup.
static bool runs_programs(const fs::path &dir, std::string &why) {
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
}

void move_library(const std::string &from_in, const std::string &picked, LibraryMove &m) {
    auto fail = [&](const std::string &h, const std::string &e) {
        m.ok = false, m.headline = h, m.explain = e;
        m.finished.store(true);
    };
    std::error_code ec;
    fs::path from = fs::weakly_canonical(from_in, ec);
    fs::path to = library_target(picked);
    m.from = from.string(), m.to = to.string();
    if (picked.empty()) return fail("No folder was chosen.", "");
    if (to == from) return fail("Your library is already there.", m.to);
    if (to.string().rfind(from.string() + "/", 0) == 0)
        return fail("That folder is inside your library.", "Pick a folder outside " + m.from + ".");
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
        if (!kStay.count(it->path().filename().string())) items.push_back(it->path());
    if (ec && fs::exists(from)) return fail("Your library can't be read.", m.from + ": " + ec.message());
    bool made_to = !fs::exists(to, ec);
    fs::create_directories(to, ec);
    if (ec) return fail("That folder can't be used.", m.to + ": " + ec.message());
    if (std::string why; !runs_programs(to, why)) {
        std::error_code e;
        if (made_to) fs::remove_all(to, e);
        return fail("Games can't run from that drive.", m.to + ": " + why + ". Pick a folder on a Linux-formatted drive (ext4, btrfs, xfs…).");
    }
    auto undo_dir = [&] {
        std::error_code e;
        if (made_to) fs::remove_all(to, e);
        else
            for (auto &d : fs::directory_iterator(to, e))
                if (!kStay.count(d.path().filename().string())) fs::remove_all(d.path(), e);
    };
    struct stat a {}, b {};
    bool same_drive = stat(from.c_str(), &a) == 0 && stat(to.c_str(), &b) == 0 && a.st_dev == b.st_dev;
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
        if (!write_location(m.to, err)) {
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
        rewrite_builds(to / "library.ini", m.from, m.to);
        if (!write_location(m.to, err)) {
            undo_dir();
            return fail("The new place could not be recorded.", err);
        }
        for (auto &p : items) fs::remove_all(p, ec);   // only now: the copy is complete and recorded
    }
    // the old folder goes too when nothing is left in it and it is not the default place (which keeps tools)
    if (fs::weakly_canonical(default_launcher_home(), ec) != from && is_empty_dir(from)) fs::remove(from, ec);
    m.ok = true;
    m.finished.store(true);
}
