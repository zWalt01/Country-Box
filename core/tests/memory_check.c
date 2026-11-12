// Country-Box: check of runtime/memory.c on Linux (reservations, heap, write tracking, dma-buf export, stack moves,
// QueryMemory), linked with the real memory.c and stubs for its neighbours. Needs a layout.h from a generated tree.
//
//   clang -std=gnu17 -O1 -march=x86-64-v3 -Iruntime -I<generated> tests/memory_check.c runtime/memory.c \
//         runtime/platform/linux.c -o build/memory_check && build/memory_check
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cfg.h"
#include "dispatch.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

// ---- stubs of what memory.c calls in the rest of the runtime
const RtLayoutEntry rt_layout[] = {
#define X(n, N) {#n, MOD_##N##_BASE, MOD_##N##_SIZE},
    MOD_LIST(X)
#undef X
};
const int rt_layout_count = MOD_COUNT;
static RtLoaded g_mods[MOD_COUNT];
int ldr_count(void) { return MOD_COUNT; }
const RtLoaded *ldr_module(int i) { return &g_mods[i]; }
uint64_t res_heap_extra_bytes(void) { return 0; }
void cfg_init(void) {}
const char *cfg_get(const char *name) { return getenv(name); }
int cfg_bool(const char *name) { const char *v = getenv(name); return v && *v && strcmp(v, "0"); }
void rt_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    fputs("  [log] ", stdout);
    vprintf(fmt, a);
    putchar('\n');
    va_end(a);
}
void rt_fatal(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    fputs("  FATAL: ", stdout);
    vprintf(fmt, a);
    putchar('\n');
    va_end(a);
    exit(2);
}

int mem_dmabuf(uint64_t addr, uint64_t size);   // memory.c (Linux)

static int failures, checks;
#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(cond)) { failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); putchar('\n'); } \
    } while (0)

