// Country-Box: conformance check of the platform layer (runtime/platform/platform.h) against its documented contract,
// on whatever implementation it is linked with (linux.c, win32.c). Needs no game files.
//
//   clang -std=gnu17 -O2 -march=x86-64-v3 -Iruntime tests/platform_check.c runtime/platform/linux.c -o build/platform_check
//   build/platform_check
//
// Exits 0 when every check passes. Prints the sleep overshoot and the lock costs, which matter on a Steam Deck.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "platform/platform.h"

static int failures, checks;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); putchar('\n'); } \
    } while (0)

// ---- threads and locks

#define LOCK_THREADS 8
#define LOCK_ITERS 200000

static PlatMutex g_lock = PLAT_MUTEX_INIT;   // zero storage must be a valid unlocked mutex
static uint64_t g_counter;

static void lock_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < LOCK_ITERS; i++) {
        plat_mutex_lock(&g_lock);
        g_counter++;
        plat_mutex_unlock(&g_lock);
    }
}

typedef struct {
    PlatMutex m;
    PlatCond c;
    int queue[64], head, tail, done;
    long sum;
} Queue;

static void consumer(void *arg) {
    Queue *q = arg;
    plat_mutex_lock(&q->m);
    for (;;) {
        while (q->head == q->tail && !q->done) plat_cond_wait(&q->c, &q->m);
        if (q->head == q->tail && q->done) break;
        q->sum += q->queue[q->head++ % 64];
        plat_cond_broadcast(&q->c);
    }
    plat_mutex_unlock(&q->m);
}

static void set_ran(void *arg) { atomic_store((atomic_int *)arg, 1); }

static void check_threads(void) {
    printf("threads, locks, condition variables\n");
    atomic_int ran = 0;
    PlatThread *t = plat_thread_new(set_ran, &ran, "plat-check-name-longer-than-15");
    CHECK(t, "plat_thread_new");
    if (t) plat_thread_join(t);
    CHECK(atomic_load(&ran) == 1, "thread fn ran before join returned");
    CHECK(plat_cpu_count() >= 1, "cpu count %u", plat_cpu_count());
    uint64_t prev = plat_thread_affinity(NULL, 1);
    CHECK(prev != 0, "affinity returns the previous mask");
    if (prev) CHECK(plat_thread_affinity(NULL, prev) == 1, "affinity reports the mask just set");
    CHECK(plat_thread_id() != 0, "thread id");

    PlatThread *w[LOCK_THREADS];
    uint64_t t0 = plat_now_ns();
    for (int i = 0; i < LOCK_THREADS; i++) w[i] = plat_thread_new(lock_worker, NULL, "lock");
    for (int i = 0; i < LOCK_THREADS; i++) plat_thread_join(w[i]);
    uint64_t dt = plat_now_ns() - t0;
    CHECK(g_counter == (uint64_t)LOCK_THREADS * LOCK_ITERS, "contended mutex lost updates: %llu",
          (unsigned long long)g_counter);
    printf("  contended mutex: %d threads x %d: %.1f ns per lock/unlock\n", LOCK_THREADS, LOCK_ITERS,
           (double)dt / ((double)LOCK_THREADS * LOCK_ITERS));

    PlatMutex m;
    plat_mutex_init(&m);
    CHECK(plat_mutex_trylock(&m) == 1, "trylock on a free mutex");
    CHECK(plat_mutex_trylock(&m) == 0, "trylock on a held mutex");
    plat_mutex_unlock(&m);
    t0 = plat_now_ns();
    for (int i = 0; i < 1000000; i++) { plat_mutex_lock(&m); plat_mutex_unlock(&m); }
    printf("  uncontended mutex: %.1f ns per lock/unlock\n", (double)(plat_now_ns() - t0) / 1e6);

    Queue *q = calloc(1, sizeof *q);   // zero storage: valid mutex and condition variable
    PlatThread *c[3];
    for (int i = 0; i < 3; i++) c[i] = plat_thread_new(consumer, q, "consumer");
    long want = 0;
    for (int i = 1; i <= 20000; i++) {
        plat_mutex_lock(&q->m);
        while (q->tail - q->head == 64) plat_cond_wait(&q->c, &q->m);
        q->queue[q->tail++ % 64] = i;
        want += i;
        plat_cond_signal(&q->c);
        plat_mutex_unlock(&q->m);
    }
    plat_mutex_lock(&q->m);
    q->done = 1;
    plat_cond_broadcast(&q->c);
    plat_mutex_unlock(&q->m);
    for (int i = 0; i < 3; i++) plat_thread_join(c[i]);
    CHECK(q->sum == want, "producer/consumer sum %ld, want %ld", q->sum, want);

    plat_mutex_lock(&q->m);
    t0 = plat_now_ns();
    int r = plat_cond_wait_ns(&q->c, &q->m, 20000000);   // nobody signals: a timeout after ~20 ms
    dt = plat_now_ns() - t0;
    plat_mutex_unlock(&q->m);
    CHECK(r == 0, "cond_wait_ns returns 0 on timeout");
    CHECK(dt >= 19000000 && dt < 60000000, "cond_wait_ns waited %.2f ms for 20", dt / 1e6);
    free(q);
}

