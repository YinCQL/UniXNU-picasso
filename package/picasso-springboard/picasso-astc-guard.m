// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included by the board's already scoped and signed UI injection library.
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include "picasso-astc-guard-core.h"

@protocol PicassoASTCLayout <NSObject>
- (NSUInteger)size;
@end
@protocol PicassoASTCDevice <NSObject>
- (id<PicassoASTCLayout>)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor
                                 isHeapOrBufferBacked:(BOOL)backed;
@end

typedef int (*PicassoASTCDecode)(void *, unsigned char *, size_t);
static PicassoASTCDecode picasso_astc_decode, picasso_atx_decode;
static uintptr_t picasso_astc_texture_vtable;

static void picasso_astc_note(const char *reason, const struct picasso_astc_request *request,
                              uint64_t layout_bytes)
{
    static unsigned count;
    if (__atomic_fetch_add(&count, 1, __ATOMIC_RELAXED) >= 24) return;
    char key[96], line[256];
    snprintf(key, sizeof(key), "kern.picasso_astc_%s", reason);
    picasso_ack(key);
    int length = snprintf(line, sizeof(line),
        "PICASSO_ASTC_GUARD pid=%d reason=%s width=%u height=%u block=%u/%u linear=%llu layout=%llu\n",
        getpid(), reason, request ? request->width : 0, request ? request->height : 0,
        request ? request->block_width : 0, request ? request->block_height : 0,
        (unsigned long long)(request ? request->linear_bytes : 0), (unsigned long long)layout_bytes);
    int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) {
        if (length > 0 && (size_t)length < sizeof(line)) (void)write(fd, line, (size_t)length);
        close(fd);
    }
}

static int picasso_astc_preflight(void *plugin, int atx)
{
    struct picasso_astc_request request = {0};
    enum picasso_astc_action action = picasso_astc_request(plugin, atx,
        picasso_astc_texture_vtable, &request);
    if (action == PICASSO_ASTC_PASS) return 1;
    if (action == PICASSO_ASTC_INVALID) {
        picasso_astc_note("invalid", &request, 0);
        return 0;
    }
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            picasso_astc_note("no_device", &request, 0);
            return 0;
        }
        SEL selector = @selector(newTextureLayoutWithDescriptor:isHeapOrBufferBacked:);
        if (![device respondsToSelector:selector]) {
            picasso_astc_note("no_layout", &request, 0);
            return 0;
        }
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:(MTLPixelFormat)request.format
            width:request.width height:request.height mipmapped:NO];
        descriptor.mipmapLevelCount = 1;
        id<PicassoASTCLayout> layout = [(id<PicassoASTCDevice>)device
            newTextureLayoutWithDescriptor:descriptor isHeapOrBufferBacked:NO];
        SEL copy = NSSelectorFromString(@"copyFromTextureMemory:textureSlice:textureLevel:textureRegion:toLinearBytes:linearOffset:linearBytesPerRow:linearBytesPerImage:");
        if (!layout || ![layout respondsToSelector:@selector(size)] ||
            ![layout respondsToSelector:copy]) {
            picasso_astc_note("no_layout", &request, 0);
            return 0;
        }
        uint64_t bytes = [layout size];
        if (!picasso_astc_layout_fits(&request, bytes)) {
            picasso_astc_note("invalid", &request, bytes);
            return 0;
        }
        /* This preflight does not establish a foreign layout's compatibility
         * with Apple texture data. The original decoder still owns that work.
         * Do not cache failure: a real device may become available later. */
        return 1;
    }
}

static int picasso_astc_guard(void *plugin, unsigned char *pixels, size_t bytes)
{
    /* Original ASTCReadPlugin returns -50 on failure; callers discard blocks. */
    if (!picasso_astc_preflight(plugin, 0)) return -50;
    return picasso_astc_decode(plugin, pixels, bytes);
}

static int picasso_atx_guard(void *plugin, unsigned char *pixels, size_t bytes)
{
    /* ATXReadPlugin's corresponding error is -4. */
    if (!picasso_astc_preflight(plugin, 1)) return -4;
    return picasso_atx_decode(plugin, pixels, bytes);
}

static int picasso_astc_hash(uintptr_t base, uintptr_t offset, size_t size, const char *expected)
{
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    CC_SHA256((const void *)(base + offset), (CC_LONG)size, hash);
    for (unsigned i = 0; i < sizeof(hash); ++i) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    return !strcmp(hex, expected);
}

