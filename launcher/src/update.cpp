// Country-Box launcher: the update channel (update.h). HTTP is libcurl loaded with dlopen — the same pattern as
// sha256.c's libcrypto: no new link-time dependency (SDL3 is all the launcher links). The CURLOPT numbers below are
// libcurl's stable ABI values, verified against curl/curl.h (they have not changed since 7.x).
//
// Install layout, all under the library home:
//
//   app/<version>/   one directory per Country-Box version. Today it holds country-box-launcher; Prepare's payload
//                    (prep/) travels here too when it ships — prep/ next to the binary is Prepare's contract
//   app/current      symlink to the version that runs; the swap is one rename(2), so a launcher is never half
//                    installed, and the previous version's directory stays for a rollback
//   backups/<id>/    the saves (each game's profile/ and mp4.ini) as they were just before an update was installed
//   cache/           the downloaded package, moved into place by the install
//
// The games' builds (<home>/games/<game>/ or wherever the library points), the checked copies and library.ini are
// never touched by an update; the saves are additionally copied to backups/ before anything is installed.
#include "update.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "json.h"
#include "library.h"
#include "sha256.h"

namespace fs = std::filesystem;

typedef void CURL;
typedef int CURLcode;

// Verified against /usr/include/curl/curl.h.
enum {
    kOptWriteData = 10001,
    kOptUrl = 10002,
    kOptErrorBuffer = 10010,
    kOptUserAgent = 10018,
    kOptSslVerifyPeer = 64,
    kOptMaxRedirs = 68,
    kOptFollowLocation = 52,
    kOptNoProgress = 43,
    kOptFailOnError = 45,
    kOptLowSpeedLimit = 19,
    kOptLowSpeedTime = 20,
    kOptXferInfoData = 10057,
    kOptWriteFunction = 20011,
    kOptXferInfoFunction = 20219,
    kOptTimeoutMs = 155,
    kOptConnectTimeoutMs = 156,
    kCurlGlobalDefault = 3,
};

struct Curl {
    void *so = nullptr;
    CURLcode (*global_init)(long) = nullptr;
    CURL *(*easy_init)(void) = nullptr;
    CURLcode (*easy_setopt)(CURL *, int, ...) = nullptr;
    CURLcode (*easy_perform)(CURL *) = nullptr;
    void (*easy_cleanup)(CURL *) = nullptr;
    const char *(*easy_strerror)(CURLcode) = nullptr;
    bool ok = false;
    std::string err;
};

static Curl &curl() {
    static Curl c = [] {
        Curl c;
        for (const char *name : {"libcurl.so.4", "libcurl.so"}) {
            c.so = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (c.so) break;
        }
        if (!c.so) {
            c.err = "libcurl could not be loaded (is curl installed?)";
            return c;
        }
        c.global_init = (CURLcode(*)(long))dlsym(c.so, "curl_global_init");
        c.easy_init = (CURL * (*)(void)) dlsym(c.so, "curl_easy_init");
        c.easy_setopt = (CURLcode(*)(CURL *, int, ...))dlsym(c.so, "curl_easy_setopt");
        c.easy_perform = (CURLcode(*)(CURL *))dlsym(c.so, "curl_easy_perform");
        c.easy_cleanup = (void (*)(CURL *))dlsym(c.so, "curl_easy_cleanup");
        c.easy_strerror = (const char *(*)(CURLcode))dlsym(c.so, "curl_easy_strerror");
        if (!c.global_init || !c.easy_init || !c.easy_setopt || !c.easy_perform || !c.easy_cleanup || !c.easy_strerror)
            c.err = "libcurl is missing a function this build needs";
        else {
            c.global_init(kCurlGlobalDefault);
            c.ok = true;
        }
        return c;
    }();
    return c;
}

struct HttpErr {
    bool ok = false;
    std::string err;   // plain words, for the UI
};

// ---- GET into a string --------------------------------------------------------------------------------

static size_t append_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    ((std::string *)ud)->append(ptr, size * nmemb);
    return size * nmemb;
}

