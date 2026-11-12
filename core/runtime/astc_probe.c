// From fe3h-pc-recomp runtime/astc_probe.c @ 47ef99126793dbd13e4a475feb910a13ef6639b8 (MIT, Copyright (c) 2026 Jeff Aigner), copied with fe3h renamed to mp4; adds --bench-astc (bench_astc).
// From totk-pc-recomp runtime/astc_probe.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026 Valiantsin Dzerakh), copied with totk renamed to fe3h only. Country-Box: plat_file_list and plat_now_ns (Linux).
// --probe-astc: the compute ASTC decoder (vk_astc.c, shaders/astc_decode.comp) against astc_decode_ldr, bit for bit,
// for every block footprint, linear and sRGB. Synthetic blocks only (no game files): valid void extents, invalid and
// HDR void extents, random blocks kept by what they exercise (1-4 partitions, dual plane with 1-3 partitions, every
// weight grid and quantization the random modes reach), then plain random blocks (mostly error blocks, HDR endpoint
// modes among them). Each case decodes two regions into two mip levels of an RGBA8 image with cropped edges, and
// compares both the decoder's texels (readback) and what the image holds afterwards. The same regions also go through
// the BC3 encoder (MP4_ASTC_BC) into a BC3 image, whose blocks are decoded here and held to error bounds (bc_check).
// Also times one 2048x2048 texture on the CPU (astc_decode_ldr_mt) and on the GPU (wall time of the upload batch).
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "platform/platform.h"
#include "host.h"
#include "tex.h"
#include "vk.h"

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static uint64_t rnd64(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

static const uint8_t dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                    {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

// Buckets of valid blocks: partitions 1-4 single plane, then dual plane with 1-3 partitions.
enum { NB = 7, PER_BUCKET = 48 };
static const char *const bucket_name[NB] = {"1p", "2p", "3p", "4p", "dual 1p", "dual 2p", "dual 3p"};
static uint64_t g_bucket[NB], g_void, g_err, g_blocks, g_texels, g_bc_cases;
static double g_bc_db = 99;   // the lowest BC3 PSNR of all cases

static int dual_bit(uint32_t m) {
    if (!(m & 3) && (m >> 7 & 3) == 2) return 0;   // the 6..10 x 6..10 modes have no dual plane
    return (int)(m >> 10 & 1);
}

static void void_block(uint8_t b[16], int kind) {
    // kind 0: valid LDR extent (random coordinates s0 < s1, t0 < t1), 1: all-ones extent, 2: s0 >= s1 (error),
    // 3: HDR (error), 4: reserved bits not 11 (error)
    uint64_t lo = 0x1FC | 3ull << 10, s0 = rnd64() & 0xFFF, s1 = s0 + 1 + (rnd64() & 0xFFF), t0 = rnd64() & 0xFFF,
             t1 = t0 + 1 + (rnd64() & 0xFFF);
    if (s1 > 0x1FFF) s1 = 0x1FFF;
    if (t1 > 0x1FFF) t1 = 0x1FFF;
    if (kind == 1) s0 = s1 = t0 = t1 = 0x1FFF;
    if (kind == 2) { uint64_t x = s0; s0 = s1; s1 = x; }
    lo |= s0 << 12 | s1 << 25 | t0 << 38 | t1 << 51;
    if (kind == 3) lo |= 0x200;
    if (kind == 4) lo &= ~(1ull << 11);
    uint64_t hi = rnd64();
    memcpy(b, &lo, 8), memcpy(b + 8, &hi, 8);
}

// n blocks for footprint bw x bh: the buckets first, void extents, then random.
static void make_blocks(uint8_t *blk, size_t n, uint32_t bw, uint32_t bh) {
    size_t k = 0;
    uint8_t tmp[12 * 12 * 4];
    int have[NB] = {0};
    for (int tries = 0; tries < 400000 && k < n; tries++) {
        uint64_t v[2] = {rnd64(), rnd64()};
        uint8_t *b = (uint8_t *)v;
        uint32_t m = (uint32_t)(v[0] & 0x7FF);
        if ((m & 0x1FF) == 0x1FC || !astc_decode_block(b, bw, bh, tries & 1, tmp)) continue;
        int p = (int)(v[0] >> 11 & 3), bk = dual_bit(m) ? 4 + p : p;
        if (bk >= NB || have[bk] >= PER_BUCKET) continue;
        have[bk]++, g_bucket[bk]++;
        memcpy(blk + 16 * k++, b, 16);
    }
    for (int i = 0; i < 40 && k < n; i++, g_void++) void_block(blk + 16 * k++, i % 5);
    for (; k < n; k++) {
        uint64_t v[2] = {rnd64(), rnd64()};
        memcpy(blk + 16 * k, v, 16);
        if (!astc_decode_block(blk + 16 * k, bw, bh, false, tmp)) g_err++;
    }
}

typedef struct {
    VkImage img;
    VmaAllocation alloc;
} Img;

static int make_image(VkCtx *c, uint32_t w, uint32_t h, uint32_t levels, VkFormat format, Img *o) {
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = format, .extent = {w, h, 1},
                            .mipLevels = levels, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    return vmaCreateImage(c->vma, &ci, &ai, &o->img, &o->alloc, NULL) == VK_SUCCESS;
}

static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = src, .srcAccessMask = sa,
                           .dstStageMask = dst, .dstAccessMask = da};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
#define ALL_RW VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT

static void to_general(VkCommandBuffer cb, VkImage img) {
    VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
    vkCmdPipelineBarrier2(cb, &di);
}

static double now_ms(void) {
    return (double)plat_now_ns() / 1e6;
}

// Decodes regions (w0 x h0 at level 0, w1 x h1 at level 1) of blk on the GPU; out_rb: the decoder's texels, out_img:
// the image's two levels read back. 0 on a Vulkan failure.
static int gpu_decode(VkCtx *c, const uint8_t *blk, size_t n0, size_t n1, uint32_t w0, uint32_t h0, uint32_t w1,
                      uint32_t h1, uint32_t bw, uint32_t bh, int srgb, uint8_t *out_rb, uint8_t *out_img, double *ms) {
    Img im;
    if (!make_image(c, w0, h0, 2, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, &im)) return 0;
    size_t t0 = (size_t)w0 * h0 * 4, t1 = (size_t)w1 * h1 * 4;
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer sb, rb, ib;
    VmaAllocation ra, ia;
    uint8_t *stg = vk_staging((n0 + n1) * 16, &sb, NULL);
    uint8_t *rmap = vk_staging(t0 + t1, &rb, &ra), *imap = vk_staging(t0 + t1, &ib, &ia);
    memcpy(stg, blk, (n0 + n1) * 16);
    to_general(cb, im.img);
    VkAstcRegion ar[2] = {{.src = 0, .w = w0, .h = h0, .slices = 1, .level = 0},
                          {.src = n0 * 16, .w = w1, .h = h1, .slices = 1, .level = 1}};
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int ok = vk_astc_record(cb, sb, (n0 + n1) * 16, ar, 2, bw, bh, srgb, 0, im.img, VK_IMAGE_ASPECT_COLOR_BIT, rb);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    VkBufferImageCopy cp[2] = {{.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w0, h0, 1}},
                               {.bufferOffset = t0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1}, .imageExtent = {w1, h1, 1}}};
    vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_GENERAL, ib, 2, cp);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    double a = now_ms();
    vk_upload_flush(1);
    if (ms) *ms = now_ms() - a;
    vmaInvalidateAllocation(c->vma, ra, 0, VK_WHOLE_SIZE);
    vmaInvalidateAllocation(c->vma, ia, 0, VK_WHOLE_SIZE);
    if (out_rb) memcpy(out_rb, rmap, t0 + t1);
    if (out_img) memcpy(out_img, imap, t0 + t1);
    vmaDestroyBuffer(c->vma, rb, ra), vmaDestroyBuffer(c->vma, ib, ia);
    vk_defer_image(im.img, im.alloc);
    vk_upload_flush(1);
    return ok;
}

