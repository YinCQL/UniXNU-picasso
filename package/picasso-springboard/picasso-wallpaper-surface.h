// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_WALLPAPER_SURFACE_H
#define PICASSO_WALLPAPER_SURFACE_H
#include "picasso-pixel-sample.h"
struct picasso_wallpaper_surface_result {
    size_t width, height, row, bytes, element, planes;
    uint32_t identifier, format, seed, unlock_seed;
    int lock_result, unlock_result;
    struct picasso_pixel_sample pixels;
};
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
    /* Only established packed 8-bit layouts. Never render, decode, change
     * storage or read unsupported planes/HDR/compressed formats. */
    if (result->width && result->height && result->width <= 8192 && result->height <= 8192 &&
        result->width * result->height <= 16 * 1024 * 1024 &&
        result->bytes <= 64 * 1024 * 1024 && result->element == 4 && !result->planes &&
        (result->format == UINT32_C(0x42475241) || result->format == UINT32_C(0x52474241))) {
        const unsigned char *base = IOSurfaceGetBaseAddress(surface);
        state = picasso_sample_rgb32(base, result->bytes, (uint32_t)result->width,
            (uint32_t)result->height, result->row, &result->pixels, 0) ? "pixels" : "no-linear-pixels";
    }
    result->unlock_result = IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, &result->unlock_seed);
    if (result->unlock_result) { memset(&result->pixels, 0, sizeof(result->pixels)); return "unlock-failed"; }
    if (result->unlock_seed != result->seed) {
        memset(&result->pixels, 0, sizeof(result->pixels)); return "seed-changed";
    }
    return state;
}
#endif
