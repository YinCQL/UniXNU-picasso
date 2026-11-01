// SPDX-License-Identifier: Apache-2.0
// See NvtProtocol.h and NOTICE.
#include "NvtProtocol.h"
#include <string.h>

// NT36672C/NT36675 memory map (sm7150-mainline nt36xxx_mem_map.h; the NT36672C trim entry selects NT36675_IC).
static const struct nvt_map map_nt36675 = {
    .name = "NT36672C/NT36675",
    .event_buf = 0x22D00,
    .boot_rdy = 0x3F10D,
    .spi_rd_fast = 0x3F310,
    .ilm_des = 0x3F128,
    .ilm_length = 0x3F118,
    .dlm_des = 0x3F12C,
    .dlm_length = 0x3F130,
    .g_ilm_checksum = 0x3F100,
    .g_dlm_checksum = 0x3F104,
    .r_ilm_checksum = 0x3F120,
    .r_dlm_checksum = 0x3F124,
    .bld_ilm_dlm_crc = 0x3F133,
    .bld_crc_en = 0x3F30E,
    .dma_crc_flag = 0x3F134,
    .eng_rst = 0x7FFF80,
    .chip_info = 0x1F64E,
    .swrst_sif = 0x3F0FE,
};

// Trim table entry from sm7150-mainline commit e1795b0. Bytes 1 and 2 are masked out; the six id bytes are at
// chip_info + 1 .. + 6. hw_crc = 2 is annotated "3Bytes" in that driver (TODO: confirm the exact CRC bank width).
struct nvt_trim_id {
    uint8_t id[6], mask[6];
    const struct nvt_map *map;
    unsigned hw_crc;
};
static const struct nvt_trim_id trim_ids[] = {
    {{0x0c, 0xff, 0xff, 0x72, 0x66, 0x03}, {1, 0, 0, 1, 1, 1}, &map_nt36675, 2},
};

static void delay(struct nvt_chip *c, unsigned ms) {
    if (c->bus && c->bus->delay_ms) c->bus->delay_ms(c->bus->ctx, ms);
}
static int bus_xfer(struct nvt_chip *c, size_t len, uint8_t *rx) {
    if (!c->bus || !c->bus->xfer || !c->scratch) return NVT_ERR_PARAM;
    if (!len || len > NVT_TRANSFER_LEN + 1) return NVT_ERR_PARAM;
    return c->bus->xfer(c->bus->ctx, c->scratch, rx, len) ? NVT_ERR_BUS : 0;
}

// Page select: 0xff, bits 22:15, bits 14:7. The low 7 bits follow as the first byte of the data transfer, with
// bit 7 set for a write (NT36xxx_spi.c; the third page byte never carries the write bit).
int nvt_set_page(struct nvt_chip *c, uint32_t addr) {
    c->scratch[0] = 0xff;
    c->scratch[1] = (uint8_t)((addr >> 15) & 0xff);
    c->scratch[2] = (uint8_t)((addr >> 7) & 0xff);
    return bus_xfer(c, 3, NULL);
}
int nvt_write(struct nvt_chip *c, uint32_t addr, const uint8_t *data, size_t len) {
    if (!len || len > NVT_TRANSFER_LEN || !data) return NVT_ERR_PARAM;
    const int r = nvt_set_page(c, addr);
    if (r) return r;
    c->scratch[0] = (uint8_t)((addr & 0x7f) | 0x80);
    memcpy(c->scratch + 1, data, len);
    return bus_xfer(c, len + 1, NULL);
}
int nvt_write_byte(struct nvt_chip *c, uint32_t addr, uint8_t value) { return nvt_write(c, addr, &value, 1); }

// Read: page select, then the low address byte followed by the data. The chip answers with two lead-in bytes, so
// the first data byte is the third one received (vendor spi_read_write: transfer length len + DUMMY_BYTES with
// DUMMY_BYTES = 1, then memcpy(buf + 1, rbuf + 2, len - 1)). Reading from rbuf + 1 instead shifted every value
// by one byte: the firmware-info block only looked right after adding an offset of one to every field.
int nvt_read(struct nvt_chip *c, uint32_t addr, uint8_t *out, size_t len) {
    if (!len || len > NVT_READ_MAX || !out || !c->rxbuf) return NVT_ERR_PARAM;
    const int r = nvt_set_page(c, addr);
    if (r) return r;
    memset(c->scratch, 0, len + 2);
    c->scratch[0] = (uint8_t)(addr & 0x7f);
    const int x = bus_xfer(c, len + 2, c->rxbuf);
    if (x) return x;
    memcpy(out, c->rxbuf + 2, len);
    return 0;
}

