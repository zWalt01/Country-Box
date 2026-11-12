// Country-Box launcher: self-update from the project's GitHub Releases (the update channel). New code only: nothing
// of the games, the builds or the library is touched here — see update.cpp's install layout.
//
// The channel is the Releases API of one repository (default in update_api_url(), overridable for tests with
// CB_UPDATE_API or <home>/update.ini's api_url). A release must publish a SHA-256 for the package: either the asset's
// own "digest" field ("sha256:<hex>", what GitHub writes when the upload is checksummed) or a SHA256SUMS asset with a
// "<hex>  <name>" line for it. No hash, no install. There is no signature yet: SHA-256 over HTTPS is the whole
// guarantee today, and a signing scheme is planned (LAUNCHER.md, "Actualizaciones").
//
//   UpdateChecker  one GET of releases/latest on its own thread (the hub opens without waiting for the network)
//   update_check   the same, blocking (the command line)
//   UpdateJob      download to the cache with progress, verify the hash, back the saves up, install atomically
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct UpdateInfo {
    bool available = false;
    std::string current;        // this launcher's version
    std::string version;        // the release's tag, "v" stripped
    std::string title;          // the release's name
    std::string notes;          // the release's body ("what changed")
    std::string asset_name;
    std::string asset_url;
    int64_t asset_size = 0;
    std::string sha256;         // hex, lowercase
    std::string sha256_from;    // "release digest" | "SHA256SUMS"
    std::string err;            // check failed: plain words for the UI ("" when it didn't)
};

// One background check. Poll done(); then read info(). Destructing joins the thread.
class UpdateChecker {
public:
    explicit UpdateChecker(const std::string &home);
    ~UpdateChecker();
    bool done() const { return done_.load(); }
    const UpdateInfo &info() const { return info_; }

private:
    void run();
    std::string home_;
    std::atomic<bool> done_{false};
    UpdateInfo info_;
    std::thread thread_;
};

// The whole update on its own thread: download (progress in done_bytes/total_bytes), verify the hash, back the saves
// up, install. game_builds is each game with a build (id, build dir): the saves inside are what gets backed up, and
// the ones with a built game are what need preparing afterwards. Poll done(); then read the result fields. Cancel
// stops the download and leaves everything as it was.
class UpdateJob {
public:
    enum Phase { Download, Verify, Backup, Install, Finished };
    UpdateJob(UpdateInfo info, std::string home, std::vector<std::pair<std::string, std::string>> game_builds = {});
    ~UpdateJob();
    void cancel() { __atomic_store_n(&cancel_flag_, 1, __ATOMIC_RELAXED); }
    bool done() const { return phase.load() == Finished; }

    std::atomic<int> phase{Download};
    std::atomic<int64_t> done_bytes{0}, total_bytes{0};

    bool ok = false;                     // valid once done()
    bool cancelled = false;
    std::string err;                     // why it failed, plain words
    std::string from_version, to_version;
    std::string installed_dir;           // <home>/app/<version> (the old version's directory stays for rollback)
    std::string backup_dir;              // where the saves were backed up to
    std::vector<std::string> saved;      // what went into backup_dir, one line per game
    std::vector<std::string> need_prepare;   // game ids whose build is now older than this Country-Box

private:
    void run();
    UpdateInfo info_;
    std::string home_;
    std::vector<std::pair<std::string, std::string>> game_builds_;
    volatile int cancel_flag_ = 0;
    std::thread thread_;
};

// The check, blocking (the command line's --check-update). err (of info) is set on failure.
UpdateInfo update_check(const std::string &home, std::string *err = nullptr);
// The check as the command line's JSON answer.
std::string update_check_json(const UpdateInfo &i);
// A finished UpdateJob as the command line's JSON answer (--update).
std::string update_result_json(const UpdateJob &j);

// Which API URL is used: CB_UPDATE_API, else <home>/update.ini's api_url, else the project's repository.
std::string update_api_url(const std::string &home);
// <home>/update.ini: "key = value" lines; update_ini_set rewrites one key keeping the rest.
std::string update_ini_value(const std::string &home, const std::string &key);
bool update_ini_set(const std::string &home, const std::string &key, const std::string &value);
// "<home>/<leaf>" (app, cache, backups), made if missing; "" with err on failure.
std::string update_ensure_dir(const std::string &home, const char *leaf, std::string *err);