static HttpErr http_get_string(const std::string &url, std::string &out) {
    Curl &c = curl();
    if (!c.ok) return {false, c.err};
    CURL *h = c.easy_init();
    if (!h) return {false, "libcurl could not start a request"};
    char errbuf[256] = {0};
    c.easy_setopt(h, kOptUrl, url.c_str());
    c.easy_setopt(h, kOptWriteData, &out);
    c.easy_setopt(h, kOptWriteFunction, (void *)&append_cb);
    c.easy_setopt(h, kOptUserAgent, ("country-box-launcher/" + launcher_version()).c_str());
    c.easy_setopt(h, kOptFollowLocation, 1L);
    c.easy_setopt(h, kOptMaxRedirs, 5L);
    c.easy_setopt(h, kOptFailOnError, 1L);
    c.easy_setopt(h, kOptSslVerifyPeer, 1L);
    c.easy_setopt(h, kOptErrorBuffer, errbuf);
    c.easy_setopt(h, kOptConnectTimeoutMs, 10000L);
    c.easy_setopt(h, kOptTimeoutMs, 20000L);
    CURLcode rc = c.easy_perform(h);
    c.easy_cleanup(h);
    if (rc != 0) return {false, errbuf[0] ? errbuf : "the update server could not be reached"};
    return {true, ""};
}

// ---- GET into a file, hashing and reporting bytes as they arrive ---------------------------------------------------

struct DownloadCtx {
    FILE *f = nullptr;
    Sha256 *hash = nullptr;
    std::atomic<int64_t> *done = nullptr;
    volatile int *cancel = nullptr;
};

static size_t download_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    DownloadCtx *x = (DownloadCtx *)ud;
    size_t n = size * nmemb;
    if (x->cancel && __atomic_load_n(x->cancel, __ATOMIC_RELAXED)) return 0;   // aborts: CURLE_WRITE_ERROR
    if (fwrite(ptr, 1, n, x->f) != n) return 0;
    if (x->hash) sha256_update(x->hash, ptr, n);
    if (x->done) x->done->fetch_add((int64_t)n, std::memory_order_relaxed);
    return n;
}

static int progress_cb(void *ud, long long, long long, long long, long long) {
    DownloadCtx *x = (DownloadCtx *)ud;
    return x->cancel && __atomic_load_n(x->cancel, __ATOMIC_RELAXED) ? 1 : 0;   // 1 aborts: CURLE_ABORTED_BY_CALLBACK
}

static HttpErr http_download(const std::string &url, FILE *sink, Sha256 *hash, std::atomic<int64_t> *done,
                             volatile int *cancel) {
    Curl &c = curl();
    if (!c.ok) return {false, c.err};
    CURL *h = c.easy_init();
    if (!h) return {false, "libcurl could not start a download"};
    DownloadCtx ctx{sink, hash, done, cancel};
    char errbuf[256] = {0};
    c.easy_setopt(h, kOptUrl, url.c_str());
    c.easy_setopt(h, kOptWriteData, &ctx);
    c.easy_setopt(h, kOptWriteFunction, (void *)&download_cb);
    c.easy_setopt(h, kOptXferInfoData, &ctx);
    c.easy_setopt(h, kOptXferInfoFunction, (void *)&progress_cb);
    c.easy_setopt(h, kOptNoProgress, 0L);
    c.easy_setopt(h, kOptUserAgent, ("country-box-launcher/" + launcher_version()).c_str());
    c.easy_setopt(h, kOptFollowLocation, 1L);   // a release asset redirects to the object server
    c.easy_setopt(h, kOptMaxRedirs, 5L);
    c.easy_setopt(h, kOptFailOnError, 1L);
    c.easy_setopt(h, kOptSslVerifyPeer, 1L);
    c.easy_setopt(h, kOptErrorBuffer, errbuf);
    c.easy_setopt(h, kOptConnectTimeoutMs, 10000L);
    c.easy_setopt(h, kOptLowSpeedLimit, 1L);    // a stalled transfer (1 byte/s for 30 s) is interrupted, not hung
    c.easy_setopt(h, kOptLowSpeedTime, 30L);
    CURLcode rc = c.easy_perform(h);
    c.easy_cleanup(h);
    if (rc != 0) {
        if (cancel && __atomic_load_n(cancel, __ATOMIC_RELAXED)) return {false, "cancelled"};
        return {false, std::string("the download did not finish (") + (errbuf[0] ? errbuf : "interrupted") + ")"};
    }
    return {true, ""};
}

