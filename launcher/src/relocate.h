// Country-Box launcher: where the launcher keeps everything (the "data folder"), and moving it.
//
// Portable by default (1.0.7): run from the AppImage or from the extracted tarball, everything Country-Box writes —
// the library (builds, the C made from the game, saves, settings, logs), its Python, caches and temporary files —
// lives in ONE folder next to it: <folder holding the .AppImage or the extracted Country-Box/>/Country-Box-data.
// Deleting that folder removes the game completely. AppRun says where the package is (CB_PORTABLE_EXE). A bare
// binary (development, a distribution package) keeps the old place, $XDG_DATA_HOME/country-box or
// ~/.local/share/country-box; so does the AppImage when the folder next to it can't hold programs (read-only, exFAT,
// noexec), and it says why.
//
// That place is the "base": it also records where the player moved the library (<base>/library-location), so later
// runs (and updates) find it there; --home / --data-dir / CB_LAUNCHER_HOME still win over it for one run.
//
// The move is all or nothing: on the same drive every piece is renamed; across drives everything is copied first,
// checked (every file's size, and the bytes of saves and settings), the new place is recorded, and only then the old
// copy is removed. Any failure before that leaves the library where it was and removes what the move had made. A
// player's move leaves the launcher's Python (python/) and its own install (app/) in the base: they are tools.
//
// Saves first (1.0.7): before any move, every game's saves (games/<id>/profile and mp4.ini) are copied to a
// new folder on the player's Desktop ("Country-Box saves backup <date>", with a README) and read back byte for byte;
// if that copy fails, nothing moves. The saves then move with the games, so they are in two places afterwards.
//
// Migration (first start of a portable Country-Box, 1.0.7): when the data folder holds no library yet and the old
// place (~/.local/share/country-box) does, the launcher ASKS once — keep it where it is (recorded, never asked
// again), move it next to Country-Box (the data folder), or move it to a folder the player picks (the tools and the
// record of that place go to the data folder). Nothing moves without that answer; starting a game straight away
// (Steam shortcut) just uses the old place. A library the player had already moved elsewhere stays where it is:
// only its record and the tools come over, without asking. If the move fails, Country-Box keeps the old place.
//
// RecompLauncher (Linux only): it unpacks the AppImage into the game's folder, runs its AppRun and passes
// RECOMPLAUNCHER_DATA ({data}, recompdata inside that folder). That is the data folder then, so everything stays with
// the game the launcher moves, backs up and uninstalls; without the variable nothing changes. The first start with it
// COPIES in what 1.0.7 to 1.0.9 kept next to the game's folder (<library>/Country-Box-data); nothing there is moved or
// deleted. Each item goes to <name>.copying, is read back, and only then is renamed into place; a marker
// (.country-box-data-copied) is written last and is the only sign it is done. A start cut short leaves .copying items
// that the next start removes and copies again. A folder {data} already has (the launcher writes
// games/<game>/mp4.ini before a game starts) is gone into, and what it lacks is copied file by file; nothing there is
// overwritten except each game's mp4.ini, which the old one replaces (also through .copying) until the marker is
// written. Temporary files and caches are not copied. If the copy fails, that run keeps the old folder, and the next
// start tries again.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

std::string default_launcher_home();   // the base: the data folder when portable, else the legacy place (above)
std::string legacy_launcher_home();    // $XDG_DATA_HOME/country-box, else ~/.local/share/country-box
std::string portable_data_dir();       // RecompLauncher's {data} (above), else <next to the AppImage / extracted
                                       // folder>/Country-Box-data; "" for a bare binary
std::string recomplauncher_data_dir(); // RECOMPLAUNCHER_DATA when it is an absolute path (Linux), else ""
bool under_recomplauncher();           // RECOMPLAUNCHER=1 (Linux): RecompLauncher installs updates, never the hub
std::string base_note();               // why the base is not the data folder ("" when it is, or for a bare binary)
void set_base_home(const std::string &dir);   // this run uses dir as the base (migration pending or failed; done)
std::string library_location_file();   // <base>/library-location
bool launcher_home_fixed();            // --home, --data-dir or CB_LAUNCHER_HOME chose this run's home (can't be moved)

// Portable runs (and --data-dir): the children (Python, clang, the game) and the launcher itself write their temporary files and
// caches inside the base too (TMPDIR=<base>/tmp, XDG_CACHE_HOME=<base>/cache) — nothing in /tmp or ~/.cache.
void contain_temp_files(const std::string &base);

// Where a move to `picked` puts the library: picked itself when it is empty (or does not exist yet) or is the
// default place, else a "Country-Box" folder inside it.
std::string library_target(const std::string &picked);

struct LibraryMove {
    bool everything = false;                  // migration: python/ and app/ move too, the target is taken as is
    std::atomic<int64_t> done{0}, total{0};   // bytes (a copy across drives); total 0 while counting or renaming
    std::atomic<bool> finished{false};
    bool ok = false;
    std::string from, to;                     // to: the library's new folder
    std::string headline, explain;            // why it failed
    bool backup_saves = true;                 // copy the saves to the Desktop before anything moves (above)
    bool no_record = false;                   // the caller records the new place itself (a migration)
    std::string backup;                       // where that copy went; "" when there were no saves to copy
};

// The player's Desktop (XDG_DESKTOP_DIR in user-dirs.dirs, else ~/Desktop, else the home). CB_DESKTOP_DIR overrides.
std::string desktop_dir();
// Copies the saves of every game in `library` (games/<id>/profile, games/<id>/mp4.ini) into a new folder on the
// Desktop and reads it back byte for byte. true with dir "" when there is nothing to copy; false with err on failure.
bool backup_saves(const std::string &library, const std::string &moving_to, std::string &dir, std::string &err);

// Moves the library from `from` to library_target(picked). Run it on a thread; read the fields once finished is true.
void move_library(const std::string &from, const std::string &picked, LibraryMove &m);

// The first-run migration (above). needed: the data folder has no library and the old place has one, and the player
// has not chosen to keep it there (stay). same_drive: the data folder is on the old place's drive. busy: a game runs
// from the old place right now (try again next start).
struct Migration {
    bool needed = false, same_drive = false, busy = false;
    bool stay = false;       // the player chose "keep it where it is": the old place stays the base, not asked again
    std::string from;        // the old base (~/.local/share/country-box)
    std::string to;          // the data folder
    std::string library;     // where the library is now: from, or the folder the player moved it to
    bool ask() const { return needed && !busy && library == from; }   // the player decides; else it just happens
};
Migration plan_migration();
// "Keep it where it is": recorded in the old place (<from>/library-choice), so later starts don't ask again.
bool keep_library_here(const Migration &plan, std::string &err);
// Runs it (blocking; run on a thread for progress). picked: "" = the data folder, else the folder the player chose
// (library_target rules). On success the base is the data folder (set_base_home), recording where the library is.
void migrate_library(const Migration &plan, const std::string &picked, LibraryMove &m);

// The first-start copy into RecompLauncher's {data} (above). needed: there is something to copy and it was not
// copied before. busy: a game runs from the old folder right now (it stays this run's; try again next start).
struct OldDataCopy {
    bool needed = false, busy = false;
    std::string from;   // <library>/Country-Box-data
    std::string to;     // RECOMPLAUNCHER_DATA
};
OldDataCopy plan_old_data_copy();
// Runs it (blocking; run on a thread for progress): m.ok, or m.headline and m.explain; then m.finished.
void copy_old_data(const OldDataCopy &plan, LibraryMove &m);
