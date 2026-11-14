// SPDX-License-Identifier: BSD-2-Clause-Patent
// The 20H392 factory reports its 320-point template before elaborating the
// 360-point phone keyplanes. Keep the container and keyplanes in agreement.
@protocol PicassoKeyboardTraits
- (NSInteger)idiom;
- (BOOL)isFloating;
- (BOOL)isSplit;
- (CGFloat)keyboardWidth;
@end

typedef CGSize (*PicassoKeyboardSize)(id, SEL, id, id, NSInteger);
static PicassoKeyboardSize picasso_keyboard_original;
static Class picasso_keyboard_traits;

static void picasso_keyboard_note(const char *reason)
{
    static unsigned count;
    if (__atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) >= 8) return;
    char line[128];
    int length = snprintf(line, sizeof(line), "PICASSO_KEYBOARD_SIZE pid=%d reason=%s\n", getpid(), reason);
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        if (length > 0 && (size_t)length < sizeof(line)) (void)write(fd, line, (size_t)length);
        close(fd);
    }
}

static BOOL picasso_keyboard_signature(Method method, unsigned count, const char *result,
                                       const char *last)
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

static CGSize picasso_keyboard_size(id receiver, SEL selector, id mode, id traits, NSInteger type)
{
    CGSize result = picasso_keyboard_original(receiver, selector, mode, traits, type);
    // Preserve floating/split, tablet, landscape, and already-correct results.
    // The stock size notification otherwise recenters a narrow container and
    // reenters setFrame during a keyplane change, resetting it to letters.
    if (result.width != 320 || !(result.height > 0 && result.height < 400) ||
        ![traits isKindOfClass:picasso_keyboard_traits]) return result;
    UIScreen *screen = [UIScreen mainScreen];
    CGSize pixels = screen.nativeBounds.size, points = screen.bounds.size;
    if (pixels.width != 1080 || pixels.height != 2400 || screen.nativeScale != 3 ||
        screen.scale != 3 || points.width != 360 || points.height != 800) return result;
    id<PicassoKeyboardTraits> keyboardTraits = traits;
    if ([keyboardTraits idiom] != 0 || [keyboardTraits isFloating] ||
        [keyboardTraits isSplit] || [keyboardTraits keyboardWidth] != 360) return result;
    result.width = 360;
    picasso_keyboard_note("applied_320_to_360");
    return result;
}

static void picasso_keyboard_install(uintptr_t base)
{
    static BOOL installed;
    if (installed) return;
    Class cls = objc_getClass("UIKeyboardLayoutStar");
    Class traits = objc_getClass("UIKBScreenTraits");
    SEL selector = sel_registerName("keyboardSizeForInputMode:screenTraits:keyboardType:");
    Method method = class_getClassMethod(cls, selector);
    if (!picasso_keyboard_signature(method, 5, @encode(CGSize), @encode(NSInteger)) ||
        (uintptr_t)method_getImplementation(method) != base + 0x399c0c ||
        !picasso_keyboard_signature(class_getInstanceMethod(traits, sel_registerName("idiom")), 2, @encode(NSInteger), NULL) ||
        !picasso_keyboard_signature(class_getInstanceMethod(traits, sel_registerName("isFloating")), 2, @encode(BOOL), NULL) ||
        !picasso_keyboard_signature(class_getInstanceMethod(traits, sel_registerName("isSplit")), 2, @encode(BOOL), NULL) ||
        !picasso_keyboard_signature(class_getInstanceMethod(traits, sel_registerName("keyboardWidth")), 2, @encode(CGFloat), NULL)) {
        picasso_keyboard_note("unavailable"); return;
    }
    picasso_keyboard_traits = traits;
    picasso_keyboard_original = (PicassoKeyboardSize)method_getImplementation(method);
    class_replaceMethod(object_getClass(cls), selector, (IMP)picasso_keyboard_size, method_getTypeEncoding(method));
    installed = YES;
    picasso_keyboard_note("installed");
}

static void picasso_keyboard_image_added(const struct mach_header *header, intptr_t slide)
{
    static const unsigned char uuid[16] = {
        0x12,0x42,0x97,0x8a,0x2c,0x2c,0x37,0x81,0x8d,0x6c,0x97,0x77,0xed,0xce,0x28,0x04
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
    if (!matched || p != end || text_end < base || text_end - base < 0x399e1c) return;
    if (!picasso_astc_hash(base, 0x399c0c, 528, "78ca24436047a33211b84a625dc936dc70c2c027fd706a9767b53aa3bd645c12")) {
        picasso_keyboard_note("unavailable"); return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{ picasso_keyboard_install(base); });
}

__attribute__((constructor))
static void picasso_keyboard_start(void)
{
    _dyld_register_func_for_add_image(picasso_keyboard_image_added);
}