uint32_t nvt_crc32_words(const uint8_t *data, size_t len, size_t avail) {
    uint32_t checksum = 0xffffffffu;
    const size_t words = (len + 4) / 4;
    for (size_t i = 0; i < words; ++i) {
        uint32_t w = 0;
        for (unsigned b = 0; b < 4; ++b) {
            const size_t at = i * 4 + b;
            if (at < avail) w |= (uint32_t)data[at] << (8 * b);
        }
        checksum ^= w;
        for (unsigned j = 0; j < 32; ++j)
            checksum = (checksum & 0x80000000u) ? (checksum << 1) ^ 0x04c11db7u : (checksum << 1);
    }
    return checksum;
}
static int word_at(const uint8_t *d, size_t size, size_t pos, uint32_t *out) {
    if (pos > size || size - pos < 4) return NVT_ERR_FW;
    *out = (uint32_t)d[pos] | ((uint32_t)d[pos + 1] << 8) | ((uint32_t)d[pos + 2] << 16) |
           ((uint32_t)d[pos + 3] << 24);
    return 0;
}

// NT36xxx firmware header (nt36xxx_core.c nvt_bin_header_parser; the cascade variant of the NT36532E reference is
// not present in this family). The image ends at the last 4 KiB block whose last three bytes are "NVT" or "MOD";
// FW_VER and FW_VER_BAR sit at write_size - 4096 and - 4095 and must sum to 0xFF.
//
// CRC semantics: the vendor parser reads the ILM/DLM golden checksums verbatim from 0x18/0x1C when hw_crc != 0
// (nt36xxx_fw_update.c), which is the NT36672C case (hw_crc = 2). These are boot-ROM hardware checksums, not values
// this host can derive. For hw_crc == 0 the vendor instead computes CheckSum(data, size) =
// ~(sum(data[0..size]) + size) + 1, an additive checksum; that path does not apply here. crc_calc is the NT36532E
// word-CRC diagnostic and is not expected to equal part[].crc for these images.
int nvt_fw_parse(struct nvt_fw *fw, const uint8_t *d, size_t size) {
    if (!fw || !d) return NVT_ERR_PARAM;
    memset(fw, 0, sizeof(*fw));
    fw->data = d;
    fw->size = size;
    if (size < 0x40) return NVT_ERR_FW;
    uint32_t end;
    if (word_at(d, size, 0, &end) || end < 0x30 || end > size) return NVT_ERR_FW;

    for (size_t i = size / 4096; i > 0; --i) {
        const size_t at = i * 4096 - 3;
        if (!memcmp(d + at, "NVT", 3) || !memcmp(d + at, "MOD", 3)) { fw->write_size = i * 4096; break; }
    }
    if (!fw->write_size) return NVT_ERR_FW;
    fw->fw_ver = d[fw->write_size - 4096];
    fw->fw_ver_bar = d[fw->write_size - 4095];
    if ((uint8_t)(fw->fw_ver + fw->fw_ver_bar) != 0xff) return NVT_ERR_FW;

    unsigned info_sec_num = 0;
    for (uint32_t pos = 0x30; pos < end; pos += 0x10) ++info_sec_num;
    const unsigned ovly_info = (d[0x28] & 0x10) >> 4;
    const unsigned ovly_sec_num = ovly_info ? (d[0x28] & 0x0f) : 0;
    const unsigned partition = 2 + ovly_sec_num + info_sec_num;
    if (partition > NVT_MAX_PARTITIONS) return NVT_ERR_FW;
    fw->count = partition;

    for (unsigned list = 0; list < partition; ++list) {
        struct nvt_fw_partition *p = &fw->part[list];
        if (list < 2) {
            if (word_at(d, size, list * 12, &p->bin_addr) || word_at(d, size, 4 + list * 12, &p->sram_addr) ||
                word_at(d, size, 8 + list * 12, &p->size)) return NVT_ERR_FW;
            if (word_at(d, size, 0x18 + list * 4, &p->crc)) return NVT_ERR_FW;
            memcpy(p->name, list == 0 ? "ILM" : "DLM", 4);
        } else if (list < 2 + info_sec_num) {
            const size_t pos = 0x30 + 0x10 * (list - 2);
            if (word_at(d, size, pos, &p->sram_addr) || word_at(d, size, pos + 4, &p->size) ||
                word_at(d, size, pos + 8, &p->bin_addr) || word_at(d, size, pos + 12, &p->crc)) return NVT_ERR_FW;
            if (p->bin_addr == 0 && p->size != 0) memcpy(p->name, "Header", 7);
            else {
                memcpy(p->name, "Info-", 6);
                p->name[5] = (char)('0' + ((list - 2) % 10));
                p->name[6] = 0;
            }
        } else {
            const size_t pos = (size_t)fw->part[1].bin_addr + 0x10 * (list - 2 - info_sec_num);
            if (word_at(d, size, pos, &p->sram_addr) || word_at(d, size, pos + 4, &p->size) ||
                word_at(d, size, pos + 8, &p->bin_addr) || word_at(d, size, pos + 12, &p->crc)) return NVT_ERR_FW;
            memcpy(p->name, "Overlay-", 9);
            p->name[8] = (char)('0' + ((list - 2 - info_sec_num) % 10));
            p->name[9] = 0;
        }
        if ((uint64_t)p->bin_addr + p->size > size) return NVT_ERR_FW;
        p->crc_calc = p->size ? nvt_crc32_words(d + p->bin_addr, p->size, size - p->bin_addr) : 0;
    }
    return 0;
}