// The same two regions decoded and encoded to BC3 (MP4_ASTC_BC) into a BC3 image; out: its blocks read back, level 0
// then level 1, rows of 4x4 blocks. 0 on a Vulkan failure or when nothing was recorded.
static int gpu_bc(VkCtx *c, const uint8_t *blk, size_t n0, size_t n1, uint32_t w0, uint32_t h0, uint32_t w1, uint32_t h1,
                  uint32_t bw, uint32_t bh, int srgb, uint8_t *out) {
    Img im;
    if (!make_image(c, w0, h0, 2, srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK, &im)) return 0;
    size_t b0 = (size_t)((w0 + 3) / 4) * ((h0 + 3) / 4) * 16, b1 = (size_t)((w1 + 3) / 4) * ((h1 + 3) / 4) * 16;
    VkCommandBuffer cb = vk_upload_begin();
    VkBuffer sb, ib;
    VmaAllocation ia;
    uint8_t *stg = vk_staging((n0 + n1) * 16, &sb, NULL), *imap = vk_staging(b0 + b1, &ib, &ia);
    memcpy(stg, blk, (n0 + n1) * 16);
    to_general(cb, im.img);
    VkAstcRegion ar[2] = {{.src = 0, .w = w0, .h = h0, .slices = 1, .level = 0},
                          {.src = n0 * 16, .w = w1, .h = h1, .slices = 1, .level = 1}};
    barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    int ok = vk_astc_record(cb, sb, (n0 + n1) * 16, ar, 2, bw, bh, srgb, 1, im.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_NULL_HANDLE);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
    VkBufferImageCopy cp[2] = {{.bufferOffset = 0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w0, h0, 1}},
                               {.bufferOffset = b0, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 0, 1}, .imageExtent = {w1, h1, 1}}};
    vkCmdCopyImageToBuffer(cb, im.img, VK_IMAGE_LAYOUT_GENERAL, ib, 2, cp);
    barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    vk_upload_end();
    vk_upload_flush(1);
    vmaInvalidateAllocation(c->vma, ia, 0, VK_WHOLE_SIZE);
    memcpy(out, imap, b0 + b1);
    vmaDestroyBuffer(c->vma, ib, ia);
    vk_defer_image(im.img, im.alloc);
    vk_upload_flush(1);
    return ok;
}

