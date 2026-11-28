#ifndef WIN32_COMPAT_H
#define WIN32_COMPAT_H

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <io.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

static inline struct tm *win32_localtime_r(const time_t *timep, struct tm *result) {
    return localtime_s(result, timep) == 0 ? result : NULL;
}
#ifndef localtime_r
#define localtime_r(t, r) win32_localtime_r(t, r)
#endif

static inline struct tm *win32_gmtime_r(const time_t *timep, struct tm *result) {
    return gmtime_s(result, timep) == 0 ? result : NULL;
}
#ifndef gmtime_r
#define gmtime_r(t, r) win32_gmtime_r(t, r)
#endif

#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif

#ifndef F_OK
#define F_OK 0
#endif
#ifndef R_OK
#define R_OK 4
#endif
#ifndef W_OK
#define W_OK 2
#endif
#ifndef X_OK
#define X_OK 1
#endif

#ifndef sched_yield
#define sched_yield() SwitchToThread()
#endif

#ifndef setenv
#define setenv(k, v, o) _putenv_s(k, v)
#endif
#ifndef unsetenv
#define unsetenv(k) _putenv_s(k, "")
#endif
#ifndef lstat
#define lstat(p, b) stat(p, b)
#endif
#ifndef fsync
#define fsync(fd) _commit(fd)
#endif
#ifndef mkdir
#define mkdir(p, m) _mkdir(p)
#endif

#ifndef PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
#define PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP ((pthread_mutex_t)-3)
#endif

#ifndef MADV_NORMAL
#define MADV_NORMAL 0
#define MADV_RANDOM 1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED 3
#define MADV_DONTNEED 4
#endif
static inline int madvise(void *addr, size_t length, int advice) {
    (void)addr; (void)length; (void)advice;
    return 0;
}

static inline void *win32_memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen) {
    if (!haystack || !needle || needlelen == 0 || haystacklen < needlelen) return NULL;
    const char *h = (const char *)haystack;
    const char *n = (const char *)needle;
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0) return (void *)(h + i);
    }
    return NULL;
}
#ifndef memmem
#define memmem(h, hl, n, nl) win32_memmem(h, hl, n, nl)
#endif

#ifndef _SC_CLK_TCK
#define _SC_CLK_TCK 1
#endif
#ifndef _SYSCONF_DEFINED
#define _SYSCONF_DEFINED
static inline long sysconf(int name) {
    (void)name;
    return 100;
}
#endif

#ifndef PROT_NONE
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#endif

#ifndef MAP_SHARED
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 0x20
#define MAP_NORESERVE 0
#define MAP_FAILED ((void *)-1)
#endif

static inline void *win32_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)addr; (void)flags;
    if (fd == -1) {
        return VirtualAlloc(NULL, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);
    if (hFile == INVALID_HANDLE_VALUE) return MAP_FAILED;
    DWORD flProtect = (prot & PROT_WRITE) ? PAGE_READWRITE : PAGE_READONLY;
    HANDLE hMap = CreateFileMappingA(hFile, NULL, flProtect, 0, 0, NULL);
    if (!hMap) return MAP_FAILED;
    DWORD dwAccess = (prot & PROT_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
    void *ptr = MapViewOfFile(hMap, dwAccess, (DWORD)((uint64_t)offset >> 32), (DWORD)offset, length);
    CloseHandle(hMap);
    return ptr ? ptr : MAP_FAILED;
}

static inline int win32_munmap(void *addr, size_t length) {
    (void)length;
    if (UnmapViewOfFile(addr)) return 0;
    if (VirtualFree(addr, 0, MEM_RELEASE)) return 0;
    return -1;
}

static inline int win32_mprotect(void *addr, size_t len, int prot) {
    DWORD old;
    DWORD win_prot = PAGE_NOACCESS;
    if (prot & PROT_WRITE) win_prot = PAGE_READWRITE;
    else if (prot & PROT_READ) win_prot = PAGE_READONLY;
    return VirtualProtect(addr, len, win_prot, &old) ? 0 : -1;
}

#ifndef mmap
#define mmap(a, l, p, f, fd, off) win32_mmap(a, l, p, f, fd, off)
#endif
#ifndef munmap
#define munmap(a, l) win32_munmap(a, l)
#endif
#ifndef mprotect
#define mprotect(a, l, p) win32_mprotect(a, l, p)
#endif

static inline int getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    if (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)buflen, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0)
        return (int)buflen;
    return -1;
}

static inline void *win32_aligned_alloc(size_t alignment, size_t size) {
    return _aligned_malloc(size, alignment);
}
#define aligned_alloc(a, s) win32_aligned_alloc(a, s)

static inline ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD bytes = 0;
    if (!ReadFile(h, buf, (DWORD)count, &bytes, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        return -1;
    }
    return (ssize_t)bytes;
}

static inline ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    DWORD bytes = 0;
    if (!WriteFile(h, buf, (DWORD)count, &bytes, &ov)) return -1;
    return (ssize_t)bytes;
}

#endif /* _WIN32 */
#endif /* WIN32_COMPAT_H */