// ---- configuration (update.ini) and paths ---------------------------------------------------------------------------

std::string update_ini_value(const std::string &home, const std::string &key) {
    std::ifstream f(home + "/update.ini");
    for (std::string line; std::getline(f, line);) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
            return a == std::string::npos ? "" : s.substr(a, b - a + 1);
        };
        if (trim(line.substr(0, eq)) == key) return trim(line.substr(eq + 1));
    }
    return "";
}

static std::string trim_ws(std::string s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool update_ini_set(const std::string &home, const std::string &key, const std::string &value) {
    std::error_code ec;
    fs::create_directories(home, ec);
    std::vector<std::string> lines;
    bool found = false;
    {
        std::ifstream f(home + "/update.ini");
        for (std::string line; std::getline(f, line);) {
            size_t eq = line.find('=');
            if (eq != std::string::npos && trim_ws(line.substr(0, eq)) == key) {
                lines.push_back(key + " = " + value);
                found = true;
            } else {
                lines.push_back(line);
            }
        }
    }
    if (!found) lines.push_back(key + " = " + value);
    std::string tmp = home + "/update.ini.tmp";
    {
        std::ofstream f(tmp);
        f << "# Country-Box launcher: update settings, written by the launcher.\n";
        for (auto &l : lines)
            if (!l.empty()) f << l << "\n";
        if (!f) return false;
    }
    return ::rename(tmp.c_str(), (home + "/update.ini").c_str()) == 0;
}

std::string update_api_url(const std::string &home) {
    const char *e = getenv("CB_UPDATE_API");
    if (e && *e) return e;
    std::string ini = update_ini_value(home, "api_url");
    if (!ini.empty()) return ini;
    return "https://api.github.com/repos/zWalt01/Country-Box/releases/latest";
}

std::string update_ensure_dir(const std::string &home, const char *leaf, std::string *err) {
    std::error_code ec;
    std::string p = home + "/" + leaf;
    fs::create_directories(p, ec);
    if (!fs::is_directory(p, ec)) {
        if (err) *err = "cannot create " + p;
        return "";
    }
    return p;
}

// ---- the check ------------------------------------------------------------------------------------------------------

static std::string lowercase(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// "<hex>  <name>" of a SHA256SUMS line for name; "" when the file has no line for it.
static std::string sha_from_sums(const std::string &sums, const std::string &name) {
    std::istringstream s(sums);
    for (std::string line; std::getline(s, line);) {
        size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos || sp != 64) continue;
        std::string hex = lowercase(line.substr(0, 64)), rest = line.substr(sp);
        size_t nb = rest.find_first_not_of(" \t");
        if (nb == std::string::npos) continue;
        std::string n = rest.substr(nb);
        if (!n.empty() && n[0] == '*') n = n.substr(1);                 // the binary marker some tools write
        else if (n.compare(0, 2, "./") == 0) n = n.substr(2);
        if (n == name || n == "./" + name) return hex;
    }
    return "";
}

UpdateInfo update_check(const std::string &home, std::string *err_out) {
    UpdateInfo i;
    i.current = launcher_version();
    std::string body, err;
    HttpErr h = http_get_string(update_api_url(home), body);
    if (!h.ok) {
        if (err_out) *err_out = h.err;
        i.err = h.err;
        return i;
    }
    Json j;
    std::string perr;
    if (!json_parse(body, j, &perr) || j["tag_name"].type != Json::String) {
        i.err = "the update server answered something unexpected";
        if (err_out) *err_out = i.err;
        return i;
    }
    std::string tag = j["tag_name"].s();
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag = tag.substr(1);
    i.version = tag;
    i.title = j["name"].s();
    i.notes = j["body"].s();
    // the package asset: the launcher binary (later the whole version tree, prep/ included)
    const Json *asset = nullptr;
    for (size_t k = 0; k < j["assets"].size(); k++) {
        const Json &a = j["assets"][k];
        std::string n = a["name"].s();
        // Run from an AppImage ($APPIMAGE): the package is the release's .AppImage, which replaces that file.
        bool want_appimage = getenv("APPIMAGE") && *getenv("APPIMAGE");
        bool is_appimage = n.size() > 9 && n.compare(n.size() - 9, 9, ".AppImage") == 0;
        if (n == "SHA256SUMS" || (want_appimage ? !is_appimage : n.find("country-box-launcher") == std::string::npos)) continue;
        asset = &a;
        break;
    }
    if (!asset) {
        i.err = "the release has no Country-Box package";
        if (err_out) *err_out = i.err;
        return i;
    }
    i.asset_name = (*asset)["name"].s();
    i.asset_url = (*asset)["browser_download_url"].s();
    if (i.asset_url.empty()) i.asset_url = (*asset)["url"].s();
    i.asset_size = (int64_t)(*asset)["size"].n();
    std::string digest = (*asset)["digest"].s();
    if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64) {
        i.sha256 = lowercase(digest.substr(7));
        i.sha256_from = "release digest";
    } else {
        // a SHA256SUMS asset with a line for the package
        const Json *sums = nullptr;
        for (size_t k = 0; k < j["assets"].size(); k++)
            if (j["assets"][k]["name"].s() == "SHA256SUMS") sums = &j["assets"][k];
        std::string text;
        if (!sums || !http_get_string((*sums)["browser_download_url"].s(), text).ok || sha_from_sums(text, i.asset_name).empty()) {
            i.err = "the release publishes no SHA-256 for the package";
            if (err_out) *err_out = i.err;
            return i;
        }
        i.sha256 = sha_from_sums(text, i.asset_name);
        i.sha256_from = "SHA256SUMS";
    }
    i.available = !i.version.empty() && version_compare(i.version, i.current) > 0;
    return i;
}

