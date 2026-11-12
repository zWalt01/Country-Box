// Country-Box launcher: child processes (the game binary's settings commands, and PLAY).
#pragma once
#include <string>
#include <sys/types.h>
#include <vector>

// Runs argv in cwd with extra environment (NAME=value entries), captures stdout into out (stderr is dropped), waits at
// most timeout_ms. Returns the exit code, -1 when it could not start or timed out (then it is killed).
int proc_run(const std::vector<std::string> &argv, const std::string &cwd, const std::vector<std::string> &env,
             std::string &out, int timeout_ms = 10000);
// Starts argv in cwd in its own session with stdout and stderr appended to log; returns its pid, -1 on failure.
pid_t proc_spawn(const std::vector<std::string> &argv, const std::string &cwd, const std::string &log);
// 1 while pid runs, 0 once it ended (reaped; *status gets the exit code or 128+signal), -1 unknown.
int proc_poll(pid_t pid, int *status);
