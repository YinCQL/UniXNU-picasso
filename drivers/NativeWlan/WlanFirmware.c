// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware inventory, ELF/MBN + board-data container parsers, and the pure probe/handshake state machines.
// See WlanFirmware.h for the provenance of every layout decision.
#include "WlanFirmware.h"
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------
// Inventory. Sizes and SHA-256 values were measured read-only from /tmp/picasso-vendor/vendor.img (V12 ext4) and
// /tmp/picasso-vendor/super.raw (V13 vendor ext4, partition base 0x17b300000). No vendor file is stored here.
// Two extraction facts that are easy to get wrong: /firmware/wlan/qca_cld/{WCNSS_qcom_cfg.ini,wlan_mac.bin} are
// symlinks in both images (the real config is /vendor/etc/wifi, the MAC/NV target is in /data or persist), and
// qca_cld3_wlan.ko contains ext4 holes: its size/hash below are the logical file with holes read as zeros.
// ---------------------------------------------------------------------------------------------------------------
const struct wlan_fw_inventory wlan_firmware_inventory[] = {
    {"modem.mdt", "expected", "PIL container for the modem subsystem; pil_modem loads qcom,firmware-name=\"modem\" "
     "into modem_wlan_mem, next to the MSA region", "picasso.dts:6451-6477 (pil_modem), 485-489", WLAN_FW_MBN, 0, 0, NULL},
    {"wlanmdsp.mbn", "expected", "conventional WLAN/WCSS DSP image; this DT has no wlan PIL node and the stock Image "
     "does not contain the name", "lito convention; absent from picasso.dts and stock-kernel strings", WLAN_FW_MBN, 0, 0, NULL},
    {"athwlan.bin", "referenced by qca_cld3_wlan.ko", "WLAN MAC firmware downloaded by qcacld over BMI/SNOC",
     "V12/V13 qca_cld3_wlan.ko: athwlan.bin, bmi_download_firmware, \"Loading firmware file %s\"", WLAN_FW_OPAQUE, 0, 0, NULL},
    {"athsetup.bin", "referenced by qca_cld3_wlan.ko", "BMI setup payload", "V12/V13 qca_cld3_wlan.ko strings",
     WLAN_FW_OPAQUE, 0, 0, NULL},
    {"otp.bin", "referenced by qca_cld3_wlan.ko", "one-time-programmable calibration payload",
     "V12/V13 qca_cld3_wlan.ko strings", WLAN_FW_OPAQUE, 0, 0, NULL},
    {"epping.bin", "referenced by qca_cld3_wlan.ko", "EPPING factory-test firmware", "V12/V13 qca_cld3_wlan.ko strings",
     WLAN_FW_OPAQUE, 0, 0, NULL},
    {"utf.bin", "referenced by qca_cld3_wlan.ko", "factory test firmware", "V12/V13 qca_cld3_wlan.ko strings",
     WLAN_FW_OPAQUE, 0, 0, NULL},
    {"fakeboar.bin", "referenced by qca_cld3_wlan.ko", "factory board-data stand-in", "V12/V13 qca_cld3_wlan.ko strings",
     WLAN_FW_OPAQUE, 0, 0, NULL},
    {"bdwlan.%03x.b%02x", "expected", "board data selected by chip id and board id",
     "V13 /bin/cnss-daemon: bdwlan.%03x, bdwlan.b%02x, bdwlan.bin", WLAN_FW_BDF, 0, 0, NULL},
    {"bdwlan.bin", "expected", "default board data", "V13 /bin/cnss-daemon strings", WLAN_FW_BDF, 0, 0, NULL},
    {"regdb.bin", "expected", "regulatory database handed to the WLAN firmware",
     "V13 /bin/cnss-daemon string", WLAN_FW_OPAQUE, 0, 0, NULL},
    {"WCNSS_qcom_cfg.ini", "V12 vendor.img /etc/wifi", "the real V12 driver configuration (the "
     "/firmware/wlan/qca_cld path is a symlink here)", "ext4 read; SHA-256 measured", WLAN_FW_INI, 1, 16038,
     "1741faf3a16ba10f02eff0202d85bb68101d0f76ddebec291ccfd484ada8aea7"},
    {"WCNSS_qcom_cfg.ini", "V13 super.raw /etc/wifi", "the real V13 driver configuration (the "
     "/firmware/wlan/qca_cld path is a symlink here)", "ext4 read of V13 vendor; SHA-256 measured",
     WLAN_FW_INI, 1, 16744,
     "fce09190b8dd9651a85aed06eb25b8a405ed1d62b60724524395d5f6fe3eeccf"},
    {"wigig.ini", "V12+V13 /firmware/wigig", "60 GHz/wigig configuration; identical bytes in both images",
     "ext4 read; SHA-256 measured", WLAN_FW_INI, 1, 6650,
     "7a416a31c72929bc43cb31b52ae0dea9782ced339f5d23ff4bd04c3c4b430222"},
    {"qca_cld3_wlan.ko", "V12 vendor.img /lib/modules", "vendor qcacld driver: SNOC/CE, BMI, athwlan.bin; the "
     "file has ext4 holes, size/hash are the logical file", "ext4 logical read; strings also used as transport "
     "evidence", WLAN_FW_KO, 1, 9998584,
     "c6fd999fa602cf68f06ec1e7b6914641d43380e2fd2d2147f701f44129b8e680"},
    {"qca_cld3_wlan.ko", "V13 super.raw /lib/modules", "newer vendor qcacld driver; same SNOC/BMI strings; the "
     "file has ext4 holes, size/hash are the logical file", "ext4 logical read; strings also used as transport "
     "evidence", WLAN_FW_KO, 1, 10426184,
     "31d1f1afd43c63eb3ec1308c01da0d32aa86bb2eccd8cfbf038675477190580f"},
    {"cnss-daemon", "V13 super.raw /bin", "userspace CNSS helper: board-data names and MSA-ready wait",
     "ext4 read; SHA-256 measured", WLAN_FW_OPAQUE, 1, 95152,
     "efce32d0054ed8574aadf024170d4618ba27362fbe1f88440ae7dda384630bab"},
};
const unsigned wlan_firmware_count = (unsigned)(sizeof(wlan_firmware_inventory) / sizeof(wlan_firmware_inventory[0]));

