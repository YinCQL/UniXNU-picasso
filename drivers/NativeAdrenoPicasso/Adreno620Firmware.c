/* SPDX-License-Identifier: MIT */
#include "Adreno620Firmware.h"
#include <string.h>

static const size_t ElfHeaderBytes = 0x34;
static const size_t ElfProgramHeaderBytes = 0x20;
static const size_t GmuBlockHeaderBytes = 16;
static const size_t SqeHeaderBytes = 4;
static const uint16_t EmQcom = 0xa4;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t Adreno620Crc32Update(uint32_t crc, const void *bytes, size_t length)
{
    const uint8_t *p = (const uint8_t *)bytes;
    if (!p && length) return crc;
    crc = ~crc;
    for (size_t index = 0; index < length; ++index) {
        crc ^= p[index];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t Adreno620Crc32(const void *bytes, size_t length)
{
    return Adreno620Crc32Update(0, bytes, length);
}

int Adreno620FirmwareCheck(const uint8_t *bytes, size_t length,
    uint64_t expected_bytes, uint32_t expected_crc32)
{
    if (!bytes || !expected_bytes || expected_bytes > (uint64_t)SIZE_MAX)
        return 0;
    if ((uint64_t)length != expected_bytes) return 0;
    return Adreno620Crc32(bytes, length) == expected_crc32 ? 1 : 0;
}

static int powerOfTwo(uint32_t value)
{
    return value && !(value & (value - 1u));
}

Adreno620FwStatus Adreno620MdtParse(const uint8_t *image, size_t bytes,
    const uint8_t *const segment_data[], const size_t segment_bytes[],
    size_t segment_count, Adreno620MdtImage *out)
{
    if (!image || !out || bytes < ElfHeaderBytes) return Adreno620FwTruncated;
    if (image[0] != 0x7f || image[1] != 'E' || image[2] != 'L' ||
        image[3] != 'F')
        return Adreno620FwBadMagic;
    if (image[4] != 1 || image[5] != 1 || image[6] != 1)
        return Adreno620FwInvalid;          /* ELFCLASS32, LSB, version 1 */
    if (le16(image + 0x12) != EmQcom) return Adreno620FwBadMachine;
    if (le16(image + 0x2a) != (uint16_t)ElfProgramHeaderBytes)
        return Adreno620FwInvalid;
    const uint32_t phnum = le16(image + 0x2c);
    const uint32_t phoff = le32(image + 0x1c);
    const uint32_t entry = le32(image + 0x18);
    if (!phnum || phnum > (uint32_t)Adreno620MaxMdtSegments)
        return Adreno620FwNoSpace;
    if (phoff > bytes || phnum > (bytes - phoff) / ElfProgramHeaderBytes)
        return Adreno620FwTruncated;
    const int split = segment_data != NULL;
    if (split && segment_count < phnum) return Adreno620FwTruncated;

    Adreno620MdtImage parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.machine = EmQcom;
    parsed.entry = entry;
    parsed.segments = phnum;
    uint32_t image_crc = 0;
    int entry_found = 0;
    for (uint32_t index = 0; index < phnum; ++index) {
        const uint8_t *ph = image + phoff + index * ElfProgramHeaderBytes;
        Adreno620MdtSegment *segment = &parsed.segment[index];
        segment->type = le32(ph + 0);
        segment->offset = le32(ph + 4);
        segment->vaddr = le32(ph + 8);
        segment->filesz = le32(ph + 16);
        segment->memsz = le32(ph + 20);
        segment->flags = le32(ph + 24);
        segment->align = le32(ph + 28);
        segment->loadable = segment->type == 1;
        if (segment->vaddr + segment->filesz > UINT64_C(0x100000000) ||
            segment->vaddr + segment->memsz > UINT64_C(0x100000000))
            return Adreno620FwBadRange;
        if (segment->loadable) {
            if (segment->memsz < segment->filesz) return Adreno620FwBadRange;
            if (segment->align && !powerOfTwo((uint32_t)segment->align))
                return Adreno620FwBadRange;
            if (entry >= segment->vaddr &&
                entry < segment->vaddr + segment->filesz)
                entry_found = 1;
        }
        if (split) {
            if (segment->filesz) {
                if (!segment_data[index] ||
                    segment_bytes[index] < segment->filesz)
                    return Adreno620FwTruncated;
                segment->data = segment_data[index];
                segment->bytes = segment_bytes[index];
            }
        } else if (segment->filesz) {
            if (segment->offset > bytes ||
                segment->filesz > bytes - segment->offset)
                return Adreno620FwTruncated;
            segment->data = image + segment->offset;
            segment->bytes = (size_t)segment->filesz;
        }
        if (segment->filesz) {
            segment->crc32 = Adreno620Crc32(segment->data,
                (size_t)segment->filesz);
            if (segment->loadable) {
                if (parsed.loaded_bytes > UINT64_MAX - segment->filesz)
                    return Adreno620FwBadRange;
                parsed.loaded_bytes += segment->filesz;
                image_crc = Adreno620Crc32Update(image_crc, segment->data,
                    (size_t)segment->filesz);
            }
        }
    }
    if (!entry_found) return Adreno620FwBadRange;
    parsed.image_crc32 = image_crc;
    *out = parsed;
    return Adreno620FwOk;
}

Adreno620FwStatus Adreno620SqeParse(const uint8_t *image, size_t bytes,
    uint32_t minimum_version, Adreno620SqeImage *out)
{
    if (!image || !out) return Adreno620FwInvalid;
    if (bytes < SqeHeaderBytes + 8) return Adreno620FwTruncated;
    if ((bytes - SqeHeaderBytes) % 4) return Adreno620FwInvalid;
    const size_t payload_bytes = bytes - SqeHeaderBytes;
    Adreno620SqeImage parsed;
    parsed.header = le32(image);
    parsed.payload = image + SqeHeaderBytes;
    parsed.payload_bytes = payload_bytes;
    parsed.version = le32(image + SqeHeaderBytes) & 0xfffu;
    parsed.crc32 = Adreno620Crc32(parsed.payload, payload_bytes);
    parsed.security_ok = minimum_version == 0 ||
        parsed.version >= minimum_version;
    *out = parsed;
    return Adreno620FwOk;
}

static int regionContains(const Adreno620GmuRegions *regions, uint32_t address,
    uint32_t size, Adreno620GmuDestination *destination)
{
    struct {
        uint64_t base, bytes;
        Adreno620GmuDestination id;
    } table[4] = {
        {regions->itcm_base, regions->itcm_bytes, Adreno620GmuItcm},
        {regions->dtcm_base, regions->dtcm_bytes, Adreno620GmuDtcm},
        {regions->icache_base, regions->icache_bytes, Adreno620GmuIcache},
        {regions->dummy_base, regions->dummy_bytes, Adreno620GmuDummy},
    };
    for (unsigned index = 0; index < 4; ++index) {
        if (size > table[index].bytes) continue;
        if (address < table[index].base) continue;
        if ((uint64_t)address - table[index].base >
            table[index].bytes - size)
            continue;
        *destination = table[index].id;
        return 1;
    }
    return 0;
}

Adreno620FwStatus Adreno620GmuParse(const uint8_t *image, size_t bytes,
    const Adreno620GmuRegions *regions, Adreno620GmuImage *out)
{
    if (!image || !regions || !out) return Adreno620FwInvalid;
    /* payload_offset is a 32-bit field; a multi-gigabyte container is not a GMU
     * image and would silently truncate. */
    if (bytes > UINT32_MAX) return Adreno620FwBadRange;
    struct {
        uint64_t base, bytes;
    } table[4] = {
        {regions->itcm_base, regions->itcm_bytes},
        {regions->dtcm_base, regions->dtcm_bytes},
        {regions->icache_base, regions->icache_bytes},
        {regions->dummy_base, regions->dummy_bytes},
    };
    for (unsigned index = 0; index < 4; ++index) {
        /* The ITCM legitimately starts at address 0; only a zero size or an
         * overflowing span is invalid. */
        if (!table[index].bytes ||
            table[index].base + table[index].bytes < table[index].base)
            return Adreno620FwInvalid;
        for (unsigned other = 0; other < index; ++other)
            if (table[index].base < table[other].base + table[other].bytes &&
                table[other].base < table[index].base + table[index].bytes)
                return Adreno620FwBadRange;
    }

    Adreno620GmuImage parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.entry = 0;
    uint32_t image_crc = 0;
    int entry_found = 0;
    size_t offset = 0;
    while (offset < bytes) {
        if (bytes - offset < GmuBlockHeaderBytes) return Adreno620FwTruncated;
        const uint8_t *header = image + offset;
        const uint32_t address = le32(header + 0);
        const uint32_t size = le32(header + 4);
        const uint32_t type = le32(header + 8);
        const uint32_t value = le32(header + 12);
        if (size > bytes - offset - GmuBlockHeaderBytes)
            return Adreno620FwTruncated;
        if (size & 3u) return Adreno620FwBadRange;
        if (parsed.blocks == (size_t)Adreno620MaxGmuBlocks)
            return Adreno620FwNoSpace;
        Adreno620GmuBlock *block = &parsed.block[parsed.blocks];
        block->address = address;
        block->bytes = size;
        block->type = type;
        block->value = value;
        if (size) {
            if ((uint64_t)address + size > UINT64_C(0x100000000))
                return Adreno620FwBadRange;
            Adreno620GmuDestination destination;
            if (!regionContains(regions, address, size, &destination))
                return Adreno620FwUnmapped;
            block->destination = (uint8_t)destination;
            block->payload_offset = (uint32_t)offset + GmuBlockHeaderBytes;
            block->crc32 = Adreno620Crc32(image + block->payload_offset, size);
            image_crc = Adreno620Crc32Update(image_crc,
                image + block->payload_offset, size);
            if (parsed.data_bytes > UINT64_MAX - size)
                return Adreno620FwBadRange;
            parsed.data_bytes += size;
            for (size_t earlier = 0; earlier < parsed.blocks; ++earlier) {
                const Adreno620GmuBlock *previous = &parsed.block[earlier];
                if (!previous->bytes) continue;
                if ((uint64_t)address <
                        (uint64_t)previous->address + previous->bytes &&
                    (uint64_t)previous->address < (uint64_t)address + size)
                    return Adreno620FwBadRange;
            }
            if (block->destination == Adreno620GmuItcm && address == 0 &&
                size >= 8)
                entry_found = 1;
        }
        ++parsed.blocks;
        offset += GmuBlockHeaderBytes + size;
    }
    if (!parsed.blocks) return Adreno620FwTruncated;
    if (!entry_found) return Adreno620FwBadRange;
    parsed.image_crc32 = image_crc;
    *out = parsed;
    return Adreno620FwOk;
}
