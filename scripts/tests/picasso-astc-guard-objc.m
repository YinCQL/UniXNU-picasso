// SPDX-License-Identifier: BSD-2-Clause-Patent
// macOS host models for the production Objective-C wrapper; no Apple texture
// bytes or real GPU qualification. The iPhoneOS dylib is built separately.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
static unsigned notices;
static void picasso_ack(const char *name) { assert(strstr(name, "kern.picasso_astc_")); ++notices; }
static id<MTLDevice> test_device(void);
#define MTLCreateSystemDefaultDevice test_device
#include "picasso-astc-guard.m"
#undef MTLCreateSystemDefaultDevice

static unsigned mode, calls, decoded;
static NSUInteger layout_bytes;
@interface PicassoTestLayout : NSObject <PicassoASTCLayout>
@end
@implementation PicassoTestLayout
- (NSUInteger)size { return layout_bytes; }
- (BOOL)respondsToSelector:(SEL)selector
{
    if ([NSStringFromSelector(selector) hasPrefix:@"copyFromTextureMemory:"]) return mode != 4;
    return [super respondsToSelector:selector];
}
@end
@interface PicassoTestDevice : NSObject <PicassoASTCDevice>
@end
@implementation PicassoTestDevice
- (id<PicassoASTCLayout>)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)d
                                 isHeapOrBufferBacked:(BOOL)backed
{
    ++calls;
    assert(d.width == 1080 && d.height == 2400 && d.pixelFormat == 204);
    assert(d.mipmapLevelCount == 1 && !backed);
    return mode == 3 ? nil : [PicassoTestLayout new];
}
@end
static id<MTLDevice> test_device(void)
{
    if (mode == 1) return nil;
    if (mode == 2) return (id<MTLDevice>)[NSObject new];
    return (id<MTLDevice>)[PicassoTestDevice new];
}
static int test_decode(void *plugin, unsigned char *pixels, size_t bytes)
{
    assert(plugin && bytes == 32); ++decoded; pixels[0] = 0x42; return -17;
}
static void put_pointer(void *p, unsigned offset, uintptr_t value)
{ memcpy((unsigned char *)p + offset, &value, sizeof(value)); }
static void put_number(void *p, unsigned offset, uint32_t value)
{ memcpy((unsigned char *)p + offset, &value, sizeof(value)); }

int main(void)
{
    @autoreleasepool {
        unsigned char plugin[0x250] = {0}, texture[0xe0] = {0}, pixels[32], original[32];
        picasso_astc_texture_vtable = 0x12345678;
        picasso_astc_decode = picasso_atx_decode = test_decode;
        put_pointer(plugin, 0x1c0, (uintptr_t)texture);
        put_pointer(texture, 0, picasso_astc_texture_vtable);
        put_number(texture, 0x1c, 4); put_number(texture, 0x20, 4);
        put_number(texture, 0x64, 1080); put_number(texture, 0x68, 2400); texture[0x29] = 1;
        memset(original, 0xab, sizeof(original));
        for (mode = 1; mode <= 6; ++mode) {
            layout_bytes = mode == 5 ? 2591999 : mode == 6 ? (UINT64_C(1) << 32) : 0;
            memcpy(pixels, original, sizeof(pixels));
            assert(picasso_astc_guard(plugin, pixels, sizeof(pixels)) == -50);
            assert(!memcmp(pixels, original, sizeof(pixels)) && decoded == 0);
            put_pointer(plugin, 0x228, 1);
            assert(picasso_atx_guard(plugin, pixels, sizeof(pixels)) == -4);
            assert(!memcmp(pixels, original, sizeof(pixels)) && decoded == 0);
        }
        mode = 0; layout_bytes = 0;
        assert(picasso_astc_guard(plugin, pixels, sizeof(pixels)) == -50);
        layout_bytes = 2592000;
        assert(picasso_astc_guard(plugin, pixels, sizeof(pixels)) == -17);
        assert(decoded == 1 && pixels[0] == 0x42);
        /* Missing device was not cached. Valid future calls reach the original
         * and preserve its error, rather than reporting synthetic success. */
        assert(picasso_atx_guard(plugin, pixels, sizeof(pixels)) == -17 && decoded == 2);
        unsigned before = calls;
        texture[0x29] = 0; mode = 1;
        assert(picasso_astc_guard(plugin, pixels, sizeof(pixels)) == -17 && decoded == 3);
        assert(calls == before);
        texture[0x29] = 1; put_pointer(plugin, 0x230, 1);
        assert(picasso_atx_guard(plugin, pixels, sizeof(pixels)) == -17 && decoded == 4);
        assert(calls == before && notices > 0);
        /* A non-matching image must not be installed or touched. */
        struct mach_header_64 wrong = {.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64};
        picasso_astc_image_added((struct mach_header *)&wrong, 0);
        picasso_install_astc_guard();
        puts("PASS: Objective-C preflight rejects nil/missing/undersized layouts without touching pixels; pass-through preserves errors");
    }
    return 0;
}
