/* SPDX-License-Identifier: MIT */
#ifndef ADRENO620_FIRMWARE_H
#define ADRENO620_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Container validation for the three Adreno 620 firmware kinds found in the
 * picasso vendor image:
 *   - a620_zap.mdt plus a620_zap.b00..b02 (or the combined a620_zap.elf):
 *     a 32-bit little-endian Qualcomm ELF. The .mdt carries the ELF header,
 *     program headers and the hash segment; .bNN carry the segment payloads in
 *     program-header order.
 *   - a630_sqe.fw (or a650_sqe.fw): a 4-byte preamble followed by the SQE
 *     microcode. Linux adreno_fw_create_bo copies fw->data + 4, fw->size - 4;
 *     the first microcode dword's low 12 bits are the version the security
 *     check reads (a6xx_gpu.c a6xx_ucode_check_version).
 *   - a650_gmu.bin: a TLV list of {address,size,type,value} blocks, exactly the
 *     struct block_header a6xx_gmu_fw_load walks. size == 0 blocks are
 *     metadata; the others are copied to ITCM, DTCM, icache or dummy by address.
 * This proves structure, sizes and CRC32, not authenticity or execution. Output
 * is unchanged on failure. No allocation is performed. */

enum {
    Adreno620MaxMdtSegments = 8,
    Adreno620MaxGmuBlocks = 64
};

typedef enum {
    Adreno620FwOk = 0,
    Adreno620FwInvalid,      /* malformed field or unsupported container */
    Adreno620FwTruncated,    /* a declared range is not present */
    Adreno620FwBadMagic,
    Adreno620FwBadMachine,
    Adreno620FwBadRange,
    Adreno620FwNoSpace,
    Adreno620FwUnmapped,
    Adreno620FwCrcMismatch,
    Adreno620FwVersionTooOld
} Adreno620FwStatus;

/* IEEE 802.3 CRC32 (reflected 0xedb88320, init/xorout 0xffffffff). */
uint32_t Adreno620Crc32(const void *bytes, size_t length);
uint32_t Adreno620Crc32Update(uint32_t crc, const void *bytes, size_t length);

/* Whole-file bounds and CRC check against a board descriptor. Returns 1 only
 * when both match. */
int Adreno620FirmwareCheck(const uint8_t *bytes, size_t length,
    uint64_t expected_bytes, uint32_t expected_crc32);

/* ---------------------------------------------------------------------- */
/* Qualcomm MDT / split ELF                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint32_t type;           /* ELF p_type; 1 is PT_LOAD */
    uint32_t flags;          /* ELF p_flags */
    uint32_t offset;         /* p_offset in the combined ELF */
    uint64_t vaddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
    uint32_t crc32;          /* over filesz bytes */
    const uint8_t *data;
    size_t bytes;            /* available bytes, >= filesz */
    int loadable;            /* type == PT_LOAD */
} Adreno620MdtSegment;

typedef struct {
    uint16_t machine;
    uint32_t entry;
    size_t segments;
    Adreno620MdtSegment segment[Adreno620MaxMdtSegments];
    uint64_t loaded_bytes;   /* sum of loadable filesz */
    uint32_t image_crc32;    /* loadable payloads concatenated in order */
} Adreno620MdtImage;

/* image is the .mdt or the combined .elf. When segment_data/segment_bytes are
 * non-null, segment i is taken from segment_data[i] (the .bNN file) and must be
 * at least p_filesz bytes; otherwise the payload is read from p_offset inside
 * image, which must then be the combined .elf. The entry point must fall inside
 * a PT_LOAD segment, and a PT_LOAD segment must have memsz >= filesz. The
 * vendor hash segment is a non-load p_type 0 block whose filesz (0x1a38) is
 * larger than memsz (0x2000); that is accepted for non-load segments. */
Adreno620FwStatus Adreno620MdtParse(const uint8_t *image, size_t bytes,
    const uint8_t *const segment_data[], const size_t segment_bytes[],
    size_t segment_count, Adreno620MdtImage *out);

/* ---------------------------------------------------------------------- */
/* SQE microcode                                                           */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint32_t header;         /* first dword, skipped by the kernel loader */
    const uint8_t *payload;
    size_t payload_bytes;
    uint32_t version;        /* payload dword 0 & 0xfff */
    uint32_t crc32;          /* over payload */
    int security_ok;         /* version >= minimum_version passed by caller */
} Adreno620SqeImage;

/* The vendor a630_sqe.fw/a650_sqe.fw have header 0 and a payload that is a
 * whole number of dwords. minimum_version is the mainline security floor for
 * the chosen file (0x190 for a630, 0x095 for a650); pass 0 to skip the flag.
 * The vendor files measured here are 0x188 and 0x087, below both floors, so
 * security_ok is reported instead of failing the structural parse. */
Adreno620FwStatus Adreno620SqeParse(const uint8_t *image, size_t bytes,
    uint32_t minimum_version, Adreno620SqeImage *out);

/* ---------------------------------------------------------------------- */
/* GMU firmware container                                                  */
/* ---------------------------------------------------------------------- */

typedef enum {
    Adreno620GmuMetadata = 0,
    Adreno620GmuItcm,
    Adreno620GmuDtcm,
    Adreno620GmuIcache,
    Adreno620GmuDummy
} Adreno620GmuDestination;

typedef struct {
    uint64_t itcm_base, itcm_bytes;
    uint64_t dtcm_base, dtcm_bytes;
    uint64_t icache_base, icache_bytes;
    uint64_t dummy_base, dummy_bytes;
} Adreno620GmuRegions;

/* The first five fields line up with kits/AdrenoFirmware's
 * AdrenoFirmwareBlock (address, bytes, type, value, payload_offset); the
 * destination replaces that kit's region_identifier, and crc32 is added. */
typedef struct {
    uint32_t address;
    uint32_t bytes;
    uint32_t type;           /* observed 1/7 on metadata, 0xdeadbeef on data */
    uint32_t value;
    uint32_t payload_offset; /* 0 for metadata */
    uint32_t crc32;          /* payload only, 0 for metadata */
    uint8_t destination;     /* Adreno620GmuDestination */
} Adreno620GmuBlock;

typedef struct {
    size_t blocks;
    Adreno620GmuBlock block[Adreno620MaxGmuBlocks];
    uint64_t data_bytes;
    uint32_t image_crc32;    /* data payloads concatenated in order */
    uint32_t entry;          /* CM3 reset vector: 0 in ITCM */
} Adreno620GmuImage;

/* Regions must be non-zero and must not overlap. Every data block must fit
 * exactly one region; a partial fit, an unmapped address or an overlap between
 * data blocks is refused. A data block covering [0, 8) must exist because the
 * CM3 fetches its initial SP and reset vector from address 0. */
Adreno620FwStatus Adreno620GmuParse(const uint8_t *image, size_t bytes,
    const Adreno620GmuRegions *regions, Adreno620GmuImage *out);

#ifdef __cplusplus
}
#endif

#endif /* ADRENO620_FIRMWARE_H */