static void picasso_astc_image_added(const struct mach_header *header, intptr_t slide)
{
    static const unsigned char uuid[16] = {
        0xf8,0x6c,0xaa,0xd0,0xa0,0xc0,0x35,0x46,0x9c,0xf7,0x93,0xc6,0xee,0xc4,0x6f,0xf2
    };
    if (header->magic != MH_MAGIC_64 || header->cputype != CPU_TYPE_ARM64) return;
    const struct mach_header_64 *h = (const struct mach_header_64 *)header;
    if (h->sizeofcmds > 65536 || h->ncmds > 128) return;
    const unsigned char *p = (const unsigned char *)(h + 1), *end = p + h->sizeofcmds;
    int matched = 0;
    uintptr_t text_start = 0, text_end = 0, data_start = 0, data_end = 0;
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
            if (!strncmp(seg->segname, "__TEXT", 16)) {
                text_start = start; text_end = start + seg->vmsize;
            } else if (!strncmp(seg->segname, "__DATA_CONST", 16)) {
                data_start = start; data_end = start + seg->vmsize;
            }
        }
        p += cmd->cmdsize;
    }
    if (!matched) return;
    /* No executable-page edits, interpose-all-malloc or kernel bypass. Change
     * only these two C++ dispatch slots in this process's private COW pages. */
    uintptr_t base = (uintptr_t)header;
    uintptr_t astc_slot = base + 0x4a496e58, atx_slot = base + 0x4a495a58;
    uintptr_t texture_vtable = base + 0x4a496ed0;
    if (base != text_start || text_end < base + 0x1e7620 ||
        data_start > atx_slot || data_end < texture_vtable + sizeof(uintptr_t) ||
        !picasso_astc_hash(base, 0x1e7598, 136, "c586ad90558397317ba19d7346d825d61ac5a618d8ae6c898af279a7b33a73ba") ||
        !picasso_astc_hash(base, 0x15e59c, 40, "3b935f85450bf3bc588fe97f43474db64931282e97c44753003372613de6d993") ||
        !picasso_astc_hash(base, 0x15e90c, 136, "8e790ff8ebd684664292c564e553891e70ed932ab95516072c68bedd78566f69") ||
        !picasso_astc_hash(base, 0x52fe4, 652, "37df5709c0e24dc83577f2133d3449c2706d81216fbedadb505533435883d83e") ||
        !picasso_astc_hash(base, 0x17c7c8, 92, "679adf0818dfda37dd312afe1ba15255a2a785856d5a06eb29841016f0c640f2") ||
        !picasso_astc_hash(base, 0x5b6c8, 40, "a6710508d7118288711aae523af0aa4059fa08c020e79b5cbb8173d97fa7bcd4") ||
        !picasso_astc_hash(base, 0x59b4, 8, "08f512c23cbfb4ca4e1633deabe639fc8c480d23f00975968cb4e70b6151d738") ||
        !picasso_astc_hash(base, 0x74f28, 8, "1e20666c05e98db358ca32522f1309eec35a048d9afbfc473e2d7a0901670bb8")) {
        picasso_astc_note("unavailable", NULL, 0);
        return;
    }
    static unsigned installed;
    if (__atomic_exchange_n(&installed, 1, __ATOMIC_RELAXED)) return;
    uintptr_t *slots[2] = {(uintptr_t *)astc_slot, (uintptr_t *)atx_slot};
    uintptr_t originals[2] = {base + 0x1e7598, base + 0x15e59c};
    uintptr_t replacements[2] = {(uintptr_t)picasso_astc_guard, (uintptr_t)picasso_atx_guard};
    mach_vm_address_t pages[2];
    vm_prot_t protections[2];
    if (!vm_page_size || (vm_page_size & (vm_page_size - 1))) return;
    for (unsigned i = 0; i < 2; ++i) {
        if (__atomic_load_n(slots[i], __ATOMIC_ACQUIRE) != originals[i]) {
            picasso_astc_note("unavailable", NULL, 0); return;
        }
        mach_vm_address_t address = (mach_vm_address_t)slots[i];
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &address, &size,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
        pages[i] = (uintptr_t)slots[i] & ~((uintptr_t)vm_page_size - 1);
        if (kr != KERN_SUCCESS || address > pages[i] || size < vm_page_size ||
            pages[i] - address > size - vm_page_size ||
            !(info.protection & VM_PROT_READ) || (info.protection & VM_PROT_EXECUTE)) {
            picasso_astc_note("unavailable", NULL, 0); return;
        }
        protections[i] = info.protection;
    }
    picasso_astc_decode = (PicassoASTCDecode)originals[0];
    picasso_atx_decode = (PicassoASTCDecode)originals[1];
    picasso_astc_texture_vtable = texture_vtable;
    unsigned writable = 0;
    for (; writable < 2; ++writable) {
        if (mach_vm_protect(mach_task_self(), pages[writable], vm_page_size, FALSE,
                            VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) break;
    }
    if (writable == 2) {
        for (unsigned i = 0; i < 2; ++i)
            __atomic_store_n(slots[i], replacements[i], __ATOMIC_RELEASE);
    }
    int restored = 1;
    for (unsigned i = 0; i < writable; ++i) {
        if (mach_vm_protect(mach_task_self(), pages[i], vm_page_size, FALSE,
                            protections[i]) != KERN_SUCCESS) restored = 0;
    }
    picasso_astc_note(!restored ? "restore_failed" : writable == 2 ? "installed" : "unavailable", NULL, 0);
}

static void picasso_install_astc_guard(void)
{
    /* dyld calls back for already loaded images and subsequently loaded ones. */
    _dyld_register_func_for_add_image(picasso_astc_image_added);
}