// One BC3 block to 16 RGBA8 texels, as the format defines it (565 endpoints widened by bit replication; the colour
// block always in four-colour mode). pos: each texel's place on the line from endpoint 0 (0) to endpoint 1 (3).
static void bc3_decode(const uint8_t b[16], uint8_t t[16][4], uint8_t ep[2][3], int pos[16]) {
    uint8_t a[8] = {b[0], b[1]}, p[4][3];
    if (a[0] > a[1])
        for (int i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1] + 3) / 7);
    else {
        for (int i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1] + 2) / 5);
        a[6] = 0, a[7] = 255;
    }
    uint64_t ai = 0;
    for (int i = 0; i < 6; i++) ai |= (uint64_t)b[2 + i] << 8 * i;
    for (int k = 0; k < 2; k++) {
        unsigned v = b[8 + 2 * k] | b[9 + 2 * k] << 8, r = v >> 11, g = v >> 5 & 63, bl = v & 31;
        p[k][0] = (uint8_t)(r << 3 | r >> 2), p[k][1] = (uint8_t)(g << 2 | g >> 4), p[k][2] = (uint8_t)(bl << 3 | bl >> 2);
    }
    for (int ch = 0; ch < 3; ch++)
        p[2][ch] = (uint8_t)((2 * p[0][ch] + p[1][ch] + 1) / 3), p[3][ch] = (uint8_t)((p[0][ch] + 2 * p[1][ch] + 1) / 3);
    uint32_t ci = b[12] | b[13] << 8 | b[14] << 16 | (uint32_t)b[15] << 24;
    static const int code_pos[4] = {0, 3, 1, 2};
    for (int i = 0; i < 16; i++) {
        memcpy(t[i], p[ci >> 2 * i & 3], 3);
        t[i][3] = a[ai >> 3 * i & 7];
        pos[i] = code_pos[ci >> 2 * i & 3];
    }
    memcpy(ep, p, 6);
}

