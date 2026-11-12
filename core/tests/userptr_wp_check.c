// Country-Box: does a GPU submission that reads a host-pointer import (amdgpu userptr) make userfaultfd's
// asynchronous write protection report pages as CPU-written? The runtime re-uploads a texture when PAGEMAP_SCAN says its
// storage was written (vk_memory.c vk_mem_changed); in Returns HD five 16 MiB textures in imported pools were re-uploaded
// every frame. Suspected loop: the scan's re-protection invalidates the userptr BO (MMU notifier), the next submission
// faults the pages back in for writing (hmm_range_fault), which lifts the protection, so the pages read as written.
//
// Steps over one imported range, no CPU write after the first: write once, reset; scan (expect 0); submit a copy
// reading the import, scan (the question); reset, submit again, scan (does the loop sustain). Once with transparent
// huge pages allowed, once with MADV_NOHUGEPAGE; then the same over a udmabuf (memfd) import when /dev/udmabuf exists.
//
//   clang -std=gnu17 -O1 tests/userptr_wp_check.c -lvulkan -o /tmp/userptr_wp_check && /tmp/userptr_wp_check
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/udmabuf.h>
#include <linux/userfaultfd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define SIZE (32ull << 20)
#define PAGE 4096ull

static int uffd = -1, pagemap = -1;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkPhysicalDevice phys;
static VkCommandPool cpool;

#define CK(x)                                                                    \
    do {                                                                         \
        VkResult r_ = (x);                                                       \
        if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed (%d)\n", #x, r_); exit(1); } \
    } while (0)

static void track_init(void) {
    uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    struct uffdio_api api = {.api = UFFD_API,
                             .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED};
    pagemap = open("/proc/self/pagemap", O_RDONLY);
    if (uffd < 0 || pagemap < 0 || ioctl(uffd, UFFDIO_API, &api)) { perror("userfaultfd"); exit(1); }
}

static void track(void *p, uint64_t n) {
    struct uffdio_register reg = {.range = {(uintptr_t)p, n}, .mode = UFFDIO_REGISTER_MODE_WP};
    if (ioctl(uffd, UFFDIO_REGISTER, &reg)) { perror("UFFDIO_REGISTER"); exit(1); }
}

// Written pages of [p, p+n); reset re-protects them (the runtime's mem_written_pages).
static uint64_t scan(void *p, uint64_t n, int reset) {
    struct page_region rg[512];
    struct pm_scan_arg a = {.size = sizeof a, .flags = (reset ? PM_SCAN_WP_MATCHING : 0) | PM_SCAN_CHECK_WPASYNC,
                            .start = (uintptr_t)p, .end = (uintptr_t)p + n, .vec = (uintptr_t)rg, .vec_len = 512,
                            .category_mask = PAGE_IS_WRITTEN, .return_mask = PAGE_IS_WRITTEN};
    uint64_t pages = 0;
    for (;;) {
        a.walk_end = 0;
        int r = ioctl(pagemap, PAGEMAP_SCAN, &a);
        if (r < 0) { perror("PAGEMAP_SCAN"); exit(1); }
        for (int i = 0; i < r; i++) pages += (rg[i].end - rg[i].start) / PAGE;
        if (!a.walk_end || a.walk_end >= a.end) break;
        a.start = a.walk_end;
    }
    return pages;
}

static void vk_init(void) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CK(vkCreateInstance(&ii, NULL, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &phys);
    if (!n) { fprintf(stderr, "no GPU\n"); exit(1); }
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(phys, &pp);
    printf("device: %s\n", pp.deviceName);
    VkQueueFamilyProperties qf[16];
    uint32_t nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    for (qfam = 0; qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_GRAPHICS_BIT); qfam++) {}
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    const char *ext[] = {"VK_EXT_external_memory_host", "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = 3, .ppEnabledExtensionNames = ext};
    CK(vkCreateDevice(phys, &di, NULL, &dev));
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT};
    CK(vkCreateCommandPool(dev, &ci, NULL, &cpool));
}

