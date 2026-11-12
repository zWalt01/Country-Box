// Country-Box: Linux implementation of platform.h (the interface of fe3h-pc-recomp / totk-pc-recomp, MIT; see
// platform.h and win32.c). Written for Country-Box; techniques noted per section, the Linux layer of PrimeRecomp
// (gminustwo/primerecomp runtime/platform/linux.c and posix.c, MIT, Copyright (c) 2026 Peiikai) was read as reference.
//
// Locks are futex words so that PlatMutex and PlatCond keep the 8-byte, zero-is-valid layout the Windows SRW lock and
// condition variable have (pthread_mutex_t is 40 bytes and needs no zero-init guarantee). Threads get a 16 MiB stack,
// what the Windows build gives every thread through the linker's --stack (tools/build_game.py LINK_FLAGS): lifted code
// runs on the host stack and some guest call chains are deep.
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"

extern char **environ;

#define THREAD_STACK (16u << 20)
#define LPATH 4096

_Static_assert(sizeof(PlatMutex) >= sizeof(uint32_t), "PlatMutex holds a futex word");
_Static_assert(sizeof(PlatCond) >= sizeof(uint32_t), "PlatCond holds a futex word");

// ---- threads

struct PlatThread {
    pthread_t h;
    PlatThreadFn fn;
    void *arg;
    char name[16];   // Linux thread names are at most 15 bytes plus the NUL
};

void plat_signal_stack(void *buf, size_t n) {
    stack_t st = {.ss_sp = buf, .ss_size = n};
    sigaltstack(&st, NULL);
}

static void *thread_main(void *p) {
    PlatThread *t = p;
    _Alignas(16) char alt[PLAT_SIGNAL_STACK];
    plat_signal_stack(alt, sizeof alt);
    if (t->name[0]) pthread_setname_np(pthread_self(), t->name);
    t->fn(t->arg);
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, THREAD_STACK);
    int e = pthread_create(&t->h, &a, thread_main, t);
    pthread_attr_destroy(&a);
    if (e) { free(t); return NULL; }
    return t;
}

void plat_thread_join(PlatThread *t) {
    pthread_join(t->h, NULL);
    free(t);
}

// As in win32.c the struct stays allocated: the thread keeps reading fn, arg and name.
void plat_thread_detach(PlatThread *t) { pthread_detach(t->h); }

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    pthread_t h = t ? t->h : pthread_self();
    cpu_set_t old, set;
    CPU_ZERO(&old);
    if (pthread_getaffinity_np(h, sizeof old, &old)) return 0;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    if (pthread_setaffinity_np(h, sizeof set, &set)) return 0;
    uint64_t prev = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, &old)) prev |= 1ull << i;
    return prev ? prev : 1;   // 0 means failure to the caller
}

uint32_t plat_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}

// Cached per thread: gettid is a syscall, GetCurrentThreadId was a TEB read, and callers use it in log and trace paths.
uint32_t plat_thread_id(void) {
    static __thread uint32_t id;
    if (!id) id = (uint32_t)syscall(SYS_gettid);
    return id;
}

// ---- time: CLOCK_MONOTONIC through the vDSO (no system call)

uint64_t plat_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

// clock_nanosleep to an absolute deadline wakes within the thread's timer slack (50 us by default); the slack is set
// to 1 us once per thread and the last PLAT_SPIN_NS are spent yielding, as win32.c does, so the deadline is met
// closely. The spin is short on purpose: on a Steam Deck every spinning microsecond is battery.
#define PLAT_SPIN_NS 50000u

static __thread int slack_set;

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (!slack_set) {
        slack_set = 1;
        prctl(PR_SET_TIMERSLACK, 1ul, 0ul, 0ul, 0ul);
    }
    if (ns > PLAT_SPIN_NS) {
        uint64_t until = end - PLAT_SPIN_NS;
        struct timespec ts = {(time_t)(until / 1000000000u), (long)(until % 1000000000u)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    }
    while (plat_now_ns() < end) sched_yield();
}