// BC3 is lossy, so these are bounds, not bit-exactness. In every block: alpha within a fourteenth of the block's alpha
// range (the eight-value mode's half step); each texel on the palette entry nearest its projection onto the endpoint
// line (within rounding, when the endpoints are apart); a block of one colour within the 565 rounding, alpha exact.
// Per level, a PSNR floor against astc_decode_ldr for gross damage: the random blocks are noise, so it is low (19 dB
// at worst for 5x5 level 1 on an RX 9070). *db: the level's PSNR.
enum { BC_MIN_DB = 15 };
static int bc_check(const uint8_t *want, const uint8_t *blocks, uint32_t w, uint32_t h, const char *what, double *db) {
    uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    double se = 0;
    for (uint32_t y4 = 0; y4 < by; y4++)
        for (uint32_t x4 = 0; x4 < bx; x4++) {
            uint8_t t[16][4], ep[2][3], lo[4] = {255, 255, 255, 255}, hi[4] = {0};
            int pos[16];
            bc3_decode(blocks + 16 * ((size_t)y4 * bx + x4), t, ep, pos);
            double axis[3], len2 = 0;
            for (int ch = 0; ch < 3; ch++) axis[ch] = (double)ep[1][ch] - ep[0][ch], len2 += axis[ch] * axis[ch];
            for (int pass = 0; pass < 2; pass++)
                for (int i = 0; i < 16; i++) {
                    uint32_t x = x4 * 4 + (i & 3), y = y4 * 4 + (i >> 2);
                    if (x >= w || y >= h) continue;
                    const uint8_t *s = want + 4 * ((size_t)y * w + x);
                    if (!pass) {
                        for (int ch = 0; ch < 4; ch++) lo[ch] = s[ch] < lo[ch] ? s[ch] : lo[ch], hi[ch] = s[ch] > hi[ch] ? s[ch] : hi[ch];
                        continue;
                    }
                    int flat = !memcmp(lo, hi, 4), lim[4] = {5, 3, 5, 0};
                    if (len2 >= 64) {   // shorter lines: rounding alone can move a texel to the next entry
                        double proj = 0;
                        for (int ch = 0; ch < 3; ch++) proj += ((double)s[ch] - ep[0][ch]) * axis[ch];
                        proj = proj / len2 * 3;
                        proj = proj < 0 ? 0 : proj > 3 ? 3 : proj;
                        if (fabs(pos[i] - proj) > 0.75) {   // a wrong code mapping is a whole place off
                            printf("probe-astc: FAIL BC3 %s: texel (%u,%u) on palette place %d, its projection is %.2f\n",
                                   what, x, y, pos[i], proj);
                            return 1;
                        }
                    }
                    for (int ch = 0; ch < 4; ch++) {
                        int d = abs((int)t[i][ch] - s[ch]);
                        se += (double)d * d;
                        if ((ch == 3 && d > (hi[3] - lo[3]) / 14 + 1) || (flat && d > lim[ch])) {
                            printf("probe-astc: FAIL BC3 %s: texel (%u,%u) channel %d: astc %02x%02x%02x%02x bc3 %02x%02x%02x%02x\n",
                                   what, x, y, ch, s[0], s[1], s[2], s[3], t[i][0], t[i][1], t[i][2], t[i][3]);
                            return 1;
                        }
                    }
                }
        }
    double mse = se / ((double)w * h * 4);
    *db = mse > 0 ? 10 * log10(255.0 * 255.0 / mse) : 99;
    if (*db < BC_MIN_DB) {
        printf("probe-astc: FAIL BC3 %s: PSNR %.1f dB, below %d\n", what, *db, BC_MIN_DB);
        return 1;
    }
    return 0;
}

static int first_diff(const uint8_t *a, const uint8_t *b, size_t texels, size_t *at) {
    for (size_t i = 0; i < texels; i++)
        if (memcmp(a + 4 * i, b + 4 * i, 4)) { *at = i; return 1; }
    return 0;
}