static VkBuffer make_buffer(uint64_t n, const void *ext) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = ext, .size = n,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b;
    CK(vkCreateBuffer(dev, &bi, NULL, &b));
    return b;
}

static int type_of(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits >> i & 1) && (mp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if (bits >> i & 1) return (int)i;
    return -1;
}

static VkBuffer dst;
static void make_dst(void) {
    dst = make_buffer(SIZE, NULL);
    VkMemoryRequirements rq;
    vkGetBufferMemoryRequirements(dev, dst, &rq);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size,
                               .memoryTypeIndex = (uint32_t)type_of(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory m;
    CK(vkAllocateMemory(dev, &ai, NULL, &m));
    CK(vkBindBufferMemory(dev, dst, m, 0));
}

// One submission that reads all of src (a copy into device memory), waited for.
static void submit_read(VkBuffer src) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    CK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CK(vkBeginCommandBuffer(cb, &bi));
    VkBufferCopy c = {0, 0, SIZE};
    vkCmdCopyBuffer(cb, src, dst, 1, &c);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(queue));
    vkFreeCommandBuffers(dev, cpool, 1, &cb);
}

static VkBuffer import_host(void *p) {
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBuffer b = make_buffer(SIZE, &eb);
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    CK(props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p, &hp));
    VkMemoryRequirements rq;
    vkGetBufferMemoryRequirements(dev, b, &rq);
    VkImportMemoryHostPointerInfoEXT im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                           .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                           .pHostPointer = p};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &im, .allocationSize = SIZE,
                               .memoryTypeIndex = (uint32_t)type_of(rq.memoryTypeBits & hp.memoryTypeBits,
                                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    VkDeviceMemory m;
    CK(vkAllocateMemory(dev, &ai, NULL, &m));
    CK(vkBindBufferMemory(dev, b, m, 0));
    return b;
}

static VkBuffer import_dmabuf(int dfd) {
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBuffer b = make_buffer(SIZE, &eb);
    PFN_vkGetMemoryFdPropertiesKHR props = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    CK(props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dfd, &fp));
    VkMemoryRequirements rq;
    vkGetBufferMemoryRequirements(dev, b, &rq);
    VkImportMemoryFdInfoKHR im = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                  .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dfd};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &im, .allocationSize = SIZE,
                               .memoryTypeIndex = (uint32_t)type_of(rq.memoryTypeBits & fp.memoryTypeBits,
                                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    VkDeviceMemory m;
    CK(vkAllocateMemory(dev, &ai, NULL, &m));
    CK(vkBindBufferMemory(dev, b, m, 0));
    return b;
}

static int fails;   // udmabuf checks only: the host pointer imports' false dirt is what this measures, not a failure

