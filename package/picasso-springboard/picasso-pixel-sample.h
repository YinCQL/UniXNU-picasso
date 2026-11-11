// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_PIXEL_SAMPLE_H
#define PICASSO_PIXEL_SAMPLE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct picasso_pixel_sample {
    unsigned count, rgb_nonzero, alpha_nonzero, varied;
    uint32_t first, hash;
};

/* Sample a 4x4 grid of an existing RGBA/RGBX buffer, without decoding,
 * allocating or modifying it. A caller must establish its pixel format.
 * A grid can miss details: zero samples do not prove the whole image black. */
static int picasso_sample_rgb32(const unsigned char *pixels, size_t bytes,
                               uint32_t width, uint32_t height, size_t row,
                               struct picasso_pixel_sample *sample, int alpha_first)
{
    memset(sample, 0, sizeof(*sample));
    if (!pixels || !width || !height || (uint64_t)width * 4 > SIZE_MAX ||
        row < (size_t)width * 4 || row > SIZE_MAX / height ||
        bytes < row * (height - 1) + (size_t)width * 4) return 0;
    sample->hash = UINT32_C(2166136261);
    for (unsigned y = 0; y < 4; ++y) {
        size_t py = ((uint64_t)(2 * y + 1) * height) / 8;
        for (unsigned x = 0; x < 4; ++x) {
            size_t px = ((uint64_t)(2 * x + 1) * width) / 8;
            const unsigned char *pixel = pixels + py * row + px * 4;
            uint32_t value;
            memcpy(&value, pixel, sizeof(value));
            if (!sample->count) sample->first = value;
            unsigned rgb = alpha_first ? 1 : 0;
            sample->rgb_nonzero += (pixel[rgb] | pixel[rgb + 1] | pixel[rgb + 2]) != 0;
            sample->alpha_nonzero += pixel[alpha_first ? 0 : 3] != 0;
            sample->varied += value != sample->first;
            for (unsigned k = 0; k < 4; ++k)
                sample->hash = (sample->hash ^ pixel[k]) * UINT32_C(16777619);
            ++sample->count;
        }
    }
    return 1;
}
static int picasso_sample_rgba(const unsigned char *pixels, size_t bytes,
                               uint32_t width, uint32_t height, size_t row,
                               struct picasso_pixel_sample *sample)
{
    return picasso_sample_rgb32(pixels, bytes, width, height, row, sample, 0);
}
#endif
