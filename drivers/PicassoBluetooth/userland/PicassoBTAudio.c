/* Install only the audited BTAudioHALPlugin's private Skywalk bindings.
 * The shared cache and unrelated audio plug-ins keep their native imports. */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <string.h>
#include <skywalk/os_skywalk_private.h>

static const uint8_t expected_uuid[16] = {
    0x98,0x99,0xd1,0xc1,0x2f,0x49,0x3a,0xab,
    0x92,0x23,0xdf,0xad,0x56,0x4c,0xfe,0x2e
};

__attribute__((constructor)) static void btaudio_init(void)
{
    for (uint32_t i = 0; i < _dyld_image_count(); ++i) {
        const struct mach_header_64 *h = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (!h || h->magic != MH_MAGIC_64 || h->filetype != MH_BUNDLE ||
            h->sizeofcmds > 16384 || h->ncmds > 128) continue;
        const uint8_t *lc = (const uint8_t *)(h + 1), *end = lc + h->sizeofcmds;
        int match = 0;
        for (uint32_t j = 0; j < h->ncmds; ++j) {
            if ((size_t)(end - lc) < sizeof(struct load_command)) break;
            const struct load_command *c = (const struct load_command *)lc;
            if (c->cmdsize < sizeof(*c) || c->cmdsize > (size_t)(end - lc)) break;
            if (c->cmd == LC_UUID && c->cmdsize == sizeof(struct uuid_command))
                match = !memcmp(((const struct uuid_command *)c)->uuid, expected_uuid, 16);
            lc += c->cmdsize;
        }
        if (!match || lc != end) continue;
        intptr_t slide = _dyld_get_image_vmaddr_slide(i);
#include "btaudio-slots.inc"
        return;
    }
}