std::string update_check_json(const UpdateInfo &i) {
    std::string o = "{\"ok\": " + std::string(i.err.empty() ? "true" : "false");
    if (!i.err.empty()) o += ", \"error\": " + json_quote(i.err);
    o += ", \"current\": " + json_quote(i.current) + ", \"latest\": " + json_quote(i.version) +
         ", \"available\": " + (i.available ? "true" : "false");
    if (!i.title.empty()) o += ", \"title\": " + json_quote(i.title);
    if (!i.notes.empty()) o += ", \"notes\": " + json_quote(i.notes);
    if (!i.asset_name.empty())
        o += ", \"asset\": {\"name\": " + json_quote(i.asset_name) + ", \"url\": " + json_quote(i.asset_url) +
             ", \"size\": " + std::to_string(i.asset_size) + ", \"sha256\": " + json_quote(i.sha256) +
             ", \"sha256_from\": " + json_quote(i.sha256_from) + "}";
    return o + "}";
}

UpdateChecker::UpdateChecker(const std::string &home) : home_(home) { thread_ = std::thread([this] { run(); }); }
UpdateChecker::~UpdateChecker() {
    if (thread_.joinable()) thread_.join();
}
void UpdateChecker::run() {
    std::string err;
    info_ = update_check(home_, &err);
    done_.store(true);
}

// ---- the update: download, verify, back up, install -----------------------------------------------------------------

static std::string now_stamp(const char *fmt) {
    char t[32];
    time_t now = time(nullptr);
    strftime(t, sizeof t, fmt, localtime(&now));
    return t;
}