// Coherence through the import: a CPU write is what a GPU copy reads, and a GPU fill is what the CPU then reads.
static void coherence(const char *name, void *p, VkBuffer b) {
    VkBuffer rb = make_buffer(4096, NULL);
    VkMemoryRequirements rq;
    vkGetBufferMemoryRequirements(dev, rb, &rq);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size,
                               .memoryTypeIndex = (uint32_t)type_of(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m;
    CK(vkAllocateMemory(dev, &ai, NULL, &m));
    CK(vkBindBufferMemory(dev, rb, m, 0));
    uint32_t *rp;
    CK(vkMapMemory(dev, m, 0, 4096, 0, (void **)&rp));
    volatile uint32_t *g = p;
    g[1024] = 0x12345678;             // CPU write in the second page
    g[0] = 0;
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cpool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &bi));
    vkCmdFillBuffer(cb, b, 0, 4096, 0xdeadbeef);
    VkBufferCopy c = {4096, 0, 4096};
    vkCmdCopyBuffer(cb, b, rb, 1, &c);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(queue));
    int ok = g[0] == 0xdeadbeef && rp[0] == 0x12345678;
    printf("%-28s coherence: GPU fill seen by CPU %s, CPU write seen by GPU %s\n", name,
           g[0] == 0xdeadbeef ? "yes" : "NO", rp[0] == 0x12345678 ? "yes" : "NO");
    if (!ok) fails++;
}
static void run(const char *name, void *p, VkBuffer b, int must_be_exact) {
    uint64_t total = SIZE / PAGE;
    memset(p, 0x5a, SIZE);
    uint64_t w0 = scan(p, SIZE, 1);   // the first write, reset
    uint64_t w1 = scan(p, SIZE, 1);   // nothing written since
    submit_read(b);
    uint64_t w2 = scan(p, SIZE, 1);   // after a GPU read, no CPU write
    submit_read(b);
    uint64_t w3 = scan(p, SIZE, 1);   // again: the loop sustains itself?
    volatile uint8_t *q = p;
    q[5 * PAGE] = 1;                  // one real CPU write
    submit_read(b);
    uint64_t w4 = scan(p, SIZE, 1);
    printf("%-28s pages %llu: first write %llu, idle %llu, after GPU read %llu, again %llu, 1 CPU write + read %llu\n",
           name, (unsigned long long)total, (unsigned long long)w0, (unsigned long long)w1, (unsigned long long)w2,
           (unsigned long long)w3, (unsigned long long)w4);
    if (must_be_exact && (w2 || w3 || w4 != 1)) fails++;
    if (!must_be_exact && (w2 || w3)) printf("%-28s GPU use marks its pages as CPU-written (false dirt)\n", name);
}

static void *anon(int thp) {
    void *p = mmap(NULL, SIZE + (2ull << 20), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    p = (void *)(((uintptr_t)p + (2ull << 20) - 1) & ~((2ull << 20) - 1));
    madvise(p, SIZE, thp ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    return p;
}

int main(void) {
    track_init();
    vk_init();
    make_dst();
    void *a = anon(1);
    track(a, SIZE);
    VkBuffer ia = import_host(a);
    run("userptr, THP allowed", a, ia, 0);
    coherence("userptr, THP allowed", a, ia);
    void *b = anon(0);
    track(b, SIZE);
    run("userptr, MADV_NOHUGEPAGE", b, import_host(b), 0);
    int u = open("/dev/udmabuf", O_RDWR);
    if (u < 0) {
        printf("udmabuf: /dev/udmabuf not available (%s)\n", strerror(errno));
        fails++;
    } else {
        int mfd = memfd_create("wp-check", MFD_ALLOW_SEALING);
        if (mfd < 0 || ftruncate(mfd, SIZE) || fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK)) { perror("memfd"); return 1; }
        struct udmabuf_create c = {.memfd = (uint32_t)mfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = SIZE};
        int dfd = ioctl(u, UDMABUF_CREATE, &c);
        if (dfd < 0) {
            printf("udmabuf: UDMABUF_CREATE of %llu MiB failed (%s)\n", (unsigned long long)(SIZE >> 20), strerror(errno));
            fails++;
        } else {
            void *m = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
            track(m, SIZE);
            VkBuffer buf = import_dmabuf(dfd);
            run("udmabuf (memfd shared)", m, buf, 1);
            coherence("udmabuf (memfd shared)", m, buf);
            uint64_t gw = scan(m, SIZE, 1);
            printf("%-28s written pages reported after the GPU fill + CPU writes: %llu (expect 2: the CPU's)\n",
                   "udmabuf (memfd shared)", (unsigned long long)gw);
            if (gw != 2) fails++;
            // coherence: a CPU write after the import is what the GPU reads (no copy behind the dma-buf)
        }
    }
    printf("RESULT: udmabuf import %s\n", fails ? "FAILED (false dirt or incoherence, see above)" : "exact and coherent");
    return fails != 0;
}
