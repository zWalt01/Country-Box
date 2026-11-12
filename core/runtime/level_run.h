// Country-Box: the measured run of one warped level, shared by the games' tools (dkcr_tools.c for Returns HD,
// ov_dktf.c for Tropical Freeze; moved out of dkcr_tools.c, its log lines unchanged). The game's tools say when the warp
// sent the game to the level, what the play time is at each present, whether the game is still in the level and
// whether the player has control, and when the level-clear screen appears; this file measures and prints the
// "level: ..." lines tools/sweep.py parses, the same for every game.
//
// The run starts at the first of 30 presents in a row in which the play time advanced (it stands still while a level
// loads) and ends at the level-clear screen (2 s of it are left out of the numbers), when the game leaves the level, or
// at the --seconds limit (level_run_finish). The load time runs from the warp to that first present.
//
//   CB_TURBO_HZ=<N>   fast-forward the level once it plays (vsync and pacing at N Hz; the boot rate comes back at its end)
//   CB_WARP_EXIT=0    keep running after the level ends (by default the run ends there)
//   CB_WARP_TRACE=1   log the play time every 60 measured presents
//   CB_SPIKE_DIAG=1   what the threads did in each slow present (spike_diag.c)
#pragma once
#include <stdint.h>

// The warp sent the game to this level (name and the game's own level number, for the result line; the play time
// now, which the level's start is judged against). Any thread.
void level_run_warped(const char *name, int32_t world_id, double play_time);
// Every present while a run is on (the presenting thread). play_time: the game's play time in seconds (only whether
// it changed matters); in_level: the game is still in the warped level; control: the player can move (1), cannot (0),
// unknown (-1, counted as without control); left: called once the game left the level, gives the end text ("left for
// <level>"). Returns 1 when the run ended and the program should end (CB_WARP_EXIT unset).
int level_run_frame(uint64_t frame, double play_time, int in_level, int control, const char *(*left)(void));
// The level-clear screen appeared (an entry hook on the game thread): logs "level: LEVEL CLEAR (<source>)".
void level_run_clear(const char *source);
// The program ends (--seconds limit): the report of a run still measuring.
void level_run_finish(const char *why);
// A host flip of a presented frame at t_ns (vk_backend.c present thread): the same frames timed at the display, logged
// as "level: flip ms ..." beside the game's present-call times.
void level_run_flip(uint64_t t_ns);
// 1 while a run is waiting for the level or measuring it.
int level_run_active(void);