// Country-Box: a sleep that never spins, for waits that need no precision (the guest's SleepThread): the thread's 1 us
// timer slack still applies, so it wakes a few to some tens of microseconds late, as the scheduler allows.
void plat_sleep_lax_ns(uint64_t ns) {
    if (!slack_set) {
        slack_set = 1;
        prctl(PR_SET_TIMERSLACK, 1ul, 0ul, 0ul, 0ul);
    }
    uint64_t end = plat_now_ns() + ns;
    struct timespec ts = {(time_t)(end / 1000000000u), (long)(end % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

// ---- locks and condition variables: futex words (U. Drepper, "Futexes Are Tricky", mutex 2: 0 unlocked, 1 locked,
// 2 locked with waiters; the condition variable is a sequence number)

static long futex(_Atomic uint32_t *w, int op, uint32_t val, const struct timespec *to) {
    return syscall(SYS_futex, (uint32_t *)w, op | FUTEX_PRIVATE_FLAG, val, to, NULL, 0);
}

static _Atomic uint32_t *word(void *p) { return (_Atomic uint32_t *)p; }

void plat_mutex_init(PlatMutex *m) { atomic_store_explicit(word(m), 0, memory_order_relaxed); }

void plat_mutex_lock(PlatMutex *m) {
    _Atomic uint32_t *w = word(m);
    uint32_t c = 0;
    if (atomic_compare_exchange_strong_explicit(w, &c, 1, memory_order_acquire, memory_order_relaxed)) return;
    // a short spin first: most game locks are held for a few hundred nanoseconds
    for (int i = 0; i < 64; i++) {
        __builtin_ia32_pause();
        c = 0;
        if (atomic_load_explicit(w, memory_order_relaxed) == 0 &&
            atomic_compare_exchange_weak_explicit(w, &c, 1, memory_order_acquire, memory_order_relaxed))
            return;
    }
    if (c != 2) c = atomic_exchange_explicit(w, 2, memory_order_acquire);
    while (c != 0) {
        futex(w, FUTEX_WAIT, 2, NULL);
        c = atomic_exchange_explicit(w, 2, memory_order_acquire);
    }
}

int plat_mutex_trylock(PlatMutex *m) {
    uint32_t c = 0;
    return atomic_compare_exchange_strong_explicit(word(m), &c, 1, memory_order_acquire, memory_order_relaxed);
}

void plat_mutex_unlock(PlatMutex *m) {
    _Atomic uint32_t *w = word(m);
    if (atomic_exchange_explicit(w, 0, memory_order_release) == 2) futex(w, FUTEX_WAKE, 1, NULL);
}

// The mutex is re-taken in its contended state after a wait: a waker may have left other waiters asleep on it.
static void mutex_lock_contended(PlatMutex *m) {
    _Atomic uint32_t *w = word(m);
    while (atomic_exchange_explicit(w, 2, memory_order_acquire) != 0) futex(w, FUTEX_WAIT, 2, NULL);
}

// The condition variable is two words: a sequence number the waiters sleep on and the count of waiters, so a signal
// with nobody waiting stays in user space as WakeConditionVariable does (the converted runtime signals on every queue
// push and submit). Sequentially consistent order between a waiter's count increment and sequence load and a
// signaller's sequence increment and count load: a signaller that reads no waiters bumped the sequence before any
// later waiter read it, and such a waiter re-checks its predicate under the mutex before it waits.
static _Atomic uint32_t *cond_waiters(PlatCond *c) { return (_Atomic uint32_t *)c + 1; }
_Static_assert(sizeof(PlatCond) >= 2 * sizeof(uint32_t), "PlatCond holds the sequence and the waiter count");

void plat_cond_init(PlatCond *c) {
    atomic_store_explicit(word(c), 0, memory_order_relaxed);
    atomic_store_explicit(cond_waiters(c), 0, memory_order_relaxed);
}

static long cond_sleep(PlatCond *c, PlatMutex *m, const struct timespec *to) {
    _Atomic uint32_t *s = word(c);
    atomic_fetch_add_explicit(cond_waiters(c), 1, memory_order_seq_cst);
    uint32_t seq = atomic_load_explicit(s, memory_order_seq_cst);
    plat_mutex_unlock(m);
    long r = futex(s, FUTEX_WAIT, seq, to);
    int err = errno;
    atomic_fetch_sub_explicit(cond_waiters(c), 1, memory_order_relaxed);
    mutex_lock_contended(m);
    errno = err;
    return r;
}

void plat_cond_wait(PlatCond *c, PlatMutex *m) { cond_sleep(c, m, NULL); }

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    struct timespec to = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};   // FUTEX_WAIT: relative
    long r = cond_sleep(c, m, &to);
    return !(r == -1 && errno == ETIMEDOUT);
}

void plat_cond_signal(PlatCond *c) {
    atomic_fetch_add_explicit(word(c), 1, memory_order_seq_cst);
    if (atomic_load_explicit(cond_waiters(c), memory_order_seq_cst)) futex(word(c), FUTEX_WAKE, 1, NULL);
}

void plat_cond_broadcast(PlatCond *c) {
    atomic_fetch_add_explicit(word(c), 1, memory_order_seq_cst);
    if (atomic_load_explicit(cond_waiters(c), memory_order_seq_cst)) futex(word(c), FUTEX_WAKE, INT_MAX, NULL);
}

// ---- one-time initialisation: the first caller runs fn, the others wait on the state word until it is done.
void plat_once(PlatOnce *o, void (*fn)(void)) {
    _Atomic uint32_t *w = (_Atomic uint32_t *)&o->state;
    if (atomic_load_explicit(w, memory_order_acquire) == 2) return;
    uint32_t z = 0;
    if (atomic_compare_exchange_strong_explicit(w, &z, 1, memory_order_acq_rel, memory_order_acquire)) {
        fn();
        atomic_store_explicit(w, 2, memory_order_release);
        futex(w, FUTEX_WAKE, INT_MAX, NULL);
        return;
    }
    while (atomic_load_explicit(w, memory_order_acquire) != 2) futex(w, FUTEX_WAIT, 1, NULL);
}

// ---- memory

// The range was reserved inaccessible (PROT_NONE) by the memory layer; committing makes its pages read-write. As
// VirtualAlloc's MEM_COMMIT, the pages touched by [addr, addr + n) are committed.
int plat_mem_commit(uint64_t addr, size_t n) {
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    uint64_t lo = addr & ~(page - 1), hi = (addr + n + page - 1) & ~(page - 1);
    return mprotect((void *)(uintptr_t)lo, hi - lo, PROT_READ | PROT_WRITE) == 0;
}

// process_vm_readv on our own pid copies without faulting: an unmapped or unreadable page ends the copy short.
int plat_mem_read(uint64_t addr, void *out, size_t n) {
    struct iovec local = {out, n}, remote = {(void *)(uintptr_t)addr, n};
    ssize_t got = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return got >= 0 && (size_t)got == n;
}

// ---- files. Paths come with either separator; Linux wants '/'.

static int native(const char *path, char *out, size_t cap) {
    size_t n = strlen(path);
    if (n >= cap) { out[0] = 0; return 0; }
    for (size_t i = 0; i <= n; i++) out[i] = path[i] == '\\' ? '/' : path[i];
    return 1;
}

// The directory of path is synced after a rename so the new name survives a power cut (the write-through half of
// MoveFileEx's MOVEFILE_WRITE_THROUGH).
static void sync_parent(const char *p) {
    char dir[LPATH];
    snprintf(dir, sizeof dir, "%s", p);
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else snprintf(dir, sizeof dir, ".");
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
}

static int write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return 0; }
        p += w, n -= (size_t)w;
    }
    return 1;
}

