// SPDX-License-Identifier: BSD-2-Clause-Patent
// Observe the qualified wallpaper snapshot boundaries without changing images.
#import <CoreGraphics/CoreGraphics.h>
typedef id (*PicassoWallpaperRead)(id, SEL, id *);
typedef id (*PicassoWallpaperCache)(id, SEL, id);
typedef id (*PicassoWallpaperComposite)(id, SEL, id, id);
static PicassoWallpaperRead picasso_wallpaper_read_original;
static PicassoWallpaperCache picasso_wallpaper_cache_original;
static PicassoWallpaperComposite picasso_wallpaper_composite_original;

static void picasso_wallpaper_note(const char *stage, unsigned ticket, const char *state,
                                  size_t width, size_t height, size_t row, size_t bytes,
                                  unsigned bpp, unsigned alpha,
                                  const struct picasso_pixel_sample *sample)
{
    struct picasso_pixel_sample empty = {0};
    if (!sample) sample = &empty;
    char line[352];
    int length = snprintf(line, sizeof(line),
        "PICASSO_WALLPAPER pid=%d stage=%s ticket=%u state=%s size=%llu/%llu row=%llu bytes=%llu bpp=%u alpha=%u samples=%u rgb_nonzero=%u alpha_nonzero=%u varied=%u first=%08x hash=%08x\n",
        getpid(), stage, ticket, state, (unsigned long long)width,
        (unsigned long long)height, (unsigned long long)row, (unsigned long long)bytes,
        bpp, alpha, sample->count, sample->rgb_nonzero, sample->alpha_nonzero,
        sample->varied, sample->first, sample->hash);
    picasso_pixel_console(line, length, sizeof(line));
}

static void picasso_wallpaper_observe(id image, unsigned kind, unsigned ticket)
{
    static unsigned counts[4];
    static const char *stages[] = {"disk", "effect-input", "effect-output", "cache"};
    if (kind >= 4 || __atomic_fetch_add(&counts[kind], 1, __ATOMIC_RELAXED) >= 8) return;
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
                    picasso_wallpaper_note(stage, ticket, "no-cgimage", 0, 0, 0, 0, 0, 0, NULL);
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
    id result = picasso_wallpaper_read_original(receiver, selector, error);
    picasso_wallpaper_observe(result, 0, picasso_wallpaper_ticket());
    return result;
}

static id picasso_wallpaper_cache(id receiver, SEL selector, id configuration)
{
    id result = picasso_wallpaper_cache_original(receiver, selector, configuration);
    picasso_wallpaper_observe(result, 3, picasso_wallpaper_ticket());
    return result;
}

static id picasso_wallpaper_composite(id receiver, SEL selector, id image, id configuration)
{
    unsigned ticket = picasso_wallpaper_ticket();
    id result = picasso_wallpaper_composite_original(receiver, selector, image, configuration);
    picasso_wallpaper_observe(image, 1, ticket);
    picasso_wallpaper_observe(result, 2, ticket);
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

static void picasso_wallpaper_install(uintptr_t base)
{
    static unsigned installed;
    Class encoder = objc_getClass("PBUIImageEncoder");
    Class provider = objc_getClass("PBUICachedSnapshotEffectProvider");
    Class classes[] = {encoder, provider, provider};
    SEL selectors[] = {sel_registerName("createUIImageWithError:"),
        sel_registerName("imageForConfiguration:"), sel_registerName("compositeImage:effectConfiguration:")};
    uintptr_t originals[] = {base + 0x2e9d0, base + 0x30260, base + 0x304a4};
    IMP replacements[] = {(IMP)picasso_wallpaper_read, (IMP)picasso_wallpaper_cache,
        (IMP)picasso_wallpaper_composite};
    for (unsigned i = 0; i < 3; ++i) {
        if (installed & (1u << i)) continue;
        Method method = class_getInstanceMethod(classes[i], selectors[i]);
        if (!picasso_wallpaper_method(method, originals[i], i == 2 ? 4 : 3, i == 0)) continue;
        if (i == 0) picasso_wallpaper_read_original = (PicassoWallpaperRead)originals[i];
        if (i == 1) picasso_wallpaper_cache_original = (PicassoWallpaperCache)originals[i];
        if (i == 2) picasso_wallpaper_composite_original = (PicassoWallpaperComposite)originals[i];
        class_replaceMethod(classes[i], selectors[i], replacements[i], method_getTypeEncoding(method));
        installed |= 1u << i;
        picasso_wallpaper_note("install", i, "installed", 0, 0, 0, 0, 0, 0, NULL);
    }
}

static void picasso_wallpaper_image_added(const struct mach_header *header, intptr_t slide)
{
    static const unsigned char uuid[16] = {
        0xcb,0x92,0x3b,0xf8,0x25,0x57,0x37,0xcc,0xa0,0x20,0xdf,0x77,0x41,0x41,0xea,0x44
    };
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64) return;
    const struct mach_header_64 *h = (const struct mach_header_64 *)header;
    if (h->sizeofcmds > 65536 || h->ncmds > 128) return;
    const unsigned char *p = (const unsigned char *)(h + 1), *end = p + h->sizeofcmds;
    int matched = 0;
    uintptr_t base = (uintptr_t)header, text_end = 0;
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        if ((size_t)(end - p) < sizeof(struct load_command)) return;
        const struct load_command *cmd = (const struct load_command *)p;
        if (cmd->cmdsize < sizeof(*cmd) || cmd->cmdsize > (size_t)(end - p)) return;
        if (cmd->cmd == LC_UUID && cmd->cmdsize == sizeof(struct uuid_command))
            matched = !memcmp(((const struct uuid_command *)cmd)->uuid, uuid, 16);
        if (cmd->cmd == LC_SEGMENT_64 && cmd->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)cmd;
            uintptr_t start = seg->vmaddr + slide;
            if (seg->vmsize > UINTPTR_MAX - start) return;
            if (!strncmp(seg->segname, "__TEXT", 16) && start == base) text_end = start + seg->vmsize;
        }
        p += cmd->cmdsize;
    }
    if (!matched || p != end || text_end < base || text_end - base < 0x30530) return;
    if (!picasso_astc_hash(base, 0x2e9d0, 776, "92c2cd035e34840468e4f1c1d6d69ec6f649e3f748b76f532a2ae0ff6b2385e2") ||
        !picasso_astc_hash(base, 0x30260, 308, "b2c13ef1327aaf8bcddfcca844f8bba9fb9111b1a25c4d55ec91d413ed796921") ||
        !picasso_astc_hash(base, 0x304a4, 140, "8f429fd2cc17f1b36db216cb5fc6683d2f01eb2df1ff6b6499afbec000b58c70")) return;
    dispatch_async(dispatch_get_main_queue(), ^{ picasso_wallpaper_install(base); });
}
