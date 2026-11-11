// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_WALLPAPER_SURFACE_H
#define PICASSO_WALLPAPER_SURFACE_H
#include "picasso-pixel-sample.h"
struct picasso_wallpaper_surface_result {
    size_t width, height, row, bytes, element, planes;
    uint32_t identifier, format, seed, unlock_seed;
    int lock_result, unlock_result;
    unsigned w30r_min, w30r_max, w30r_zero, w30r_black, w30r_padding;
    struct picasso_pixel_sample pixels;
};
/* CoreVideo w30r: little-endian RGB101010, two zero padding MSBs.
 * Encoded black is 384 and white is 895. Preserve raw words in first/hash;
 * positive RGB counts channels above black, never interpret padding as alpha.
 */
static int picasso_wallpaper_sample_w30r(const unsigned char *base,
    struct picasso_wallpaper_surface_result *result)
{
    struct picasso_pixel_sample *sample = &result->pixels;
    if (!picasso_sample_rgb32(base, result->bytes, (uint32_t)result->width,
        (uint32_t)result->height, result->row, sample, 0)) return 0;
    sample->rgb_nonzero = sample->alpha_nonzero = 0;
    result->w30r_min = 1023;
    for (unsigned y = 0; y < 4; ++y) {
        size_t py = ((uint64_t)(2 * y + 1) * result->height) / 8;
        for (unsigned x = 0; x < 4; ++x) {
            size_t px = ((uint64_t)(2 * x + 1) * result->width) / 8;
            const unsigned char *p = base + py * result->row + px * 4;
            uint32_t word = (uint32_t)p[0] | (uint32_t)p[1] << 8 |
                (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
            unsigned positive = 0, black = 1;
            for (unsigned k = 0; k < 3; ++k) {
                unsigned code = (word >> (10 * k)) & 1023;
                if (code < result->w30r_min) result->w30r_min = code;
                if (code > result->w30r_max) result->w30r_max = code;
                positive |= code > 384;
                black &= code == 384;
            }
            sample->rgb_nonzero += positive;
            result->w30r_black += black;
            result->w30r_zero += (word & UINT32_C(0x3fffffff)) == 0;
            result->w30r_padding += (word >> 30) != 0;
        }
    }
    return 1;
}
static const char *picasso_wallpaper_sample_surface(IOSurfaceRef surface,
    struct picasso_wallpaper_surface_result *result)
{
    memset(result, 0, sizeof(*result));
    if (!surface) return "no-surface";
    result->identifier = IOSurfaceGetID(surface);
    result->lock_result = IOSurfaceLock(surface, kIOSurfaceLockReadOnly, &result->seed);
    if (result->lock_result) return "lock-failed";
    result->width = IOSurfaceGetWidth(surface);
    result->height = IOSurfaceGetHeight(surface);
    result->row = IOSurfaceGetBytesPerRow(surface);
    result->bytes = IOSurfaceGetAllocSize(surface);
    result->element = IOSurfaceGetBytesPerElement(surface);
    result->planes = IOSurfaceGetPlaneCount(surface);
    result->format = IOSurfaceGetPixelFormat(surface);
    const char *state = "metadata";
    /* Only established packed layouts. Never render, decode, change
     * storage or read unsupported planes/HDR/compressed formats. */
    if (result->width && result->height && result->width <= 8192 && result->height <= 8192 &&
        result->width * result->height <= 16 * 1024 * 1024 &&
        result->bytes <= 64 * 1024 * 1024 && result->element == 4 && !result->planes &&
        (result->format == UINT32_C(0x42475241) || result->format == UINT32_C(0x52474241) ||
            result->format == UINT32_C(0x77333072))) {
        const unsigned char *base = IOSurfaceGetBaseAddress(surface);
        int sampled = result->format == UINT32_C(0x77333072) ? picasso_wallpaper_sample_w30r(base, result) :
            picasso_sample_rgb32(base, result->bytes, (uint32_t)result->width,
                (uint32_t)result->height, result->row, &result->pixels, 0);
        state = sampled ? "pixels" : "no-linear-pixels";
    }
    result->unlock_result = IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, &result->unlock_seed);
    if (result->unlock_result || result->unlock_seed != result->seed) {
        memset(&result->pixels, 0, sizeof(result->pixels));
        result->w30r_min = result->w30r_max = result->w30r_zero = result->w30r_black = result->w30r_padding = 0;
        return result->unlock_result ? "unlock-failed" : "seed-changed";
    }
    return state;
}
#endif
