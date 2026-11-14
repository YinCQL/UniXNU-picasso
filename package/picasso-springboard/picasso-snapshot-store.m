// SPDX-License-Identifier: BSD-2-Clause-Patent
// Recover scene PNG writes only on the board's explicitly unprotected Data.
#include <dlfcn.h>
#include <limits.h>
#include "picasso-snapshot-store-core.h"

typedef BOOL (*PicassoSnapshotSave)(id, SEL, id, id);
static PicassoSnapshotSave picasso_snapshot_save_original;

static void picasso_snapshot_store_note(const char *reason)
{
    static unsigned count;
    if (__atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) >= 8) return;
    int saved = errno;
    char line[128];
    int n = snprintf(line, sizeof(line), "PICASSO_SNAPSHOT_STORE pid=%d reason=%s\n", getpid(), reason);
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        if (n > 0 && (size_t)n < sizeof(line)) (void)write(fd, line, (size_t)n);
        close(fd);
    }
    errno = saved;
}

static BOOL picasso_snapshot_store_data_mode(void)
{
    typedef mach_port_t (*EntryFromPath)(mach_port_t, const char *);
    typedef CFTypeRef (*EntryProperty)(mach_port_t, CFStringRef, CFAllocatorRef, uint32_t);
    typedef kern_return_t (*ReleaseObject)(mach_port_t);
    static EntryFromPath from;
    static EntryProperty property;
    static ReleaseObject release;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        void *library = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY | RTLD_LOCAL);
        if (library) {
            from = (EntryFromPath)dlsym(library, "IORegistryEntryFromPath");
            property = (EntryProperty)dlsym(library, "IORegistryEntryCreateCFProperty");
            release = (ReleaseObject)dlsym(library, "IOObjectRelease");
        }
    });
    if (!from || !property || !release) return NO;
    mach_port_t chosen = from(MACH_PORT_NULL, "IODeviceTree:/chosen");
    if (!chosen) return NO;
    CFTypeRef data = property(chosen, CFSTR("picasso-unprotected-data"), kCFAllocatorDefault, 0);
    CFTypeRef ephemeral = property(chosen, CFSTR("ephemeral-storage"), kCFAllocatorDefault, 0);
    static const unsigned char enabled[4] = {1, 0, 0, 0};
    BOOL ok = data && CFGetTypeID(data) == CFDataGetTypeID() && CFDataGetLength(data) == 4 &&
              !memcmp(CFDataGetBytePtr(data), enabled, 4) && !ephemeral;
    if (data) CFRelease(data);
    if (ephemeral) CFRelease(ephemeral);
    release(chosen);
    return ok;
}

static id picasso_snapshot_store_object(id object, const char *name)
{
    SEL selector = sel_registerName(name);
    Method method = class_getInstanceMethod(object_getClass(object), selector);
    if (!picasso_snapshot_signature(method, 2, @encode(id), NULL)) return nil;
    return ((id (*)(id, SEL))objc_msgSend)(object, selector);
}

static BOOL picasso_snapshot_store_retry(id owner, NSData *data)
{
    if (![data isKindOfClass:[NSData class]] || !picasso_snapshot_store_png(data.bytes, data.length) ||
        !picasso_snapshot_store_data_mode()) return NO;
    @synchronized(owner) {
        BOOL protected = ((BOOL (*)(id, SEL))objc_msgSend)(owner, sel_registerName("hasProtectedContent"));
        BOOL invalidated = ((BOOL (*)(id, SEL))objc_msgSend)(owner, sel_registerName("_isInvalidated"));
        long content = ((long (*)(id, SEL))objc_msgSend)(owner, sel_registerName("contentType"));
        if (!picasso_snapshot_store_policy(true, false, content, protected, invalidated)) return NO;
        NSString *path = picasso_snapshot_store_object(owner, "path");
        id identity = picasso_snapshot_store_object(owner, "containerIdentity");
        NSString *root = picasso_snapshot_store_object(identity, "snapshotContainerPath");
        if (![path isKindOfClass:[NSString class]] || ![root isKindOfClass:[NSString class]] ||
            ![path.pathExtension isEqualToString:@"png"]) return NO;
        char root_real[PATH_MAX], parent_real[PATH_MAX];
        if (!realpath(root.fileSystemRepresentation, root_real) ||
            !realpath(path.stringByDeletingLastPathComponent.fileSystemRepresentation, parent_real) ||
            !picasso_snapshot_store_beneath(root_real, parent_real) ||
            !strstr(root_real, "/Library/SplashBoard/Snapshots")) return NO;
        return picasso_snapshot_store_write(path.fileSystemRepresentation, data.bytes, data.length);
    }
}

