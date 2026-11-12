// From fe3h-pc-recomp runtime/memory.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4; SetHeapSize refuses a size it cannot grant with OutOfMemory; shadow reservations for virtual pools mapped from CPU-visible pools; adds mem_is_shadow and mem_heap_range.
// From totk-pc-recomp runtime/memory.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh); renamed to fe3h; MapMemory also takes stacks from module data (FE3H).
// Adapted from MacNativeGames games/totk/runtime/services.c and loader.c @ b75be8c (MIT, Copyright (c) 2026
// MacNativeGames contributors), and LAS Recomp runtime/main.c and platform/windows.c @ da35b83 (MIT, Copyright (c)
// 2026 Peiikai).
// Guest address space and the memory SVCs: heap, module data permissions, stack moves,
// QueryMemory, physical memory in the alias region, GetInfo memory ids, and the kernel-region allocator.
//
// Every region is a plain reservation; pages are committed and decommitted at 4 KiB granularity. Decommitted pages
// come back zeroed, so no stale-page bookkeeping is needed. The stack and heap block and the alias region are
// reserved with MEM_WRITE_WATCH: mem_written_pages reports the pages the CPU wrote, which
// the Vulkan back end uploads again.
// Country-Box: Linux only. Reservations are mmap(PROT_NONE); the watched regions (stack and heap block, alias
// region, virtual pool shadows) are memfd-backed so the GPU can take them as udmabuf dma-bufs (mem_dmabuf), and their
// writes are tracked with userfaultfd's asynchronous write protection read by PAGEMAP_SCAN (Linux 6.7+), the Linux
// GetWriteWatch. Decommit punches the pages out of the memfd, so they come back zeroed. The Windows build's frozen
// regions (read-only 2 MiB regions that skip write-watch queries) are not used: PAGEMAP_SCAN walks only the range it is
// asked about. Technique of the udmabuf heap and the PAGEMAP_SCAN tracking from PrimeRecomp runtime/platform/linux.c
// (gminustwo/primerecomp, MIT, Copyright (c) 2026 Peiikai), which measured the host-pointer alternative on a Steam
// Deck: amdgpu walks every page of a userptr buffer at each submission, ~5 ms for a 3.3 GB heap.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/udmabuf.h>
#include <linux/userfaultfd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "cfg.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

#define PAGE 0x1000ull
#define GiB (1ull << 30)
#define LOW_SEARCH_FROM 0x100000000ull
#define LOW_SIZE (6 * GiB)
#define STACK_OFF 0x10000000ull
#define STACK_SIZE 0x10000000ull
#define HEAP_OFF (STACK_OFF + STACK_SIZE + 0x10000000ull)
#define HEAP_CAP (4 * GiB)
#define ALIAS_SIZE (64 * GiB)
#define ALIAS_ALIGN (4 * GiB)
#define ALIAS_PAGES (ALIAS_SIZE / PAGE)
#define DEFAULT_POOL 0xCD500000ull       // retail application pool
#define SYSTEM_RESOURCE_BUDGET (32ull << 20)
#define MAX_STACK_MAPS 4096
#define FRZ_SIZE (2ull << 20)          // the 2 MiB unit of the write-watch snapshots (vk_memory.c snap_regions)
#define MAX_WATCHED 72                   // the stack and heap block, the alias region and up to 64 shadows (+ spare)

// Horizon memory states, permissions, attributes and results used here.
enum { ST_FREE = 0, ST_CODE = 3, ST_CODEDATA = 4, ST_NORMAL = 5, ST_STACK = 0xB, ST_THREADLOCAL = 0xC,
       ST_INACCESSIBLE = 0x10 };
enum { PERM_NONE = 0, PERM_R = 1, PERM_RW = 3, PERM_RX = 5 };
enum { ATTR_PERMISSION_LOCKED = 16 };
#undef R_OK   // unistd.h access() mode; here R_OK is the Horizon result 0
#define R_OK 0u
#define R_INVALID_ADDRESS 0xcc01u   // misaligned address
#define R_INVALID_SIZE 0xca01u
#define R_OUT_OF_MEMORY 0xd001u    // the kernel's answer to a heap it cannot grow to
#define R_INVALID_NEW_PERM 0xd801u
#define R_INVALID_STATE 0xd401u     // InvalidCurrentMemory
#define R_INVALID_REGION 0xdc01u
#define R_LIMIT_REACHED 0x10801u

static RtGuestLayout L;
static uint64_t mods_lo, mods_hi;   // fixed module window
static uint64_t shadow_next, shadow_end;   // virtual pool shadow reservations (mem_shadow_reserve): next candidate, limit
static uint64_t shadow_lo[64], shadow_hi[64];   // the reservations made, for watched()
static uint8_t shadow_free[64];               // released by nvnMemoryPoolFinalize, kept for the next pool that fits
static int nshadow;
static uint32_t shadow_events;               // reservations and releases (mem_shadow_events: input script sync points)
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t heap_size, alias_bytes;
static uint8_t *alias_pages;        // 1 per mapped 4 KiB page of the alias region
typedef struct { uint64_t dst, src, size; } StackMap;
static StackMap stack_maps[MAX_STACK_MAPS];
static unsigned nstack_maps;
static uint8_t *module_perm[MOD_COUNT];   // per data page of each module: perm | ATTR_PERMISSION_LOCKED

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// ---- watched reservations: memfd-backed, write-tracked
static int use_memfd(void);
// Country-Box: precise write tracking (see reserve_at and commit) is on whenever guest memory is memfd-backed (udmabuf
// imports): there it costs nothing measurable (level 1-1, RX 9070 XT: 60.0 fps, 1 % low 56.3 either way), and without
// it a fresh page read in by fault-around counts as written (--probe-gfx-abi: 8 pages for one write). Over anonymous
// memory (host pointer imports) it cost 53 -> 32 fps on the title screen and stays off. CB_PRECISE_WRITE_WATCH=0/1
// overrides.
static int precise_watch(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("CB_PRECISE_WRITE_WATCH");
        v = e && *e ? !strcmp(e, "1") : use_memfd();
    }
    return v;
}
typedef struct { uint64_t lo, hi; int fd; } Watched;
static Watched g_watched[MAX_WATCHED];
static int g_nwatched;
static int g_uffd = -1, g_pagemap = -1;   // write tracking; -1: off (every query answers "untracked")

