// Country-Box launcher: the per-game manifests (games/<game>/manifest.toml, written by launcher/make_manifest.py and
// embedded in the binary) and the check of a player's extracted copy against them. The launcher never decrypts or
// unpacks anything: a copy is a folder holding exefs/ and romfs/; Country-Box never decrypts.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

struct ManifestFile {
    std::string path;     // "exefs/main", "romfs/..."
    uint64_t size = 0;
    std::string sha256;   // hex
};

struct Manifest {
    std::string game;          // "dkcrhd", "dkctf"
    std::string name;          // display name (text only, never a logo)
    std::string title_id;      // 16 hex digits, upper case
    std::string version;       // "1.0.0"
    std::string version_key;   // build_linux.py's key ("v0")
    uint64_t bytes = 0;
    std::vector<ManifestFile> files;
    std::string id;            // SHA-256 of the manifest text: a copy checked against another manifest is checked again
};

// The embedded manifest of a game, NULL when unknown or unreadable.
const Manifest *manifest_get(const std::string &game);
// Every game the launcher knows, in display order.
const std::vector<std::string> &manifest_games();
// Parses the TOML subset make_manifest.py writes (top-level keys, [[file]] tables, strings and integers).
bool manifest_parse(const std::string &text, Manifest &m, std::string *err);

// The title id in <exefs>/main.npdm (ACI0 program id), "" when the file is missing or not an NPDM.
std::string npdm_title_id(const std::string &exefs_dir);

struct VerifyResult {
    enum Kind {
        Ok,
        NotFound,        // the path is not a folder
        Packed,          // .nsp/.xci/.nca files instead of the extracted folders
        NotExtracted,    // no exefs/ + romfs/
        OtherGame,       // a supported game, but the other one (other_game)
        UnknownGame,     // a title id we do not support
        WrongVersion,    // right game, other program files: an update applied, or another version
        MissingFiles,
        DamagedFiles,    // size or hash differs
        ReadError,
        Cancelled,
    } kind = Ok;
    std::string root;           // the folder holding exefs/ and romfs/ that was checked
    std::string note;           // how root was found when it differs from the chosen path
    std::string title_id;       // found in main.npdm
    std::string other_game;     // OtherGame: its id
    std::string headline;       // one line, plain words
    std::string explain;        // what is wrong and why it matters
    std::string hint;           // what to do
    std::vector<std::string> details;   // file names (missing, damaged), at most 12 plus a count line
    int missing = 0, damaged = 0, extra = 0;
    uint64_t bytes = 0;         // hashed
    double seconds = 0;
};

// One check, on its own threads. Poll done(); read progress from phase/done_bytes/total_bytes.
class VerifyJob {
public:
    enum Phase { Starting, Listing, Hashing, Finished };
    VerifyJob(std::string game, std::string path, bool deep = true);
    ~VerifyJob();
    void cancel() { __atomic_store_n(&cancel_flag_, 1, __ATOMIC_RELAXED); }
    bool done() const { return phase.load() == Finished; }
    const VerifyResult &result() const { return result_; }   // valid once done()
    const std::string &game() const { return game_; }
    const std::string &path() const { return path_; }

    std::atomic<int> phase{Starting};
    volatile int64_t done_bytes = 0;   // read with __atomic_load_n
    std::atomic<int64_t> total_bytes{0};
    std::atomic<int> files_done{0}, files_total{0};

private:
    void run();
    std::string game_, path_;
    bool deep_;
    volatile int cancel_flag_ = 0;
    VerifyResult result_;
    std::thread thread_;
};

// The same check without threads in the caller's way (the command line): blocks until done.
VerifyResult verify_copy(const std::string &game, const std::string &path, bool deep = true);
// The result as JSON (the command line's answer).
std::string verify_json(const std::string &game, const VerifyResult &r);