static int test_case(VkCtx *c, uint32_t bw, uint32_t bh, int srgb) {
    // level 0: 37 x 23 blocks, cropped by 1-3 texels; level 1 half of that, other blocks
    uint32_t w0 = 37 * bw - 1 - (bw > 5), h0 = 23 * bh - 2, w1 = w0 > 1 ? w0 >> 1 : 1, h1 = h0 > 1 ? h0 >> 1 : 1;
    size_t n0 = (size_t)((w0 + bw - 1) / bw) * ((h0 + bh - 1) / bh), n1 = (size_t)((w1 + bw - 1) / bw) * ((h1 + bh - 1) / bh);
    uint8_t *blk = malloc((n0 + n1) * 16);
    make_blocks(blk, n0, bw, bh);
    make_blocks(blk + n0 * 16, n1, bw, bh);
    size_t t0 = (size_t)w0 * h0, t1 = (size_t)w1 * h1;
    uint8_t *want = malloc((t0 + t1) * 4), *rb = malloc((t0 + t1) * 4), *im = malloc((t0 + t1) * 4);
    memset(rb, 0x5A, (t0 + t1) * 4), memset(im, 0xA5, (t0 + t1) * 4);
    astc_decode_ldr(blk, w0, h0, bw, bh, srgb, want);
    astc_decode_ldr(blk + n0 * 16, w1, h1, bw, bh, srgb, want + t0 * 4);
    int bad = 0;
    if (!gpu_decode(c, blk, n0, n1, w0, h0, w1, h1, bw, bh, srgb, rb, im, NULL)) {
        printf("probe-astc: FAIL %ux%u %s: the GPU decode was not recorded\n", bw, bh, srgb ? "sRGB" : "linear");
        bad = 1;
    }
    for (int lv = 0; lv < 2 && !bad; lv++) {
        size_t off = lv ? t0 * 4 : 0, n = lv ? t1 : t0, at;
        uint32_t w = lv ? w1 : w0;
        for (int which = 0; which < 2 && !bad; which++) {
            const uint8_t *got = (which ? im : rb) + off;
            if (first_diff(want + off, got, n, &at)) {
                const uint8_t *e = want + off + 4 * at, *g = got + 4 * at;
                printf("probe-astc: FAIL %ux%u %s level %d (%s): texel (%zu,%zu) cpu %02x%02x%02x%02x gpu %02x%02x%02x%02x\n",
                       bw, bh, srgb ? "sRGB" : "linear", lv, which ? "image" : "decoder output", at % w, at / w,
                       e[0], e[1], e[2], e[3], g[0], g[1], g[2], g[3]);
                bad = 1;
            }
        }
    }
    if (!bad && vk_astc_bc()) {
        size_t b0 = (size_t)((w0 + 3) / 4) * ((h0 + 3) / 4) * 16, b1 = (size_t)((w1 + 3) / 4) * ((h1 + 3) / 4) * 16;
        uint8_t *bc = malloc(b0 + b1);
        char what[2][48];
        double db[2] = {0, 0};
        for (int lv = 0; lv < 2; lv++) snprintf(what[lv], sizeof what[lv], "%ux%u %s level %d", bw, bh, srgb ? "sRGB" : "linear", lv);
        if (!gpu_bc(c, blk, n0, n1, w0, h0, w1, h1, bw, bh, srgb, bc)) {
            printf("probe-astc: FAIL BC3 %s: the encode was not recorded\n", what[0]);
            bad = 1;
        }
        bad = bad || bc_check(want, bc, w0, h0, what[0], &db[0]) || bc_check(want + t0 * 4, bc + b0, w1, h1, what[1], &db[1]);
        for (int lv = 0; lv < 2 && !bad; lv++)
            if (db[lv] < g_bc_db) g_bc_db = db[lv];
        g_bc_cases += !bad;
        free(bc);
    }
    g_blocks += n0 + n1, g_texels += t0 + t1;
    free(blk), free(want), free(rb), free(im);
    return bad;
}

// One 2048 x 2048 texture of valid blocks: the CPU worker-pool decode against the GPU time of the dispatches alone
// (vk_astc.c timestamps, MP4_ASTC_TIME) and the wall time of the upload batch.
static void timing(VkCtx *c, uint32_t bw, uint32_t bh) {
    uint32_t w = 2048, h = 2048;
    size_t n = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh);
    uint8_t *blk = malloc(n * 16), *out = malloc((size_t)w * h * 4), tmp[12 * 12 * 4];
    for (size_t k = 0; k < 4096;) {   // valid, non-constant blocks: error blocks would flatter the GPU
        uint64_t v[2] = {rnd64(), rnd64()};
        if ((v[0] & 0x1FF) != 0x1FC && astc_decode_block((uint8_t *)v, bw, bh, false, tmp)) memcpy(blk + 16 * k++, v, 16);
    }
    for (size_t k = 4096; k < n; k++) memcpy(blk + 16 * k, blk + 16 * (k % 4096), 16);
    double a = now_ms();
    astc_decode_ldr_mt(blk, w, h, bw, bh, false, out);
    double cpu = now_ms() - a, wall = 0;
    vk_astc_time_enable(1);
    uint64_t t0, c0, t1, c1;
    double u0, u1;
    vk_astc_time_get(bw, bh, &t0, &c0, &u0);
    for (int i = 0; i < 4; i++) gpu_decode(c, blk, n, 1, w, h, 1, 1, bw, bh, 0, NULL, NULL, i ? &wall : NULL);
    vk_astc_time_get(bw, bh, &t1, &c1, &u1);
    vk_astc_time_enable(0);
    double mtex = (double)(t1 - t0) / 1e6;
    printf("probe-astc: 2048x2048 %ux%u: CPU (worker pool) %.1f ms; GPU dispatches %.0f us per M texel (%llu decodes "
           "timed, %.2f M texels), batch wall %.1f ms\n", bw, bh, cpu, mtex > 0 ? (u1 - u0) / mtex : 0.0,
           (unsigned long long)(c1 - c0), mtex, wall);
    free(blk), free(out);
}

