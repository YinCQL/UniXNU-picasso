// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included by the runtime UI entry point after the existing board helpers.
typedef id (*PicassoOfflineStatus)(id, SEL);
static PicassoOfflineStatus picasso_original_offline_status;
static uintptr_t picasso_offline_query_return;

static void picasso_input_note(const char *reason)
{
    static unsigned count;
    if (__atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) >= 16) return;
    char marker[96], line[160];
    snprintf(marker, sizeof(marker), "kern.picasso_input_%s", reason);
    picasso_ack(marker);
    int length = snprintf(line, sizeof(line), "PICASSO_INPUT_CACHE pid=%d reason=%s\n", getpid(), reason);
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        if (length > 0 && (size_t)length < sizeof(line)) (void)write(fd, line, (size_t)length);
        close(fd);
    }
}

__attribute__((noinline))
static id picasso_offline_status(id receiver, SEL selector)
{
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    id result = picasso_original_offline_status(receiver, selector);
    // The qualified query sends keysOfEntriesPassingTest: to this result.
    // nil produces nil, leaving its language cache uninitialized, but still
    // queues an availability notification. UIKit then queries it again.
    // An empty dictionary produces an empty set and completes initialization.
    // Keep every non-nil result and every other caller's nil semantics intact.
    if (!result && caller == picasso_offline_query_return) {
        picasso_input_note("empty_cached");
        return @{};
    }
    return result;
}

static void picasso_input_install(uintptr_t base)
{
    static BOOL installed;
    if (installed) return;
    Class cls = objc_getClass("AFPreferences");
    SEL selector = sel_registerName("offlineDictationStatus");
    Method method = class_getInstanceMethod(cls, selector);
    if (!method || method_getNumberOfArguments(method) != 2 ||
        (uintptr_t)method_getImplementation(method) != base + 0x2910) {
        picasso_input_note("unavailable"); return;
    }
    char *type = method_copyReturnType(method);
    BOOL compatible = type && !strcmp(type, @encode(id));
    free(type);
    if (!compatible) { picasso_input_note("unavailable"); return; }
    picasso_original_offline_status = (PicassoOfflineStatus)method_getImplementation(method);
    picasso_offline_query_return = base + 0x37e8;
    class_replaceMethod(cls, selector, (IMP)picasso_offline_status, method_getTypeEncoding(method));
    installed = YES;
    picasso_input_note("installed");
}

static void picasso_input_image_added(const struct mach_header *header, intptr_t slide)
{
    static const unsigned char uuid[16] = {
        0xe4,0x39,0x10,0x18,0x74,0x76,0x3c,0x28,0x9e,0x99,0x6a,0xe1,0xb9,0x07,0xa8,0x74
    };
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64) return;
    const struct mach_header_64 *h = (const struct mach_header_64 *)header;
    if (h->sizeofcmds > 65536 || h->ncmds > 128) return;
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
    if (!matched || p != end || text_end < base || text_end - base < 0x803a8) return;
    if (!picasso_astc_hash(base, 0x2910, 104, "b213fee9a81845187d32506a9e982bc2a47f4e81370e504bfe31dda8e56136d3") ||
        !picasso_astc_hash(base, 0x37a4, 228, "c14978576edaf7c0f6279720358caf591957f5776e19b0c52d49d2ee8847f809") ||
        !picasso_astc_hash(base, 0x800ec, 8, "fb83cf801355dc65e41531f7e1c7202ccf1a097f5821157eeda51481dd3178c8") ||
        !picasso_astc_hash(base, 0x803a4, 4, "bc0adaff9a47d35066fd5097b6d91b84c78f54c8cfa2cf952d0f4b784bd9bd33")) {
        picasso_input_note("unavailable"); return;
    }
    // Leave the dyld callback before asking the Objective-C runtime for classes.
    dispatch_async(dispatch_get_main_queue(), ^{ picasso_input_install(base); });
}