static const Watched *watched_of(uint64_t addr, uint64_t size) {
    for (int i = 0; i < __atomic_load_n(&g_nwatched, __ATOMIC_ACQUIRE); i++) {
        const Watched *w = &g_watched[i];
        if (addr >= w->lo && size <= w->hi - w->lo && addr - w->lo <= w->hi - w->lo - size) return w;
    }
    return NULL;
}

static int watched(uint64_t addr, uint64_t size) { return watched_of(addr, size) != NULL; }

// userfaultfd in user mode (allowed with vm.unprivileged_userfaultfd=0) with asynchronous write protection: the kernel
// lifts the protection of a page at its first write without a handler, and PAGEMAP_SCAN reads and re-arms it.
static void track_init(void) {
    if (g_uffd >= 0 || cfg_bool("MP4_NO_WRITE_WATCH")) return;
    int u = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    struct uffdio_api api = {.api = UFFD_API,
                             .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED};
    int pm = u >= 0 ? open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC) : -1;
    if (u < 0 || pm < 0 || ioctl(u, UFFDIO_API, &api) != 0) {
        rt_log("memory: write tracking unavailable (userfaultfd %s); every guest buffer counts as written",
               u < 0 ? strerror(errno) : "API refused");
        if (pm >= 0) close(pm);
        if (u >= 0) close(u);
        return;
    }
    g_uffd = u, g_pagemap = pm;
}

// Country-Box: watched regions are memfd-backed when /dev/udmabuf can be opened (CB_GUEST_MEMFD=0 forces anonymous
// memory, =1 forces memfd), so the Vulkan back end imports guest memory as udmabuf dma-bufs (mem_dmabuf). The other
// import, a host pointer (RADV's VK_EXT_external_memory_host = amdgpu userptr), works only over anonymous memory and
// couples with the write tracking: each PAGEMAP_SCAN re-protection invalidates the userptr BO, and the next submission
// faults its pages back in for writing, so every page of the import reads as CPU-written after any GPU use
// (tests/userptr_wp_check.c, RX 9070 XT RADV: 8192 of 8192 pages per submission, 0 with udmabuf). In Returns HD that
// re-uploaded five 16 MiB textures every frame. udmabuf caps each dma-buf at size_limit_mb (64 MiB by default); a pool
// above it gets no GPU import over memfd (vk_memory.c logs it).
static int use_memfd(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("CB_GUEST_MEMFD");
        if (e && *e) v = !strcmp(e, "1");
        else {
            int fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
            v = fd >= 0;
            if (fd >= 0) close(fd);
        }
    }
    return v;
}

static void *reserve_at(uint64_t at, uint64_t size, int watch) {
    int fd = -1;
    if (watch && g_nwatched == MAX_WATCHED) return NULL;
    if (watch && use_memfd()) {
        if (g_nwatched == MAX_WATCHED) return NULL;
        fd = memfd_create("country-box-guest", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0 || ftruncate(fd, (off_t)size) != 0 || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
            if (fd >= 0) close(fd);
            return NULL;
        }
    }
    void *p = mmap((void *)(uintptr_t)at, size, PROT_NONE,
                   (fd >= 0 ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS) | MAP_NORESERVE | MAP_FIXED_NOREPLACE, fd, 0);
    if (p == MAP_FAILED || p != (void *)(uintptr_t)at) {
        if (p != MAP_FAILED) munmap(p, size);
        if (fd >= 0) close(fd);
        return NULL;
    }
    if (watch) {
        // Country-Box: with precise tracking (precise_watch), no transparent huge pages under anonymous watched memory,
        // so a write to one page reads as one (with THP "always" a huge page faulted in reported runs: probe-gfx-abi 8
        // for 1). memfd memory gets 4 KiB pages anyway (shmem THP "advise").
        if (fd < 0 && precise_watch() && madvise(p, size, MADV_NOHUGEPAGE) != 0)
            rt_log("memory: MADV_NOHUGEPAGE refused over %#llx+%#llx (%s)", (unsigned long long)at,
                   (unsigned long long)size, strerror(errno));
        if (g_uffd >= 0) {
            struct uffdio_register reg = {.range = {at, size}, .mode = UFFDIO_REGISTER_MODE_WP};
            if (ioctl(g_uffd, UFFDIO_REGISTER, &reg) != 0)
                rt_log("memory: write tracking not registered over %#llx+%#llx (%s)", (unsigned long long)at,
                       (unsigned long long)size, strerror(errno));
        }
        g_watched[g_nwatched] = (Watched){at, at + size, fd};
        __atomic_store_n(&g_nwatched, g_nwatched + 1, __ATOMIC_RELEASE);
    }
    return p;
}

// ---- frozen regions: not used on Linux (see the top of the file); the interface stays for vk_memory.c.
void mem_thaw(uint64_t a, uint64_t n) { (void)a, (void)n; }
int mem_freeze(uint64_t a) { (void)a; return 0; }
int mem_frozen(uint64_t a) { (void)a; return 0; }
void mem_freeze_stats(uint64_t *frozen, uint64_t *freezes, uint64_t *fault_thaws, uint64_t *other_thaws) {
    *frozen = *freezes = *fault_thaws = *other_thaws = 0;
}

