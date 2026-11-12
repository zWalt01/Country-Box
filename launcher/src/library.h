// Country-Box launcher: the player's library. Where each game's checked copy and its build (the recompiled game
// binary, country-box) live, and the state each card shows. Kept in <home>/library.ini, where home is
// CB_LAUNCHER_HOME, else $XDG_DATA_HOME/country-box, else ~/.local/share/country-box.
//
// Designed for the later milestones: a build is normally <home>/games/<game>/ (what setup will write when it
// recompiles or copies a prebuilt build); "build = <dir>" in the library points at another one. Saves and settings
// live with the build (the game's profile/ and mp4.ini) and an update must never touch them.
#pragma once
#include <string>
#include <sys/types.h>
#include <vector>

#include "json.h"
#include "manifest.h"

enum class GameState {
    NotInstalled,     // no copy added
    CopyMissing,      // the copy's folder is gone (drive unplugged, moved)
    NeedsCheck,       // the copy was checked against an older manifest
    NeedsBuild,       // copy fine, the game is not built yet (setup: a later milestone)
    NeedsShaders,     // built; the game prepares its shaders at the next start ("Preparing shaders" screen)
    Ready,
    UpdateAvailable,  // the build is older than this Country-Box
};

struct Game {
    std::string id;                 // "dkcrhd", "dkctf"
    const Manifest *manifest = nullptr;
    std::string copy;               // the checked folder (holds exefs/ and romfs/)
    std::string copy_checked;       // when (local time, ISO)
    std::string copy_manifest;      // the manifest id it was checked against
    std::string build_dir;          // where country-box is
    bool build_set = false;         // build_dir came from the library (not the default place)
    std::string exe;                // build_dir/country-box when it exists
    std::string build_version;      // from --settings-dump
    std::string build_game;         // the game the build says it runs ("dkcr-hd", "dkctf")
    GameState state = GameState::NotInstalled;
    std::string detail;             // one line under the state: why
    // PLAY
    pid_t pid = -1;
    std::string log;
    int last_exit = -1;
};

const char *game_state_name(GameState s);   // "not_installed", ... (command line)
const char *game_state_label(GameState s);  // "Not installed", ... (the card)

class Library {
public:
    explicit Library(std::string home);
    const std::string &home() const { return home_; }
    std::vector<Game> &games() { return games_; }
    Game *find(const std::string &id);
    void load();
    bool save(std::string *err = nullptr) const;
    void refresh();                    // every game's state, from disk
    void refresh(Game &g);
    // Records a copy that passed the check (root of the result).
    void set_copy(Game &g, const VerifyResult &r);
    void forget_copy(Game &g);
    void set_build(Game &g, const std::string &dir);

    // PLAY: starts the build with the checked copy. false with err set when it can't.
    bool play(Game &g, std::string *err);
    // Polls running games; true when one ended since the last call.
    bool poll_running();
    bool any_running() const;

    // The game's settings (--settings-dump); false with err.
    bool settings_dump(const Game &g, Json &out, std::string *err) const;
    // --settings-set key=value; the answer JSON in out. false with err when it ran badly (a refusal is out.ok=false).
    bool settings_set(const Game &g, const std::vector<std::string> &pairs, Json &out, std::string *err) const;

    std::string status_json();         // the command line's --status

private:
    std::string home_;
    std::vector<Game> games_;
};

std::string launcher_home(const std::string &override_dir);
std::string launcher_version();       // VERSION at build time
int version_compare(const std::string &a, const std::string &b);   // dotted numbers: <0, 0, >0
