// SPDX-License-Identifier: BSD-2-Clause-Patent
// WLAN firmware inventory, Qualcomm ELF/MBN container parsing and the staged probe/handshake state machines.
//
// Everything in this file is pure C: it is linked into the kext (WlanProbe.cpp calls it) and into the host tests.
// No allocation, no I/O, no vendor blob is stored in this tree. The MBN layout below follows the Qualcomm PIL
// ELF32 format used by the vendor kernel's "%s.mdt" loader and the split .mdt/.b00... convention; the parser was
// checked against a real ipa_fws.mdt/.b00.../ipa_fws.elf set extracted read-only from vendor.img.
#ifndef NATIVE_WLAN_FIRMWARE_H
#define NATIVE_WLAN_FIRMWARE_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// BSD errno values (this header is shared with kernel-side C++).
#define WLAN_ERR_NOENT (-2)
#define WLAN_ERR_IO (-5)
#define WLAN_ERR_NOMEM (-12)
#define WLAN_ERR_INVAL (-22)
#define WLAN_ERR_NOTSUP (-45)
#define WLAN_ERR_TIMEOUT (-110)

// ---------------------------------------------------------------------------------------------------------------
// Inventory. "present" means the bytes (or the exact size/SHA-256 pair) were observed in the extracted V12
// vendor.img or V13 super.raw. Names that the stock kernel or the vendor module reference but that are not in
// those images are listed with present = 0: the real binaries live on the modem/firmware partition, which was not
// part of the backup. Do not copy any of these files into the repository.
enum wlan_fw_kind {
    WLAN_FW_OPAQUE = 0,   // raw firmware/BMI payload; no public container description
    WLAN_FW_MBN = 1,      // ELF32 PIL container (.mdt/.mbn/.b00...)
    WLAN_FW_BDF = 2,      // board data: raw or ELF container (internal BDF layout is NDA)
    WLAN_FW_INI = 3,      // text configuration
    WLAN_FW_KO = 4,       // vendor kernel module, evidence only
};

struct wlan_fw_inventory {
    const char *name;      // file name or naming pattern
    const char *variant;   // image/artifact where it was observed, or "expected"
    const char *role;
    const char *source;    // evidence: DTS line, kernel/module string, vendor file
    enum wlan_fw_kind kind;
    uint8_t present;       // 1 = size/sha256 observed below
    uint32_t size;         // observed size, 0 when not observed
    const char *sha256;    // observed SHA-256, NULL when not observed
};

extern const struct wlan_fw_inventory wlan_firmware_inventory[];
extern const unsigned wlan_firmware_count;

// ---------------------------------------------------------------------------------------------------------------
// Qualcomm ELF32 / MBN. The parser accepts both a single-file image (all segments at their p_offset in one file,
// as in ipa_fws.elf) and the split .mdt + .b00/.b01... form used by the stock PIL loader (every program header
// has one segment file, in header order). A signed image's program header 0 is the hash segment: PT_LOAD with
// p_filesz == 0 and p_memsz == the hash-table size. The hash table starts immediately after the program header
// array; each entry is a little-endian u32 length followed by a 20-byte SHA-1 digest of that segment. A table may
// also carry one trailing self-hash entry for the table itself; verification covers the per-segment entries.
#define WLAN_MBN_MAX_SEGMENTS 24
#define WLAN_MBN_MAX_SIZE (64u << 20)
#define WLAN_MBN_HASH_LEN 20
#define WLAN_MBN_HASH_ENTRY (4u + WLAN_MBN_HASH_LEN)
#define WLAN_MBN_NO_HASH 0xffffffffu

