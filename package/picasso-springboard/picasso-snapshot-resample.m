// SPDX-License-Identifier: BSD-2-Clause-Patent
// Compatibility path for the qualified scene snapshot downscale on this board.
#import <IOSurface/IOSurfaceRef.h>
#include "picasso-snapshot-resample-core.h"
#include "picasso-snapshot-surface.h"

typedef id (*PicassoSnapshotSurfaceMethod)(id, SEL, long);
static PicassoSnapshotSurfaceMethod picasso_snapshot_original;
static Ivar picasso_snapshot_source, picasso_snapshot_factor;
static Ivar picasso_snapshot_context, picasso_snapshot_image, picasso_snapshot_processed;

static void picasso_snapshot_note(const char *reason)
{
    static unsigned count;
    if (__atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) >= 8) return;
    char line[128];
    int length = snprintf(line, sizeof(line), "PICASSO_SNAPSHOT_RESAMPLE pid=%d reason=%s\n", getpid(), reason);
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        if (length > 0 && (size_t)length < sizeof(line)) (void)write(fd, line, (size_t)length);
        close(fd);
    }
}

static BOOL picasso_snapshot_signature(Method method, unsigned count, const char *result, const char *last)
{
    if (!method || method_getNumberOfArguments(method) != count) return NO;
    char *type = method_copyReturnType(method);
    BOOL valid = type && !strcmp(type, result);
    free(type);
    for (unsigned i = 0; valid && i < count; ++i) {
        type = method_copyArgumentType(method, i);
        const char *expected = i == 1 ? @encode(SEL) : @encode(id);
        if (last && i == count - 1) expected = last;
        valid = type && !strcmp(type, expected);
        free(type);
    }
    return valid;
}

static id picasso_snapshot_surface(id self, SEL selector, long format)
{
    // Match the stock provider's recursive monitor and preserve its ownership,
    // processed-surface cache, image cache, and fallback behavior.
    @synchronized(self) {
        double factor = 0;
        memcpy(&factor, (const char *)(__bridge const void *)self +
               ivar_getOffset(picasso_snapshot_factor), sizeof(factor));
        BOOL eligible = format == 0 && factor == 0.7 &&
            !object_getIvar(self, picasso_snapshot_processed) &&
            !object_getIvar(self, picasso_snapshot_image);
        id source = nil;
        BOOL opaque = NO;
        if (eligible) {
            id context = object_getIvar(self, picasso_snapshot_context);
            SEL opaqueSelector = sel_registerName("isOpaque");
            Method opaqueMethod = class_getInstanceMethod(object_getClass(context), opaqueSelector);
            if (context && picasso_snapshot_signature(opaqueMethod, 2, @encode(BOOL), NULL))
                opaque = ((BOOL (*)(id, SEL))objc_msgSend)(context, opaqueSelector);
            SEL protectedSelector = sel_registerName("hasProtectedContent");
            eligible = !((BOOL (*)(id, SEL))objc_msgSend)(self, protectedSelector);
            id snapshot = object_getIvar(self, picasso_snapshot_source);
            SEL surfaceSelector = sel_registerName("IOSurface");
            if (eligible && [snapshot respondsToSelector:surfaceSelector])
                source = ((id (*)(id, SEL))objc_msgSend)(snapshot, surfaceSelector);
            eligible = source && picasso_snapshot_bgra((__bridge IOSurfaceRef)source, 1080, 2400);
        }
        id result = picasso_snapshot_original(self, selector, format);
        if (eligible && result && object_getIvar(self, picasso_snapshot_processed) == result &&
            picasso_snapshot_resample_surface((__bridge IOSurfaceRef)source,
                                              (__bridge IOSurfaceRef)result, opaque))
            picasso_snapshot_note("applied");
        return result;
    }
}

