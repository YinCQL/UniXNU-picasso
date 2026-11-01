#include <stdint.h>
#include <stddef.h>

/* The pinned picasso source kernel predates these two diagnostics exported by
 * the upstream Adreno patch series. Keep weak definitions so a newer kernel
 * can replace both without a duplicate symbol. */
__attribute__((weak))
volatile uint32_t native_gpu_dcvs[15];

__attribute__((weak))
unsigned int native_pmap_user_mappings(uint32_t page, void **pmap,
    uint64_t *virtual_address, char *name) {
    (void)page;
    (void)pmap;
    (void)virtual_address;
    (void)name;
    return 0;
}

__attribute__((weak))
int boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset, const void *data, uint32_t size) {
    (void)generation;
    (void)total;
    (void)offset;
    (void)data;
    (void)size;
    return 0;
}

__attribute__((weak))
int native_gop_handoff_call(uint32_t op, void *task, const void *in, size_t inSize, void *out, size_t outSize) {
    (void)op;
    (void)task;
    (void)in;
    (void)inSize;
    (void)out;
    (void)outSize;
    return (int)0xe00002c7; // kIOReturnUnsupported
}

__attribute__((weak))
void *native_gop_handoff_memory(void) {
    return (void *)0;
}
