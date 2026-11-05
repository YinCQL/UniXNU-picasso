// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_ASTC_GUARD_CORE_H
#define PICASSO_ASTC_GUARD_CORE_H
#include <stdint.h>
#include <string.h>

/* Private object offsets are used only after the ImageIO UUID, code hashes
 * and original vtable entries have all matched the arm64 20H392 image. */
struct picasso_astc_request {
    uint32_t width, height, block_width, block_height, format;
    uint64_t linear_bytes;
};
enum picasso_astc_action { PICASSO_ASTC_PASS, PICASSO_ASTC_LAYOUT, PICASSO_ASTC_INVALID };

static uintptr_t picasso_astc_pointer(const void *object, unsigned offset)
{
    uintptr_t value;
    memcpy(&value, (const unsigned char *)object + offset, sizeof(value));
    return value;
}

static uint32_t picasso_astc_u32(const void *object, unsigned offset)
{
    uint32_t value;
    memcpy(&value, (const unsigned char *)object + offset, sizeof(value));
    return value;
}

static enum picasso_astc_action
picasso_astc_request(const void *plugin, int atx, uintptr_t texture_vtable,
                    struct picasso_astc_request *request)
{
    if (!plugin) return PICASSO_ASTC_INVALID;
    /* ATX's 420f and non-ASTC dispatch are unrelated to this defect. */
    if (atx && (picasso_astc_pointer(plugin, 0x230) ||
                picasso_astc_pointer(plugin, 0x248) ||
                !picasso_astc_pointer(plugin, 0x228))) return PICASSO_ASTC_PASS;
    const void *texture = (const void *)picasso_astc_pointer(plugin, 0x1c0);
    if (!texture || picasso_astc_pointer(texture, 0) != texture_vtable)
        return PICASSO_ASTC_INVALID;
    if (!*((const unsigned char *)texture + 0x29)) return PICASSO_ASTC_PASS;
    /* LZFSE+twiddled already takes the original error path, without libate. */
    if (*((const unsigned char *)texture + 0x28)) return PICASSO_ASTC_PASS;
    request->block_width = picasso_astc_u32(texture, 0x1c);
    request->block_height = picasso_astc_u32(texture, 0x20);
    request->width = picasso_astc_u32(texture, 0x64);
    request->height = picasso_astc_u32(texture, 0x68);
    if (request->block_width != request->block_height ||
        (request->block_width != 4 && request->block_width != 8) ||
        !request->width || !request->height) return PICASSO_ASTC_INVALID;
    /* Match decodeRGBXFromTwiddled's descriptor, including its 32-bit
     * allocation and row arithmetic. Never truncate an unrepresentable size. */
    uint64_t columns = ((uint64_t)request->width + request->block_width - 1) / request->block_width;
    uint64_t rows = ((uint64_t)request->height + request->block_height - 1) / request->block_height;
    if (columns > UINT32_MAX / 16 || rows > UINT32_MAX / (columns * 16))
        return PICASSO_ASTC_INVALID;
    request->linear_bytes = columns * rows * 16;
    request->format = request->block_width == 4 ? 204 : 212;
    return PICASSO_ASTC_LAYOUT;
}

static int picasso_astc_layout_fits(const struct picasso_astc_request *request, uint64_t bytes)
{
    return bytes >= request->linear_bytes && bytes <= UINT32_MAX;
}
#endif
