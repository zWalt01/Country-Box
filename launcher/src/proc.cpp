// Country-Box launcher: child processes (proc.h).
#include "proc.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static std::vector<char *> cargv(const std::vector<std::string> &v) {
    std::vector<char *> a;
    for (auto &s : v) a.push_back(const_cast<char *>(s.c_str()));
    a.push_back(nullptr);
    return a;
}

int proc_run(const std::vector<std::string> &argv, const std::string &cwd, const std::vector<std::string> &env,
             std::string &out, int timeout_ms) {
    out.clear();
    int fds[2];
    if (argv.empty() || pipe2(fds, O_CLOEXEC)) return -1;
    auto args = cargv(argv);
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]), close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], 1);
        int null = open("/dev/null", O_WRONLY);
        if (null >= 0) dup2(null, 2);
        if (!cwd.empty() && chdir(cwd.c_str())) _exit(127);
        for (auto &e : env) putenv(const_cast<char *>(e.c_str()));
        execv(args[0], args.data());
        _exit(127);
    }
    close(fds[1]);
    auto t0 = std::chrono::steady_clock::now();
    char buf[16384];
    bool timed_out = false;
    for (;;) {
        int left = timeout_ms - (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        if (left <= 0) {
            timed_out = true;
            break;
        }
        struct pollfd p = {fds[0], POLLIN, 0};
        int r = poll(&p, 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            timed_out = r == 0;
            break;
        }
        ssize_t n = read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    close(fds[0]);
    if (timed_out) kill(pid, SIGKILL);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (timed_out) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

pid_t proc_spawn(const std::vector<std::string> &argv, const std::string &cwd, const std::string &log) {
    if (argv.empty()) return -1;
    auto args = cargv(argv);
    pid_t pid = fork();
    if (pid != 0) return pid;
    setsid();
    int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) dup2(fd, 1), dup2(fd, 2);
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) dup2(null, 0);
    if (!cwd.empty() && chdir(cwd.c_str())) _exit(127);
    execv(args[0], args.data());
    _exit(127);
}

int proc_poll(pid_t pid, int *status) {
    int st = 0;
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == 0) return 1;
    if (r < 0) return -1;
    if (status) *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return 0;
}