int plat_file_exists(const char *path) {
    char p[LPATH];
    struct stat st;
    return native(path, p, sizeof p) && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

void plat_file_mkdir_p(const char *path) {
    char p[LPATH];
    if (!native(path, p, sizeof p)) return;
    for (char *c = p + 1; *c; c++) {
        if (*c != '/') continue;
        *c = 0;
        mkdir(p, 0755);
        *c = '/';
    }
    mkdir(p, 0755);
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    char p[LPATH];
    if (!native(path, p, sizeof p)) return NULL;
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (1ll << 30)) {
        buf = malloc((size_t)st.st_size + 1);
        size_t got = 0;
        while (buf && got < (size_t)st.st_size) {
            ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { free(buf); buf = NULL; break; }
            got += (size_t)r;
        }
        if (buf) {
            buf[st.st_size] = 0;
            *size = (uint64_t)st.st_size;
        }
    }
    close(fd);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    char p[LPATH];
    if (!native(path, p, sizeof p)) return 0;
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n);
    return close(fd) == 0 && ok;
}

int plat_file_replace(const char *from, const char *to) {
    char a[LPATH], b[LPATH];
    if (!native(from, a, sizeof a) || !native(to, b, sizeof b) || rename(a, b) != 0) return 0;
    sync_parent(b);
    return 1;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
    char p[LPATH], tmp[LPATH + 48];
    if (!native(path, p, sizeof p)) return 0;
    int len = snprintf(tmp, sizeof tmp, "%s.%d.%u.tmp", p, (int)getpid(), plat_thread_id());
    if (len < 0 || len >= (int)sizeof tmp) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n) && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok && rename(tmp, p) == 0) {
        sync_parent(p);
        return 1;
    }
    unlink(tmp);
    return 0;
}

