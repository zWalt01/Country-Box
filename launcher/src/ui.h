// Country-Box launcher: the hub window's screens, drawn with Dear ImGui each frame. Usable with a controller (Steam
// Deck Game Mode: A selects, B goes back, LB/RB switch tabs, left/right change a setting) and with a mouse.
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "json.h"
#include "library.h"
#include "manifest.h"
#include "prepare.h"
#include "relocate.h"
#include "update.h"

struct UiFonts {
    ImFont *body = nullptr, *small = nullptr, *big = nullptr, *title = nullptr, *logo = nullptr;
};

class Ui {
public:
    enum class Tab { Games, Settings, About };
    enum class Flow { None, Browse, Checking, Result, Prepare, Import, UpdateAsk, Updating, UpdateResult, Moving };

    ~Ui();
    explicit Ui(Library &lib) : lib_(lib) {}
    void frame(float scale, float t);   // inside ImGui::NewFrame / Render
    bool wants_quit() const { return quit_; }
    bool busy() const;                  // something animates (a check, a build, an update, a running game): keep drawing

    // Scripted steps (--auto) and the command line use these too.
    void set_tab(Tab t);
    void open_browse(const std::string &game, const std::string &dir);
    void start_check(const std::string &game, const std::string &path);
    void start_prepare(const std::string &game, bool shaders_only = false);
    void start_import(const std::string &game, const std::string &dir = "");
    void start_import_dir(const std::string &game, const std::string &path);
    bool checking() const { return flow_ == Flow::Checking; }
    bool preparing() const { return flow_ == Flow::Prepare || flow_ == Flow::Import; }
    // the scripted runs wait on this: every background piece of Prepare/Import ended
    bool prep_finished() const { return (!prep_ || prep_->done()) && (!import_thread_.joinable() || import_done_.load()); }
    void select_settings_game(const std::string &game);
    const std::string &browse_dir() const { return dir_; }   // the scripted use-folder step
    // A typed or pasted path in the browser: a game copy (or, importing, a build) is taken as is; any other folder
    // is opened in the list. Quotes, file:// and a leading ~ are understood.
    void browse_go(const std::string &text);
    void open_system_picker();
    // Moving the library (relocate.h): the browser picks the new folder, then the move runs with its progress.
    void start_library_browse();
    void start_library_move(const std::string &picked);
    bool moving_finished() const { return !move_ || move_->finished.load(); }   // the desktop's folder dialog; its answer goes through browse_go
    // Prepare progress for --auto: true once the stage reached `stage` (and `done` units of it, if given).
    bool prep_reached(int stage, int done) const {
        if (!prep_) return prep_finished();
        int s = prep_->stage.load();
        return s > stage || (s == stage && prep_->stage_done.load() >= done) || prep_->done();
    }
    const std::string &flow_game() const { return flow_game_; }
    void request_focus() { focus_ = 2; }

    // The update channel (update.h). start_update_check asks in the background; the hub opens without waiting.
    void start_update_check();          // no-op with CB_LAUNCHER_NO_UPDATE=1
    bool update_checking() const { return checker_ != nullptr; }
    bool update_known() const { return checker_ == nullptr && checked_; }   // this run's check answered
    void show_update_ask();             // the dialog (also opens on its own when a new version is found)
    void start_update();                // Update now: download, verify, back up, install
    void decline_update();              // Not now: this version is not asked about again
    bool updating() const { return flow_ == Flow::Updating || checker_ != nullptr; }
    void restart_launcher();            // after an install: run app/current, leave
    void set_scripted(bool s) { scripted_ = s; }   // --auto: the update dialog waits for its step

    UiFonts fonts;

private:
    struct Entry {
        std::string name, path;
        std::string game;   // a game copy found right there (exefs/main.npdm title id), "" otherwise
        bool copy = false;  // holds exefs/ and romfs/
        bool build = false; // holds country-box: a build to import
    };