int probe_astc(void) {
    setenv("MP4_ASTC_BC", "1", 1);   // also builds the BC3 encoder; the RGBA8 cases pass bc 0 and do not use it
    if (!vk_init()) { printf("probe-astc: FAIL vk_init\n"); return 1; }
    VkCtx *c = vk_ctx();
    printf("probe-astc: GPU %s, textureCompressionASTC_LDR %d, mode %d\n", c->props.deviceName, c->has_astc_ldr,
           vk_astc_mode());
    int bad = 0;
    if (!vk_astc_gpu()) {
        printf("probe-astc: FAIL the compute decoder is not available (MP4_ASTC=cpu or pipeline creation failed)\n");
        bad = 1;
    }
    for (int i = 0; i < 14 && !bad; i++)
        for (int s = 0; s < 2 && !bad; s++) bad = test_case(c, dims[i][0], dims[i][1], s);
    for (int b = 0; b < NB && !bad; b++)
        if (!g_bucket[b]) { printf("probe-astc: FAIL no valid %s block was generated\n", bucket_name[b]); bad = 1; }
    if (!bad && !vk_astc_bc()) printf("probe-astc: BC3 encoder not available on this device: not checked\n");
    else if (!bad) printf("probe-astc: BC3 encode: %llu cases within the bounds, lowest PSNR %.1f dB (floor %d)\n",
                          (unsigned long long)g_bc_cases, g_bc_db, BC_MIN_DB);
    if (!bad) {
        timing(c, 4, 4);
        timing(c, 6, 6);
        timing(c, 8, 8);
        timing(c, 12, 12);
    }
    vk_mem_shutdown();
    vk_shutdown();
    long errors = vk_validation_errors_total();
    if (bad || errors) {
        printf("probe-astc: FAILED (%s, %ld validation errors)\n", bad ? "a check failed" : "checks passed", errors);
        return 1;
    }
    printf("probe-astc: passed (14 footprints x linear/sRGB, %llu blocks, %llu texels bit-exact with astc_decode_ldr; "
           "valid blocks by kind: 1p %llu, 2p %llu, 3p %llu, 4p %llu, dual 1p %llu, dual 2p %llu, dual 3p %llu; "
           "%llu void extents, %llu random error blocks; 0 validation errors)\n",
           (unsigned long long)g_blocks, (unsigned long long)g_texels, (unsigned long long)g_bucket[0],
           (unsigned long long)g_bucket[1], (unsigned long long)g_bucket[2], (unsigned long long)g_bucket[3],
           (unsigned long long)g_bucket[4], (unsigned long long)g_bucket[5], (unsigned long long)g_bucket[6],
           (unsigned long long)g_void, (unsigned long long)g_err);
    return 0;
}

// --bench-astc: the compute decoder timed on real uploads, away from the game. Reads the files MP4_ASTC_DUMP wrote
// (vk_texture.c astc_dump) from that folder, groups them by the frame that uploaded them, and records each group into
// one upload batch, as the game did: GPU time of the dispatches per footprint (MP4_ASTC_TIME timestamps) and the batch
// wall time, best of MP4_ASTC_BENCH_RUNS (default 5). A last run reads the texels back and holds them to
// astc_decode_ldr bit for bit. 3D textures (slices > 1) are skipped.
typedef struct { uint64_t src; uint32_t w, h, slices, level, layer; } BenchReg;
typedef struct {
    uint32_t frame, bw, bh, srgb, width, height, levels, layers, n;
    BenchReg *r;
    uint8_t *blk;
    uint64_t bytes;
} BenchFile;

static int bench_load(const char *path, BenchFile *o) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8];
    uint32_t h[9];
    int ok = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "MP4ASTC1", 8) && fread(h, 4, 9, f) == 9;
    if (ok) {
        *o = (BenchFile){h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8]};
        o->r = calloc(o->n ? o->n : 1, sizeof *o->r);
        for (uint32_t i = 0; ok && i < o->n; i++) {
            uint32_t r[5];
            ok = fread(&o->r[i].src, 8, 1, f) == 1 && fread(r, 4, 5, f) == 5;
            o->r[i].w = r[0], o->r[i].h = r[1], o->r[i].slices = r[2], o->r[i].level = r[3], o->r[i].layer = r[4];
            if (r[2] != 1) ok = 0;
        }
        ok = ok && fread(&o->bytes, 8, 1, f) == 1 && (o->blk = malloc(o->bytes ? o->bytes : 1)) &&
             fread(o->blk, 1, o->bytes, f) == o->bytes;
        if (!ok) free(o->r), free(o->blk);
    }
    fclose(f);
    return ok;
}