static void fsync_path(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

// Copies one save item (the profile/ tree or mp4.ini) of a game into the backup; returns what it copied, "" nothing.
static std::string backup_item(const std::string &from, const std::string &to, int *files) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return "";
    fs::create_directories(fs::path(to).parent_path(), ec);
    if (fs::is_directory(from, ec)) {
        fs::copy(from, to, fs::copy_options::recursive, ec);
        if (ec) throw std::string("the saves could not be backed up: " + ec.message());
        for (auto it = fs::recursive_directory_iterator(to, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec)) (*files)++;
        return fs::path(from).filename().string() + "/";
    }
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) throw std::string("the settings could not be backed up: " + ec.message());
    (*files)++;
    return fs::path(from).filename().string();
}

void UpdateJob::run() {
    auto finish = [&]() { phase.store(Finished); };
    auto fail = [&](const std::string &m) {
        if (cancel_flag_) cancelled = true;
        err = m;
        finish();
    };
    auto finish_ok = [&] {
        ok = true;
        finish();
    };
    std::string err;
    std::error_code ec;

    // 1. Download to the cache, hashing as it goes.
    std::string cache = update_ensure_dir(home_, "cache", &err);
    if (cache.empty()) return fail(err);
    total_bytes.store(info_.asset_size);
    std::string part = cache + "/" + info_.asset_name + ".part";
    ::remove(part.c_str());
    FILE *f = fopen(part.c_str(), "wb");
    if (!f) return fail("the download could not be started (cannot write " + part + ")");
    Sha256 *hash = sha256_new();
    HttpErr h = http_download(info_.asset_url, f, hash, &done_bytes, &cancel_flag_);
    fflush(f);
    fclose(f);
    uint8_t digest[32];
    sha256_final(hash, digest);
    if (!h.ok) {
        ::remove(part.c_str());
        return fail(h.err);
    }
    char hex[65];
    for (int k = 0; k < 32; k++) snprintf(hex + k * 2, 3, "%02x", digest[k]);
    phase.store(Verify);

    // 2. Verify: the published SHA-256 is the only authority on what gets installed (no signature yet: planned).
    if (std::string(hex) != lowercase(info_.sha256)) {
        ::remove(part.c_str());
        return fail("the downloaded package does not match its published SHA-256; nothing was installed");
    }

    // 3. Back the saves up (they are not touched by the install; this is the extra copy before one).
    phase.store(Backup);
    std::string stamp = now_stamp("%Y%m%d-%H%M%S");
    backup_dir = home_ + "/backups/update-" + stamp;
    try {
        for (auto &gb : game_builds_) {
            int files = 0;
            std::string what;
            std::string d = gb.second;
            if (!d.empty() && d.back() == '/') d.pop_back();
            for (const char *item : {"profile", "mp4.ini"}) {
                std::string one = backup_item(d + "/" + item, backup_dir + "/" + gb.first + "/" + item, &files);
                if (!one.empty()) what += (what.empty() ? "" : ", ") + one;
            }
            if (!what.empty()) saved.push_back(gb.first + ": " + what + " (" + std::to_string(files) + " files)");
        }
    } catch (const std::string &m) {
        return fail(m + "; nothing was installed");
    }

    // 4a. AppImage: the verified download replaces the AppImage the player runs, in one rename in its folder; the
    // previous one stays next to it as <name>.old (the rollback).
    phase.store(Install);
    if (const char *ai = getenv("APPIMAGE"); ai && *ai) {
        std::string target = ai, fresh = target + ".new", old_copy = target + ".old";
        from_version = launcher_version();
        fs::remove(fresh, ec);
        fs::copy_file(part, fresh, fs::copy_options::overwrite_existing, ec);
        if (ec) return fail("the new AppImage could not be written next to " + target + ": " + ec.message());
        fs::permissions(fresh, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                   fs::perms::others_read | fs::perms::others_exec, ec);
        fsync_path(fresh);
        fs::remove(old_copy, ec);
        if (::link(target.c_str(), old_copy.c_str()) != 0) fs::copy_file(target, old_copy, ec);
        if (::rename(fresh.c_str(), target.c_str()) != 0) {
            fs::remove(fresh, ec);
            return fail("the new AppImage could not replace " + target);
        }
        ::remove(part.c_str());
        installed_dir = target;
        to_version = info_.version;
        for (auto &gb : game_builds_)
            if (access((gb.second + "/country-box").c_str(), X_OK) == 0) need_prepare.push_back(gb.first);
        return finish_ok();
    }

    // 4. Install: <home>/app/<version>, then swap the app/current symlink in one rename.
    std::string app = update_ensure_dir(home_, "app", &err);
    if (app.empty()) return fail(err);
    {
        char link[4096];
        ssize_t n = ::readlink((app + "/current").c_str(), link, sizeof link - 1);
        from_version = n > 0 ? std::string(link, (size_t)n) : launcher_version();
    }
    std::string staging = app + "/.staging-" + std::to_string(getpid());
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    fs::rename(part, staging + "/country-box-launcher", ec);
    if (ec) {
        fs::remove_all(staging, ec);
        return fail("the new version could not be prepared: " + ec.message());
    }
    fs::permissions(staging + "/country-box-launcher",
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_exec |
                        fs::perms::group_read | fs::perms::others_exec | fs::perms::others_read,
                    ec);
    fsync_path(staging + "/country-box-launcher");
    installed_dir = app + "/" + info_.version;
    std::string trash = app + "/.trash-" + stamp;
    bool had_target = fs::exists(installed_dir, ec);
    if (had_target) fs::rename(installed_dir, trash, ec);   // same version re-installed: the old copy goes, not a rollback
    fs::rename(staging, installed_dir, ec);
    if (ec) {
        if (had_target) fs::rename(trash, installed_dir, ec);
        fs::remove_all(staging, ec);
        installed_dir.clear();
        return fail("the new version could not be moved into place: " + ec.message());
    }
    std::string tmp_link = app + "/.current.tmp";
    ::remove(tmp_link.c_str());
    fs::create_symlink(info_.version, tmp_link, ec);
    if (ec || ::rename(tmp_link.c_str(), (app + "/current").c_str()) != 0) {
        ec = std::make_error_code(std::errc::io_error);
        return fail("the new version is installed at " + installed_dir + " but could not be made current");
    }
    fsync_path(app);
    fs::remove_all(trash, ec);   // same-version reinstall only; the previous version's directory stays
    to_version = info_.version;

    // 5. What the games now need: their builds are older than this Country-Box (they rebuild keeping saves/settings).
    for (auto &gb : game_builds_)
        if (access((gb.second + "/country-box").c_str(), X_OK) == 0) need_prepare.push_back(gb.first);
    finish_ok();
}

