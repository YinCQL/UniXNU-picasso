#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <initializer_list>
#ifdef _MSC_VER
#define __builtin_memcpy memcpy
#endif
#include "NativeLegacyRequest.h"
#include "PicassoGop.h"
static bool allocationFails;
static unsigned allocations,frees;
static void *IOMalloc(uint64_t bytes) {++allocations;return allocationFails?nullptr:malloc(static_cast<size_t>(bytes));}
static void IOFree(void *pointer,uint64_t) {++frees;free(pointer);}
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
static void differentialCopies() {
    alignas(8) unsigned char src[320],dst[352],alternate[352];
    for(unsigned i=0;i<sizeof(src);++i)src[i]=static_cast<unsigned char>(i);
    memset(dst,0xaa,sizeof(dst));memset(alternate,0xbb,sizeof(alternate));
    CopyFixture copy;
    assert(copy.copySurface(dst,sizeof(dst),176,src,sizeof(src),160,34,2)==272);
    assert(copy.copySurface(dst,sizeof(dst),176,src,sizeof(src),160,34,2)==0);
    src[0]^=1;src[160+135]^=1;
    assert(copy.copySurface(dst,sizeof(dst),176,src,sizeof(src),160,34,2)==72);
    for(unsigned row=0;row<2;++row) {
        assert(!memcmp(dst+row*176,src+row*160,136));
        for(unsigned i=136;i<176;++i)assert(dst[row*176+i]==0xaa);
    }
    // A cleared scanout cannot use the previous shadow, even if the next surface is identical.
    assert(native_legacy_fill_pixels(dst,sizeof(dst),176,34,2,0xff000000));
    copy.copyShadowValid=false;
    assert(copy.copySurface(dst,sizeof(dst),176,src,sizeof(src),160,34,2)==272);
    // Changing mapping or dimensions forces a full initialization of the new shadow.
    assert(copy.copySurface(alternate,sizeof(alternate),176,src,sizeof(src),160,34,2)==272);
    assert(!memcmp(alternate,src,136));
    assert(copy.copySurface(alternate,sizeof(alternate),176,src,sizeof(src),160,32,2)==256);
    assert(copy.copySurface(alternate,1,176,src,sizeof(src),160,32,2)==UINT64_MAX);
    copy.resetCopyShadow();copy.resetCopyShadow();
    assert(allocations==frees);
    // Allocation failure is tried once and preserves correct full-copy behavior.
    allocationFails=true;unsigned tried=allocations;
    assert(copy.copySurface(dst,sizeof(dst),176,src,sizeof(src),160,34,2)==272);
    src[7]^=1;
    assert(copy.copySurface(dst,sizeof(dst),176,src,sizeof(src),160,34,2)==272);
    assert(allocations==tried+1 && !memcmp(dst,src,136));
    copy.resetCopyShadow();allocationFails=false;
    // Odd widths retain the aligned 32-bit fallback and never allocate a shadow.
    tried=allocations;
    assert(copy.copySurface(dst,sizeof(dst),176,src,sizeof(src),160,33,2)==264);
    assert(allocations==tried);copy.resetCopyShadow();
    // Unaligned normal-memory sources still use the differential path.
    assert(copy.copySurface(dst,sizeof(dst),176,src+1,sizeof(src)-1,144,32,2)==256);
    assert(copy.copySurface(dst,sizeof(dst),176,src+1,sizeof(src)-1,144,32,2)==0);
    copy.resetCopyShadow();
    // Device alignment and the 16 MiB cap keep the bounded full-copy fallback.
    tried=allocations;
    assert(copy.copySurface(dst+4,sizeof(dst)-4,168,src,sizeof(src),160,32,2)==256);
    assert(allocations==tried);copy.resetCopyShadow();
    constexpr uint64_t largeBytes=2048ULL*2049*4;
    void *largeSource=calloc(1,static_cast<size_t>(largeBytes));
    void *largeDestination=malloc(static_cast<size_t>(largeBytes));
    assert(largeSource && largeDestination);
    assert(copy.copySurface(largeDestination,largeBytes,8192,largeSource,largeBytes,8192,2048,2049)==largeBytes);
    assert(allocations==tried && !copy.copyShadow);
    copy.resetCopyShadow();free(largeSource);free(largeDestination);
    puts("PASS: differential copy bytes, unchanged frames, tails/padding, blank invalidation, mapping/geometry reset, allocation failure and ownership");
}
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

#include "NativeLegacyBootLogo.h"
static void bootLogo() {
    uint64_t logo[5]={29457,430,1000,650,1300}, ui[5]={900000,0,0,1079,2399};
    NativeLegacyBootLogo p;
    p.observe(1000,1080,2400,logo);assert(!p.holdBlank(2000));
    p={};p.enabled=true;
    assert(!p.holdBlank(999));
    p.observe(1000,1080,2400,logo);assert(p.holdBlank(2000));
    p.observe(2000,1080,2400,logo);assert(p.deadline==301000);
    logo[0]=29441;p.observe(3000,1080,2400,logo);assert(p.holdBlank(3001));
    logo[0]=29457;
    assert(p.holdBlank(300999) && !p.holdBlank(301000));
    p.observe(301001,1080,2400,logo);assert(!p.holdBlank(301002));
    p={};p.enabled=true;p.observe(1000,1080,2400,logo);
    p.observe(2000,1080,2400,ui);assert(!p.holdBlank(2001) && p.ended);
    p.observe(3000,1080,2400,logo);assert(!p.holdBlank(3001));
    p={};p.enabled=true;p.observe(1000,1080,2400,ui);assert(!p.holdBlank(1001));
    p={};p.enabled=true;p.observe(1000,1125,2436,logo);assert(!p.holdBlank(1001));
    logo[1]=0;p={};p.enabled=true;p.observe(1000,1080,2400,logo);assert(!p.holdBlank(1001));
    logo[1]=430;logo[0]=29456;p={};p.enabled=true;p.observe(1000,1080,2400,logo);assert(!p.holdBlank(1001));
    puts("PASS: initial-logo qualification, bounded blank hold, UI release and no rearming");
}

int main() {
    video(); pixels(); queue(); bootLogo(); differentialCopies();
    puts("PASS: GOP handoff, linear format, bounded pixel copies, blank frames and original queue ownership");
}