int plat_file_flush(const char *path) {
    char p[LPATH];
    if (!native(path, p, sizeof p)) return 0;
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

void plat_file_delete(const char *path) {
    char p[LPATH];
    if (native(path, p, sizeof p)) unlink(p);
}

int plat_file_truncate(const char *path, uint64_t size) {
    char p[LPATH];
    return native(path, p, sizeof p) && truncate(p, (off_t)size) == 0;
}

int64_t plat_file_size(const char *path) {
    char p[LPATH];
    struct stat st;
    if (!native(path, p, sizeof p) || stat(p, &st) != 0) return -1;
    return (int64_t)st.st_size;
}

// Windows' read-only attribute: the write permission bits.
int plat_file_set_readonly(const char *path, int on) {
    char p[LPATH];
    struct stat st;
    if (!native(path, p, sizeof p) || stat(p, &st) != 0) return 0;
    mode_t m = st.st_mode & 07777;
    m = on ? m & ~(mode_t)0222 : m | S_IWUSR;
    return chmod(p, m) == 0;
}

// O_APPEND: each write() lands whole at the end of the file, atomically against other writers; nothing is buffered
// in user space, so what was written survives the process dying.
PlatAppend *plat_append_open(const char *path) {
    char p[LPATH];
    if (!native(path, p, sizeof p)) return NULL;
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    return fd < 0 ? NULL : (PlatAppend *)(intptr_t)(fd + 1);   // +1: fd 0 must not read as NULL
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    int fd = (int)(intptr_t)f - 1;
    ssize_t w;
    do w = write(fd, data, n); while (w < 0 && errno == EINTR);
    return w == (ssize_t)n;
}

// FindFirstFile's "*<ext>" matches case-insensitively; so does this.
int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    char p[LPATH];
    if (!native(dir, p, sizeof p)) return 0;
    DIR *d = opendir(p);
    if (!d) return 0;
    size_t el = strlen(ext);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
        size_t nl = strlen(e->d_name);
        if (nl < el || strcasecmp(e->d_name + nl - el, ext)) continue;
        if (!cb(e->d_name, ctx)) break;
    }
    closedir(d);
    return 1;
}

int plat_exe_path(char *out, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap);
    if (n <= 0 || (size_t)n >= cap) return 0;
    out[n] = 0;
    return 1;
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    size_t n = v ? strlen(v) : 0;
    if (!n || n >= cap) return 0;
    memcpy(out, v, n + 1);
    return n;
}

// LC_ALL, LC_MESSAGES, then LANG, as gettext reads them: "es_MX.UTF-8@euro" becomes "es-MX"; "C" and "POSIX" give "".
const char *plat_ui_language(void) {
    static char tag[32];
    static int done;
    if (!done) {
        const char *v = NULL, *names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
        for (int i = 0; i < 3 && !v; i++) {
            v = getenv(names[i]);
            if (v && !*v) v = NULL;
        }
        size_t n = 0;
        if (v && strcmp(v, "C") && strcmp(v, "POSIX") && strncmp(v, "C.", 2)) {
            for (; v[n] && v[n] != '.' && v[n] != '@' && n < sizeof tag - 1; n++) tag[n] = v[n] == '_' ? '-' : v[n];
        }
        tag[n] = 0;
        done = 1;
    }
    return tag;
}