static int bench_cmp(const void *a, const void *b) {
    const BenchFile *x = a, *y = b;
    return x->frame < y->frame ? -1 : x->frame > y->frame;
}

// One frame's files decoded in one batch; check: read back and compare (returns the mismatching textures).
static int bench_group(VkCtx *c, BenchFile *fs, int nf, int check, double *wall) {
    Img *im = calloc((size_t)nf, sizeof *im);
    VkBuffer *rbuf = calloc((size_t)nf, sizeof *rbuf);
    VmaAllocation *ralloc = calloc((size_t)nf, sizeof *ralloc);
    uint8_t **rmap = calloc((size_t)nf, sizeof *rmap);
    VkCommandBuffer cb = vk_upload_begin();
    for (int i = 0; i < nf; i++) {
        BenchFile *f = &fs[i];
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = f->srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                                .extent = {f->width, f->height, 1}, .mipLevels = f->levels, .arrayLayers = f->layers,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateImage(c->vma, &ci, &ai, &im[i].img, &im[i].alloc, NULL) != VK_SUCCESS) rt_fatal("bench-astc: image");
        VkImageMemoryBarrier2 ib = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
                                    .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                    .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = im[i].img,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &ib};
        vkCmdPipelineBarrier2(cb, &di);
        VkBuffer sb;
        memcpy(vk_staging(f->bytes, &sb, NULL), f->blk, f->bytes);
        VkAstcRegion *ar = calloc(f->n, sizeof *ar);
        uint64_t out = 0;
        for (uint32_t k = 0; k < f->n; k++) {
            ar[k] = (VkAstcRegion){.src = f->r[k].src, .w = f->r[k].w, .h = f->r[k].h, .slices = 1,
                                   .level = f->r[k].level, .layer = f->r[k].layer};
            out += (uint64_t)f->r[k].w * f->r[k].h * 4;
        }
        if (check) rmap[i] = vk_staging(out, &rbuf[i], &ralloc[i]);
        barrier(cb, ALL_RW, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        if (!vk_astc_record(cb, sb, f->bytes, ar, (int)f->n, f->bw, f->bh, (int)f->srgb, 0, im[i].img,
                            VK_IMAGE_ASPECT_COLOR_BIT, check ? rbuf[i] : VK_NULL_HANDLE))
            rt_fatal("bench-astc: vk_astc_record refused %ux%u", f->bw, f->bh);
        barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, ALL_RW);
        free(ar);
    }
    vk_upload_end();
    double a = now_ms();
    vk_upload_flush(1);
    *wall = now_ms() - a;
    int bad = 0;
    for (int i = 0; i < nf; i++) {
        if (check) {
            BenchFile *f = &fs[i];
            vmaInvalidateAllocation(c->vma, ralloc[i], 0, VK_WHOLE_SIZE);
            uint64_t off = 0;
            int diff = 0;
            for (uint32_t k = 0; k < f->n; k++) {
                size_t t = (size_t)f->r[k].w * f->r[k].h * 4;
                uint8_t *want = malloc(t);
                astc_decode_ldr(f->blk + f->r[k].src, f->r[k].w, f->r[k].h, f->bw, f->bh, f->srgb != 0, want);
                diff |= memcmp(want, rmap[i] + off, t) != 0;
                off += t;
                free(want);
            }
            if (diff && bad++ < 8)
                printf("bench-astc: MISMATCH frame %u %ux%u %ux%u texture (%u regions)\n", f->frame, f->bw, f->bh,
                       f->width, f->height, f->n);
            vmaDestroyBuffer(c->vma, rbuf[i], ralloc[i]);
        }
        vk_defer_image(im[i].img, im[i].alloc);
    }
    vk_upload_flush(1);
    free(im), free(rbuf), free(ralloc), free(rmap);
    return bad;
}

typedef struct { const char *dir; BenchFile *fs; int n, cap, skipped; } BenchList;

static int bench_collect(const char *name, void *ctx) {
    BenchList *l = ctx;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/%s", l->dir, name);
    if (l->n == l->cap) l->fs = realloc(l->fs, (size_t)(l->cap = l->cap ? l->cap * 2 : 64) * sizeof *l->fs);
    if (bench_load(p, &l->fs[l->n])) l->n++;
    else l->skipped++;
    return 1;
}