struct wlan_mbn_segment {
    uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

struct wlan_mbn {
    const uint8_t *data;      // header buffer (the .mdt or the single file); never owns it
    size_t size;              // size of data
    uint16_t machine;         // e_machine: 40 EM_ARM, 164 EM_HEXAGON
    uint16_t phnum;
    unsigned segments;        // == phnum, every program header is one segment
    struct wlan_mbn_segment seg[WLAN_MBN_MAX_SEGMENTS];
    unsigned hash_index;      // index of the hash segment, or WLAN_MBN_NO_HASH
    uint32_t hash_offset;     // absolute offset of the hash table inside data
    uint32_t hash_bytes;      // hash segment p_memsz
    unsigned hash_count;      // number of entries in the hash table
    uint64_t payload_size;    // sum of p_filesz over all non-hash segments
    uint64_t image_extent;    // max(p_offset + p_filesz) over all segments
};

// Parse and structurally validate the ELF header, program headers and hash-table shape. Does not require the
// payload segments to be present in data, so it works for a header-only .mdt. Returns 0 or a WLAN_ERR_* value.
int wlan_mbn_parse_header(struct wlan_mbn *out, const uint8_t *data, size_t size);

// Parse a single-file image: header plus every segment extent inside data. Payload segments may not overlap each
// other. Segment 0 may cover the header itself (that is how ipa_fws.elf stores its header block).
int wlan_mbn_parse(struct wlan_mbn *out, const uint8_t *data, size_t size);

// SHA-1 of every non-hash segment, compared with the hash table. Returns 0 when all digests match, -22 on a
// malformed table, -5 on a digest mismatch. A valid image with no hash segment returns 0 (nothing to verify).
int wlan_mbn_verify_hashes(const struct wlan_mbn *mbn, const uint8_t *data, size_t size);

// Split form: header in mdt, one part per program header (part[0] is the header/hash segment when it has filesz).
// Every part must be at least as large as its segment; where the mdt itself still contains a segment, it must
// equal the corresponding part. Returns 0 or a WLAN_ERR_* value.
int wlan_mbn_parse_split(struct wlan_mbn *out, const uint8_t *mdt, size_t mdt_size,
                         const uint8_t *const *parts, const size_t *part_sizes, unsigned nparts);
int wlan_mbn_verify_hashes_split(const struct wlan_mbn *mbn, const uint8_t *const *parts,
                                 const size_t *part_sizes, unsigned nparts);

// Board data container. Two forms are observed in Qualcomm firmware trees: an opaque raw blob and an ELF image
// (ath11k sniffs 0x7f 'E' 'L' 'F' for QCA6390 board files). The internal BDF layout is NDA, so a raw blob is
// validated only as an outer container. WLAN_BDF_MAX_SIZE is a sanity cap, not a format field.
#define WLAN_BDF_MAX_SIZE (16u << 20)
enum wlan_bdf_kind { WLAN_BDF_NONE = 0, WLAN_BDF_RAW = 1, WLAN_BDF_ELF = 2 };

struct wlan_bdf {
    const uint8_t *data;
    size_t size;
    enum wlan_bdf_kind kind;
    struct wlan_mbn elf;      // valid when kind == WLAN_BDF_ELF
};

int wlan_bdf_parse(struct wlan_bdf *out, const uint8_t *data, size_t size);

// Board data file names. The fragments come from /vendor/bin/cnss-daemon (V13): "bdwlan.%03x", "bdwlan.b%02x",
// "bdwlan.bin". Formats: 0 default, 1 board, 2 chip, 3 chip+board. Returns 0 or -22 on overflow/invalid ids.
enum wlan_bdf_name_format {
    WLAN_BDF_NAME_DEFAULT = 0,
    WLAN_BDF_NAME_BOARD = 1,
    WLAN_BDF_NAME_CHIP = 2,
    WLAN_BDF_NAME_CHIP_BOARD = 3,
};
int wlan_bdf_name(char *out, size_t capacity, unsigned chip_id, unsigned board_id, unsigned format);

// ---------------------------------------------------------------------------------------------------------------
// WLFW/QMI handshake as reconstructed from the stock kernel strings (drivers/soc/qcom/icnss_qmi.c). Message
// names and order are evidence, not a captured trace: the kernel prints "Sending capability message", "Sending
// config request", "Sending indication register message", "Sending ini sync request", "Received FW Ready
// Indication", "Sending MSA mem info", "Received MSA Ready Indication", "Sending Mode request". The probe cannot
// speak QMI/GLINK, so this state machine exists to make the sequence explicit and host-testable, with the actual
// transport supplied as callbacks.
enum wlan_wlfw_request {
    WLAN_WLFW_CAPABILITY = 1,
    WLAN_WLFW_CONFIG = 2,
    WLAN_WLFW_IND_REGISTER = 3,
    WLAN_WLFW_INI = 4,
    WLAN_WLFW_MSA_MEM_INFO = 5,
    WLAN_WLFW_MSA_READY = 6,
    WLAN_WLFW_MODE = 7,
};
enum wlan_wlfw_indication {
    WLAN_WLFW_IND_FW_READY = 1,
    WLAN_WLFW_IND_MSA_READY = 2,
};
enum wlan_handshake_step {
    WLAN_HS_LOAD = 1,
    WLAN_HS_CAPABILITY = 2,
    WLAN_HS_CONFIG = 3,
    WLAN_HS_IND_REGISTER = 4,
    WLAN_HS_INI = 5,
    WLAN_HS_FW_READY = 6,
    WLAN_HS_MSA_INFO = 7,
    WLAN_HS_MSA_READY = 8,
    WLAN_HS_MODE_ON = 9,
    WLAN_HS_DONE = 10,
};

struct wlan_handshake_ops {
    void *ctx;
    int (*load_image)(void *ctx, const struct wlan_mbn *image);
    int (*request)(void *ctx, unsigned request, uint64_t arg0, uint64_t arg1);
    int (*wait_indication)(void *ctx, unsigned indication, unsigned timeout_ms);
};
struct wlan_handshake_state {
    unsigned step;        // last step attempted
    int error;            // first failure, 0 on success
    unsigned requests;    // successful request callbacks
};

int wlan_handshake_run(const struct wlan_handshake_ops *ops, const struct wlan_mbn *image,
                       uint64_t msa_base, uint64_t msa_size, unsigned timeout_ms,
                       struct wlan_handshake_state *state);

// ---------------------------------------------------------------------------------------------------------------
// Staged probe orchestration. WlanProbe.cpp implements the five callbacks with real MMIO/IOKit work; the host
// test supplies fakes. Stage 5 is expected to fail with WLAN_ERR_NOTSUP on real hardware because the Apple-side
// 802.11 stack does not exist for this chip.
enum wlan_probe_stage { WLAN_PROBE_STAGE_MAX = 5 };

struct wlan_probe_ops {
    void *ctx;
    int (*stage1)(void *ctx);   // clocks/regulators/icnss identity
    int (*stage2)(void *ctx);   // MSA + SMMU mapping plan
    int (*stage3)(void *ctx);   // firmware parse + PIL/QMI handshake plan
    int (*stage4)(void *ctx);   // transport attach, only if PCIe/SDIO
    int (*stage5)(void *ctx);   // Apple 802.11 stack check
};
struct wlan_probe_state {
    unsigned stage_req;
    unsigned stage;       // last stage entered
    unsigned step;        // == stage for the pure orchestrator
    int error;
};

int wlan_probe_run(const struct wlan_probe_ops *ops, unsigned stage_req, struct wlan_probe_state *state);

// SHA-1 exposed for the host tests and for diagnostics. Standard FIPS 180-1, no dependencies.
void wlan_sha1(const uint8_t *data, size_t size, uint8_t out[WLAN_MBN_HASH_LEN]);

#ifdef __cplusplus
}
#endif

#endif