// Country-Box: with CB_PRECISE_WRITE_WATCH=1, committed watched pages start write-protected, as a fresh
// MEM_WRITE_WATCH commit reads unwritten: PAGEMAP_SCAN counts an anonymous page that is neither present nor marked as
// written, so without the marker (UFFD_FEATURE_WP_UNPOPULATED installs it on empty entries) a 2 MiB heap commit read
// as 512 written pages. On by default with memfd memory (precise_watch); over anonymous memory the markers interacted
// with amdgpu's userptr imports (title 53 -> 32 fps).
static void commit(uint64_t a, uint64_t n, const char *what) {
    if (n && mprotect((void *)(uintptr_t)a, n, PROT_READ | PROT_WRITE) != 0)
        rt_fatal("memory: commit %s %#llx+%#llx failed (%s)", what, (unsigned long long)a, (unsigned long long)n,
                 strerror(errno));
    if (n && g_uffd >= 0 && precise_watch() && watched(a, n)) {
        struct uffdio_writeprotect wp = {.range = {a, n}, .mode = UFFDIO_WRITEPROTECT_MODE_WP};
        if (ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp) != 0)
            rt_log("memory: write protection of %s %#llx+%#llx failed (%s)", what, (unsigned long long)a,
                   (unsigned long long)n, strerror(errno));
    }
}

// Decommitted pages read back as zeros at their next commit: a watched range punches them out of its memfd, private
// memory drops them (MADV_DONTNEED on anonymous memory gives zero pages).
// Country-Box: a range that a live Vulkan import still covers (vk_memory.c vk_mem_import_overlap) is zeroed and stays
// mapped read-write instead. A udmabuf pins its memfd pages: a punched hole would give the CPU fresh pages while the
// GPU keeps the old ones. A userptr import over PROT_NONE pages makes amdgpu reject every submission (EFAULT). Unity
// may release a pool's storage before finalizing the pool. Weak: the probes link this file without the Vulkan back end.
int vk_mem_import_overlap(uint64_t a, uint64_t n, uint64_t *lo, uint64_t *hi) __attribute__((weak));
static void decommit(uint64_t a, uint64_t n, const char *what) {
    if (!n) return;
    const Watched *w = watched_of(a, n);
    uint64_t ilo, ihi;
    if (vk_mem_import_overlap && vk_mem_import_overlap(a, n, &ilo, &ihi)) {
        static int logged;
        if (logged++ < 8)
            rt_log("memory: decommit %s %#llx+%#llx overlaps the live GPU import %#llx-%#llx: zeroed, stays mapped",
                   what, (unsigned long long)a, (unsigned long long)n, (unsigned long long)ilo, (unsigned long long)ihi);
        if (w && w->fd >= 0) memset((void *)(uintptr_t)a, 0, n);
        else if (madvise((void *)(uintptr_t)a, n, MADV_DONTNEED) != 0)
            rt_fatal("memory: decommit %s %#llx+%#llx failed (%s)", what, (unsigned long long)a, (unsigned long long)n,
                     strerror(errno));
        return;
    }
    int ok = w && w->fd >= 0 ? fallocate(w->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)(a - w->lo), (off_t)n) == 0
               : madvise((void *)(uintptr_t)a, n, MADV_DONTNEED) == 0;
    ok = ok && mprotect((void *)(uintptr_t)a, n, PROT_NONE) == 0;
    if (!ok)
        rt_fatal("memory: decommit %s %#llx+%#llx failed (%s)", what, (unsigned long long)a, (unsigned long long)n,
                 strerror(errno));
}

static void protect(uint64_t a, uint64_t n, int prot, const char *what) {
    if (n && mprotect((void *)(uintptr_t)a, n, prot) != 0)
        rt_fatal("memory: protect %s %#llx+%#llx failed (%s)", what, (unsigned long long)a, (unsigned long long)n,
                 strerror(errno));
}

// Linux keeps the low 2^39 of the address space free of libraries, heaps and stacks (they sit near 2^47), so a taken
// region is not a random collision to retry as on Windows: it is reported.
static void region_taken(const char *what) {
    rt_fatal("memory: %s is not free in the address space (another mapping is there)", what);
}

void mem_init(void) {
    if (L.pool) return;
    // Fixed window: modules, kernel region, host-function addresses.
    mods_lo = MOD_MAIN_BASE;
    mods_hi = align_up(rt_layout[rt_layout_count - 1].base + rt_layout[rt_layout_count - 1].size, 0x10000);
    if (mods_hi > RT_KERNEL_BASE) rt_fatal("memory: modules end at %#llx, inside the kernel region",
                                           (unsigned long long)mods_hi);
    if (!reserve_at(mods_lo, mods_hi - mods_lo, 0)) region_taken("the module window");
    if (!reserve_at(RT_KERNEL_BASE, RT_KERNEL_SIZE, 0)) region_taken("the kernel region");
    if (!reserve_at(RT_HOSTFN_BASE, RT_HOSTFN_SIZE, 0)) region_taken("the host-function window");

    // Stack and heap block, then the alias region above it, both searched below the module window.
    track_init();
    uint64_t low = 0, alias = 0;
    for (uint64_t at = LOW_SEARCH_FROM; at + LOW_SIZE <= mods_lo && !low; at += GiB)
        if (reserve_at(at, LOW_SIZE, 1)) low = at;
    if (!low) region_taken("a 6 GiB stack and heap block below 2^39");
    for (uint64_t at = align_up(low + LOW_SIZE, ALIAS_ALIGN); at + ALIAS_SIZE <= mods_lo && !alias; at += ALIAS_ALIGN)
        if (reserve_at(at, ALIAS_SIZE, 1)) alias = at;
    if (!alias) region_taken("a 64 GiB alias region below 2^39");
    shadow_next = align_up(alias + ALIAS_SIZE, GiB), shadow_end = mods_lo;

    alias_pages = mmap(NULL, ALIAS_PAGES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (alias_pages == MAP_FAILED) rt_fatal("memory: cannot allocate the alias page map");
    // mp4.ini is read here, once the address space is claimed and before the first setting: the pool size below
    // depends on MP4_APP_POOL and on the resolution settings (resolution.c decides them once, on this first call).
    cfg_init();
    const char *pool = cfg_get("MP4_APP_POOL");
    L = (RtGuestLayout){
        .aslr = {low, RT_ADDRESS_LIMIT - low},
        .stack = {low + STACK_OFF, STACK_SIZE},
        .heap = {low + HEAP_OFF, HEAP_CAP},
        .alias = {alias, ALIAS_SIZE},
        .kernel = {RT_KERNEL_BASE, RT_KERNEL_SIZE},
        .hostfn = {RT_HOSTFN_BASE, RT_HOSTFN_SIZE},
        .pool = pool ? strtoull(pool, NULL, 0) : DEFAULT_POOL + res_heap_extra_bytes(),   // resolution.c
    };
    if (!L.pool) rt_fatal("memory: MP4_APP_POOL is 0");
    rt_log("memory: stack and heap block %#llx, alias %#llx (%s); write tracking %s", (unsigned long long)low,
           (unsigned long long)alias, use_memfd() ? "memfd, dma-buf exportable" : "anonymous",
           g_uffd >= 0 ? "userfaultfd + PAGEMAP_SCAN" : "off");
}

const RtGuestLayout *mem_layout(void) { return &L; }

// A dma-buf of [addr, addr+size) of a watched (memfd-backed) range, for the Vulkan back end to import as ordinary GPU
// memory; -1 when the range is not watched, unaligned, or udmabuf is missing or refuses (its size_limit_mb caps each
// buffer, 64 MiB by default: the caller asks for pieces). The caller owns the descriptor.
int mem_dmabuf(uint64_t addr, uint64_t size) {
    const Watched *w = watched_of(addr, size);
    if (!w || w->fd < 0 || !size || ((addr | size) & (PAGE - 1))) return -1;
    int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev < 0) return -1;
    struct udmabuf_create c = {.memfd = (uint32_t)w->fd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = addr - w->lo,
                               .size = size};
    int fd = ioctl(dev, UDMABUF_CREATE, &c);
    close(dev);
    return fd;
}