int bench_astc(void) {
    const char *dir = cfg_get("MP4_ASTC_DUMP");
    if (!dir || !*dir) { printf("bench-astc: set MP4_ASTC_DUMP to the folder of dumped uploads\n"); return 2; }
    const char *rs = cfg_get("MP4_ASTC_BENCH_RUNS");
    int runs = rs && *rs ? atoi(rs) : 5;
    BenchList bl = {dir, NULL, 0, 0, 0};
    if (!plat_file_list(dir, ".astc", bench_collect, &bl) || !bl.n) {
        printf("bench-astc: no .astc files in %s\n", dir);
        free(bl.fs);
        return 2;
    }
    BenchFile *fs = bl.fs;
    int nf = bl.n, skipped = bl.skipped;
    qsort(fs, (size_t)nf, sizeof *fs, bench_cmp);
    if (!vk_init()) { printf("bench-astc: FAIL vk_init\n"); return 1; }
    VkCtx *c = vk_ctx();
    printf("bench-astc: GPU %s; %d textures in %s (%d skipped: 3D or unreadable), best of %d runs\n",
           c->props.deviceName, nf, dir, skipped, runs);
    static const uint8_t fp[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8},
                                    {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    enum { NFP = sizeof fp / sizeof fp[0] };
    double best_us[NFP], sum_wall = 0, sum_gpu = 0;
    uint64_t texels[NFP] = {0};
    for (int k = 0; k < NFP; k++) best_us[k] = 0;
    int bad = 0;
    // Groups: the textures one frame uploaded.
    for (int g0 = 0; g0 < nf;) {
        int g1 = g0;
        uint64_t gt = 0;
        while (g1 < nf && fs[g1].frame == fs[g0].frame) {
            for (uint32_t k = 0; k < fs[g1].n; k++) gt += (uint64_t)fs[g1].r[k].w * fs[g1].r[k].h;
            g1++;
        }
        double bw_ = 1e300, bg = 1e300, gus[NFP], first_g = 0, first_w = 0;
        uint64_t gtex[NFP];
        if (gt > 10000000) plat_sleep_ns((uint64_t)(2000) * 1000000ull);   // a large group starts from an idle GPU, as in the game: the first run is cold
        for (int run = 0; run < runs; run++) {
            double t0[NFP], wall, tot = 0;
            uint64_t x0[NFP], cc;
            vk_astc_time_enable(1);
            for (int k = 0; k < NFP; k++) vk_astc_time_get(fp[k][0], fp[k][1], &x0[k], &cc, &t0[k]);
            bench_group(c, fs + g0, g1 - g0, 0, &wall);
            vk_upload_flush(1);
            double us[NFP];
            uint64_t tx[NFP];
            for (int k = 0; k < NFP; k++) {
                uint64_t x1;
                double t1;
                vk_astc_time_get(fp[k][0], fp[k][1], &x1, &cc, &t1);
                us[k] = t1 - t0[k], tx[k] = x1 - x0[k], tot += us[k];
            }
            vk_astc_time_enable(0);
            if (!run) first_g = tot, first_w = wall;
            if (tot < bg) { bg = tot; memcpy(gus, us, sizeof us), memcpy(gtex, tx, sizeof tx); }
            if (wall < bw_) bw_ = wall;
        }
        printf("bench-astc: frame %u: %d textures, %.2f M texels: dispatches %.2f ms GPU, batch wall %.2f ms (first run: "
               "%.2f, %.2f)\n", fs[g0].frame, g1 - g0, gt / 1e6, bg / 1e3, bw_, first_g / 1e3, first_w);
        for (int k = 0; k < NFP; k++)
            if (gtex[k]) {
                texels[k] += gtex[k];
                best_us[k] += gus[k];
            }
        sum_wall += bw_, sum_gpu += bg;
        double w;
        bad += bench_group(c, fs + g0, g1 - g0, 1, &w);
        g0 = g1;
    }
    for (int k = 0; k < NFP; k++)
        if (texels[k])
            printf("bench-astc: %ux%u: %.2f M texels, %.2f ms GPU, %.0f us per M texel\n", fp[k][0], fp[k][1],
                   texels[k] / 1e6, best_us[k] / 1e3, best_us[k] / (texels[k] / 1e6));
    printf("bench-astc: all: dispatches %.2f ms GPU, batch wall %.2f ms; %s\n", sum_gpu / 1e3, sum_wall,
           bad ? "MISMATCHES (see above)" : "every texel bit-exact with astc_decode_ldr");
    for (int i = 0; i < nf; i++) free(fs[i].r), free(fs[i].blk);
    free(fs);
    vk_mem_shutdown();
    vk_shutdown();
    return bad ? 1 : 0;
}