// ---------------------------------------------------------------------------------------------------------------
// Small endian helpers. The MBN is ELF32 little-endian on this target.
// ---------------------------------------------------------------------------------------------------------------
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t rol32(uint32_t v, unsigned n) { return (v << n) | (v >> (32 - n)); }

// ---------------------------------------------------------------------------------------------------------------
// SHA-1 (FIPS 180-1). Used only to check the PIL hash table; no keyed use.
// ---------------------------------------------------------------------------------------------------------------
static void sha1_block(uint32_t h[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (unsigned i = 0; i < 16; ++i) w[i] = ((uint32_t)block[4 * i] << 24) | ((uint32_t)block[4 * i + 1] << 16) |
                                             ((uint32_t)block[4 * i + 2] << 8) | block[4 * i + 3];
    for (unsigned i = 16; i < 80; ++i) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (unsigned i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5a827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcu; }
        else { f = b ^ c ^ d; k = 0xca62c1d6u; }
        const uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}
void wlan_sha1(const uint8_t *data, size_t size, uint8_t out[WLAN_MBN_HASH_LEN]) {
    uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    uint8_t block[64];
    const uint64_t bits = (uint64_t)size * 8;
    size_t off = 0;
    while (size - off >= 64) { sha1_block(h, data + off); off += 64; }
    const size_t rem = size - off;
    memset(block, 0, sizeof block);
    if (data && rem) memcpy(block, data + off, rem);
    block[rem] = 0x80;
    if (rem >= 56) { sha1_block(h, block); memset(block, 0, sizeof block); }
    for (unsigned i = 0; i < 8; ++i) block[63 - i] = (uint8_t)(bits >> (8 * i));
    sha1_block(h, block);
    for (unsigned i = 0; i < 5; ++i) wr32be(out + 4 * i, h[i]);
}

// ---------------------------------------------------------------------------------------------------------------
// ELF32/MBN.
// ---------------------------------------------------------------------------------------------------------------
static int mbn_read_header(struct wlan_mbn *out, const uint8_t *data, size_t size) {
    if (size < 52) return WLAN_ERR_INVAL;
    if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') return WLAN_ERR_INVAL;
    // ELFCLASS32, ELFDATA2LSB, EV_CURRENT. The PIL images on this target are 32-bit little-endian.
    if (data[4] != 1 || data[5] != 1 || data[6] != 1) return WLAN_ERR_INVAL;
    if (rd16(data + 16) != 2) return WLAN_ERR_INVAL;   // ET_EXEC
    const uint16_t machine = rd16(data + 18);
    if (machine != 40 && machine != 164) return WLAN_ERR_INVAL;   // EM_ARM, EM_HEXAGON
    const uint32_t phoff = rd32(data + 28);
    const uint16_t ehsize = rd16(data + 40), phentsize = rd16(data + 42), phnum = rd16(data + 44);
    if (ehsize < 52 || phentsize != 32 || phnum < 1 || phnum > WLAN_MBN_MAX_SEGMENTS) return WLAN_ERR_INVAL;
    if (phoff < 52 || (uint64_t)phoff + (uint64_t)phnum * 32u > size) return WLAN_ERR_INVAL;
    if (size > WLAN_MBN_MAX_SIZE) return WLAN_ERR_INVAL;

    out->machine = machine;
    out->phnum = phnum;
    out->segments = phnum;
    for (unsigned i = 0; i < phnum; ++i) {
        const uint8_t *p = data + phoff + 32u * i;
        struct wlan_mbn_segment *s = &out->seg[i];
        s->type = rd32(p + 0); s->offset = rd32(p + 4); s->vaddr = rd32(p + 8); s->paddr = rd32(p + 12);
        s->filesz = rd32(p + 16); s->memsz = rd32(p + 20); s->flags = rd32(p + 24); s->align = rd32(p + 28);
        if ((uint64_t)s->offset + s->filesz > 0xffffffffULL) return WLAN_ERR_INVAL;
    }
    // Hash segment convention: program header 0 is PT_LOAD with no file bytes and a non-zero memory size.
    // The hash table is stored immediately after the program header array.
    if (out->seg[0].type == 1 && out->seg[0].filesz == 0 && out->seg[0].memsz > 0) {
        const uint64_t at = (uint64_t)phoff + (uint64_t)phnum * 32u;
        if (at + out->seg[0].memsz > size) return WLAN_ERR_INVAL;
        if (out->seg[0].memsz % WLAN_MBN_HASH_ENTRY) return WLAN_ERR_INVAL;
        out->hash_index = 0;
        out->hash_offset = (uint32_t)at;
        out->hash_bytes = out->seg[0].memsz;
        out->hash_count = out->hash_bytes / WLAN_MBN_HASH_ENTRY;
        // One entry per non-hash segment. Some images append a self-hash entry for the table itself, which the
        // host cannot recompute (it is not the SHA-1 of a file segment); accept either count and verify only the
        // per-segment entries.
        if (out->hash_count != (unsigned)phnum - 1 && out->hash_count != (unsigned)phnum)
            return WLAN_ERR_INVAL;
    }
    for (unsigned i = 0; i < phnum; ++i) {
        const struct wlan_mbn_segment *s = &out->seg[i];
        if (i != out->hash_index) out->payload_size += s->filesz;
        const uint64_t end = (uint64_t)s->offset + s->filesz;
        if (end > out->image_extent) out->image_extent = end;
    }
    return 0;
}

int wlan_mbn_parse_header(struct wlan_mbn *out, const uint8_t *data, size_t size) {
    if (!out || !data) return WLAN_ERR_INVAL;
    memset(out, 0, sizeof *out);
    out->data = data;
    out->size = size;
    out->hash_index = WLAN_MBN_NO_HASH;
    return mbn_read_header(out, data, size);
}

int wlan_mbn_parse(struct wlan_mbn *out, const uint8_t *data, size_t size) {
    int rc = wlan_mbn_parse_header(out, data, size);
    if (rc) return rc;
    for (unsigned i = 0; i < out->segments; ++i) {
        const struct wlan_mbn_segment *s = &out->seg[i];
        if ((uint64_t)s->offset + s->filesz > size) return WLAN_ERR_INVAL;
    }
    // Payload ranges must not overlap. The hash segment has no file bytes, so it cannot collide.
    for (unsigned i = 0; i < out->segments; ++i) {
        if (i == out->hash_index || !out->seg[i].filesz) continue;
        for (unsigned j = i + 1; j < out->segments; ++j) {
            if (j == out->hash_index || !out->seg[j].filesz) continue;
            const uint64_t ai = out->seg[i].offset, ae = ai + out->seg[i].filesz;
            const uint64_t bi = out->seg[j].offset, be = bi + out->seg[j].filesz;
            if (ai < be && bi < ae) return WLAN_ERR_INVAL;
        }
    }
    // In a signed image the payload starts after the hash table. Segment 0 may be the header block itself.
    if (out->hash_index != WLAN_MBN_NO_HASH) {
        const uint64_t hash_end = (uint64_t)out->hash_offset + out->hash_bytes;
        for (unsigned i = 1; i < out->segments; ++i)
            if (out->seg[i].filesz && out->seg[i].offset < hash_end) return WLAN_ERR_INVAL;
    }
    return 0;
}

int wlan_mbn_verify_hashes(const struct wlan_mbn *mbn, const uint8_t *data, size_t size) {
    if (!mbn || !data) return WLAN_ERR_INVAL;
    if (mbn->hash_index == WLAN_MBN_NO_HASH) return 0;   // unsigned image: nothing to verify
    if (mbn->hash_index != 0) return WLAN_ERR_INVAL;
    if ((uint64_t)mbn->hash_offset + mbn->hash_bytes > size) return WLAN_ERR_INVAL;
    if (mbn->hash_count < mbn->segments - 1) return WLAN_ERR_INVAL;
    for (unsigned k = 0; k + 1 < mbn->segments; ++k) {
        const uint8_t *entry = data + mbn->hash_offset + (size_t)k * WLAN_MBN_HASH_ENTRY;
        const uint32_t len = rd32(entry);
        const struct wlan_mbn_segment *s = &mbn->seg[k + 1];
        if (len != s->filesz) return WLAN_ERR_INVAL;
        if ((uint64_t)s->offset + s->filesz > size) return WLAN_ERR_INVAL;
        uint8_t digest[WLAN_MBN_HASH_LEN];
        wlan_sha1(data + s->offset, s->filesz, digest);
        if (memcmp(digest, entry + 4, WLAN_MBN_HASH_LEN)) return WLAN_ERR_IO;
    }
    return 0;
}

int wlan_mbn_parse_split(struct wlan_mbn *out, const uint8_t *mdt, size_t mdt_size,
                         const uint8_t *const *parts, const size_t *part_sizes, unsigned nparts) {
    int rc = wlan_mbn_parse_header(out, mdt, mdt_size);
    if (rc) return rc;
    if (!parts || !part_sizes || nparts < out->segments) return WLAN_ERR_INVAL;
    for (unsigned i = 0; i < out->segments; ++i) {
        const struct wlan_mbn_segment *s = &out->seg[i];
        if (part_sizes[i] < s->filesz) return WLAN_ERR_INVAL;
        if (s->filesz && !parts[i]) return WLAN_ERR_INVAL;
        // Some vendor .mdt files still contain the first segments (ipa_fws.mdt is b00+b01). When the bytes are
        // present in the mdt, they must agree with the split part.
        if (s->filesz && (uint64_t)s->offset + s->filesz <= mdt_size &&
            memcmp(mdt + s->offset, parts[i], s->filesz)) return WLAN_ERR_INVAL;
    }
    return 0;
}

int wlan_mbn_verify_hashes_split(const struct wlan_mbn *mbn, const uint8_t *const *parts,
                                 const size_t *part_sizes, unsigned nparts) {
    if (!mbn || !parts || !part_sizes) return WLAN_ERR_INVAL;
    if (mbn->hash_index == WLAN_MBN_NO_HASH) return 0;
    if (mbn->hash_index != 0 || nparts < mbn->segments) return WLAN_ERR_INVAL;
    if (mbn->hash_count < mbn->segments - 1) return WLAN_ERR_INVAL;
    for (unsigned k = 0; k + 1 < mbn->segments; ++k) {
        const uint8_t *entry = mbn->data + mbn->hash_offset + (size_t)k * WLAN_MBN_HASH_ENTRY;
        const uint32_t len = rd32(entry);
        const struct wlan_mbn_segment *s = &mbn->seg[k + 1];
        if (len != s->filesz || part_sizes[k + 1] < s->filesz || (s->filesz && !parts[k + 1]))
            return WLAN_ERR_INVAL;
        uint8_t digest[WLAN_MBN_HASH_LEN];
        wlan_sha1(s->filesz ? parts[k + 1] : NULL, s->filesz, digest);
        if (memcmp(digest, entry + 4, WLAN_MBN_HASH_LEN)) return WLAN_ERR_IO;
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Board data container and names.
// ---------------------------------------------------------------------------------------------------------------
int wlan_bdf_parse(struct wlan_bdf *out, const uint8_t *data, size_t size) {
    if (!out || !data) return WLAN_ERR_INVAL;
    memset(out, 0, sizeof *out);
    if (!size || size > WLAN_BDF_MAX_SIZE) return WLAN_ERR_INVAL;
    out->data = data;
    out->size = size;
    if (size >= 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        const int rc = wlan_mbn_parse(&out->elf, data, size);
        if (rc) return rc;
        out->kind = WLAN_BDF_ELF;
        return 0;
    }
    // The internal BDF layout is NDA; validate only the outer blob: non-empty, word-oriented and bounded.
    if (size < 4 || (size & 3u)) return WLAN_ERR_INVAL;
    out->kind = WLAN_BDF_RAW;
    return 0;
}

static int put_str(char *out, size_t capacity, size_t *at, const char *s) {
    while (*s) {
        if (*at + 1 >= capacity) return WLAN_ERR_INVAL;
        out[(*at)++] = *s++;
    }
    return 0;
}
static int put_hex(char *out, size_t capacity, size_t *at, unsigned value, unsigned digits) {
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = digits; i > 0; --i) {
        if (*at + 1 >= capacity) return WLAN_ERR_INVAL;
        out[(*at)++] = hex[(value >> (4 * (i - 1))) & 0xf];
    }
    return 0;
}
int wlan_bdf_name(char *out, size_t capacity, unsigned chip_id, unsigned board_id, unsigned format) {
    if (!out || !capacity) return WLAN_ERR_INVAL;
    size_t at = 0;
    int rc = 0;
    if (format == WLAN_BDF_NAME_DEFAULT) {
        rc = put_str(out, capacity, &at, "bdwlan.bin");
    } else if (format == WLAN_BDF_NAME_BOARD) {
        if (board_id > 0xff) return WLAN_ERR_INVAL;
        rc = put_str(out, capacity, &at, "bdwlan.b");
        if (!rc) rc = put_hex(out, capacity, &at, board_id, 2);
    } else if (format == WLAN_BDF_NAME_CHIP) {
        if (chip_id > 0xfff) return WLAN_ERR_INVAL;
        rc = put_str(out, capacity, &at, "bdwlan.");
        if (!rc) rc = put_hex(out, capacity, &at, chip_id, 3);
    } else if (format == WLAN_BDF_NAME_CHIP_BOARD) {
        if (chip_id > 0xfff || board_id > 0xff) return WLAN_ERR_INVAL;
        rc = put_str(out, capacity, &at, "bdwlan.");
        if (!rc) rc = put_hex(out, capacity, &at, chip_id, 3);
        if (!rc) rc = put_str(out, capacity, &at, ".b");
        if (!rc) rc = put_hex(out, capacity, &at, board_id, 2);
    } else {
        return WLAN_ERR_INVAL;
    }
    if (rc) { out[0] = 0; return rc; }
    out[at] = 0;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// WLFW/QMI handshake plan.
// ---------------------------------------------------------------------------------------------------------------
static int hs_fail(struct wlan_handshake_state *st, unsigned step, int rc) {
    st->step = step;
    st->error = rc ? rc : WLAN_ERR_IO;
    return st->error;
}
static int hs_request(const struct wlan_handshake_ops *ops, struct wlan_handshake_state *st, unsigned step,
                      unsigned request, uint64_t a0, uint64_t a1) {
    st->step = step;
    if (!ops->request) return hs_fail(st, step, WLAN_ERR_NOTSUP);
    const int rc = ops->request(ops->ctx, request, a0, a1);
    if (rc) return hs_fail(st, step, rc);
    ++st->requests;
    return 0;
}
static int hs_wait(const struct wlan_handshake_ops *ops, struct wlan_handshake_state *st, unsigned step,
                   unsigned indication, unsigned timeout_ms) {
    st->step = step;
    if (!ops->wait_indication) return hs_fail(st, step, WLAN_ERR_NOTSUP);
    const int rc = ops->wait_indication(ops->ctx, indication, timeout_ms);
    if (rc) return hs_fail(st, step, rc);
    return 0;
}

int wlan_handshake_run(const struct wlan_handshake_ops *ops, const struct wlan_mbn *image,
                       uint64_t msa_base, uint64_t msa_size, unsigned timeout_ms,
                       struct wlan_handshake_state *state) {
    if (!ops || !state) return WLAN_ERR_INVAL;
    memset(state, 0, sizeof *state);
    if (!image) return hs_fail(state, WLAN_HS_LOAD, WLAN_ERR_INVAL);
    state->step = WLAN_HS_LOAD;
    if (!ops->load_image) return hs_fail(state, WLAN_HS_LOAD, WLAN_ERR_NOTSUP);
    int rc = ops->load_image(ops->ctx, image);
    if (rc) return hs_fail(state, WLAN_HS_LOAD, rc);

    if ((rc = hs_request(ops, state, WLAN_HS_CAPABILITY, WLAN_WLFW_CAPABILITY, 0, 0))) return rc;
    if ((rc = hs_request(ops, state, WLAN_HS_CONFIG, WLAN_WLFW_CONFIG, 0, 0))) return rc;
    if ((rc = hs_request(ops, state, WLAN_HS_IND_REGISTER, WLAN_WLFW_IND_REGISTER, 0, 0))) return rc;
    if ((rc = hs_request(ops, state, WLAN_HS_INI, WLAN_WLFW_INI, 0, 0))) return rc;
    if ((rc = hs_wait(ops, state, WLAN_HS_FW_READY, WLAN_WLFW_IND_FW_READY, timeout_ms))) return rc;

    // The MSA region must be page aligned and non-empty; the stock kernel logs the translated PA and size before
    // sending these two requests ("MSA pa: ... MSA Memory Size: 0x%x").
    if (!msa_size || (msa_base & 0xfffu) || (msa_size & 0xfffu))
        return hs_fail(state, WLAN_HS_MSA_INFO, WLAN_ERR_INVAL);
    if ((rc = hs_request(ops, state, WLAN_HS_MSA_INFO, WLAN_WLFW_MSA_MEM_INFO, msa_base, msa_size))) return rc;
    if ((rc = hs_wait(ops, state, WLAN_HS_MSA_READY, WLAN_WLFW_IND_MSA_READY, timeout_ms))) return rc;
    if ((rc = hs_request(ops, state, WLAN_HS_MODE_ON, WLAN_WLFW_MODE, 1, 0))) return rc;

    state->step = WLAN_HS_DONE;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Staged probe orchestration.
// ---------------------------------------------------------------------------------------------------------------
int wlan_probe_run(const struct wlan_probe_ops *ops, unsigned stage_req, struct wlan_probe_state *state) {
    if (!ops || !state) return WLAN_ERR_INVAL;
    memset(state, 0, sizeof *state);
    state->stage_req = stage_req;
    if (stage_req < 1 || stage_req > WLAN_PROBE_STAGE_MAX) {
        state->error = WLAN_ERR_INVAL;
        return state->error;
    }
    int (*const stages[WLAN_PROBE_STAGE_MAX])(void *) = {
        ops->stage1, ops->stage2, ops->stage3, ops->stage4, ops->stage5,
    };
    for (unsigned i = 0; i < stage_req; ++i) {
        state->stage = i + 1;
        state->step = i + 1;
        if (!stages[i]) { state->error = WLAN_ERR_INVAL; return state->error; }
        const int rc = stages[i](ops->ctx);
        if (rc) { state->error = rc; return rc; }
    }
    return 0;
}