// Shadow storage of a virtual pool mapped from CPU-visible pool memory (nvn_api_core.c): a watched reservation below
// the address limit between the alias region and the module window, so textures over it are uploaded from it like
// any CPU-written storage and the pages the mapping copies are tracked. The whole pool is committed at once (zero
// pages cost nothing until touched): a texture upload reads its whole storage, mapped or not. The reservation is
// rounded up to whole 2 MiB regions, the unit of the write-watch snapshots (vk_memory.c snap_regions) and freezes
// (FRZ_SIZE): a query over the last region of a pool that ends inside one (this game's 207 MiB light volume pools)
// would reach past the reservation, GetWriteWatch refuses it, and every texture over the pool counted as untracked,
// so each was uploaded again, whole, at every use (5 x 207 MiB a frame, about 5 fps, after the first boss).
uint64_t mem_shadow_reserve(uint64_t size) {
    size = align_up(size, FRZ_SIZE);
    plat_mutex_lock(&lock);
    for (int i = 0; i < nshadow; i++)   // a released reservation that holds the pool (smallest first fit)
        if (shadow_free[i] && shadow_hi[i] - shadow_lo[i] >= size) {
            int best = i;
            for (int j = i + 1; j < nshadow; j++)
                if (shadow_free[j] && shadow_hi[j] - shadow_lo[j] >= size && shadow_hi[j] - shadow_lo[j] < shadow_hi[best] - shadow_lo[best])
                    best = j;
            shadow_free[best] = 0;
            plat_mutex_unlock(&lock);
            rt_log("memory: virtual pool shadow %#llx+%#llx (reused)", (unsigned long long)shadow_lo[best], (unsigned long long)size);
            __atomic_add_fetch(&shadow_events, 1, __ATOMIC_RELAXED);
            return shadow_lo[best];
        }
    plat_mutex_unlock(&lock);
    for (uint64_t at = shadow_next; at + size <= shadow_end; at += GiB)
        if (reserve_at(at, size, 1)) {
            shadow_next = align_up(at + size, GiB);
            commit(at, size, "virtual pool shadow");
            plat_mutex_lock(&lock);
            if (nshadow < 64) shadow_lo[nshadow] = at, shadow_hi[nshadow] = at + size, shadow_free[nshadow] = 0, nshadow++;
            plat_mutex_unlock(&lock);
            rt_log("memory: virtual pool shadow %#llx+%#llx", (unsigned long long)at, (unsigned long long)size);
            __atomic_add_fetch(&shadow_events, 1, __ATOMIC_RELAXED);
            return at;
        }
    return 0;
}
int mem_shadow_commit(uint64_t addr, uint64_t size) {
    return mprotect((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE) == 0;
}
// A finalized pool's shadow is not given back: textures over it can outlive the pool in the host's tables, and the
// first upload of one after the pool is gone read freed pages (nvnQueueSubmitCommands faulting in tex_bl_copy on the
// way out of Sol Valley, issue #7; on the console that read meets unmapped GPU memory, not a fault). Its pages go back
// to zero pages (no memory until touched) and the reservation is kept for the next virtual pool that fits, which keeps
// the address space bounded. A reservation past the 64 tracked is freed as before.
void mem_shadow_release(uint64_t addr) {
    plat_mutex_lock(&lock);
    int i = 0;
    while (i < nshadow && shadow_lo[i] != addr) i++;
    if (i < nshadow) {
        uint64_t size = shadow_hi[i] - shadow_lo[i];
        decommit(addr, size, "virtual pool shadow");   // back to zero pages, no memory until touched
        commit(addr, size, "virtual pool shadow");
        uint64_t none[1];
        mem_written_pages(addr, size, none, 0);   // re-arm the write tracking (max 0: only the reset)
        shadow_free[i] = 1;
    }
    plat_mutex_unlock(&lock);
    rt_log("memory: virtual pool shadow %#llx released%s", (unsigned long long)addr, i < nshadow ? " (kept as zero pages)" : "");
    __atomic_add_fetch(&shadow_events, 1, __ATOMIC_RELAXED);
    if (i == nshadow)   // past the 64 tracked: freed (its Watched entry stays; queries on it then fail as untracked)
        for (int k = 0; k < g_nwatched; k++)
            if (g_watched[k].lo == addr) munmap((void *)(uintptr_t)addr, g_watched[k].hi - g_watched[k].lo);
}
// 1 when [addr, addr+size) lies in one shadow reservation: committed as zero pages, so a page nothing wrote (no write
// generation, vk_memory.c) holds zeros (vk_texture.c upload_part).
// The committed guest heap [lo, hi); 0 before SetHeapSize. vk_memory.c arena_get imports chunks of it.
int mem_heap_range(uint64_t *lo, uint64_t *hi) {
    plat_mutex_lock(&lock);
    *lo = L.heap.base, *hi = L.heap.base + heap_size;
    plat_mutex_unlock(&lock);
    return *hi > *lo;
}

// Virtual pool creations and finalizations so far: the game makes them at loads, in the same order in every run, so
// input recordings resynchronise on them (input_win.c, "SYNC <n>" lines).
uint32_t mem_shadow_events(void) { return __atomic_load_n(&shadow_events, __ATOMIC_RELAXED); }

int mem_is_shadow(uint64_t addr, uint64_t size) {
    for (int i = 0; i < nshadow; i++)
        if (addr >= shadow_lo[i] && addr + size <= shadow_hi[i] && addr + size >= addr) return 1;
    return 0;
}

// The pages of [addr, addr+size) written since the last reset, as GetWriteWatch reports them (LAS Recomp
// runtime/platform/windows.c @ da35b83, MIT, Copyright (c) 2026 Peiikai, the interface): PAGEMAP_SCAN over the
// userfaultfd-protected range, re-arming what it reports when reset is set. A range inside one watched reservation
// only; UINT64_MAX when the range is not watched or tracking is off. More written pages than max: the rest stays
// unprotected and is reported by the next query (PrimeRecomp runtime/platform/linux.c plat_written_pages, MIT). max 0
// with reset re-arms the whole range.
static uint64_t written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max, int reset) {
    if (!size) return 0;
    if (g_pagemap < 0 || !watched(addr, size)) return UINT64_MAX;
    struct page_region rg[256];
    struct pm_scan_arg a = {.size = sizeof a, .flags = (reset ? PM_SCAN_WP_MATCHING : 0) | PM_SCAN_CHECK_WPASYNC,
                            .start = addr & ~(PAGE - 1), .end = align_up(addr + size, PAGE), .vec = (uintptr_t)rg,
                            .vec_len = 256, .category_mask = PAGE_IS_WRITTEN, .return_mask = PAGE_IS_WRITTEN};
    uint64_t n = 0;
    for (;;) {
        a.max_pages = max ? max - n : 0;   // 0: no limit (only with max 0, the reset of the whole range)
        a.walk_end = 0;
        int r = ioctl(g_pagemap, PAGEMAP_SCAN, &a);
        if (r < 0) return UINT64_MAX;
        for (int i = 0; i < r; i++)
            for (uint64_t pg = rg[i].start; pg < rg[i].end && n < max; pg += PAGE) pages[n++] = pg;
        if ((max && n >= max) || !a.walk_end || a.walk_end >= a.end) break;
        a.start = a.walk_end;
    }
    return n;
}