// ---- time

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void check_time(void) {
    printf("time\n");
    uint64_t a = plat_now_ns(), b = plat_now_ns();
    CHECK(b >= a, "plat_now_ns monotonic");
    const uint64_t asks[] = {100000, 1000000, 4000000, 16666667};
    for (size_t k = 0; k < sizeof asks / sizeof *asks; k++) {
        uint64_t over[50];
        for (int i = 0; i < 50; i++) {
            uint64_t t0 = plat_now_ns();
            plat_sleep_ns(asks[k]);
            uint64_t d = plat_now_ns() - t0;
            CHECK(d >= asks[k], "sleep returned early: %llu < %llu", (unsigned long long)d, (unsigned long long)asks[k]);
            over[i] = d - asks[k];
        }
        qsort(over, 50, sizeof *over, cmp_u64);
        printf("  sleep %6.3f ms: overshoot median %5.1f us, max %6.1f us\n", asks[k] / 1e6, over[25] / 1e3,
               over[49] / 1e3);
        CHECK(over[49] < 1000000, "sleep overshoot %llu ns is not well under 1 ms", (unsigned long long)over[49]);
    }
    PlatScale s = plat_scale(625, 32);   // 19.2 MHz guest ticks from ns, as rt_ticks does
    CHECK(plat_scaled(s, 32) == 625, "plat_scaled exact");
}

// ---- memory

static void check_memory(void) {
    printf("memory\n");
    size_t n = 1 << 20;
    uint8_t *res = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(res != MAP_FAILED, "reserve");
    if (res == MAP_FAILED) return;
    uint8_t buf[16];
    CHECK(plat_mem_read((uint64_t)(uintptr_t)res, buf, sizeof buf) == 0, "mem_read of a reserved page fails");
    CHECK(plat_mem_commit((uint64_t)(uintptr_t)res + 100, 5000) == 1, "commit");
    res[100] = 0xab;           // committed: writable
    res[4096 + 1000] = 0xcd;   // second page touched by [100, 5100)
    CHECK(plat_mem_read((uint64_t)(uintptr_t)res + 100, buf, 1) == 1 && buf[0] == 0xab, "mem_read committed");
    CHECK(plat_mem_read((uint64_t)(uintptr_t)res + 8192, buf, 1) == 0, "mem_read past the committed pages fails");
    CHECK(plat_mem_read(0, buf, 1) == 0, "mem_read of address 0 fails without faulting");
    munmap(res, n);
}

// ---- files

static int count_cb(const char *name, void *ctx) {
    (void)name;
    (*(int *)ctx)++;
    return 1;
}

typedef struct { PlatAppend *f; int id; } AppendArg;
static void append_worker(void *arg) {
    AppendArg *a = arg;
    char line[64];
    for (int i = 0; i < 1000; i++) {
        int n = snprintf(line, sizeof line, "thread %d line %04d ......................\n", a->id, i);
        plat_append_write(a->f, line, (size_t)n);
    }
}

