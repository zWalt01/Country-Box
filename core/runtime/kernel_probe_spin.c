// Country-Box: the stretch of "game code" the --probe-kernel asynchronous preemption test runs (kernel_probe.c
// test_async_preempt). A long loop with no call, no SVC and no indirect branch, as lifted code between checkpoints;
// it keeps its state in general registers and YMM registers, so a preemption that loses one changes the result.
//
// It must be built like the lifted code (-O1 -mno-red-zone -march=x86-64-v3) and linked between the lifted objects,
// inside the host range runtime/kernel.c ap_init takes as game code (tools/build_probe.sh does both).
#include <immintrin.h>
#include <stdint.h>

uint64_t kern_probe_spin(uint64_t n, uint64_t seed) {
    __m256i a = _mm256_set1_epi64x((long long)seed), b = _mm256_set_epi64x(1, 3, 5, 7);
    __m256i c = _mm256_set_epi64x(11, 13, 17, 19), d = _mm256_set1_epi64x(0x9E3779B97F4A7C15ll);
    uint64_t x0 = seed, x1 = seed ^ 0xA5A5A5A5u, x2 = seed * 3 + 1, x3 = ~seed, x4 = 0x1234, x5 = 0x5678;
    for (uint64_t i = 0; i < n; i++) {
        a = _mm256_add_epi64(a, b);
        b = _mm256_xor_si256(b, _mm256_slli_epi64(a, 1));
        c = _mm256_add_epi64(c, _mm256_srli_epi64(b, 3));
        d = _mm256_xor_si256(d, _mm256_add_epi64(c, a));
        x0 += x1 ^ i;
        x1 = ((x1 << 7) | (x1 >> 57)) + x2;
        x2 ^= x3 + i;
        x3 += x0 >> 3;
        x4 = x4 * 6364136223846793005ull + x5;
        x5 ^= x4 >> 29;
        __asm__ volatile("" : "+x"(a), "+x"(b), "+x"(c), "+x"(d));   // the vectors stay in registers every pass
    }
    uint64_t v[4];
    _mm256_storeu_si256((__m256i *)v, _mm256_xor_si256(_mm256_xor_si256(a, b), _mm256_xor_si256(c, d)));
    return x0 ^ x1 ^ x2 ^ x3 ^ x4 ^ x5 ^ v[0] ^ (v[1] << 1) ^ (v[2] << 2) ^ (v[3] << 3);
}