uint64_t mem_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    return written_pages(addr, size, pages, max, 1);
}

uint64_t mem_written_peek(uint64_t addr, uint64_t size, uint64_t *pages, uint64_t max) {
    if (!max) return 0;
    return written_pages(addr, size, pages, max, 0);
}

static uint64_t image_bytes(void) {
    uint64_t n = 0;
    for (int i = 0; i < ldr_count(); i++) n += ldr_module(i)->size;
    return align_up(n, PAGE);
}

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) { return a < b + m && b < a + n; }
static int inside(uint64_t a, uint64_t n, RtRange r) { return a >= r.base && n <= r.size && a - r.base <= r.size - n; }

// ---- 0x01 SetHeapSize
// A size the heap cannot hold is refused with the kernel's OutOfMemory: the SDK's SetMemoryHeapSize turns
// OutOfResource, OutOfMemory and LimitReached into its own ResultOutOfMemory and aborts on any other result, and
// this game's start-up probes the largest heap the kernel grants by binary search between 0 and 8 GiB (nninitStartup
// in main), so it relies on that clean failure. An unaligned size is the caller's error (InvalidSize).
uint32_t mem_set_heap_size(uint64_t size, uint64_t *addr) {
    if (size % 0x200000) return R_INVALID_SIZE;
    plat_mutex_lock(&lock);
    if (size > HEAP_CAP || size + image_bytes() > L.pool) {
        plat_mutex_unlock(&lock);
        return R_OUT_OF_MEMORY;
    }
    if (size > heap_size) commit(L.heap.base + heap_size, size - heap_size, "heap");
    if (size < heap_size) decommit(L.heap.base + size, heap_size - size, "heap");
    heap_size = size;
    plat_mutex_unlock(&lock);
    rt_log("memory: heap %#llx+%#llx (pool %#llx, images %#llx)", (unsigned long long)L.heap.base, (unsigned long long)size,
           (unsigned long long)L.pool, (unsigned long long)image_bytes());
    if (addr) *addr = L.heap.base;
    return R_OK;
}

// ---- module data permissions (0x02, 0x03) and module pages for QueryMemory
static int module_index(uint64_t a) {
    for (int i = 0; i < ldr_count(); i++) {
        const RtLoaded *m = ldr_module(i);
        if (a - m->base < m->size) return i;
    }
    return -1;
}

static uint8_t *perm_table(int i) {   // under lock
    if (!module_perm[i]) {
        const RtLoaded *m = ldr_module(i);
        size_t n = (m->size - m->data_off) / PAGE;
        module_perm[i] = malloc(n);
        if (!module_perm[i]) rt_fatal("memory: module permission table");
        memset(module_perm[i], PERM_RW, n);
    }
    return module_perm[i];
}

