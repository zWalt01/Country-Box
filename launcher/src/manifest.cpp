// Country-Box launcher: manifests and the copy check (manifest.h).
#include "manifest.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include "json.h"
#include "sha256.h"

namespace fs = std::filesystem;

// ---- embedded manifests (games/<game>/manifest.toml)

static const char kManifestDkcr[] = {
#embed "../../games/dkcrhd/manifest.toml"
    , 0};
static const char kManifestDkctf[] = {
#embed "../../games/dkctf/manifest.toml"
    , 0};

const std::vector<std::string> &manifest_games() {
    static const std::vector<std::string> g = {"dkcrhd", "dkctf"};
    return g;
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool manifest_parse(const std::string &text, Manifest &m, std::string *err) {
    m = Manifest();
    ManifestFile *cur = nullptr;
    size_t pos = 0;
    int lineno = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() : nl + 1;
        lineno++;
        if (line.empty() || line[0] == '#') continue;
        if (line == "[[file]]") {
            m.files.emplace_back();
            cur = &m.files.back();
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            if (err) *err = "line " + std::to_string(lineno) + ": expected key = value";
            return false;
        }
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        std::string sv;
        uint64_t nv = 0;
        if (!v.empty() && v[0] == '"') {
            Json j;
            if (!json_parse(v, j) || j.type != Json::String) {
                if (err) *err = "line " + std::to_string(lineno) + ": bad string";
                return false;
            }
            sv = j.str;
        } else nv = strtoull(v.c_str(), nullptr, 10);
        if (cur) {
            if (k == "path") cur->path = sv;
            else if (k == "size") cur->size = nv;
            else if (k == "sha256") cur->sha256 = sv;
        } else if (k == "game") m.game = sv;
        else if (k == "name") m.name = sv;
        else if (k == "title_id") m.title_id = sv;
        else if (k == "version") m.version = sv;
        else if (k == "version_key") m.version_key = sv;
        else if (k == "bytes") m.bytes = nv;
    }
    for (auto &f : m.files)
        if (f.path.empty() || f.sha256.size() != 64) {
            if (err) *err = "a [[file]] without path or sha256";
            return false;
        }
    if (m.game.empty() || m.title_id.size() != 16 || m.files.empty()) {
        if (err) *err = "missing game, title_id or files";
        return false;
    }
    Sha256 *s = sha256_new();
    uint8_t d[32];
    sha256_update(s, text.data(), text.size());
    sha256_final(s, d);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
    m.id = hex;
    return true;
}

const Manifest *manifest_get(const std::string &game) {
    static std::once_flag once;
    static std::map<std::string, Manifest> all;
    std::call_once(once, [] {
        for (const char *t : {kManifestDkcr, kManifestDkctf}) {
            Manifest m;
            std::string err;
            if (manifest_parse(t, m, &err)) all[m.game] = std::move(m);
            else fprintf(stderr, "launcher: an embedded manifest does not parse: %s\n", err.c_str());
        }
    });
    auto it = all.find(game);
    return it == all.end() ? nullptr : &it->second;
}

std::string npdm_title_id(const std::string &exefs_dir) {
    std::ifstream f(exefs_dir + "/main.npdm", std::ios::binary);
    if (!f) return "";
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 0x80 || b.compare(0, 4, "META")) return "";
    uint32_t aci;
    memcpy(&aci, b.data() + 0x70, 4);
    if ((size_t)aci + 0x18 > b.size() || b.compare(aci, 4, "ACI0")) return "";
    uint64_t id;
    memcpy(&id, b.data() + aci + 0x10, 8);
    char out[17];
    snprintf(out, sizeof out, "%016llX", (unsigned long long)id);
    return out;
}

// ---- finding the copy's root