// picasso's stock-kernel firmware objects are 0x20000-byte images plus a 0x2000-byte trailer (see NvtProtocol.h).
// The trailer is not needed by the controller, so accept the bare image, the true object, or a 0x22000 carve that
// starts at the previous object's trailer and remember how much was skipped.
int nvt_fw_parse_stock(struct nvt_fw *fw, const uint8_t *d, size_t size) {
    if (!fw || !d) return NVT_ERR_PARAM;
    if (size < 0x40) return NVT_ERR_FW;

    if (!nvt_fw_parse(fw, d, size)) return 0;

    // Marker-anchored carve: leading 0x2000 is the previous object's trailer, image follows.
    if (size > NVT_STOCK_TRAILER_SIZE) {
        const size_t off = NVT_STOCK_TRAILER_SIZE;
        if (!nvt_fw_parse(fw, d + off, size - off)) {
            fw->source_offset = off;
            return 0;
        }
    }

    // Fallback for a differently sized leading block. The bare header starts with the ILM descriptor: u32[0] is the
    // header end/bin address (0x30..0x2000 here) and the ILM and DLM sizes at +8/+20 are nonzero. That keeps the full
    // parse below to a handful of attempts, and nvt_fw_parse() still validates the marker, version and every partition.
    const size_t scan = size < NVT_IMAGE_SCAN_MAX ? size : NVT_IMAGE_SCAN_MAX;
    for (size_t off = 4; off + 0x40 <= scan; off += 4) {
        uint32_t end, ilm_size, dlm_size;
        if (word_at(d, size, off, &end) || end < 0x30 || end > size - off) continue;
        if (word_at(d, size, off + 8, &ilm_size) || !ilm_size) continue;
        if (word_at(d, size, off + 20, &dlm_size) || !dlm_size) continue;
        if (!nvt_fw_parse(fw, d + off, size - off)) {
            fw->source_offset = off;
            return 0;
        }
    }
    return NVT_ERR_FW;
}

static int bootloader_reset(struct nvt_chip *c) {
    // NT36xxx bootloader reset: 0x69 at SWRST_N8, ~35 ms, then clear SPI fast-read (nt36xxx_core.c).
    int r = nvt_write_byte(c, c->map->swrst_sif, 0x69);
    delay(c, 35);
    if (r) return r;
    r = nvt_write_byte(c, c->map->spi_rd_fast, 0);
    delay(c, 1);
    return r;
}