static unsigned module_page_perm(int i, uint64_t off) {   // under lock
    const RtLoaded *m = ldr_module(i);
    if (off < m->data_off) return off < align_up(m->text_end, PAGE) ? PERM_RX : PERM_R;
    return module_perm[i] ? module_perm[i][(off - m->data_off) / PAGE] : PERM_RW;
}

// The module whose data segment holds [a, a+n), or -1.
static int module_data(uint64_t a, uint64_t n) {
    int i = module_index(a);
    if (i < 0) return -1;
    const RtLoaded *m = ldr_module(i);
    uint64_t off = a - m->base;
    return off >= m->data_off && n <= m->size - off ? i : -1;
}

uint32_t mem_set_permission(uint64_t a, uint64_t n, uint32_t perm) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (perm != PERM_NONE && perm != PERM_R && perm != PERM_RW) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++)
        if (t[p] & ATTR_PERMISSION_LOCKED) {
            plat_mutex_unlock(&lock);
            return R_INVALID_STATE;
        }
    memset(t, (int)perm, n / PAGE);
    protect(a, n, perm == PERM_RW ? PROT_READ | PROT_WRITE : perm == PERM_R ? PROT_READ : PROT_NONE, "module data");
    plat_mutex_unlock(&lock);
    rt_log("%s data permission +%#llx size %#llx perm %u", m->name, (unsigned long long)(a - m->base),
           (unsigned long long)n, perm);
    return R_OK;
}

// The only module attribute the SDK sets is the irreversible RELRO lock.
uint32_t mem_set_attribute(uint64_t a, uint64_t n, uint32_t mask, uint32_t at) {
    if (a & (PAGE - 1)) return R_INVALID_ADDRESS;
    if (!n || (n & (PAGE - 1))) return R_INVALID_SIZE;
    if (mask != ATTR_PERMISSION_LOCKED || at != ATTR_PERMISSION_LOCKED) return R_INVALID_NEW_PERM;
    int i = module_data(a, n);
    if (i < 0) return R_INVALID_STATE;
    const RtLoaded *m = ldr_module(i);
    plat_mutex_lock(&lock);
    uint8_t *t = perm_table(i) + (a - m->base - m->data_off) / PAGE;
    for (uint64_t p = 0; p < n / PAGE; p++) t[p] |= ATTR_PERMISSION_LOCKED;
    plat_mutex_unlock(&lock);
    return R_OK;
}

// ---- 0x04/0x05 MapMemory, UnmapMemory: thread stacks move from heap, alias or a module's data (.data/.bss) into the
// stack region by copy; the source is inaccessible while moved and gets the contents back on unmap. Module data is a
// source in FE3H: SDK 6.5.0's nn::os aliases every thread stack (ThreadManager::CreateAliasStackUnsafe), and the game
// keeps some stacks in main's .bss, which Horizon allows (code data may be aliased).
static int alias_all_mapped(uint64_t a, uint64_t n) {   // under lock
    uint64_t first = (a - L.alias.base) / PAGE;
    for (uint64_t i = 0; i < n / PAGE; i++)
        if (!alias_pages[first + i]) return 0;
    return 1;
}