static BOOL picasso_snapshot_store_save(id self, SEL selector, id data, id owner)
{
    BOOL result = picasso_snapshot_save_original(self, selector, data, owner);
    int saved = errno;
    if (!result && saved == EPERM && object_getClass(owner) == objc_getClass("XBApplicationSnapshot") &&
        picasso_snapshot_store_retry(owner, data)) {
        result = YES;
        picasso_snapshot_store_note("written_verified");
    }
    errno = saved;
    return result;
}

static void picasso_snapshot_store_install(uintptr_t base)
{
    static BOOL installed;
    if (installed) return;
    Class cls = objc_getClass("XBApplicationSnapshot");
    Class manifest = objc_getClass("XBApplicationSnapshotManifestImpl");
    SEL selector = sel_registerName("_imageAccessQueue_saveData:forSnapshot:");
    Method save = class_getInstanceMethod(manifest, selector);
    const char *names[] = {"hasProtectedContent", "_isInvalidated", "contentType", "path"};
    const char *returns[] = {@encode(BOOL), @encode(BOOL), @encode(long), @encode(id)};
    uintptr_t offsets[] = {0xf58c, 0x7eac, 0x6598, 0x5c74};
    BOOL valid = picasso_snapshot_signature(save, 4, @encode(BOOL), NULL) &&
                 (uintptr_t)method_getImplementation(save) == base + 0x5cd0;
    for (unsigned i = 0; valid && i < 4; ++i) {
        Method m = class_getInstanceMethod(cls, sel_registerName(names[i]));
        valid = picasso_snapshot_signature(m, 2, returns[i], NULL) &&
                (uintptr_t)method_getImplementation(m) == base + offsets[i];
    }
    if (!valid) { picasso_snapshot_store_note("unavailable"); return; }
    picasso_snapshot_save_original = (PicassoSnapshotSave)method_getImplementation(save);
    class_replaceMethod(manifest, selector, (IMP)picasso_snapshot_store_save, method_getTypeEncoding(save));
    installed = YES;
    picasso_snapshot_store_note("installed");
}

static void picasso_snapshot_store_added(const struct mach_header *header, intptr_t slide)
{
    static const unsigned char uuid[16] = {
        0x6f,0x1a,0xfd,0xac,0xe6,0x2a,0x3a,0xee,0x86,0xd7,0x74,0x70,0x64,0x4a,0x80,0xd7
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
    if (!matched || p != end || text_end < base || text_end - base < 0xf594) return;
    if (!picasso_astc_hash(base, 0x5cd0, 860, "0abd26f233d3aad1465166f366094f19736c522f6dffdd6a5395f0e821a51de8") ||
        !picasso_astc_hash(base, 0xf58c, 8, "3487bb83266aa4982404403bfa5f5f7086ebfc0d0cc0591060245c0bdb23e1f6") ||
        !picasso_astc_hash(base, 0x7eac, 56, "4adce0bdff9575d519df68193225ba2d325363d55ac4ed632e1adab9cb578b9e") ||
        !picasso_astc_hash(base, 0x6598, 8, "ba4c2ccef6b9b64afc1064a567fd2323a67d13fa4034430dd8b448e052ddbdf2") ||
        !picasso_astc_hash(base, 0x5c74, 84, "d6192908f978b9ff45c275a75758b58817003038702f4a9ccc49b357583e0490")) return;
    dispatch_async(dispatch_get_main_queue(), ^{ picasso_snapshot_store_install(base); });
}

__attribute__((constructor)) static void picasso_snapshot_store_start(void)
{
    if ([[NSProcessInfo processInfo].processName isEqualToString:@"SpringBoard"])
        _dyld_register_func_for_add_image(picasso_snapshot_store_added);
}