static bool is_dir(const fs::path &p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

static bool has_both(const fs::path &p) { return is_dir(p / "exefs") && is_dir(p / "romfs"); }

static bool packed_file(const fs::path &p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e == ".nsp" || e == ".xci" || e == ".nca" || e == ".nsz" || e == ".xcz";
}

// Fills r.root (or r.kind with the reason there is none).
static bool find_root(const std::string &path, VerifyResult &r) {
    fs::path p(path);
    if (!is_dir(p)) {
        r.kind = VerifyResult::NotFound;
        return false;
    }
    if (has_both(p)) return r.root = p.string(), true;
    std::string base = p.filename().string();
    if ((base == "exefs" || base == "romfs") && has_both(p.parent_path())) {
        r.root = p.parent_path().string();
        r.note = "You chose the " + base + " folder; the folder above it holds both exefs and romfs, so that one was checked.";
        return true;
    }
    // One or two levels down (a folder holding the extracted game's folder).
    std::vector<fs::path> found;
    bool packed = false;
    std::error_code ec;
    auto scan = [&](const fs::path &dir, bool deeper) {
        std::vector<fs::path> subs;
        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2) && packed_file(it->path())) packed = true;
            else if (it->is_directory(e2)) (has_both(it->path()) ? found : subs).push_back(it->path());
        }
        ec.clear();
        return deeper ? subs : std::vector<fs::path>();
    };
    for (auto &sub : scan(p, true)) scan(sub, false);
    if (found.size() == 1) {
        r.root = found[0].string();
        r.note = "The game folders were found inside " + fs::relative(found[0], p, ec).string() + ".";
        return true;
    }
    r.kind = packed ? VerifyResult::Packed : VerifyResult::NotExtracted;
    if (!packed) {
        if (is_dir(p / "exefs")) r.details.push_back("exefs is there, but romfs is missing.");
        else if (is_dir(p / "romfs")) r.details.push_back("romfs is there, but exefs is missing.");
        else if (found.size() > 1) r.details.push_back("More than one game was found inside; choose one of them.");
    }
    return false;
}

// ---- messages

