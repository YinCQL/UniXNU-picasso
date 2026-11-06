// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso V13.0.5.0 split ELF32/Hexagon layout. Independent implementation
// from peripheral-loader.c's segment/relocation contract, with bounded inputs.
// This plan neither authenticates firmware nor maps/writes the DSP carveout.
#ifndef PICASSO_ADSP_LOAD_PLAN_H
#define PICASSO_ADSP_LOAD_PLAN_H
#include <stdint.h>
#include <stddef.h>
constexpr uint32_t picassoAdspBase = 0x89200000, picassoAdspBytes = 0x02800000;
constexpr unsigned picassoAdspHeaders = 34, picassoAdspMetadataBytes = 9340;
struct PicassoAdspSegment {
    uint32_t index, fileOffset, memoryOffset, fileBytes, zeroBytes, flags;
};
struct PicassoAdspLoadPlan {
    const char *phase;
    uint32_t index, count, entry, fileBytes, zeroBytes, imageBytes, alignment;
    PicassoAdspSegment segments[picassoAdspHeaders - 2];
};
static inline uint16_t picasso_adsp_u16(const uint8_t *p) { return p[0] | uint16_t(p[1]) << 8; }
static inline uint32_t picasso_adsp_u32(const uint8_t *p) { return picasso_adsp_u16(p) | uint32_t(picasso_adsp_u16(p + 2)) << 16; }
static inline int picasso_adsp_load_plan(const uint8_t *mdt, size_t size,
    const uint32_t *pieces, size_t pieceCount, PicassoAdspLoadPlan *plan) {
    if (!plan) return -22;
    *plan = {}; plan->phase = "elf"; plan->index = ~0U;
    if (!mdt || !pieces || size != picassoAdspMetadataBytes || pieceCount != picassoAdspHeaders) return -22;
    if (picasso_adsp_u32(mdt) != 0x464c457f || mdt[4] != 1 || mdt[5] != 1 || mdt[6] != 1 ||
        picasso_adsp_u16(mdt + 16) != 2 || picasso_adsp_u16(mdt + 18) != 164 || picasso_adsp_u32(mdt + 20) != 1 ||
        picasso_adsp_u32(mdt + 28) != 52 || picasso_adsp_u16(mdt + 40) != 52 ||
        picasso_adsp_u16(mdt + 42) != 32 || picasso_adsp_u16(mdt + 44) != picassoAdspHeaders) return -22;
    plan->entry = picasso_adsp_u32(mdt + 24);
    uint32_t low = picassoAdspBytes, high = 0;
    bool entryFound = false;
    for (unsigned i = 0; i < picassoAdspHeaders; ++i) {
        plan->index = i; plan->phase = "file";
        const uint8_t *p = mdt + 52 + i * 32;
        uint32_t type = picasso_adsp_u32(p), offset = picasso_adsp_u32(p + 4), address = picasso_adsp_u32(p + 12);
        uint32_t file = picasso_adsp_u32(p + 16), mem = picasso_adsp_u32(p + 20);
        uint32_t flags = picasso_adsp_u32(p + 24), align = picasso_adsp_u32(p + 28);
        constexpr uint32_t maxFile = 64U * 1024 * 1024;
        if (offset > maxFile || file > maxFile - offset || pieces[i] != file) return -22;
        for (unsigned j = 0; file && j < i; ++j) {
            const uint8_t *prev = mdt + 52 + j * 32;
            uint32_t start = picasso_adsp_u32(prev + 4), bytes = picasso_adsp_u32(prev + 16);
            if (bytes && offset < start + bytes && start < offset + file) return -22;
        }
        if (file && offset + file > plan->imageBytes) plan->imageBytes = offset + file;
        plan->phase = "metadata";
        if (i == 0) {
            if (type || offset || address || file != 52 + 32 * picassoAdspHeaders || mem || flags != 0x07000000) return -22;
            continue;
        }
        if (i == 1) {
            if (type || offset != 4096 || file != size - pieces[0] || mem != 12288 ||
                address != picassoAdspBase + picassoAdspBytes || flags != 0x02200000) return -22;
            continue;
        }
        plan->phase = "memory";
        if (type != 1 || (flags & ~7U) != 0x08000000 ||
            !mem || file > mem || address < picassoAdspBase || address >= picassoAdspBase + picassoAdspBytes ||
            mem > picassoAdspBase + picassoAdspBytes - address || (align && (align & (align - 1)))) return -22;
        uint32_t relative = address - picassoAdspBase;
        plan->phase = "overlap";
        for (unsigned j = 0; j < plan->count; ++j) {
            const auto &s = plan->segments[j];
            if (relative < s.memoryOffset + s.fileBytes + s.zeroBytes && s.memoryOffset < relative + mem) return -22;
        }
        // Stock uses the lowest segment's p_align for relocation, not every
        // segment's physical address (several are intentionally unaligned).
        if (relative < low) { low = relative; plan->alignment = align; }
        if (relative + mem > high) high = relative + mem;
        if (plan->entry >= address && plan->entry - address < mem && (flags & 1)) entryFound = true;
        plan->segments[plan->count++] = {i, offset, relative, file, mem - file, flags};
        plan->fileBytes += file; plan->zeroBytes += mem - file;
    }
    plan->phase = "carveout"; plan->index = ~0U;
    if (low || high != picassoAdspBytes || !entryFound || plan->count != 32 ||
        (plan->alignment > 1 && (picassoAdspBase & (plan->alignment - 1)))) return -22;
    plan->phase = "planned";
    return 0;
}
#endif
