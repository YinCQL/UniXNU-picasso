/* Experimental non-cryptographic change detector. Every byte participates; no overread. */
#ifndef NATIVEGL_SURFACE_HASH_H
#define NATIVEGL_SURFACE_HASH_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint64_t
nativegl_hash_mix(uint64_t value)
{
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    value *= UINT64_C(0xc4ceb9fe1a85ec53);
    return value ^ (value >> 33);
}

static inline uint64_t
nativegl_surface_hash(const void *bytes, size_t size)
{
    const unsigned char *p = bytes;
    const uint64_t prime = UINT64_C(0x9e3779b97f4a7c15);
    uint64_t a = size ^ UINT64_C(0x243f6a8885a308d3);
    uint64_t b = size ^ UINT64_C(0x13198a2e03707344);
    uint64_t c = size ^ UINT64_C(0xa4093822299f31d0);
    uint64_t d = size ^ UINT64_C(0x082efa98ec4e6c89);
    size_t remaining = size;
    /* Four independent dependency chains instead of one per eight bytes. */
    while (remaining >= 32) {
        uint64_t words[4];
        memcpy(words, p, sizeof words);
        a = (a ^ words[0]) * prime;
        b = (b ^ words[1]) * prime;
        c = (c ^ words[2]) * prime;
        d = (d ^ words[3]) * prime;
        p += 32;
        remaining -= 32;
    }
    uint64_t result = nativegl_hash_mix(a) ^ nativegl_hash_mix(b) ^
        nativegl_hash_mix(c) ^ nativegl_hash_mix(d);
    while (remaining >= 8) {
        uint64_t word;
        memcpy(&word, p, sizeof word);
        result = (result ^ word) * prime;
        p += 8;
        remaining -= 8;
    }
    uint64_t tail = 0;
    for (size_t i = 0; i < remaining; ++i)
        tail |= (uint64_t)p[i] << (8 * i);
    return nativegl_hash_mix(result ^ tail ^ ((uint64_t)remaining << 56));
}
#endif
