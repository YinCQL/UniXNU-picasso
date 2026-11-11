// SPDX-License-Identifier: BSD-2-Clause-Patent
// Observe the qualified wallpaper snapshot boundaries without changing images.
#import <CoreGraphics/CoreGraphics.h>
#include <time.h>
#import <IOSurface/IOSurfaceRef.h>
#include "picasso-wallpaper-surface.h"
static uintptr_t picasso_wallpaper_uikit_base;
typedef id (*PicassoWallpaperRead)(id, SEL, id *);
typedef id (*PicassoWallpaperSnapshot)(id, SEL);
typedef id (*PicassoWallpaperComposite)(id, SEL, id, id);
static IMP picasso_wallpaper_read_original;
static IMP picasso_wallpaper_composite_original;
static IMP picasso_wallpaper_baked_original;
static IMP picasso_wallpaper_source_original;
static IMP picasso_wallpaper_home_original;

static void picasso_wallpaper_note(const char *stage, unsigned ticket, const char *state,
                                  size_t width, size_t height, size_t row, size_t bytes,
                                  unsigned bpp, unsigned alpha,
                                  const struct picasso_pixel_sample *sample)
{
    struct picasso_pixel_sample empty = {0};
    if (!sample) sample = &empty;
    struct timespec now = {0};
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned long long milliseconds = (unsigned long long)now.tv_sec * 1000 + (unsigned long long)now.tv_nsec / 1000000;
    char line[400];
    int length = snprintf(line, sizeof(line),
        "PICASSO_WALLPAPER pid=%d stage=%s ticket=%u state=%s uptime_ms=%llu size=%llu/%llu row=%llu bytes=%llu bpp=%u alpha=%u samples=%u rgb_nonzero=%u alpha_nonzero=%u varied=%u first=%08x hash=%08x\n",
        getpid(), stage, ticket, state, milliseconds, (unsigned long long)width,
        (unsigned long long)height, (unsigned long long)row, (unsigned long long)bytes,
        bpp, alpha, sample->count, sample->rgb_nonzero, sample->alpha_nonzero,
        sample->varied, sample->first, sample->hash);
    picasso_pixel_console(line, length, sizeof(line));
}

/* Startup nil reads must not consume the later image budget. Saturating
 * counters bound both logging and retained asynchronous image work. */
