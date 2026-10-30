#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#ifdef _MSC_VER
#define __builtin_memcpy memcpy
#endif
#include "NativeLegacyRequest.h"
#include "PicassoGop.h"
#include "PicassoQueueExtracted.h"

struct Video {
    uint64_t v_baseAddr = 0xa0000000, v_rowBytes = 4320, v_width = 1080, v_height = 2400;
    uint64_t v_depth = 32, v_rotate = 0, v_offset = 0, v_length = 0, v_display = 0;
    char v_pixelFormat[64] = "BBBBBBBBGGGGGGGGRRRRRRRR";
};

static void video() {
    Video v;
    assert(picasso_gop_video(v)); // Real text-console handoff remains usable.
    v.v_display = 1; assert(picasso_gop_video(v));
    v.v_baseAddr = 0x9c000000; assert(!picasso_gop_video(v));
    v = {}; v.v_baseAddr = 0xa2300000 - v.v_rowBytes * v.v_height;
    assert(picasso_gop_video(v));
    v.v_baseAddr += 4; assert(!picasso_gop_video(v));
    v = {}; v.v_baseAddr++; assert(!picasso_gop_video(v));
    v = {}; v.v_rowBytes = UINT64_MAX; assert(!picasso_gop_video(v));
    v = {}; v.v_width = UINT64_MAX; assert(!picasso_gop_video(v));
    v = {}; v.v_height = 0; assert(!picasso_gop_video(v));
    v = {}; v.v_depth = 24; assert(!picasso_gop_video(v));
    v = {}; v.v_rotate = 1; assert(!picasso_gop_video(v));
    v = {}; v.v_offset = 4; assert(!picasso_gop_video(v));
    v = {}; v.v_length = 4096; assert(!picasso_gop_video(v));
    v = {}; v.v_length = UINT64_MAX; assert(!picasso_gop_video(v));
    v = {}; v.v_pixelFormat[0] = 'R'; assert(!picasso_gop_video(v));
    assert(picasso_gop_format(0x42475241, 0));
    assert(!picasso_gop_format(0x26424741, 0));
    assert(!picasso_gop_format(0x26424741, 1));
    assert(!picasso_gop_format(0x42475241, 1));
}

static void pixels() {
    alignas(8) unsigned char src[128], dst[128], before[128];
    for (unsigned i = 0; i < sizeof(src); ++i) src[i] = static_cast<unsigned char>(i);
    // Exercise unaligned input, padded rows, odd/even width and both store paths.
    for (unsigned width : {1u, 2u, 7u, 8u}) {
        for (unsigned offset : {0u, 1u}) {
            memset(dst, 0xaa, sizeof(dst));
            assert(native_legacy_copy_pixels(dst, sizeof(dst), 48, src + offset, sizeof(src) - offset, 40, width, 2));
            for (unsigned row = 0; row < 2; ++row) {
                assert(!memcmp(dst + row * 48, src + offset + row * 40, width * 4));
                for (unsigned i = width * 4; i < 48; ++i) assert(dst[row * 48 + i] == 0xaa);
            }
        }
    }
    memset(dst, 0xaa, sizeof(dst)); memcpy(before, dst, sizeof(dst));
    assert(!native_legacy_copy_pixels(dst, 47, 48, src, sizeof(src), 40, 8, 2));
    assert(!native_legacy_copy_pixels(dst + 1, 127, 48, src, 128, 40, 8, 2));
    assert(!native_legacy_copy_pixels(dst, 128, 48, src, 79, 40, 8, 2));
    assert(!native_legacy_copy_pixels(dst, 128, UINT64_MAX, src, 128, 40, 8, 2));
    assert(!memcmp(dst, before, sizeof(dst))); // Rejections must not modify scanout.
    assert(native_legacy_fill_pixels(dst, 128, 48, 8, 2, 0xff000000));
    for (unsigned row = 0; row < 2; ++row) {
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t value; memcpy(&value, dst + row * 48 + i * 4, 4); assert(value == 0xff000000);
        }
        for (unsigned i = 32; i < 48; ++i) assert(dst[row * 48 + i] == 0xaa);
    }
    assert(native_legacy_single_layer_state(1, 0, 0));
    assert(!native_legacy_single_layer_state(3, 0, 0));
    assert(!native_legacy_single_layer_state(1, 1, 0));
    assert(native_legacy_blank_state(0x8000000f, 0x8000000f, 0xff000000));
    assert(!native_legacy_blank_state(0x8000000f, 0x8000000f, 0xffffffff));
}

static void setptr(unsigned char *base, unsigned offset, const void *value) { memcpy(base + offset, &value, 8); }
static const void *getptr(unsigned char *base, unsigned offset) { const void *p; memcpy(&p, base + offset, 8); return p; }
static void queue() {
    QueueFixture q = {};
    alignas(8) unsigned char first[3000] = {}, second[3000] = {};
    auto a = reinterpret_cast<const OriginalLegacySwapRequest *>(first);
    auto b = reinterpret_cast<const OriginalLegacySwapRequest *>(second);
    setptr(q.storage, 5480, a); setptr(q.storage, 5488, second + 2960);
    setptr(first, 2960, b); setptr(first, 2968, q.storage + 5480);
    setptr(second, 2968, first + 2960);
    assert(!q.activateOnlyPendingSwap(b));
    assert(q.activateOnlyPendingSwap(a));
    assert(getptr(q.storage, 5472) == a && getptr(q.storage, 5480) == b);
    assert(getptr(q.storage, 5488) == second + 2960 && getptr(second, 2968) == q.storage + 5480);
    assert(!q.activateOnlyPendingSwap(b)); // Cannot replace an unretired current swap.
    setptr(q.storage, 5472, nullptr);
    assert(q.activateOnlyPendingSwap(b));
    assert(getptr(q.storage, 5472) == b && !getptr(q.storage, 5480) && getptr(q.storage, 5488) == q.storage + 5480);
    q = {}; setptr(q.storage, 5480, a); setptr(first, 2960, nullptr); setptr(first, 2968, q.storage + 5480);
    assert(!q.activateOnlyPendingSwap(a)); // Wrong tail must leave ownership intact.
    assert(getptr(q.storage, 5480) == a && !getptr(q.storage, 5472));
    ValidationFixture valid;
    unsigned char objects[9] = {};
    for (unsigned i = 0; i < 8; ++i)
        assert(valid.addValidated(reinterpret_cast<const OriginalLegacySwapRequest *>(&objects[i])));
    assert(!valid.addValidated(reinterpret_cast<const OriginalLegacySwapRequest *>(&objects[8])));
    for (unsigned i = 0; i < 8; ++i)
        assert(valid.isValidated(reinterpret_cast<const OriginalLegacySwapRequest *>(&objects[i])));
    auto removed = reinterpret_cast<const OriginalLegacySwapRequest *>(&objects[3]);
    valid.removeValidated(removed); assert(!valid.isValidated(removed));
    assert(valid.addValidated(reinterpret_cast<const OriginalLegacySwapRequest *>(&objects[8])));
}

int main() {
    video(); pixels(); queue();
    puts("PASS: GOP handoff, linear format, bounded pixel copies, blank frames and original queue ownership");
}