static std::string plural(int n, const char *one, const char *many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

static void words(const std::string &game, VerifyResult &r) {
    const Manifest *m = manifest_get(game);
    std::string name = m ? m->name : game, version = m ? m->version : "?";
    const Manifest *other = r.other_game.empty() ? nullptr : manifest_get(r.other_game);
    switch (r.kind) {
    case VerifyResult::Ok:
        r.headline = "Your copy of " + name + " is complete and correct.";
        r.explain = "Version " + version + ", every file checked (" + plural((int)(m ? m->files.size() : 0), "file", "files") + ").";
        break;
    case VerifyResult::NotFound:
        r.headline = "That folder can't be found.";
        r.explain = "It may be on a drive or SD card that isn't connected right now.";
        r.hint = "Connect the drive, or choose the folder again.";
        break;
    case VerifyResult::Packed:
        r.headline = "This folder holds a packed game file, not the extracted game.";
        r.explain = "Country-Box works with the extracted folders exefs and romfs. It never unpacks or decrypts game files.";
        r.hint = "Extract your own copy of the game so that one folder holds exefs and romfs, then choose that folder.";
        break;
    case VerifyResult::NotExtracted:
        r.headline = "This folder doesn't contain an extracted game.";
        r.explain = "An extracted copy is one folder with two folders inside it: exefs and romfs.";
        r.hint = "Choose the folder that holds both exefs and romfs.";
        break;
    case VerifyResult::OtherGame:
        r.headline = "This is " + (other ? other->name : r.other_game) + ", not " + name + ".";
        r.explain = "The copy is fine to use, it just belongs to the other game.";
        r.hint = "Add it to " + (other ? other->name : r.other_game) + " instead.";
        break;
    case VerifyResult::UnknownGame:
        r.headline = r.title_id.empty() ? "This copy has no readable program header (exefs/main.npdm)."
                                        : "This is not a game Country-Box supports.";
        r.explain = r.title_id.empty() ? "The file exefs/main.npdm is missing or damaged, so the game can't be identified."
                                       : "Its title id is " + r.title_id + ". Country-Box supports Donkey Kong Country Returns HD "
                                         "and Donkey Kong Country: Tropical Freeze.";
        r.hint = r.title_id.empty() ? "Extract the game again." : "Choose the folder of one of the supported games.";
        break;
    case VerifyResult::WrongVersion:
        r.headline = "This is a different version of " + name + ".";
        r.explain = "Its program files don't match version " + version +
                    ", the one Country-Box is made for. Usually that means an update was applied when the game was extracted.";
        r.hint = "Extract the base game alone (version " + version + ", no update) and choose that folder.";
        break;
    case VerifyResult::MissingFiles:
        r.headline = plural(r.missing, "file is", "files are") + " missing from your copy.";
        r.explain = "Without them the game can't start, or would stop later.";
        r.hint = "Extract the game again, or copy the whole folder again if you moved it.";
        break;
    case VerifyResult::DamagedFiles:
        r.headline = plural(r.damaged, "file in your copy is", "files in your copy are") + " damaged.";
        r.explain = "They differ from the original game files: a copy that stopped halfway, or a failing disk or SD card.";
        r.hint = "Extract the game again, or copy the whole folder again if you moved it.";
        break;
    case VerifyResult::ReadError:
        r.headline = "Some files could not be read.";
        r.explain = "The disk or SD card returned an error, or the files are not readable by your user.";
        r.hint = "Check the drive and try again.";
        break;
    case VerifyResult::Cancelled:
        r.headline = "The check was stopped.";
        r.hint = "Check the copy again when you are ready.";
        break;
    }
}

static void add_detail(VerifyResult &r, const std::string &line, int count) {
    if (r.details.size() < 12) r.details.push_back(line);
    else if (count == 13) r.details.push_back("…");
}

// ---- the check

VerifyJob::VerifyJob(std::string game, std::string path, bool deep) : game_(std::move(game)), path_(std::move(path)), deep_(deep) {
    thread_ = std::thread([this] { run(); });
}

VerifyJob::~VerifyJob() {
    cancel();
    if (thread_.joinable()) thread_.join();
}

void VerifyJob::run() {
    auto t0 = std::chrono::steady_clock::now();
    VerifyResult &r = result_;
    const Manifest *m = manifest_get(game_);
    auto finish = [&] {
        words(game_, r);
        r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        phase.store(Finished);
    };
    phase.store(Listing);
    if (!m || !find_root(path_, r)) return finish();
    fs::path root(r.root);
    r.title_id = npdm_title_id((root / "exefs").string());
    if (r.title_id != m->title_id) {
        for (auto &g : manifest_games()) {
            const Manifest *o = manifest_get(g);
            if (o && g != game_ && o->title_id == r.title_id) r.other_game = g;
        }
        r.kind = r.other_game.empty() ? VerifyResult::UnknownGame : VerifyResult::OtherGame;
        return finish();
    }
    // Sizes first: missing and short files are reported without reading anything.
    std::set<std::string> listed;
    std::vector<const ManifestFile *> todo;
    bool exefs_differs = false;
    for (auto &f : m->files) {
        listed.insert(f.path);
        std::error_code ec;
        uint64_t sz = fs::file_size(root / f.path, ec);
        bool exe = f.path.rfind("exefs/", 0) == 0;
        if (ec) {
            if (exe) exefs_differs = true;
            r.missing++;
            add_detail(r, f.path, r.missing);
        } else if (sz != f.size) {
            if (exe) exefs_differs = true;
            r.damaged++;
            add_detail(r, f.path + " (size differs)", r.damaged);
        } else todo.push_back(&f);
    }
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root / "romfs", ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && !listed.count(fs::relative(it->path(), root, ec).string())) r.extra++;
    // The program files decide the version: hash them before anything else.
    std::vector<const ManifestFile *> exefs, romfs;
    for (auto *f : todo) (f->path.rfind("exefs/", 0) == 0 ? exefs : romfs).push_back(f);
    for (auto *f : exefs) {
        char hex[65];
        if (sha256_file((root / f->path).c_str(), hex, nullptr, &cancel_flag_)) {
            r.kind = cancel_flag_ ? VerifyResult::Cancelled : VerifyResult::ReadError;
            return finish();
        }
        if (f->sha256 != hex) exefs_differs = true;
    }
    if (exefs_differs) {
        r.details.clear();
        r.kind = VerifyResult::WrongVersion;
        return finish();
    }
    if (r.missing) {
        r.kind = VerifyResult::MissingFiles;
        return finish();
    }
    if (r.damaged) {
        r.kind = VerifyResult::DamagedFiles;
        return finish();
    }
    if (!deep_) return finish();
    // Every RomFS file, the biggest first, on a few threads (a disk or an SD card does not gain from more).
    std::sort(romfs.begin(), romfs.end(), [](auto *a, auto *b) { return a->size > b->size; });
    int64_t total = 0;
    for (auto *f : romfs) total += (int64_t)f->size;
    total_bytes.store(total);
    files_total.store((int)romfs.size());
    phase.store(Hashing);
    std::atomic<size_t> next{0};
    std::mutex lock;
    int read_errors = 0;
    unsigned n = std::max(1u, std::min(4u, std::thread::hardware_concurrency() / 2));
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < n; i++)
        workers.emplace_back([&] {
            for (size_t k; (k = next.fetch_add(1)) < romfs.size();) {
                if (cancel_flag_) return;
                const ManifestFile *f = romfs[k];
                char hex[65];
                int rc = sha256_file((root / f->path).c_str(), hex, &done_bytes, &cancel_flag_);
                std::lock_guard<std::mutex> g(lock);
                if (rc && !cancel_flag_) read_errors++, add_detail(r, f->path + " (can't be read)", read_errors + r.damaged);
                else if (!rc && f->sha256 != hex) r.damaged++, add_detail(r, f->path, read_errors + r.damaged);
                files_done.fetch_add(1);
            }
        });
    for (auto &w : workers) w.join();
    r.bytes = (uint64_t)__atomic_load_n(&done_bytes, __ATOMIC_RELAXED);
    if (cancel_flag_) r.kind = VerifyResult::Cancelled;
    else if (r.damaged) r.kind = VerifyResult::DamagedFiles;
    else if (read_errors) r.kind = VerifyResult::ReadError;
    finish();
}