static int picasso_wallpaper_take(unsigned kind, int present)
{
    static unsigned counts[6][2];
    if (kind >= 6) return 0;
    unsigned *slot = &counts[kind][present != 0], limit = present ? 16 : 2;
    unsigned value = __atomic_load_n(slot, __ATOMIC_RELAXED);
    while (value < limit) {
        if (__atomic_compare_exchange_n(slot, &value, value + 1, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return 1;
    }
    return 0;
}

/* This getter returns an IOSurface-backed UIImage, whose CGImage is nil.
 * Read only the exact qualified content class and its original getter. */
static IOSurfaceRef picasso_wallpaper_image_surface(id image)
{
    uintptr_t base = __atomic_load_n(&picasso_wallpaper_uikit_base, __ATOMIC_ACQUIRE);
    if (!base) return NULL;
    Class image_class = objc_getClass("UIImage"), content_class = objc_getClass("_UIImageIOSurfaceContent");
    Ivar content_ivar = image_class ? class_getInstanceVariable(image_class, "_content") : NULL;
    if (!content_class || !content_ivar || ivar_getOffset(content_ivar) != 104 ||
        strcmp(ivar_getTypeEncoding(content_ivar), "@\"_UIImageContent\"")) return NULL;
    id content = object_getIvar(image, content_ivar);
    if (!content || object_getClass(content) != content_class) return NULL;
    SEL selector = sel_registerName("IOSurface");
    Method method = class_getInstanceMethod(content_class, selector);
    if (!method || method_getNumberOfArguments(method) != 2 ||
        (uintptr_t)method_getImplementation(method) != base + 0x50af90) return NULL;
    char *type = method_copyReturnType(method);
    int valid = type && !strcmp(type, "^{__IOSurface=}");
    free(type);
    if (!valid) return NULL;
    return ((IOSurfaceRef (*)(id, SEL))method_getImplementation(method))(content, selector);
}

static void picasso_wallpaper_observe_surface(id image, const char *stage, unsigned ticket)
{
    IOSurfaceRef surface = picasso_wallpaper_image_surface(image);
    if (surface) CFRetain(surface);
    struct picasso_wallpaper_surface_result result;
    const char *state = picasso_wallpaper_sample_surface(surface, &result);
    if (surface) CFRelease(surface);
    picasso_wallpaper_note(stage, ticket, state, result.width, result.height,
        result.row, result.bytes, result.element == 4 ? 32 : 0, 0, &result.pixels);
    char line[400];
    int length = snprintf(line, sizeof(line),
        "PICASSO_WALLPAPER_SURFACE pid=%d ticket=%u state=%s qualified=%u id=%u format=%08x element=%llu planes=%llu lock=%d unlock=%d seed=%u/%u w30r=%u/%u zero=%u black=%u padding=%u\n",
        getpid(), ticket, state, __atomic_load_n(&picasso_wallpaper_uikit_base, __ATOMIC_ACQUIRE) != 0, result.identifier, result.format,
        (unsigned long long)result.element, (unsigned long long)result.planes,
        result.lock_result, result.unlock_result, result.seed, result.unlock_seed,
        result.w30r_min, result.w30r_max, result.w30r_zero, result.w30r_black, result.w30r_padding);
    picasso_pixel_console(line, length, sizeof(line));
}

static void picasso_wallpaper_observe(id image, unsigned kind, unsigned ticket)
{
    static const char *stages[] = {"disk", "effect-input", "effect-output", "cache", "source", "home-snapshot"};
    if (!picasso_wallpaper_take(kind, image != nil)) return;
    const char *stage = stages[kind];
    picasso_wallpaper_note(stage, ticket, image ? "returned" : "nil", 0, 0, 0, 0, 0, 0, NULL);
    if (!image) return;
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("picasso.wallpaper.observe", DISPATCH_QUEUE_SERIAL); });
    /* Retain the returned image until this serial read finishes. No decoding
     * or GPU readback is added to the original caller's thread. */
    dispatch_async(queue, ^{
        @autoreleasepool {
            @try {
                if (![image isKindOfClass:[UIImage class]]) {
                    picasso_wallpaper_note(stage, ticket, "other-object", 0, 0, 0, 0, 0, 0, NULL);
                    return;
                }
                CGImageRef cg = [(UIImage *)image CGImage];
                if (!cg) {
                    if (kind == 5) picasso_wallpaper_observe_surface(image, stage, ticket);
                    else picasso_wallpaper_note(stage, ticket, "no-cgimage", 0, 0, 0, 0, 0, 0, NULL);
                    return;
                }
                size_t width = CGImageGetWidth(cg), height = CGImageGetHeight(cg);
                size_t row = CGImageGetBytesPerRow(cg);
                CGColorSpaceRef color_space = CGImageGetColorSpace(cg);
                unsigned bpp = (unsigned)CGImageGetBitsPerPixel(cg);
                unsigned alpha = (unsigned)CGImageGetAlphaInfo(cg);
                unsigned order = CGImageGetBitmapInfo(cg) & kCGBitmapByteOrderMask;
                int last = alpha == kCGImageAlphaPremultipliedLast || alpha == kCGImageAlphaLast ||
                    alpha == kCGImageAlphaNoneSkipLast;
                int first = alpha == kCGImageAlphaPremultipliedFirst || alpha == kCGImageAlphaFirst ||
                    alpha == kCGImageAlphaNoneSkipFirst;
                /* Inspect only existing 8-bit RGB backing; do not render a
                 * replacement image or interpret compressed provider bytes. */
                if (!width || !height || width > 8192 || height > 8192 ||
                    width * height > 16 * 1024 * 1024 ||
                    row > 64 * 1024 * 1024 / height ||
                    bpp != 32 || CGImageGetBitsPerComponent(cg) != 8 || !color_space ||
                    CGColorSpaceGetModel(color_space) != kCGColorSpaceModelRGB ||
                    (!first && !last) || (order != kCGBitmapByteOrderDefault &&
                        order != kCGBitmapByteOrder32Big && order != kCGBitmapByteOrder32Little)) {
                    picasso_wallpaper_note(stage, ticket, "metadata", width, height, row, 0, bpp, alpha, NULL);
                    return;
                }
                CGDataProviderRef provider = CGImageGetDataProvider(cg);
                CFDataRef data = provider ? CGDataProviderCopyData(provider) : NULL;
                size_t bytes = data ? (size_t)CFDataGetLength(data) : 0;
                struct picasso_pixel_sample sample;
                int sampled = picasso_sample_rgb32(data ? CFDataGetBytePtr(data) : NULL,
                    bytes, (uint32_t)width, (uint32_t)height, row, &sample, first != (order == kCGBitmapByteOrder32Little));
                picasso_wallpaper_note(stage, ticket, sampled ? "pixels" : "no-linear-pixels",
                    width, height, row, bytes, bpp, alpha, &sample);
                if (data) CFRelease(data);
            } @catch (id exception) {
                (void)exception;
                picasso_wallpaper_note(stage, ticket, "exception", 0, 0, 0, 0, 0, 0, NULL);
            }
        }
    });
}