static Ivar picasso_snapshot_ivar(Class cls, const char *name, ptrdiff_t offset, const char *type)
{
    Ivar ivar = class_getInstanceVariable(cls, name);
    const char *encoding = ivar ? ivar_getTypeEncoding(ivar) : NULL;
    if (!ivar || ivar_getOffset(ivar) != offset || !encoding) return NULL;
    return type[0] == '@' ? (encoding[0] == '@' ? ivar : NULL) :
        (!strcmp(encoding, type) ? ivar : NULL);
}

static void picasso_snapshot_install(uintptr_t base)
{
    static BOOL installed;
    if (installed) return;
    Class cls = objc_getClass("SBSceneSnapshotDataProvider");
    SEL selector = sel_registerName("IOSurfaceForFormat:");
    Method method = class_getInstanceMethod(cls, selector);
    Method protectedMethod = class_getInstanceMethod(cls, sel_registerName("hasProtectedContent"));
    if (!picasso_snapshot_signature(method, 3, @encode(id), @encode(long)) ||
        (uintptr_t)method_getImplementation(method) != base + 0x3b4690 ||
        !picasso_snapshot_signature(protectedMethod, 2, @encode(BOOL), NULL) ||
        (uintptr_t)method_getImplementation(protectedMethod) != base + 0x51f38 ||
        class_getInstanceSize(cls) < 57) {
        picasso_snapshot_note("unavailable"); return;
    }
    picasso_snapshot_source = picasso_snapshot_ivar(cls, "_snapshot", 8, "@");
    picasso_snapshot_context = picasso_snapshot_ivar(cls, "_context", 16, "@");
    picasso_snapshot_factor = picasso_snapshot_ivar(cls, "_scaleFactor", 24, @encode(double));
    picasso_snapshot_image = picasso_snapshot_ivar(cls, "_cachedImage", 32, "@");
    picasso_snapshot_processed = picasso_snapshot_ivar(cls, "_processedSurface", 48, "@");
    if (!picasso_snapshot_source || !picasso_snapshot_context || !picasso_snapshot_factor ||
        !picasso_snapshot_image || !picasso_snapshot_processed) {
        picasso_snapshot_note("unavailable"); return;
    }
    picasso_snapshot_original = (PicassoSnapshotSurfaceMethod)method_getImplementation(method);
    class_replaceMethod(cls, selector, (IMP)picasso_snapshot_surface, method_getTypeEncoding(method));
    installed = YES;
    picasso_snapshot_note("installed");
}

static void picasso_snapshot_image_added(const struct mach_header *header, intptr_t slide)
{
    static const unsigned char uuid[16] = {
        0x22,0x03,0x4a,0xba,0x9a,0x9a,0x3b,0x94,0x93,0xa5,0xa0,0xde,0x02,0xbe,0xfd,0x2e
    };
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64) return;
    const struct mach_header_64 *h = (const struct mach_header_64 *)header;
    if (h->sizeofcmds > 65536 || h->ncmds > h->sizeofcmds / sizeof(struct load_command)) return;
    const unsigned char *p = (const unsigned char *)(h + 1), *end = p + h->sizeofcmds;
    BOOL matched = NO;
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
    if (!matched || p != end || text_end < base || text_end - base < 0x3b474c) return;
    if (!picasso_astc_hash(base, 0x3b4690, 188, "bfd8ffc41a6a7588f4f71c21b982df59bc0e3c2a0ad33adc5f83b5f731315a84") ||
        !picasso_astc_hash(base, 0x51f38, 60, "d2c46301efdb135314ee032add1aed010416222f94eb6e702ce0304de43f67d8")) {
        picasso_snapshot_note("unavailable"); return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{ picasso_snapshot_install(base); });
}

__attribute__((constructor))
static void picasso_snapshot_start(void)
{
    if ([[NSProcessInfo processInfo].processName isEqualToString:@"SpringBoard"])
        _dyld_register_func_for_add_image(picasso_snapshot_image_added);
}