// Noto CJK holds all four scripts in one collection; the paths are Arch's and SteamOS's, then Debian's and Fedora's.
int plat_font_path(const char *lang, char *out, size_t cap) {
    static const char *files[] = {
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto-cjk/NotoSerifCJK-Regular.ttc",
        "/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc",
    };
    if (strcmp(lang, "ja") && strcmp(lang, "ko") && strcmp(lang, "zh-Hans") && strcmp(lang, "zh-Hant")) return 0;
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        if (!plat_file_exists(files[i]) || strlen(files[i]) >= cap) continue;
        memcpy(out, files[i], strlen(files[i]) + 1);
        return 1;
    }
    return 0;
}

// /etc/os-release's PRETTY_NAME and the kernel release: "SteamOS (Linux 6.11.11-valve12-1-neptune-611)".
int plat_os_version(char *out, size_t cap) {
    char name[128] = "Linux";
    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "PRETTY_NAME=", 12)) continue;
            char *v = line + 12;
            v[strcspn(v, "\n")] = 0;
            if (*v == '"') { v++; v[strcspn(v, "\"")] = 0; }
            if (*v) snprintf(name, sizeof name, "%s", v);
            break;
        }
        fclose(f);
    }
    struct utsname u;
    if (uname(&u) != 0) return snprintf(out, cap, "%s", name) > 0;
    return snprintf(out, cap, "%s (Linux %s)", name, u.release) > 0;
}