static unsigned picasso_wallpaper_ticket(void)
{
    static unsigned ticket;
    return __atomic_fetch_add(&ticket, 1, __ATOMIC_RELAXED) + 1;
}

static id picasso_wallpaper_read(id receiver, SEL selector, id *error)
{
    id result = ((PicassoWallpaperRead)picasso_wallpaper_read_original)(receiver, selector, error);
    picasso_wallpaper_observe(result, 0, picasso_wallpaper_ticket());
    return result;
}

static id picasso_wallpaper_composite(id receiver, SEL selector, id configuration, id image)
{
    unsigned ticket = picasso_wallpaper_ticket();
    id result = ((PicassoWallpaperComposite)picasso_wallpaper_composite_original)(receiver, selector, configuration, image);
    picasso_wallpaper_observe(image, 1, ticket);
    picasso_wallpaper_observe(result, 2, ticket);
    return result;
}

static id picasso_wallpaper_baked(id receiver, SEL selector)
{
    id result = ((PicassoWallpaperSnapshot)picasso_wallpaper_baked_original)(receiver, selector);
    picasso_wallpaper_observe(result, 3, picasso_wallpaper_ticket());
    return result;
}

static id picasso_wallpaper_source(id receiver, SEL selector)
{
    id result = ((PicassoWallpaperSnapshot)picasso_wallpaper_source_original)(receiver, selector);
    picasso_wallpaper_observe(result, 4, picasso_wallpaper_ticket());
    return result;
}

static id picasso_wallpaper_home(id receiver, SEL selector)
{
    id result = ((PicassoWallpaperSnapshot)picasso_wallpaper_home_original)(receiver, selector);
    picasso_wallpaper_observe(result, 5, picasso_wallpaper_ticket());
    return result;
}

static int picasso_wallpaper_method(Method method, uintptr_t original, unsigned count, int error)
{
    if (!method || method_getNumberOfArguments(method) != count ||
        (uintptr_t)method_getImplementation(method) != original) return 0;
    char *type = method_copyReturnType(method);
    int valid = type && !strcmp(type, @encode(id));
    free(type);
    for (unsigned i = 2; valid && i < count; ++i) {
        type = method_copyArgumentType(method, i);
        valid = type && !strcmp(type, error ? "^@" : "@");
        free(type);
    }
    return valid;
}

static void picasso_wallpaper_install_method(Class cls, SEL selector, uintptr_t original,
    IMP replacement, IMP *saved, unsigned count, int error, unsigned ticket)
{
    static unsigned reported;
    if (*saved) return;
    Method method = cls ? class_getInstanceMethod(cls, selector) : NULL;
    const char *state = !cls ? "missing-class" : !method ? "missing-method" :
        (uintptr_t)method_getImplementation(method) != original ? "different-imp" :
        !picasso_wallpaper_method(method, original, count, error) ? "different-abi" : "installed";
    if (!strcmp(state, "installed")) {
        /* Publish the original before making the wrapper reachable. */
        *saved = (IMP)original;
        class_replaceMethod(cls, selector, replacement, method_getTypeEncoding(method));
    }
    if (ticket < 5 && !(reported & (1u << ticket))) {
        reported |= 1u << ticket;
        picasso_wallpaper_note("install", ticket, state, 0, 0, 0, 0, 0, 0, NULL);
    }
}

static void picasso_wallpaper_install(uintptr_t base)
{
    /* The cached provider's image helpers are direct calls in this image,
     * absent from its ObjC method list. Observe the dispatched compositor
     * and the actual cached/source snapshot getters instead. */
    picasso_wallpaper_install_method(objc_getClass("PBUIImageEncoder"),
        sel_registerName("createUIImageWithError:"), base + 0x2e9d0,
        (IMP)picasso_wallpaper_read, &picasso_wallpaper_read_original, 3, 1, 0);
    picasso_wallpaper_install_method(objc_getClass("PBUIBackdropCompositor"),
        sel_registerName("applyEffect:toImage:"), base + 0x2d970,
        (IMP)picasso_wallpaper_composite, &picasso_wallpaper_composite_original, 4, 0, 1);
    picasso_wallpaper_install_method(objc_getClass("PBUIBakedEffectSnapshotSource"),
        sel_registerName("snapshot"), base + 0x2fc5c,
        (IMP)picasso_wallpaper_baked, &picasso_wallpaper_baked_original, 2, 0, 2);
    picasso_wallpaper_install_method(objc_getClass("PBUIURLBackedSnapshotSource"),
        sel_registerName("snapshot"), base + 0x5c6fc,
        (IMP)picasso_wallpaper_source, &picasso_wallpaper_source_original, 2, 0, 3);
}