int main(void) {
    int i = 0;
#define X(n, N) g_mods[i++] = (RtLoaded){.name = #n, .base = MOD_##N##_BASE, .size = MOD_##N##_SIZE, .data_off = MOD_##N##_SIZE / 2};
    MOD_LIST(X)
#undef X
    mem_init();
    const RtGuestLayout *L = mem_layout();
    printf("layout: stack %#llx heap %#llx alias %#llx pool %#llx\n", (unsigned long long)L->stack.base,
           (unsigned long long)L->heap.base, (unsigned long long)L->alias.base, (unsigned long long)L->pool);
    CHECK(L->heap.base > L->stack.base && L->alias.base > L->heap.base && L->alias.base < MOD_MAIN_BASE, "region order");

    // heap: grow, write, shrink, grow again -> zeroed
    uint64_t heap = 0;
    CHECK(mem_set_heap_size(0x123, &heap) != 0, "unaligned heap size refused");
    CHECK(mem_set_heap_size(64ull << 20, &heap) == 0 && heap == L->heap.base, "heap 64 MiB");
    uint64_t lo, hi;
    CHECK(mem_heap_range(&lo, &hi) && hi - lo == 64ull << 20, "heap range");
    volatile uint8_t *h = (volatile uint8_t *)(uintptr_t)heap;
    h[0] = 1;
    h[(32ull << 20) + 5] = 7;
    CHECK(mem_set_heap_size(16ull << 20, NULL) == 0, "shrink to 16 MiB");
    CHECK(mem_set_heap_size(64ull << 20, NULL) == 0, "grow back");
    CHECK(h[0] == 1 && h[(32ull << 20) + 5] == 0, "kept page kept its data, decommitted page came back zeroed");
    CHECK(mem_set_heap_size(5ull << 30, NULL) != 0, "a heap past the cap is refused (OutOfMemory)");

    // write tracking over the heap
    static uint64_t pages[4096];
    uint64_t n = mem_written_pages(heap, 64ull << 20, pages, 4096);   // first query: everything counts as written
    CHECK(n != UINT64_MAX, "heap is watched (write tracking %s)", n == UINT64_MAX ? "off" : "on");
    if (n != UINT64_MAX) {
        while (mem_written_pages(heap, 64ull << 20, pages, 4096) == 4096) {}   // drain the initial report
        CHECK(mem_written_pages(heap, 64ull << 20, pages, 4096) == 0, "nothing written: nothing reported");
        h[0x5000] = 9;
        h[0x5800] = 9;   // same page
        h[(10ull << 20) + 0x10] = 9;
        n = mem_written_pages(heap, 64ull << 20, pages, 4096);
        CHECK(n == 2 && pages[0] == heap + 0x5000 && pages[1] == heap + (10ull << 20), "two pages written: %llu",
              (unsigned long long)n);
        h[0x9000] = 1;
        CHECK(mem_written_peek(heap, 64ull << 20, pages, 16) == 1, "peek sees the write");
        CHECK(mem_written_peek(heap, 64ull << 20, pages, 16) == 1, "peek does not reset");
        CHECK(mem_written_pages(heap, 64ull << 20, pages, 16) == 1 &&
              mem_written_pages(heap, 64ull << 20, pages, 16) == 0, "reset after the query");
        uint64_t t0 = plat_now_ns();
        for (int k = 0; k < 100; k++) mem_written_pages(heap, 64ull << 20, pages, 4096);
        printf("  query over 64 MiB, nothing written: %.1f us\n", (plat_now_ns() - t0) / 100.0 / 1e3);
    }
    CHECK(mem_written_pages(MOD_MAIN_BASE, 4096, pages, 1) == UINT64_MAX, "module window is not watched");

    // dma-buf export of heap memory (udmabuf)
    int fd = mem_dmabuf(heap, 16ull << 20);
    printf("  udmabuf of 16 MiB of heap: %s\n", fd >= 0 ? "ok" : "unavailable");
    if (fd >= 0) close(fd);
    CHECK(mem_dmabuf(MOD_MAIN_BASE, 4096) == -1, "no dma-buf of unwatched memory");

    // physical memory in the alias region
    uint64_t a = L->alias.base + (1ull << 30);
    CHECK(mem_map_physical(a, 2ull << 20) == 0, "MapPhysicalMemory 2 MiB");
    ((volatile uint8_t *)(uintptr_t)a)[100] = 5;
    RtMemoryInfo mi;
    mem_query(a + 0x1000, &mi);
    CHECK(mi.base == a && mi.size == 2ull << 20 && mi.state == 5 && mi.perm == 3, "QueryMemory mapped alias: %#llx+%#llx st %u",
          (unsigned long long)mi.base, (unsigned long long)mi.size, mi.state);
    CHECK(mem_unmap_physical(a, 2ull << 20) == 0, "UnmapPhysicalMemory");
    mem_query(a, &mi);
    CHECK(mi.state == 0, "QueryMemory after unmap: free");
    CHECK(mem_map_physical(a, 4096) == 0 && ((volatile uint8_t *)(uintptr_t)a)[100] == 0, "remapped page is zero");
    mem_unmap_physical(a, 4096);

    // stack move from the heap into the stack region
    uint64_t dst = L->stack.base + 0x100000, src = heap + 0x200000;
    ((volatile uint8_t *)(uintptr_t)src)[8] = 0x42;
    CHECK(mem_map(dst, src, 0x10000) == 0, "MapMemory (stack move)");
    CHECK(((volatile uint8_t *)(uintptr_t)dst)[8] == 0x42, "stack copy holds the data");
    mem_query(dst, &mi);
    CHECK(mi.state == 0xB && mi.base == dst, "QueryMemory: Stack");
    ((volatile uint8_t *)(uintptr_t)dst)[8] = 0x43;
    CHECK(mem_unmap(dst, src, 0x10000) == 0, "UnmapMemory");
    CHECK(((volatile uint8_t *)(uintptr_t)src)[8] == 0x43, "source got the stack's contents back");

    // kernel region allocator
    uint8_t *k1 = mem_kernel_alloc(0x200, 16), *k2 = mem_kernel_alloc(0x2000, 0x1000);
    CHECK(k1 && k2 && ((uintptr_t)k2 & 0xFFF) == 0 && (uint64_t)(uintptr_t)k1 >= RT_KERNEL_BASE, "kernel allocations");
    k1[0x1ff] = 1, k2[0x1fff] = 1;
    mem_kernel_free(k1, 0x200);
    CHECK(mem_kernel_alloc(0x200, 16) == k1, "freed block is reused");

    // virtual pool shadow
    uint64_t sh = mem_shadow_reserve(10ull << 20);
    CHECK(sh && mem_is_shadow(sh, 10ull << 20), "shadow reservation");
    if (sh) {
        ((volatile uint8_t *)(uintptr_t)sh)[123] = 1;
        mem_shadow_release(sh);
        CHECK(((volatile uint8_t *)(uintptr_t)sh)[123] == 0, "released shadow reads back zero");
        CHECK(mem_shadow_reserve(4ull << 20) == sh, "released shadow is reused");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