int nvt_identify(struct nvt_chip *c) {
    if (!c) return NVT_ERR_PARAM;
    c->map = &map_nt36675;
    c->hw_crc = 0;
    memset(c->id, 0, sizeof(c->id));
    // Engine reset, then up to five attempts of bootloader reset + 7-byte read at 0x1F64E. The six id bytes are at
    // offset 1..6 of that read (nt36xxx_chip_version_init compares buf[i + 1]).
    int r = nvt_write_byte(c, c->map->eng_rst, 0x5a);
    delay(c, 15);
    delay(c, 30);
    if (r) return r;
    int matched = 0;
    for (unsigned retry = 0; retry < 5 && !matched; ++retry) {
        r = bootloader_reset(c);
        if (r) return r;
        uint8_t raw[7] = {0};
        r = nvt_read(c, c->map->chip_info, raw, sizeof(raw));
        if (r) return r;
        for (unsigned t = 0; t < sizeof(trim_ids) / sizeof(trim_ids[0]) && !matched; ++t) {
            int ok = 1;
            for (unsigned i = 0; i < 6 && ok; ++i)
                if (trim_ids[t].mask[i] && raw[i] != trim_ids[t].id[i]) ok = 0;
            if (!ok) continue;
            memcpy(c->id, raw, sizeof(c->id));
            c->map = trim_ids[t].map;
            c->hw_crc = trim_ids[t].hw_crc;
            matched = 1;
        }
        if (!matched) delay(c, 10);
    }
    return matched ? 0 : NVT_ERR_CHIP;
}

static int write_firmware(struct nvt_chip *c, const struct nvt_fw *fw) {
    for (unsigned i = 0; i < fw->count; ++i) {
        const struct nvt_fw_partition *p = &fw->part[i];
        if (!p->size) continue;
        // The vendor writes size + 1 bytes per partition; the extra byte stays inside the image.
        uint32_t len = p->size + 1;
        if ((uint64_t)p->bin_addr + len > fw->size) len = p->size;
        uint32_t sram = p->sram_addr, bin = p->bin_addr;
        while (len) {
            const uint32_t n = len > NVT_TRANSFER_LEN ? NVT_TRANSFER_LEN : len;
            const int r = nvt_write(c, sram, fw->data + bin, n);
            if (r) {
                c->dl_part = i;
                c->dl_sram = sram;
                c->dl_left = len;
                return r;
            }
            sram += n;
            bin += n;
            len -= n;
        }
    }
    return 0;
}
static int write_3(struct nvt_chip *c, uint32_t addr, uint32_t v) {
    const uint8_t b[3] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16)};
    return nvt_write(c, addr, b, sizeof(b));
}
static int write_4(struct nvt_chip *c, uint32_t addr, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    return nvt_write(c, addr, b, sizeof(b));
}
static int set_bld_crc(struct nvt_chip *c, const struct nvt_fw *fw) {
    const struct nvt_map *m = c->map;
    int r;
    if ((r = write_3(c, m->ilm_des, fw->part[0].sram_addr)) ||
        (r = write_3(c, m->ilm_length, fw->part[0].size)) ||
        (r = write_4(c, m->g_ilm_checksum, fw->part[0].crc))) return r;
    if ((r = write_3(c, m->dlm_des, fw->part[1].sram_addr)) ||
        (r = write_3(c, m->dlm_length, fw->part[1].size)) ||
        (r = write_4(c, m->g_dlm_checksum, fw->part[1].crc))) return r;
    return 0;
}
static int enable_bld_crc(struct nvt_chip *c) {
    uint8_t v = 0;
    int r = nvt_read(c, c->map->bld_crc_en, &v, 1);
    if (r) return r;
    return nvt_write_byte(c, c->map->bld_crc_en, (uint8_t)(v | 0x80));
}
static int fw_crc_enable(struct nvt_chip *c) {
    int r = nvt_write_byte(c, c->map->event_buf + NVT_EVENT_RESET_COMPLETE, 0);
    if (r) return r;
    return nvt_write_byte(c, c->map->event_buf + NVT_EVENT_HOST_CMD, NVT_HOST_CMD_ENABLE_FW_CRC);
}
// NT36xxx checks byte 1 of the reset-complete event (the NT36532E reference checked byte 0).
static int check_reset_state(struct nvt_chip *c) {
    const unsigned max = c->reset_retries ? c->reset_retries : 10;
    for (unsigned retry = 0; retry <= max; ++retry) {
        uint8_t b[6] = {0};
        const int r = nvt_read(c, c->map->event_buf + NVT_EVENT_RESET_COMPLETE, b, sizeof(b));
        if (r) return r;
        memcpy(c->dl_reset_state, b, sizeof(c->dl_reset_state));
        c->dl_polls = retry + 1;
        if (b[0] >= NVT_RESET_STATE_INIT && b[0] <= NVT_RESET_STATE_MAX) return 0;
        delay(c, 10);
    }
    return NVT_ERR_TIMEOUT;
}

