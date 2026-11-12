// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included only in the independently signed runtime UI library.
#import <objc/message.h>

typedef BOOL (*PicassoPowerConsume)(id, SEL);
static PicassoPowerConsume picasso_original_power_consume;
static uintptr_t picasso_power_caller;

static void picasso_power_note(const char *reason)
{
    static unsigned count;
    if (__atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) >= 12) return;
    char marker[96], line[160];
    snprintf(marker, sizeof(marker), "kern.picasso_power_%s", reason);
    picasso_ack(marker);
    int length = snprintf(line, sizeof(line), "PICASSO_POWER_BUTTON pid=%d reason=%s\n", getpid(), reason);
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        if (length > 0 && (size_t)length < sizeof(line)) (void)write(fd, line, (size_t)length);
        close(fd);
    }
}

__attribute__((noinline))
static BOOL picasso_power_consume(id receiver, SEL selector)
{
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    // This call occurs after SpringBoard's interception and restore/reset checks.
    // Other Siri gestures, including the Home button, keep their own behavior.
    if (caller == picasso_power_caller && [NSThread isMainThread]) {
        Class workspaceClass = objc_getClass("SBWorkspace");
        SEL get = sel_registerName("mainWorkspace");
        SEL present = sel_registerName("presentPowerDownTransientOverlay");
        if (workspaceClass && class_getClassMethod(workspaceClass, get)) {
            id workspace = ((id (*)(id, SEL))objc_msgSend)(workspaceClass, get);
            if (workspace && [workspace respondsToSelector:present]) {
                ((void (*)(id, SEL))objc_msgSend)(workspace, present);
                picasso_power_note("requested");
                return YES;
            }
        }
        picasso_power_note("unavailable");
    }
    return picasso_original_power_consume(receiver, selector);
}

static BOOL picasso_power_signature(Method method, const char *expected)
{
    if (!method || method_getNumberOfArguments(method) != 2) return NO;
    char *type = method_copyReturnType(method);
    BOOL compatible = type && !strcmp(type, expected);
    free(type);
    return compatible;
}

static void picasso_power_install(uintptr_t base)
{
    static BOOL installed;
    if (installed || ![[NSProcessInfo processInfo].processName isEqualToString:@"SpringBoard"]) return;
    Class cls = objc_getClass("SBSiriHardwareButtonInteraction");
    SEL selector = sel_registerName("consumeLongPress");
    Method method = class_getInstanceMethod(cls, selector);
    Method get = class_getClassMethod(objc_getClass("SBWorkspace"), sel_registerName("mainWorkspace"));
    Method present = class_getInstanceMethod(objc_getClass("SBMainWorkspace"),
                                             sel_registerName("presentPowerDownTransientOverlay"));
    if (!picasso_power_signature(method, @encode(BOOL)) ||
        (uintptr_t)method_getImplementation(method) != base + 0x3a3a48 ||
        !picasso_power_signature(get, @encode(id)) || (uintptr_t)method_getImplementation(get) != base + 0x8ef4 ||
        !picasso_power_signature(present, @encode(void)) || (uintptr_t)method_getImplementation(present) != base + 0xdb294) {
        picasso_power_note("unavailable"); return;
    }
    picasso_original_power_consume = (PicassoPowerConsume)method_getImplementation(method);
    picasso_power_caller = base + 0x1193f0;
    class_replaceMethod(cls, selector, (IMP)picasso_power_consume, method_getTypeEncoding(method));
    installed = YES;
    picasso_power_note("installed");
}

static void picasso_power_image_added(const struct mach_header *header, intptr_t slide)
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
    if (!matched || p != end || text_end < base || text_end - base < 0x569b1c) return;
    if (!picasso_astc_hash(base, 0x119140, 944, "2e2c71e72d312911a592021a6d74976508fc092ed538396921656ffd85e8260e") ||
        !picasso_astc_hash(base, 0x3a3a48, 452, "a3f8e86b3cf74847d68526870ea88625d815176ed4ebedd81a379bcb11e1a18f") ||
        !picasso_astc_hash(base, 0xdb294, 252, "08ab48a782c70e97cd6757807b57df3114c959686d745d9d27cd4e55181d4366") ||
        !picasso_astc_hash(base, 0x8ef4, 16, "e91c96bb9253e7c99c084eb47ee9e8baddaa996ffd71dae9f4daf0b8dfce1123") ||
        !picasso_astc_hash(base, 0x5699b0, 364, "aaf6de844bd2e45c43d820ecdea7bf6d9d2327a2025ff461b3d16f9dbb4baf1f")) {
        picasso_power_note("unavailable"); return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{ picasso_power_install(base); });
}
