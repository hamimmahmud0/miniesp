// Benchmark kernels shared by the firmware (main/bench.c, native) and esp-bench (AOT in WAMR): the SAME source runs both ways,
// so the AOT/native ratio is meaningful. Freestanding: stdint only. One call of bk_iter() is one "iteration" (about 20-300 us).
#pragma once
#include <stdint.h>

#define BK_KINDS   8
#define BK_SCRATCH 4096                 // bytes of scratch memory a caller must provide (32-bit aligned)
#define BK_NAMES   { "int", "float", "double", "memcpy", "sort", "crc32", "matmul", "chase" }
// what one iteration does (for the unit column): int 1000 LCG/xorshift steps; float/double 400 multiply-adds; memcpy 1 KB;
// sort 128 words; crc32 64 bytes (bitwise); matmul 8x8x8 int32; chase 256 dependent loads
#define BK_UNITS   { 1000, 400, 400, 1024, 128, 64, 512, 256 }       // work per iteration: steps, flops, bytes, ...
#define BK_UNIT_NAMES { "steps", "flops", "bytes", "bytes", "keys", "bytes", "macs", "loads" }

static inline uint32_t bk_iter(int kind, uint32_t *sc, uint32_t seed)
{
    uint32_t x = seed | 1, acc = 0;
    switch (kind) {
    case 0:
        for (int i = 0; i < 1000; i++) { x = x * 1664525u + 1013904223u; x ^= x >> 13; acc += x; }
        return acc;
    case 1: {
        volatile float f = 1.0f; float g = (float)(x & 255) + 1.0f, h = f;
        for (int i = 0; i < 400; i++) { h = h * 0.99991f + g; g = g * 1.00003f - 0.25f; }
        f = h; return (uint32_t)(int32_t)h;
    }
    case 2: {
        volatile double f = 1.0; double g = (double)(x & 255) + 1.0, h = f;
        for (int i = 0; i < 400; i++) { h = h * 0.99991 + g; g = g * 1.00003 - 0.25; }
        f = h; return (uint32_t)(int32_t)h;
    }
    case 3: {                                        // 1 KB copy, word by word, between two halves of the scratch
        uint32_t *s = sc, *d = sc + 512;
        for (int i = 0; i < 256; i++) d[i] = s[i] + (uint32_t)i + x;      // x differs every call: nothing can be hoisted
        for (int i = 0; i < 256; i++) acc += d[i];
        return acc;
    }
    case 4: {                                        // shell sort of 128 pseudo-random words
        for (int i = 0; i < 128; i++) { x = x * 1664525u + 1013904223u; sc[i] = x >> 8; }
        for (int gap = 57; gap > 0; gap = gap == 57 ? 23 : gap == 23 ? 10 : gap == 10 ? 4 : gap == 4 ? 1 : 0)
            for (int i = gap; i < 128; i++) {
                uint32_t v = sc[i]; int j = i;
                while (j >= gap && sc[j - gap] > v) { sc[j] = sc[j - gap]; j -= gap; }
                sc[j] = v;
            }
        return sc[0] ^ sc[127];
    }
    case 5: {                                        // bitwise CRC-32 over 64 bytes
        uint32_t crc = 0xFFFFFFFFu ^ x;
        const uint8_t *p = (const uint8_t *)sc;
        for (int i = 0; i < 64; i++) {
            crc ^= p[i];
            for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    }
    case 6: {                                        // 8x8 int32 matrix product C = A*B (A, B, C = 3 x 64 words)
        uint32_t *A = sc, *B = sc + 64, *C = sc + 128;
        for (int i = 0; i < 64; i++) { A[i] = (uint32_t)i * 3u + x; B[i] = (uint32_t)i * 5u + 1u; }
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++) { uint32_t s = 0; for (int k = 0; k < 8; k++) s += A[i * 8 + k] * B[k * 8 + j]; C[i * 8 + j] = s; }
        return C[0] ^ C[63];
    }
    default: {                                       // pointer chase through a 256-entry ring (odd stride)
        uint32_t st = (x >> 8) | 1u;                      // odd step: one cycle through all 256 entries
        for (int i = 0; i < 256; i++) sc[i] = (uint32_t)((i + st) & 255);
        uint32_t p = x & 255;
        for (int i = 0; i < 256; i++) p = sc[p];
        return p;
    }
    }
}

// scratch preparation for kinds that read it as data (memcpy, crc32)
static inline void bk_prepare(uint32_t *sc) { for (int i = 0; i < BK_SCRATCH / 4; i++) sc[i] = (uint32_t)i * 2654435761u; }