static void check_files(void) {
    printf("files\n");
    char dir[256], path[300], other[300];
    snprintf(dir, sizeof dir, "/tmp/plat_check_%u/sub\\deeper", plat_thread_id());   // either separator
    plat_file_mkdir_p(dir);
    snprintf(path, sizeof path, "%s/a.bin", dir);
    snprintf(other, sizeof other, "%s/b.bin", dir);
    CHECK(!plat_file_exists(dir), "a directory is not a file");
    CHECK(plat_file_write(path, "hello", 5) == 1, "write");
    CHECK(plat_file_exists(path), "exists after write");
    CHECK(plat_file_size(path) == 5, "size");
    uint64_t sz = 0;
    uint8_t *d = plat_file_read(path, &sz);
    CHECK(d && sz == 5 && !memcmp(d, "hello", 5) && d[5] == 0, "read with NUL after the data");
    free(d);
    CHECK(plat_file_write_atomic(path, "atomic!", 7) == 1 && plat_file_size(path) == 7, "write_atomic");
    CHECK(plat_file_flush(path) == 1, "flush");
    CHECK(plat_file_truncate(path, 3) == 1 && plat_file_size(path) == 3, "truncate");
    CHECK(plat_file_replace(path, other) == 1 && !plat_file_exists(path) && plat_file_exists(other), "replace");
    CHECK(plat_file_set_readonly(other, 1) == 1, "set readonly");
    CHECK(plat_file_write(other, "x", 1) == 0, "write to a read-only file fails");
    CHECK(plat_file_set_readonly(other, 0) == 1 && plat_file_write(other, "x", 1) == 1, "clear readonly");
    CHECK(plat_file_read(path, &sz) == NULL, "read of a missing file is NULL");
    CHECK(plat_file_size(path) == -1, "size of a missing file is -1");

    snprintf(path, sizeof path, "%s/log.TXT", dir);
    PlatAppend *f = plat_append_open(path);
    CHECK(f, "append_open");
    if (f) {
        PlatThread *t[4];
        AppendArg args[4];
        for (int i = 0; i < 4; i++) { args[i] = (AppendArg){f, i}; t[i] = plat_thread_new(append_worker, &args[i], "app"); }
        for (int i = 0; i < 4; i++) plat_thread_join(t[i]);
        d = plat_file_read(path, &sz);
        char sample[64];
        long want_len = snprintf(sample, sizeof sample, "thread %d line %04d ......................", 0, 0);
        int lines = 0, torn = 0;
        for (char *p = (char *)d, *e; p && *p; p = e + 1) {
            e = strchr(p, '\n');
            if (!e) { torn++; break; }
            if (e - p != want_len || strncmp(p, "thread ", 7)) torn++;
            lines++;
        }
        CHECK(lines == 4000 && !torn, "4 threads x 1000 appends: %d lines, %d torn", lines, torn);
        free(d);
    }
    int n = 0;
    CHECK(plat_file_list(dir, ".txt", count_cb, &n) == 1 && n == 1, "list by extension, case-insensitive: %d", n);
    n = 0;
    CHECK(plat_file_list(dir, ".bin", count_cb, &n) == 1 && n == 1, "list .bin: %d", n);
    CHECK(plat_file_list("/nonexistent/dir", ".x", count_cb, &n) == 0, "list of a missing directory");
    plat_file_delete(path);
    plat_file_delete(other);
    CHECK(!plat_file_exists(other), "delete");
}

// ---- process, desktop, libraries, load, children

