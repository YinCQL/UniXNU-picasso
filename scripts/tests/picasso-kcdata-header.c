/* Execute the pinned XNU flag-producing helper. The descriptor and allocation
 * sink are host models; only the emitted item header is used by Python. */
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef FILE *kcdata_descriptor_t;
typedef uintptr_t mach_vm_address_t;
typedef int kern_return_t;
static uint32_t kcdata_calc_padding(uint32_t size) { return -size & 15u; }
static kern_return_t kcdata_get_memory_addr_with_flavor(
    kcdata_descriptor_t file, uint32_t type, uint32_t size, uint64_t flags,
    mach_vm_address_t *address)
{
    uint32_t words[2] = {type, size + kcdata_calc_padding(size)};
    assert(fwrite(words, sizeof(words), 1, file) == 1);
    assert(fwrite(&flags, sizeof(flags), 1, file) == 1);
    *address = 0;
    return 0;
}
#include "kcdata-header.inc"
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "wb"); assert(file);
    mach_vm_address_t address;
    assert(kcdata_get_memory_addr(file, 0x434f4d50u, 0, &address) == 0);
    assert(kcdata_get_memory_addr(file, 0x59a25807u, 0, &address) == 0);
    assert(kcdata_get_memory_addr(file, 3, 40, &address) == 0);
    assert(kcdata_get_memory_addr(file, 2, 36, &address) == 0);
    assert(fclose(file) == 0);
    return 0;
}
