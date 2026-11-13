// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_SNAPSHOT_SURFACE_H
#define PICASSO_SNAPSHOT_SURFACE_H

static bool picasso_snapshot_bgra(IOSurfaceRef surface, size_t width, size_t height)
{
    return surface && height && IOSurfaceGetWidth(surface) == width &&
        IOSurfaceGetHeight(surface) == height && IOSurfaceGetPixelFormat(surface) == 0x42475241 &&
        !IOSurfaceGetPlaneCount(surface) && IOSurfaceGetBytesPerElement(surface) == 4 &&
        IOSurfaceGetBytesPerRow(surface) >= width * 4 &&
        IOSurfaceGetBytesPerRow(surface) <= IOSurfaceGetAllocSize(surface) / height;
}

static bool picasso_snapshot_resample_surface(IOSurfaceRef source, IOSurfaceRef target, bool opaque)
{
    // Limit the workaround to the observed portrait scene snapshot conversion.
    if (source == target || !picasso_snapshot_bgra(source, 1080, 2400) ||
        !picasso_snapshot_bgra(target, 756, 1680)) return false;
    size_t sr = IOSurfaceGetBytesPerRow(source), tr = IOSurfaceGetBytesPerRow(target);
    const size_t packedRow = 756 * 4;
    unsigned char *pixels = malloc(packedRow * 1680);
    if (!pixels) return false;
    uint32_t before = 0, after = 0;
    if (IOSurfaceLock(source, kIOSurfaceLockReadOnly, &before)) { free(pixels); return false; }
    const unsigned char *input = IOSurfaceGetBaseAddress(source);
    bool valid = input != NULL;
    // The stock context uses BGRX when opaque. Otherwise accept only fully
    // opaque pixels; arbitrary straight/premultiplied alpha remains stock.
    for (size_t y = 0; valid && !opaque && y < 2400; ++y)
        for (size_t x = 0; x < 1080; ++x)
            if (input[y * sr + x * 4 + 3] != 255) { valid = false; break; }
    bool ok = valid && (opaque ?
        picasso_snapshot_resample_opaque(input, 1080, 2400, sr, pixels, 756, 1680, packedRow) :
        picasso_snapshot_resample(input, 1080, 2400, sr, pixels, 756, 1680, packedRow));
    kern_return_t unlocked = IOSurfaceUnlock(source, kIOSurfaceLockReadOnly, &after);
    if (!ok || unlocked || before != after || IOSurfaceGetSeed(source) != before) {
        free(pixels); return false;
    }
    // Keep the original destination, its color attachments, and row padding.
    // Publish only the completed private result, before returning to the caller.
    if (IOSurfaceLock(target, 0, NULL)) { free(pixels); return false; }
    unsigned char *output = IOSurfaceGetBaseAddress(target);
    ok = output && IOSurfaceGetSeed(source) == before;
    if (ok)
        for (size_t y = 0; y < 1680; ++y)
            memcpy(output + y * tr, pixels + y * packedRow, packedRow);
    kern_return_t result = IOSurfaceUnlock(target, 0, NULL);
    free(pixels);
    return ok && !result;
}
#endif