VerifyResult verify_copy(const std::string &game, const std::string &path, bool deep) {
    VerifyJob j(game, path, deep);
    while (!j.done()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return j.result();
}

static const char *kind_name(VerifyResult::Kind k) {
    static const char *n[] = {"ok", "not_found", "packed", "not_extracted", "other_game", "unknown_game",
                              "wrong_version", "missing_files", "damaged_files", "read_error", "cancelled"};
    return n[k];
}

std::string verify_json(const std::string &game, const VerifyResult &r) {
    std::string o = "{\"game\": " + json_quote(game) + ", \"ok\": " + (r.kind == VerifyResult::Ok ? "true" : "false") +
                    ", \"result\": " + json_quote(kind_name(r.kind)) + ", \"root\": " + json_quote(r.root) +
                    ", \"title_id\": " + json_quote(r.title_id) + ", \"other_game\": " + json_quote(r.other_game) +
                    ", \"missing\": " + std::to_string(r.missing) + ", \"damaged\": " + std::to_string(r.damaged) +
                    ", \"extra\": " + std::to_string(r.extra) + ", \"bytes_hashed\": " + std::to_string(r.bytes) +
                    ", \"seconds\": " + std::to_string(r.seconds) + ",\n \"headline\": " + json_quote(r.headline) +
                    ",\n \"explain\": " + json_quote(r.explain) + ",\n \"hint\": " + json_quote(r.hint) +
                    ",\n \"note\": " + json_quote(r.note) + ",\n \"details\": [";
    for (size_t i = 0; i < r.details.size(); i++) o += (i ? ", " : "") + json_quote(r.details[i]);
    return o + "]}";
}
