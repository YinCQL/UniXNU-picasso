#include "../../drivers/NativeGL/eagl/surface_hash.h"
#include <assert.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
typedef struct { unsigned numer, denom; } mach_timebase_info_data_t;
static void mach_timebase_info(mach_timebase_info_data_t *info) { info->numer=info->denom=1; }
static uint64_t mach_absolute_time(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now)==0);
    return (uint64_t)now.tv_sec*1000000000+now.tv_nsec;
}
#endif
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t old_hash(const unsigned char *p, size_t size)
{
    uint64_t sum = size, word;
    for (size_t i = 0; i + 8 <= size; i += 8) {
        memcpy(&word, p + i, 8);
        sum = (sum ^ word) * UINT64_C(0x9e3779b97f4a7c15);
        sum ^= sum >> 29;
    }
    return sum;
}

static uint64_t le_word(const unsigned char *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (8 * i);
    return value;
}

/* Independent byte-oriented reference for unaligned and tail cases. */
static uint64_t reference(const unsigned char *p, size_t size)
{
    uint64_t lanes[4] = {UINT64_C(0x243f6a8885a308d3), UINT64_C(0x13198a2e03707344),
        UINT64_C(0xa4093822299f31d0), UINT64_C(0x082efa98ec4e6c89)};
    size_t at = 0;
    for (unsigned i = 0; i < 4; ++i) lanes[i] ^= size;
    for (; size - at >= 32; at += 32)
        for (unsigned i = 0; i < 4; ++i)
            lanes[i] = (lanes[i] ^ le_word(p + at + 8 * i)) * UINT64_C(0x9e3779b97f4a7c15);
    uint64_t result = 0;
    for (unsigned i = 0; i < 4; ++i) result ^= nativegl_hash_mix(lanes[i]);
    for (; size - at >= 8; at += 8)
        result = (result ^ le_word(p + at)) * UINT64_C(0x9e3779b97f4a7c15);
    uint64_t tail = 0;
    for (size_t i = at; i < size; ++i) tail |= (uint64_t)p[i] << (8 * (i - at));
    return nativegl_hash_mix(result ^ tail ^ ((uint64_t)(size - at) << 56));
}

int main(void)
{
    unsigned char data[1024];
    uint32_t rng = 12345;
    for (size_t i = 0; i < sizeof data; ++i) { rng = rng * 1664525u + 1013904223u; data[i] = rng >> 24; }
    size_t mutations = 0;
    for (size_t offset = 0; offset < 16; ++offset)
        for (size_t n = 0; n <= 513; ++n) {
            uint64_t hash = nativegl_surface_hash(data + offset, n);
            assert(hash == reference(data + offset, n));
            for (size_t i = 0; i < n; ++i) {
                data[offset + i] ^= 1;
                assert(nativegl_surface_hash(data + offset, n) != hash);
                data[offset + i] ^= 1;
                ++mutations;
            }
        }
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *guard = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(guard != MAP_FAILED && mprotect(guard + page, page, PROT_NONE) == 0);
    memset(guard, 0xa5, page);
    for (size_t n = 0; n <= 513; ++n)
        assert(nativegl_surface_hash(guard + page - n, n) == reference(guard + page - n, n));
    munmap(guard, 2 * page);
    printf("PASS: 8224 reference cases, %zu single-byte mutations, 514 guard-page bounds\n", mutations);
    size_t size = 1080u * 2400u * 4u;
    unsigned char *large = malloc(size);
    assert(large);
    for (size_t i = 0; i < size; ++i) large[i] = (unsigned char)i;
    volatile uint64_t sink = 0;
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    for (unsigned mode = 0; mode < 2; ++mode) {
        uint64_t began = mach_absolute_time();
        for (unsigned iteration = 0; iteration < 100; ++iteration) {
            large[iteration] ^= (unsigned char)iteration;
            sink ^= mode ? nativegl_surface_hash(large, size) : old_hash(large, size);
        }
        double us = (double)(mach_absolute_time() - began) * timebase.numer / timebase.denom / 1000 / 100;
        printf("BENCH: mode=%u %.3f us / 1080x2400 BGRA surface (host only)\n", mode, us);
    }
    free(large);
    return sink == UINT64_MAX;
}