UpdateJob::UpdateJob(UpdateInfo info, std::string home, std::vector<std::pair<std::string, std::string>> game_builds)
    : info_(std::move(info)), home_(std::move(home)), game_builds_(std::move(game_builds)) {
    thread_ = std::thread([this] { run(); });
}

UpdateJob::~UpdateJob() {
    if (thread_.joinable()) thread_.join();
}

std::string update_result_json(const UpdateJob &j) {
    std::string o = "{\"ok\": " + std::string(j.ok ? "true" : "false");
    if (!j.err.empty()) o += ", \"error\": " + json_quote(j.err);
    if (j.cancelled) o += ", \"cancelled\": true";
    o += ", \"from\": " + json_quote(j.from_version) + ", \"to\": " + json_quote(j.to_version) +
         ", \"installed\": " + json_quote(j.installed_dir) + ", \"backup\": " + json_quote(j.backup_dir);
    std::string saved = "[", prep = "[";
    for (size_t k = 0; k < j.saved.size(); k++) saved += (k ? ", " : "") + json_quote(j.saved[k]);
    for (size_t k = 0; k < j.need_prepare.size(); k++) prep += (k ? ", " : "") + json_quote(j.need_prepare[k]);
    return o + ", \"saved\": " + saved + "], \"need_prepare\": " + prep + "]}";
}