int nvt_read_bld_crc(struct nvt_chip *c, const struct nvt_fw *fw) {
    if (!c->map || fw->count < 2) return NVT_ERR_PARAM;
    uint8_t b[4] = {0};
    int r = nvt_read(c, c->map->bld_ilm_dlm_crc, b, 1);
    if (r) return r;
    c->crc_flags = b[0];
    const uint32_t g[2] = {c->map->g_ilm_checksum, c->map->g_dlm_checksum};
    const uint32_t res[2] = {c->map->r_ilm_checksum, c->map->r_dlm_checksum};
    for (unsigned i = 0; i < 2; ++i) {
        if ((r = nvt_read(c, g[i], b, 4))) return r;
        c->crc_golden[i] = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
                           ((uint32_t)b[3] << 24);
        if ((r = nvt_read(c, res[i], b, 4))) return r;
        c->crc_result[i] = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
                           ((uint32_t)b[3] << 24);
        c->crc_bin[i] = fw->part[i].crc;
    }
    c->crc_valid = 1;
    return 0;
}

int nvt_verify_sram(struct nvt_chip *c, const struct nvt_fw *fw) {
    if (!c->map || fw->count < 2) return NVT_ERR_PARAM;
    const struct nvt_fw_partition *ilm = &fw->part[0], *dlm = &fw->part[1];
    const struct { uint32_t sram, bin; int valid; } w[3] = {
        {ilm->sram_addr, ilm->bin_addr, ilm->size >= 32},
        {ilm->sram_addr + ilm->size - 32, ilm->bin_addr + ilm->size - 32, ilm->size >= 32},
        {dlm->sram_addr, dlm->bin_addr, dlm->size >= 32},
    };
    c->sram_valid = 1;
    for (unsigned i = 0; i < 3; ++i) {
        if (!w[i].valid) { c->sram_ok[i] = 0; continue; }
        uint8_t b[32];
        const int r = nvt_read(c, w[i].sram, b, sizeof(b));
        if (r) return r;
        c->sram_ok[i] = !memcmp(b, fw->data + w[i].bin, sizeof(b));
    }
    return 0;
}

int nvt_download_firmware(struct nvt_chip *c, const struct nvt_fw *fw) {
    if (!c->map || fw->count < 2 || !fw->part[0].size || !fw->part[1].size) return NVT_ERR_PARAM;
    int last = NVT_ERR_TIMEOUT;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        int ret;
        c->dl_attempt = attempt + 1;
#define NVT_STAGE(n, expr) do { c->dl_stage = (n); ret = (expr); if (ret) { c->dl_rc = ret; goto failed; } } while (0)
        NVT_STAGE(1, bootloader_reset(c));
        NVT_STAGE(2, set_bld_crc(c, fw));
        NVT_STAGE(3, write_firmware(c, fw));
        NVT_STAGE(4, enable_bld_crc(c));
        NVT_STAGE(5, fw_crc_enable(c));
        NVT_STAGE(5, nvt_write_byte(c, c->map->boot_rdy, 1));
        delay(c, 10);
        NVT_STAGE(6, check_reset_state(c));
#undef NVT_STAGE
        c->dl_stage = 0;
        return 0;
    failed:
        last = ret;
    }
    (void)nvt_read_bld_crc(c, fw);       // evidence for the failure, as the vendor driver reports it
    (void)nvt_verify_sram(c, fw);
    return last;
}