static uint32_t stack_move(int unmap, uint64_t dst, uint64_t src, uint64_t n) {
    if (!n || ((dst | src | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(dst, n, L.stack)) return R_INVALID_REGION;
    int in_heap = inside(src, n, (RtRange){L.heap.base, heap_size});
    int in_alias = inside(src, n, L.alias);
    if (!in_heap && !in_alias && module_data(src, n) < 0) return R_INVALID_REGION;
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (in_alias && !alias_all_mapped(src, n)) {
        r = R_INVALID_REGION;
        goto out;
    }
    unsigned found = nstack_maps;
    for (unsigned i = 0; i < nstack_maps; i++)
        if (stack_maps[i].dst == dst && stack_maps[i].src == src && stack_maps[i].size == n) found = i;
    if (unmap) {
        if (found == nstack_maps) {
            r = R_INVALID_STATE;
            goto out;
        }
        protect(src, n, PROT_READ | PROT_WRITE, "moved stack source");
        memcpy((void *)(uintptr_t)src, (const void *)(uintptr_t)dst, n);
        decommit(dst, n, "moved stack");
        stack_maps[found] = stack_maps[--nstack_maps];
    } else {
        if (nstack_maps == MAX_STACK_MAPS) {
            r = R_INVALID_STATE;
            goto out;
        }
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(dst, n, stack_maps[i].dst, stack_maps[i].size) ||
                overlaps(src, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        commit(dst, n, "moved stack");
        memcpy((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
        protect(src, n, PROT_NONE, "moved stack source");
        stack_maps[nstack_maps++] = (StackMap){dst, src, n};
    }
out:
    plat_mutex_unlock(&lock);
    return r;
}

uint32_t mem_map(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(0, dst, src, n); }
uint32_t mem_unmap(uint64_t dst, uint64_t src, uint64_t n) { return stack_move(1, dst, src, n); }

// ---- 0x2C/0x2D MapPhysicalMemory, UnmapPhysicalMemory. Horizon maps only the still-unmapped pages of the range and
// leaves mapped pages and their contents alone; TotK's heaps grow by re-mapping ranges they partly own.
static void alias_runs(uint64_t first, uint64_t count, uint8_t want, int do_commit) {   // under lock
    uint64_t i = 0;
    while (i < count) {
        if (alias_pages[first + i] != want) { i++; continue; }
        uint64_t j = i;
        while (j < count && alias_pages[first + j] == want) j++;
        uint64_t a = L.alias.base + (first + i) * PAGE, n = (j - i) * PAGE;
        if (do_commit) commit(a, n, "physical memory");
        else decommit(a, n, "physical memory");
        memset(alias_pages + first + i, !want, j - i);
        i = j;
    }
}

static uint32_t alias_map(int unmap, uint64_t a, uint64_t n) {
    if (!n || ((a | n) & (PAGE - 1))) return R_INVALID_ADDRESS;
    if (!inside(a, n, L.alias)) return R_INVALID_REGION;
    uint64_t first = (a - L.alias.base) / PAGE, count = n / PAGE;
    plat_mutex_lock(&lock);
    uint32_t r = R_OK;
    if (unmap) {
        for (unsigned i = 0; i < nstack_maps; i++)
            if (overlaps(a, n, stack_maps[i].src, stack_maps[i].size)) {
                r = R_INVALID_REGION;
                goto out;
            }
        uint64_t released = 0;
        for (uint64_t i = 0; i < count; i++) released += alias_pages[first + i];
        alias_runs(first, count, 1, 0);
        alias_bytes -= released * PAGE;
    } else {
        uint64_t fresh = 0;
        for (uint64_t i = 0; i < count; i++) fresh += !alias_pages[first + i];
        if (fresh * PAGE + alias_bytes + heap_size + image_bytes() > L.pool) {
            r = R_LIMIT_REACHED;   // the SDK maps LimitReached to its recoverable out-of-memory result
            goto out;
        }
        alias_runs(first, count, 0, 1);
        alias_bytes += fresh * PAGE;
    }
out:
    plat_mutex_unlock(&lock);
    if (r) rt_log("%sMapPhysicalMemory %#llx+%#llx failed %#x (alias %#llx used, heap %#llx)", unmap ? "Un" : "",
                  (unsigned long long)a, (unsigned long long)n, r, (unsigned long long)alias_bytes,
                  (unsigned long long)heap_size);
    return r;
}

uint32_t mem_map_physical(uint64_t a, uint64_t n) { return alias_map(0, a, n); }
uint32_t mem_unmap_physical(uint64_t a, uint64_t n) { return alias_map(1, a, n); }

// ---- 0x06 QueryMemory
// The region around a that no known region covers: everything else below 2^39 belongs to the host and is reported
// as taken (inaccessible, mem_query), so the SDK never picks it.
static void taken_gap(uint64_t a, uint64_t *base, uint64_t *size) {
    uint64_t lo = 0, hi = RT_ADDRESS_LIMIT;
    RtRange known[3 + MOD_COUNT + 2];
    int n = 0;
    known[n++] = (RtRange){L.aslr.base, LOW_SIZE};
    known[n++] = L.alias;
    known[n++] = L.kernel;
    known[n++] = L.hostfn;
    for (int i = 0; i < ldr_count(); i++) known[n++] = (RtRange){ldr_module(i)->base, ldr_module(i)->size};
    for (int i = 0; i < n; i++) {
        uint64_t e = known[i].base + known[i].size;
        if (e <= a && e > lo) lo = e;
        if (known[i].base > a && known[i].base < hi) hi = known[i].base;
    }
    *base = lo;
    *size = hi - lo;
}

static uint64_t run_end(const uint8_t *p, uint64_t i, uint64_t n, uint8_t v) {
    while (i < n && (i & 7)) { if (p[i] != v) return i; i++; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i + 8 <= n && *(const uint64_t *)(p + i) == w) i += 8;
    while (i < n && p[i] == v) i++;
    return i;
}

static uint64_t run_start(const uint8_t *p, uint64_t i, uint8_t v) {   // first index of the run that ends at i
    while (i > 0 && (i & 7)) { if (p[i - 1] != v) return i; i--; }
    uint64_t w = v ? 0x0101010101010101ull : 0;
    while (i >= 8 && *(const uint64_t *)(p + i - 8) == w) i -= 8;
    while (i > 0 && p[i - 1] == v) i--;
    return i;
}

uint32_t mem_query(uint64_t a, RtMemoryInfo *out) {
    RtMemoryInfo r = {.base = a & ~(PAGE - 1), .size = PAGE};
    uint64_t low = L.aslr.base;
    plat_mutex_lock(&lock);
    int mi;
    if (a >= RT_ADDRESS_LIMIT) {
        r.base = RT_ADDRESS_LIMIT;
        r.size = 0 - RT_ADDRESS_LIMIT;
        r.state = ST_INACCESSIBLE;
    } else if (a < low) {
        r.base = 0;
        r.size = low;
        r.state = ST_FREE;
    } else if (a < low + LOW_SIZE) {
        uint64_t heap = L.heap.base;
        if (a >= heap && a < heap + heap_size) {
            r = (RtMemoryInfo){.base = heap, .size = heap_size, .state = ST_NORMAL, .perm = PERM_RW};
        } else if (a < heap) {
            r = (RtMemoryInfo){.base = low, .size = heap - low, .state = ST_FREE};
        } else {
            r = (RtMemoryInfo){.base = heap + heap_size, .size = low + LOW_SIZE - heap - heap_size, .state = ST_FREE};
        }
    } else if (a - L.alias.base < L.alias.size) {
        uint64_t p = (a - L.alias.base) / PAGE;
        uint8_t v = alias_pages[p];
        uint64_t s = run_start(alias_pages, p, v), e = run_end(alias_pages, p, ALIAS_PAGES, v);
        r = (RtMemoryInfo){.base = L.alias.base + s * PAGE, .size = (e - s) * PAGE, .state = v ? ST_NORMAL : ST_FREE,
                           .perm = v ? PERM_RW : PERM_NONE};
    } else if ((mi = module_index(a)) >= 0) {
        const RtLoaded *m = ldr_module(mi);
        uint64_t off = (a - m->base) & ~(PAGE - 1), lo = off, hi = off + PAGE;
        unsigned perm = module_page_perm(mi, off);
        int data = off >= m->data_off;
        uint64_t limit = data ? m->data_off : 0, end = data ? m->size : m->data_off;
        while (lo > limit && module_page_perm(mi, lo - PAGE) == perm) lo -= PAGE;
        while (hi < end && module_page_perm(mi, hi) == perm) hi += PAGE;
        r = (RtMemoryInfo){.base = m->base + lo, .size = hi - lo, .state = data ? ST_CODEDATA : ST_CODE,
                           .perm = perm & 7, .attr = perm & ATTR_PERMISSION_LOCKED};
    } else if (a - L.kernel.base < L.kernel.size) {
        r = (RtMemoryInfo){.base = L.kernel.base, .size = L.kernel.size, .state = ST_THREADLOCAL, .perm = PERM_RW};
    } else if (a - L.hostfn.base < L.hostfn.size) {
        r = (RtMemoryInfo){.base = L.hostfn.base, .size = L.hostfn.size, .state = ST_INACCESSIBLE};
    } else {
        // Country-Box: a region no known one covers (host memory, or a gap between modules of the fixed window) is
        // inaccessible, not Code RX as upstream reported it. The SDK's exception unwinder (nn::ro::detail::
        // GetExceptionInfo) finds a module by walking Code RX regions back and forth from the throwing pc, and at the
        // first C++ throw of Returns HD it walked from main's text into such a gap and read a MOD0 header there.
        // Inaccessible is still never picked by a mapping search.
        taken_gap(a, &r.base, &r.size);
        r.state = ST_INACCESSIBLE;
        r.perm = PERM_NONE;
    }
    // Moved stacks: the destination is Stack, the source Normal without access; free and normal regions around them
    // are clipped (MNG services.c).
    for (unsigned i = 0; i < nstack_maps; i++) {
        const StackMap *m = &stack_maps[i];
        uint64_t end = m->dst + m->size, send = m->src + m->size;
        if (a >= m->dst && a < end) {
            r = (RtMemoryInfo){.base = m->dst, .size = m->size, .state = ST_STACK, .perm = PERM_RW};
            break;
        }
        if (a >= m->src && a < send) {
            r = (RtMemoryInfo){.base = m->src, .size = m->size, .state = ST_NORMAL, .perm = PERM_NONE, .attr = 4};
            break;
        }
        uint64_t lo = r.base, hi = r.base + r.size;
        uint64_t b = r.state == ST_FREE ? m->dst : r.state == ST_NORMAL ? m->src : 0;
        uint64_t e = b + m->size;
        if (!b) continue;
        if (e <= a && e > lo) lo = e;
        if (b > a && b < hi) hi = b;
        r.base = lo;
        r.size = hi - lo;
    }
    plat_mutex_unlock(&lock);
    if (out) *out = r;
    return R_OK;
}

// ---- GetInfo memory ids
int mem_info(uint32_t id, uint64_t *out) {
    uint64_t v;
    switch (id) {
    case 2: v = L.alias.base; break;
    case 3: v = L.alias.size; break;
    case 4: v = L.heap.base; break;
    case 5: v = L.heap.size; break;
    case 6: case 21: v = L.pool; break;
    case 7: case 22:
        plat_mutex_lock(&lock);
        v = image_bytes() + heap_size + alias_bytes;
        plat_mutex_unlock(&lock);
        break;
    case 12: v = L.aslr.base; break;
    case 13: v = L.aslr.size; break;
    case 14: v = L.stack.base; break;
    case 15: v = L.stack.size; break;
    case 16: v = SYSTEM_RESOURCE_BUDGET; break;
    case 17: v = sizeof stack_maps + ALIAS_PAGES; break;   // system resource in use: our bookkeeping
    case 28: v = 0; break;   // AliasRegionExtraSize: no kernel-owned extension
    default: return 0;
    }
    *out = v;
    return 1;
}

// ---- kernel region: power-of-two blocks, a free list per size, bump allocation with lazy commit.
#define KCLASSES 29          // 64 B .. 256 MiB
#define KMIN_SHIFT 6
#define KCOMMIT 0x10000ull
typedef struct KFree { struct KFree *next; } KFree;
static KFree *kfree_list[KCLASSES];
static uint64_t kbump, kcommitted;
static PlatMutex klock = PLAT_MUTEX_INIT;

static int kclass(uint64_t size) {
    int s = KMIN_SHIFT;
    while ((1ull << s) < size) s++;
    return s - KMIN_SHIFT;
}

void *mem_kernel_alloc(uint64_t size, uint64_t align) {
    if (!L.pool) rt_fatal("mem_kernel_alloc before mem_init");
    if (align > KCOMMIT || (align & (align - 1))) rt_fatal("mem_kernel_alloc: alignment %#llx", (unsigned long long)align);
    uint64_t want = size > align ? size : align;
    int k = kclass(want ? want : 1);
    if (k >= KCLASSES) rt_fatal("mem_kernel_alloc: %#llx bytes", (unsigned long long)size);
    uint64_t bytes = 1ull << (k + KMIN_SHIFT), al = bytes < KCOMMIT ? bytes : KCOMMIT;
    plat_mutex_lock(&klock);
    KFree *f = kfree_list[k];
    void *p;
    if (f) {
        kfree_list[k] = f->next;
        p = f;
    } else {
        uint64_t a = align_up(kbump ? kbump : RT_KERNEL_BASE, al);
        if (a + bytes > RT_KERNEL_BASE + RT_KERNEL_SIZE) rt_fatal("kernel region exhausted (%#llx bytes)",
                                                                  (unsigned long long)bytes);
        uint64_t need = align_up(a + bytes, KCOMMIT);
        if (!kcommitted) kcommitted = RT_KERNEL_BASE;
        if (need > kcommitted) {
            commit(kcommitted, need - kcommitted, "kernel region");
            kcommitted = need;
        }
        kbump = a + bytes;
        p = (void *)(uintptr_t)a;
    }
    plat_mutex_unlock(&klock);
    memset(p, 0, size ? size : 1);
    return p;
}

void mem_kernel_free(void *p, uint64_t size) {
    if (!p) return;
    uint64_t a = (uintptr_t)p;
    if (a - RT_KERNEL_BASE >= RT_KERNEL_SIZE) rt_fatal("mem_kernel_free(%p): not in the kernel region", p);
    int k = kclass(size ? size : 1);
    // A block allocated with a larger alignment sits in a larger class; freeing it into the smaller class only
    // wastes the rest.
    plat_mutex_lock(&klock);
    KFree *f = p;
    f->next = kfree_list[k];
    kfree_list[k] = f;
    plat_mutex_unlock(&klock);
}