int plat_stderr_path(char *out, size_t cap) {
    struct stat st;
    if (fstat(2, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    ssize_t n = readlink("/proc/self/fd/2", out, cap);
    if (n <= 0 || (size_t)n >= cap) return 0;
    out[n] = 0;
    return 1;
}

// ---- desktop. On Linux the clipboard belongs to the window system, which SDL (already the runtime's window and input
// layer) speaks for both X11 and Wayland; it is looked up at run time so this file builds and links without SDL.

int plat_clipboard_set(const char *utf8) {
    typedef bool (*set_t)(const char *);
    static set_t set;
    if (!set) set = (set_t)dlsym(RTLD_DEFAULT, "SDL_SetClipboardText");
    return set && set(utf8);
}

static void *reap_main(void *arg) {
    waitpid((pid_t)(intptr_t)arg, NULL, 0);
    return NULL;
}

// xdg-open shows the folder in the desktop's file manager (in a Steam Deck's Game Mode there is none: it does nothing).
int plat_open_folder(const char *path) {
    char p[LPATH];
    struct stat st;
    if (!native(path, p, sizeof p) || stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    char *argv[] = {"xdg-open", p, NULL};
    pid_t pid;
    if (posix_spawnp(&pid, "xdg-open", NULL, NULL, argv, environ) != 0) return 0;
    pthread_t t;   // the opener can take a while to exit: reaped off the caller's thread (the present path)
    if (pthread_create(&t, NULL, reap_main, (void *)(intptr_t)pid) == 0) pthread_detach(t);
    return 1;
}

// ---- dynamic libraries

void *plat_lib_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }

// ---- machine load: /proc for CPU and memory, sysfs for temperatures and the GPU (amdgpu: gpu_busy_percent and the
// VRAM counters; the Steam Deck's APU included). Vulkan gives no LUID on Linux, so the GPU is the amdgpu card with the
// most VRAM, which on a desktop with an iGPU and a dGPU is the dGPU.

struct PlatLoad {
    uint64_t busy, total, proc;   // the previous call's jiffies; total 0 before the first
    char gpu[128];                // /sys/class/drm/cardN/device, empty when none
};

static int read_u64(const char *path, uint64_t *v) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long x;
    int ok = fscanf(f, "%llu", &x) == 1;
    fclose(f);
    if (ok) *v = x;
    return ok;
}

PlatLoad *plat_load_open(const uint8_t *gpu_luid) {
    (void)gpu_luid;
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    uint64_t best = 0;
    for (int i = 0; i < 16; i++) {
        char dev[128], path[192];
        snprintf(dev, sizeof dev, "/sys/class/drm/card%d/device", i);
        uint64_t busy, vram = 0;
        snprintf(path, sizeof path, "%s/gpu_busy_percent", dev);
        if (!read_u64(path, &busy)) continue;
        snprintf(path, sizeof path, "%s/mem_info_vram_total", dev);
        read_u64(path, &vram);
        if (!l->gpu[0] || vram > best) {
            best = vram;
            snprintf(l->gpu, sizeof l->gpu, "%s", dev);
        }
    }
    return l;
}

void plat_load_close(PlatLoad *l) { free(l); }

static float hottest_c(void) {
    float best = -1;
    uint64_t mc;
    char path[96];
    for (int i = 0; i < 32; i++) {   // ACPI and other thermal zones
        snprintf(path, sizeof path, "/sys/class/thermal/thermal_zone%d/temp", i);
        if (read_u64(path, &mc) && mc / 1000.0f > best && mc < 150000) best = (float)mc / 1000.0f;
    }
    for (int i = 0; i < 32; i++) {   // CPU package sensors: AMD k10temp / zenpower (the Deck's too), Intel coretemp
        char name[32] = "";
        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/name", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f);
        if (strncmp(name, "k10temp", 7) && strncmp(name, "zenpower", 8) && strncmp(name, "coretemp", 8)) continue;
        snprintf(path, sizeof path, "/sys/class/hwmon/hwmon%d/temp1_input", i);
        if (read_u64(path, &mc) && mc / 1000.0f > best && mc < 150000) best = (float)mc / 1000.0f;
    }
    return best;
}

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    unsigned long long u, n, s, idle, iow, irq, sirq, steal;
    FILE *f = fopen("/proc/stat", "r");
    int ok = f && fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &s, &idle, &iow, &irq, &sirq,
                         &steal) == 8;
    if (f) fclose(f);
    unsigned long long pu = 0, ps = 0;
    f = fopen("/proc/self/stat", "r");
    if (f) {   // utime and stime are fields 14 and 15, after the parenthesised command name
        char buf[1024];
        size_t got = fread(buf, 1, sizeof buf - 1, f);
        buf[got] = 0;
        fclose(f);
        char *p = strrchr(buf, ')');
        if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &pu, &ps) != 2) ok = 0;
    } else ok = 0;
    if (ok) {
        uint64_t busy = u + n + s + irq + sirq + steal, total = busy + idle + iow, proc = pu + ps;
        if (l->total && total > l->total) {
            *total_pct = 100.0f * (float)(busy - l->busy) / (float)(total - l->total);
            *proc_pct = 100.0f * (float)(proc - l->proc) / (float)(total - l->total);
        }
        l->busy = busy, l->total = total, l->proc = proc;
    }
    *temp_c = hottest_c();
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    *load_pct = *vram_used_mb = -1;
    if (!l || !l->gpu[0]) return;
    char path[192];
    uint64_t v;
    snprintf(path, sizeof path, "%s/gpu_busy_percent", l->gpu);
    if (read_u64(path, &v)) *load_pct = (float)(v > 100 ? 100 : v);
    snprintf(path, sizeof path, "%s/mem_info_vram_used", l->gpu);
    if (read_u64(path, &v)) *vram_used_mb = (float)((double)v / 1048576.0);
}

// /proc/meminfo field in kB, -1 when missing.
static double meminfo_kb(const char *key) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    size_t kl = strlen(key);
    double v = -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == ':') { v = strtod(line + kl + 1, NULL); break; }
    fclose(f);
    return v;
}

static double self_status_kb(const char *key) {
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[128];
    size_t kl = strlen(key);
    double v = -1;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, kl) && line[kl] == ':') { v = strtod(line + kl + 1, NULL); break; }
    fclose(f);
    return v;
}

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    double total = meminfo_kb("MemTotal"), avail = meminfo_kb("MemAvailable"), rss = self_status_kb("VmRSS");
    *total_mb = total >= 0 ? (float)(total / 1024) : -1;
    *used_mb = total >= 0 && avail >= 0 ? (float)((total - avail) / 1024) : -1;
    *proc_mb = rss >= 0 ? (float)(rss / 1024) : -1;
}