int nvt_get_fw_info(struct nvt_chip *c, struct nvt_fw_info *info) {
    if (!c || !c->map || !info) return NVT_ERR_PARAM;
    uint8_t b[17];
    memset(info, 0, sizeof(*info));
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int r = nvt_read(c, c->map->event_buf + NVT_EVENT_FWINFO, b, sizeof(b));
        if (r) return r;
        memcpy(c->fwinfo_raw, b, sizeof(c->fwinfo_raw));
        // Same layout the vendor reads (nt36xxx.c nvt_get_fw_info: buf[1] is the first data byte because the
        // reader drops the leading address byte): version and its complement at 0/1, geometry from 2 on.
        if ((uint8_t)(b[0] + b[1]) != 0xff) {
            delay(c, 20);   // the firmware posts FWINFO a little after the reset-complete event
            continue;
        }
        info->fw_ver = b[0];
        info->x_num = b[2];
        info->y_num = b[3];
        info->abs_x_max = (uint16_t)(((uint16_t)b[4] << 8) | b[5]);
        info->abs_y_max = (uint16_t)(((uint16_t)b[6] << 8) | b[7]);
        return 0;
    }
    info->defaulted = 1;
    return NVT_ERR_FW;
}
int nvt_set_custom_cmd(struct nvt_chip *c, uint8_t cmd, uint16_t value) {
    if (!c || !c->map) return NVT_ERR_PARAM;
    delay(c, 35);
    for (int tries = 0; tries < 5; ++tries) {
        const uint8_t b[5] = {0xbf, cmd, 0x00, (uint8_t)value, (uint8_t)(value >> 8)};
        int r = nvt_write(c, c->map->event_buf + NVT_EVENT_HOST_CMD, b, sizeof(b));
        if (r) return r;
        delay(c, 20);
        uint8_t st = 0xff;
        r = nvt_read(c, c->map->event_buf + NVT_EVENT_HOST_CMD, &st, 1);
        if (r) return r;
        if (!st) return 0;
    }
    return NVT_ERR_TIMEOUT;
}
int nvt_set_doze_delay(struct nvt_chip *c, uint16_t value) {
    if (!c) return NVT_ERR_PARAM;
    const uint8_t b[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
    return nvt_write(c, NVT_DOZE_DELAY_ADDR, b, sizeof(b));
}
int nvt_start_scanning(struct nvt_chip *c, struct nvt_fw_info *info, int legacy_commands) {
    if (!c || !c->map) return NVT_ERR_PARAM;
    int r = nvt_get_fw_info(c, info);
    const int info_err = r;
    if (r == NVT_ERR_BUS) return r;
    if (legacy_commands) {
        // NT36532E reference sequence; TODO: not confirmed for NT36672C, only used on explicit request.
        if ((r = nvt_set_custom_cmd(c, 0x19, 0x0000))) return r;
        if ((r = nvt_set_doze_delay(c, 120))) return r;
        if ((r = nvt_set_custom_cmd(c, 0x01, 0x0002))) return r;
    }
    return info_err;
}

int nvt_read_frame(struct nvt_chip *c, uint8_t *frame) {
    if (!c || !c->map) return NVT_ERR_PARAM;
    return nvt_read(c, c->map->event_buf, frame, NVT_THP_FRAME_LEN);
}

// The panel's own report format (see NvtProtocol.h): 65 bytes straight from the event buffer.
int nvt_read_report(struct nvt_chip *c, uint8_t *report) {
    if (!c || !c->map || !report) return NVT_ERR_PARAM;
    return nvt_read(c, c->map->event_buf, report, NVT_REPORT_LEN);
}
// Matrix layout: row-major, 36 rows (Y channels, long axis) x 18 columns (X channels, short axis), little-endian
// int16. TODO: confirm the row/column order against a captured frame.
void nvt_frame_matrix(const uint8_t *frame, int16_t *matrix) {
    for (unsigned i = 0; i < NVT_MATRIX_NODES; ++i)
        matrix[i] = (int16_t)((uint16_t)frame[NVT_THP_MATRIX_OFFSET + i * 2] |
                              ((uint16_t)frame[NVT_THP_MATRIX_OFFSET + i * 2 + 1] << 8));
}
unsigned nvt_frame_type(const uint8_t *frame) { return frame[24]; }
unsigned nvt_frame_counter(const uint8_t *frame) { return (unsigned)frame[2] | ((unsigned)frame[3] << 8); }
int nvt_peek_valid(const uint8_t *h) {
    const uint16_t crc = (uint16_t)(h[4] | (h[5] << 8)), inv = (uint16_t)(h[12] | (h[13] << 8));
    const uint32_t mk = (uint32_t)h[8] | ((uint32_t)h[9] << 8) | ((uint32_t)h[10] << 16) | ((uint32_t)h[11] << 24);
    const uint32_t mi = (uint32_t)h[16] | ((uint32_t)h[17] << 8) | ((uint32_t)h[18] << 16) |
                        ((uint32_t)h[19] << 24);
    return inv == (uint16_t)~crc && mi == ~mk;
}
int nvt_frame_header_valid(const uint8_t *frame) { return nvt_peek_valid(frame); }
unsigned nvt_peek_counter(const uint8_t *h) { return (unsigned)h[2] | ((unsigned)h[3] << 8); }
unsigned nvt_peek_type(const uint8_t *h) { return h[24]; }
int nvt_peek_frame_header(struct nvt_chip *c, uint8_t *hdr) {
    if (!c || !c->map) return NVT_ERR_PARAM;
    return nvt_read(c, c->map->event_buf, hdr, NVT_THP_PEEK_LEN);
}
