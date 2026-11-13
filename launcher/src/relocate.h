// Country-Box launcher: moving the library (builds, the C made from the game, saves, logs) to a folder the player
// picks. The choice is kept in <default home>/library-location, so later runs (and updates) find the library there;
// --home and CB_LAUNCHER_HOME still win over it for one run.
//
// The move is all or nothing: on the same drive every piece is renamed; across drives everything is copied first,
// the new place is recorded, and only then the old copy is removed. Any failure before that leaves the library
// where it was and removes what the move had made. The launcher's Python (python/) and its own install (app/) stay
// where they are: they are tools, not the library.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

std::string default_launcher_home();   // $XDG_DATA_HOME/country-box, else ~/.local/share/country-box
std::string library_location_file();   // <default home>/library-location
bool launcher_home_fixed();            // --home or CB_LAUNCHER_HOME chose this run's home (the folder can't be changed)

// Where a move to `picked` puts the library: picked itself when it is empty (or does not exist yet) or is the
// default place, else a "Country-Box" folder inside it.
std::string library_target(const std::string &picked);

struct LibraryMove {
    std::atomic<int64_t> done{0}, total{0};   // bytes (a copy across drives); total 0 while counting or renaming
    std::atomic<bool> finished{false};
    bool ok = false;
    std::string from, to;                     // to: the library's new folder
    std::string headline, explain;            // why it failed
};

// Moves the library from `from` to library_target(picked). Run it on a thread; read the fields once finished is true.
void move_library(const std::string &from, const std::string &picked, LibraryMove &m);