static void check_process(void) {
    printf("process\n");
    char buf[1024];
    CHECK(plat_exe_path(buf, sizeof buf) == 1 && buf[0] == '/', "exe path");
    printf("  exe: %s\n", buf);
    CHECK(plat_getenv("PATH", buf, sizeof buf) > 0, "getenv PATH");
    CHECK(plat_getenv("PLAT_CHECK_SURELY_UNSET", buf, sizeof buf) == 0, "getenv of an unset variable");
    printf("  ui language: \"%s\"\n", plat_ui_language());
    CHECK(plat_os_version(buf, sizeof buf) == 1, "os version");
    printf("  os: %s\n", buf);
    if (plat_font_path("ja", buf, sizeof buf)) printf("  ja font: %s\n", buf);
    else printf("  ja font: none on this system\n");
    CHECK(plat_font_path("xx", buf, sizeof buf) == 0, "no font for an unknown language");
    void *libc = plat_lib_open("libc.so.6");
    CHECK(libc && plat_lib_sym(libc, "malloc"), "lib_open / lib_sym");
    CHECK(plat_lib_open("libsurely-missing-lib.so") == NULL, "missing library is NULL");

    PlatLoad *l = plat_load_open(NULL);
    float tot, proc, temp, gpu, vram, used, total, pm;
    plat_cpu_load(l, &tot, &proc, &temp);
    volatile double x = 0;
    for (int i = 0; i < 30000000; i++) x += i * 0.5;   // some CPU time between the two samples
    plat_sleep_ns(100000000);
    plat_cpu_load(l, &tot, &proc, &temp);
    plat_gpu_load(l, &gpu, &vram);
    plat_mem_info(&used, &total, &pm);
    double cp, cu, cl;
    plat_commit_info(&cp, &cu, &cl);
    printf("  cpu %.1f%% (this process %.1f%%), %.1f C | gpu %.0f%%, vram %.0f MB | ram %.0f / %.0f MB, "
           "process %.0f MB | commit %.0f MB (system %.0f / %.0f)\n",
           tot, proc, temp, gpu, vram, used, total, pm, cp, cu, cl);
    CHECK(tot >= 0 && tot <= 100.5 && proc >= 0, "cpu load in range");
    CHECK(total > 0 && used > 0 && pm > 0, "memory info");
    plat_load_close(l);

    uint32_t err = 0;
    PlatChild *c = plat_child_start("/bin/cat", NULL, &err);
    CHECK(c, "child start (err %u)", err);
    if (c) {
        CHECK(plat_child_write(c, "ping\n", 5) == 1, "child write");
        char rb[16] = {0};
        long got = 0;
        while (got < 5) {
            long r = plat_child_read(c, rb + got, sizeof rb - 1 - (size_t)got);
            if (r <= 0) break;
            got += r;
        }
        CHECK(got == 5 && !memcmp(rb, "ping\n", 5), "child echoes through the pipes");
        plat_child_stop(c);
    }
    CHECK(plat_child_start("/nonexistent/exe", NULL, &err) == NULL && err != 0, "missing child exe fails with err");
}

static int g_once_runs;
static void once_fn(void) { plat_sleep_ns(2000000); g_once_runs++; }
static PlatOnce g_once = PLAT_ONCE_INIT;
static void once_worker(void *arg) { (void)arg; plat_once(&g_once, once_fn); }

static void check_atomics(void) {
    printf("atomics, once\n");
    volatile int32_t v = 5;   // Windows LONG is 32 bits: the port keeps int32_t
    CHECK(plat_atomic_inc(&v) == 6 && v == 6, "inc returns the new value");
    CHECK(plat_atomic_dec(&v) == 5, "dec returns the new value");
    CHECK(plat_atomic_xchg(&v, 9) == 5 && v == 9, "xchg returns the old value");
    CHECK(plat_atomic_add(&v, 3) == 9 && v == 12, "add returns the old value");
    CHECK(plat_atomic_cas(&v, 20, 12) == 12 && v == 20, "cas(target, new, compare) stores on a match, returns old");
    CHECK(plat_atomic_cas(&v, 30, 12) == 20 && v == 20, "cas leaves the value on a mismatch, returns old");
    volatile int64_t w = -1;
    CHECK(plat_atomic_inc(&w) == 0, "64-bit inc");
    PlatThread *t[6];
    for (int i = 0; i < 6; i++) t[i] = plat_thread_new(once_worker, NULL, "once");
    for (int i = 0; i < 6; i++) plat_thread_join(t[i]);
    CHECK(g_once_runs == 1, "plat_once ran %d times", g_once_runs);
}

int main(void) {
    check_atomics();
    check_threads();
    check_time();
    check_memory();
    check_files();
    check_process();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
