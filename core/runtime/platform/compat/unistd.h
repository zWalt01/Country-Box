#ifndef _UNISTD_H_WRAPPER
#define _UNISTD_H_WRAPPER

#include_next <unistd.h>
#include <io.h>
#include <process.h>
#include <stdlib.h>

#ifndef F_OK
#define F_OK 0
#endif
#ifndef X_OK
#define X_OK 1
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef R_OK
#define R_OK 4
#endif

#ifndef STDIN_FILENO
#define STDIN_FILENO 0
#endif
#ifndef STDOUT_FILENO
#define STDOUT_FILENO 1
#endif
#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

#ifndef isatty
#define isatty _isatty
#endif

#ifndef getpid
#define getpid _getpid
#endif

#ifndef O_BINARY
#ifdef _O_BINARY
#define O_BINARY _O_BINARY
#else
#define O_BINARY 0x8000
#endif
#endif

#ifndef O_CLOEXEC
#ifdef O_NOINHERIT
#define O_CLOEXEC (O_NOINHERIT | O_BINARY)
#else
#define O_CLOEXEC O_BINARY
#endif
#endif

#ifndef ftruncate
static inline int ftruncate(int fd, off_t length) {
    return _chsize(fd, (long)length);
}
#endif

#ifndef POSIX_FADV_SEQUENTIAL
#define POSIX_FADV_SEQUENTIAL 0
#endif
static inline int posix_fadvise(int fd, off_t offset, off_t len, int advice) {
    (void)fd; (void)offset; (void)len; (void)advice;
    return 0;
}

#ifndef setenv
static inline int setenv(const char *name, const char *value, int overwrite) {
    (void)overwrite;
    return _putenv_s(name, value);
}
#endif

#ifndef unsetenv
static inline int unsetenv(const char *name) {
    return _putenv_s(name, "");
}
#endif

#ifndef _SC_NPROCESSORS_ONLN
#define _SC_NPROCESSORS_ONLN 84
#endif
#ifndef _SC_CLK_TCK
#define _SC_CLK_TCK 1
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef _SYSCONF_DEFINED
#define _SYSCONF_DEFINED
static inline long sysconf(int name) {
    if (name == _SC_NPROCESSORS_ONLN) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (long)si.dwNumberOfProcessors;
    }
    return 100;
}
#endif

static inline ssize_t readlink(const char *path, char *buf, size_t bufsiz) {
    if (strcmp(path, "/proc/self/exe") == 0) {
        DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)bufsiz);
        if (n == 0 || n >= bufsiz) return -1;
        return (ssize_t)n;
    }
    return -1;
}

#ifndef fsync
#define fsync(fd) _commit(fd)
#endif

static inline int link(const char *oldpath, const char *newpath) {
    return CreateHardLinkA(newpath, oldpath, NULL) ? 0 : -1;
}
#endif

#endif /* _UNISTD_H_WRAPPER */
