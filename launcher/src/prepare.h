// Country-Box launcher: Prepare. Turns the player's checked copy into a ready-to-play build on
// their own machine — lift (the Python lifter from the prep/ payload makes the generated C), compile and link
// (clang/lld/ninja), the runtime files next to the binary, then the shader preparation. Nothing derived from a
// game is ever distributed: the payload carries our own code only.
//
// Progress is readable from any thread while the job runs (atomics + line(), which locks internally), so the hub
// can draw it and the command line can print it. Cancel kills the child's process group; ninja's incremental
// build and the lift's chunk-preserving rewrite make the next Prepare resume where this one stopped.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Game;

// The measured cost of preparing one game (docs/LAUNCHER.md "Prepare"): disk the launcher must find free and
// the stage weights behind the overall percentage. *Measured* on the reference PC, 2026-11-12; marked in the
// docs. gen_bytes: the generated C (thrown away never — kept for incremental updates); build_bytes: objects,
// binary and the files around it.
struct PrepProfile {
    const char *game;
    double gen_gb, build_gb;       // disk this game needs (gen + build)
    double lift_s, compile_s;      // stage weights (shader prep reads its own progress)
};

const PrepProfile *prep_profile(const std::string &game);

// Where the lifter, the runtime sources, the tools, the manifests and the prebuilt translator live: CB_PREP_SOURCES,
// else prep/ next to the launcher binary, else <share>/country-box/prep, else the repository (development).
std::string prep_payload_dir(const std::string &exe);

class PrepareJob {
public:
    // ShadersOnly re-runs just the shader preparation (the card's "Prepare shaders" when only that is left).
    enum Mode { Full, ShadersOnly };
    enum Stage { Starting, Preflight, Lift, Compile, Shaders, Finished };

    struct Outcome {
        bool ok = false;
        std::string kind;       // "", "missing_tools", "disk", "payload", "lift", "compile", "shaders", "cancelled"
        std::string headline;   // one plain-words line
        std::string explain;    // what happened, with numbers
        std::string hint;       // what to do next
        std::string log;        // the log file everything the children printed went to
        double elapsed_s = 0;
    };

    PrepareJob(std::string home, const Game &g, Mode mode);
    ~PrepareJob();
    void cancel() { __atomic_store_n(&cancel_flag_, 1, __ATOMIC_RELAXED); }
    bool done() const { return stage.load() == Finished; }
    bool cancelled() const { return __atomic_load_n(&cancel_flag_, __ATOMIC_RELAXED) != 0; }

    // Progress of the running stage: stage, its own done/total (-1: not counted), seconds in it, ETA (-1: unknown).
    std::atomic<int> stage{Starting};
    std::atomic<int> stage_done{0}, stage_total{-1};
    std::atomic<double> stage_elapsed_s{0}, eta_s{-1};
    std::string line();          // the child's latest meaningful line, for the curious
    const Outcome &outcome() const { return outcome_; }   // valid once done()

    // The whole pipeline synchronously (the command line); the same Outcome.
    static Outcome run_sync(const std::string &home, const Game &g, Mode mode, bool (*progress)(void *) = nullptr,
                            void *ctx = nullptr);

private:
    void run();
    bool preflight(std::string *python);
    bool lift_up_to_date(std::string *why);
    bool write_lift_stamp();
    bool do_lift(const std::string &python);
    bool do_compile(const std::string &python);
    bool place_runtime_files();
    bool do_shaders();
    bool child(const std::vector<std::string> &argv, const std::string &cwd, const char *parse_what);
    void set_line_text(const std::string &text) {
        std::lock_guard<std::mutex> lock(line_mu_);
        line_ = text;
    }

    std::string home_, game_, copy_, manifest_id_, version_key_, build_dir_, gen_dir_, payload_, log_, toolchain_;
    Mode mode_;
    double stage_elapsed_base_ = 0;
    volatile int cancel_flag_ = 0;
    std::mutex line_mu_;
    std::string line_;
    Outcome outcome_;
    std::thread thread_;
};

// "Import a build made on another of my machines": a build folder (or .tar/.tar.gz/.tar.zst
// archive) that Prepare produced elsewhere, from a copy of the same game version. It is verified against this
// machine's copy (the build stamp Prepare wrote there) and installed into <home>/games/<game>/ without touching
// the player's saves (profile/) or settings (mp4.ini). Synchronous: it is a copy, not a build.
struct ImportOutcome {
    bool ok = false;
    std::string kind;       // "", "not_found", "not_build", "wrong_game", "wrong_copy", "extract", "copy"
    std::string headline, explain, hint;
    std::string build;      // where it was installed
    double elapsed_s = 0;
};
ImportOutcome import_build(const std::string &home, const Game &g, const std::string &path,
                           int64_t *bytes_done = nullptr, int64_t *bytes_total = nullptr);