    void draw_header(ImVec2 mn, ImVec2 mx);
    void draw_tabs(float y);
    void draw_games(ImVec2 mn, ImVec2 mx);
    void draw_card(Game &g, ImVec2 mn, ImVec2 mx, bool first);
    void draw_settings(ImVec2 mn, ImVec2 mx);
    void draw_about(ImVec2 mn, ImVec2 mx);
    void draw_browse(ImVec2 mn, ImVec2 mx);
    void draw_checking(ImVec2 mn, ImVec2 mx);
    void draw_result(ImVec2 mn, ImVec2 mx);
    void draw_prepare(ImVec2 mn, ImVec2 mx);
    void draw_prep_notice(ImVec2 mn, ImVec2 mx);
    void draw_import(ImVec2 mn, ImVec2 mx);
    void draw_update_ask(ImVec2 mn, ImVec2 mx);
    void draw_updating(ImVec2 mn, ImVec2 mx);
    void draw_update_result(ImVec2 mn, ImVec2 mx);
    void draw_moving(ImVec2 mn, ImVec2 mx);
    void draw_footer(ImVec2 mn, ImVec2 mx);
    void draw_toast(ImVec2 mn, ImVec2 mx);
    void draw_running(ImVec2 mn, ImVec2 mx);

    bool button(const char *id, const char *label, ImVec2 pos, ImVec2 size, ImU32 col, bool enabled = true, ImFont *font = nullptr,
                bool focus_here = false);
    void list_dir(const std::string &dir);
    void load_settings();
    void step_setting(const Json &row, int dir);
    void set_setting(const std::string &key, const std::string &value);
    void toast(const std::string &text, ImU32 col);
    void finish_flow();
    bool focus_take();   // true once per focus request: the item drawn next gets the focus

    Library &lib_;
    float s_ = 1, t_ = 0;
    Tab tab_ = Tab::Games;
    Flow flow_ = Flow::None;
    std::string flow_game_;
    int focus_ = 2;
    bool quit_ = false;
    // browse
    std::string dir_;
    std::vector<Entry> entries_;
    std::vector<std::pair<std::string, std::string>> places_;
    std::string browse_note_;
    bool browse_import_ = false;   // the same browser, picking a build made elsewhere instead of a game copy
    bool browse_library_ = false;  // the same browser, picking where the library goes
    char path_buf_[4096] = {};     // the path field: the folder shown, or what the player typed or pasted
    bool list_top_ = false;        // a new folder: the list starts at its top
    bool list_drag_ = false, list_dragged_ = false;   // a press that began on the list; it moved far enough to scroll
    bool picker_open_ = false;     // the system's folder dialog is up (its answer arrives on another thread)
    void take_picker_answer();
    // check
    std::unique_ptr<VerifyJob> job_;
    VerifyResult result_;
    std::string result_game_, result_path_;
    // prepare (milestone 2): the pipeline on this machine, and importing a build made on another
    std::unique_ptr<PrepareJob> prep_;
    PrepareJob::Outcome prep_out_;
    std::string prep_game_;
    bool prep_shaders_only_ = false;
    ImportOutcome import_out_;
    std::atomic<bool> import_done_{false};
    std::atomic<int64_t> import_bytes_{0}, import_total_{0};
    std::thread import_thread_;
    // library move
    std::unique_ptr<LibraryMove> move_;
    std::thread move_thread_;
    // settings
    std::string settings_game_;
    Json dump_;
    std::string settings_err_;
    int group_ = 0;
    std::string focused_key_;
    // update
    std::unique_ptr<UpdateChecker> checker_;
    std::unique_ptr<UpdateJob> update_;
    UpdateInfo update_info_;           // the check's answer (kept for About)
    bool checked_ = false;             // this run's check answered (start_update_check ran)
    bool scripted_ = false;            // --auto: dialogs wait for their step instead of opening on their own
    struct {
        bool ok = false, cancelled = false;
        std::string err, from, to, installed, backup;
        std::vector<std::string> saved, need_prepare;
    } update_outcome_;                 // a finished UpdateJob, once it leaves its thread
    // toast
    std::string toast_;
    ImU32 toast_col_ = 0;
    float toast_until_ = 0;
};