static void picasso_wallpaper_install_home(uintptr_t base)
{
    /* Animation completion switches the backdrop to this cached snapshot. */
    picasso_wallpaper_install_method(objc_getClass("SBHomeScreenBackdropView"),
        sel_registerName("homeScreenBlurredContentSnapshotImage"), base + 0x45f8fc,
        (IMP)picasso_wallpaper_home, &picasso_wallpaper_home_original, 2, 0, 4);
}

static void picasso_wallpaper_image_added(const struct mach_header *header, intptr_t slide)
{
    static const unsigned char uuid[16] = {
        0xcb,0x92,0x3b,0xf8,0x25,0x57,0x37,0xcc,0xa0,0x20,0xdf,0x77,0x41,0x41,0xea,0x44
    };
    static const unsigned char home_uuid[16] = {
        0x22,0x03,0x4a,0xba,0x9a,0x9a,0x3b,0x94,0x93,0xa5,0xa0,0xde,0x02,0xbe,0xfd,0x2e
    };
    static const unsigned char uikit_uuid[16] = {
        0x12,0x42,0x97,0x8a,0x2c,0x2c,0x37,0x81,0x8d,0x6c,0x97,0x77,0xed,0xce,0x28,0x04
    };
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64) return;
    const struct mach_header_64 *h = (const struct mach_header_64 *)header;
    /* SpringBoard 20H392 has 224 commands. Bound their count by the
     * already capped byte region, then validate every command below. */
    if (h->sizeofcmds > 65536 ||
        h->ncmds > h->sizeofcmds / sizeof(struct load_command)) return;
    const unsigned char *p = (const unsigned char *)(h + 1), *end = p + h->sizeofcmds;
    int matched = 0;
    uintptr_t base = (uintptr_t)header, text_end = 0;
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        if ((size_t)(end - p) < sizeof(struct load_command)) return;
        const struct load_command *cmd = (const struct load_command *)p;
        if (cmd->cmdsize < sizeof(*cmd) || cmd->cmdsize > (size_t)(end - p)) return;
        if (cmd->cmd == LC_UUID && cmd->cmdsize == sizeof(struct uuid_command)) {
            const unsigned char *value = ((const struct uuid_command *)cmd)->uuid;
            matched = !memcmp(value, uuid, 16) ? 1 : !memcmp(value, home_uuid, 16) ? 2 : !memcmp(value, uikit_uuid, 16) ? 3 : 0;
        }
        if (cmd->cmd == LC_SEGMENT_64 && cmd->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)cmd;
            uintptr_t start = seg->vmaddr + slide;
            if (seg->vmsize > UINTPTR_MAX - start) return;
            if (!strncmp(seg->segname, "__TEXT", 16) && start == base) text_end = start + seg->vmsize;
        }
        p += cmd->cmdsize;
    }
    if (!matched || p != end || text_end < base) return;
    if (matched == 3) {
        if (text_end - base < 0x50afa0 ||
            !picasso_astc_hash(base, 0x68e30, 8, "b912c6b83bf24945b90607112bab1ac8efa41fd3ca03a9deec12d14fcb287292") ||
            !picasso_astc_hash(base, 0x50af90, 16, "d7c3981af85c5b56bb670e01b55a77a19d703f65aad40d480a049976ae011403")) return;
        __atomic_store_n(&picasso_wallpaper_uikit_base, base, __ATOMIC_RELEASE);
        return;
    }
    if (matched == 2) {
        if (text_end - base < 0x45f99c ||
            !picasso_astc_hash(base, 0x45f8fc, 160, "69680a3fb99e35365f80cde8d02df4257b80896cd69a10547b11886da4962a5b")) return;
        dispatch_async(dispatch_get_main_queue(), ^{ picasso_wallpaper_install_home(base); });
        return;
    }
    if (text_end - base < 0x5c704) return;
    if (!picasso_astc_hash(base, 0x2e9d0, 776, "92c2cd035e34840468e4f1c1d6d69ec6f649e3f748b76f532a2ae0ff6b2385e2") ||
        !picasso_astc_hash(base, 0x2d970, 248, "4b2cfe4a60b41469e068491d1a2a9e0a71ecc3a3ab30a49e07d8764fe23a477f") ||
        !picasso_astc_hash(base, 0x2fc5c, 8, "bd237e5d99f6462770946dd64c9d5273577f92aeb78154ba485fef49e0d73851") ||
        !picasso_astc_hash(base, 0x5c6fc, 8, "095bb56e13b6dfde16ade0b592279392884c282a7c7810cb58472b839de552de")) return;
    dispatch_async(dispatch_get_main_queue(), ^{ picasso_wallpaper_install(base); });
}