// Linux overcommits by default, so its commit limit is advice, not the hard wall Windows' is; reported all the same.
void plat_commit_info(double *proc_mb, double *used_mb, double *limit_mb) {
    double anon = self_status_kb("RssAnon"), swap = self_status_kb("VmSwap");
    double used = meminfo_kb("Committed_AS"), limit = meminfo_kb("CommitLimit");
    *proc_mb = anon >= 0 ? (anon + (swap > 0 ? swap : 0)) / 1024 : -1;
    *used_mb = used >= 0 ? used / 1024 : -1;
    *limit_mb = limit >= 0 ? limit / 1024 : -1;
}

// ---- child processes: posix_spawn with O_CLOEXEC pipes, stderr to /dev/null. The lock keeps the spawns one at a
// time, as win32.c does, although close-on-exec already keeps other children from inheriting the pipe ends.

struct PlatChild {
    pid_t pid;
    int in, out;   // in: our write end, out: our read end
};

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    static pthread_mutex_t spawn = PTHREAD_MUTEX_INITIALIZER;
    char p[LPATH], a[256] = "";
    if (err) *err = 0;
    if (!native(exe, p, sizeof p) || (arg && snprintf(a, sizeof a, "%s", arg) >= (int)sizeof a)) return NULL;
    // a write to a child that died raises SIGPIPE, which would end the game: it becomes an EPIPE error instead
    struct sigaction sa;
    if (sigaction(SIGPIPE, NULL, &sa) == 0 && sa.sa_handler == SIG_DFL) signal(SIGPIPE, SIG_IGN);
    int to[2], from[2];
    pthread_mutex_lock(&spawn);
    if (pipe2(to, O_CLOEXEC)) { pthread_mutex_unlock(&spawn); if (err) *err = (uint32_t)errno; return NULL; }
    if (pipe2(from, O_CLOEXEC)) {
        close(to[0]), close(to[1]);
        pthread_mutex_unlock(&spawn);
        if (err) *err = (uint32_t)errno;
        return NULL;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, to[0], 0);
    posix_spawn_file_actions_adddup2(&fa, from[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = {p, arg ? a : NULL, NULL};
    pid_t pid;
    int e = posix_spawn(&pid, p, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(to[0]);
    close(from[1]);
    pthread_mutex_unlock(&spawn);
    if (e) {
        close(to[1]), close(from[0]);
        if (err) *err = (uint32_t)e;
        return NULL;
    }
    // Country-Box: the child (the shader translator, gfx_shader.c) runs below the game: a level's first second
    // translated its new shaders on every core at normal priority, and the game's threads waited 10-47 ms per frame
    // for a CPU (--sweep spike diagnostic, level 8-K). With a nice value it uses only what the game leaves idle; a
    // draw whose shader is late is skipped, not waited for (vk_pipeline.c async builds). CB_XLATE_NICE=<0..19>, 10.
    static int nice_v = -1;
    if (nice_v < 0) {
        const char *e = getenv("CB_XLATE_NICE");
        nice_v = e && *e ? atoi(e) : 10;
        if (nice_v < 0 || nice_v > 19) nice_v = 10;
    }
    if (nice_v) setpriority(PRIO_PROCESS, (id_t)pid, nice_v);
    PlatChild *c = malloc(sizeof *c);
    if (!c) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(to[1]), close(from[0]);
        return NULL;
    }
    c->pid = pid, c->in = to[1], c->out = from[0];
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    close(c->in);
    close(c->out);
    kill(c->pid, SIGKILL);
    waitpid(c->pid, NULL, 0);
    free(c);
}

int plat_child_write(PlatChild *c, const void *data, size_t n) { return write_all(c->in, data, n); }

long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    ssize_t r;
    do r = read(c->out, buf, cap); while (r < 0 && errno == EINTR);
    return (long)r;
}
