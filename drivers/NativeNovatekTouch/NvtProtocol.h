// SPDX-License-Identifier: Apache-2.0
// NT36672C (NT36xxx family) host-download and THP frame protocol over an abstract SPI transport.
//
// Provenance. The transport framing, firmware partition table, download handshake and 32-byte header integrity
// checks are adapted from ShengNovatekTouch's NT36532E protocol (Apache-2.0; see NOTICE). The NT36xxx register
// addresses, chip-id address, event-buffer address and download sequence were taken from the public sm7150-mainline
// NT36xxx driver (drivers/input/touchscreen/nt36xxx_core.c/mem_map.h) and the trim table added by commit e1795b0
// ("nt36xxx: add nt36672c support"). No Linux code is copied. Values that could not be confirmed for NT36672C are
// marked TODO with their source.
//
// Frame layout. The NT36532E reference uses NVT_THP_HEADER_LEN = 257 and NVT_THP_PAYLOAD_LEN = 5160. NT36672C's
// classic THP frame is instead a 32-byte (0x20) header followed by the 18x36 matrix of little-endian int16 values.
// The vendor THP test protocol is not public, so the header field offsets below follow the NT36532E 32-byte
// sub-header (counter at +2, CRC/inverse at +4/+12, marker/inverse at +8/+16, type at +24). TODO: verify the
// classic header against a captured NT36672C frame before trusting contact data.
#ifndef NVT_PROTOCOL_H
#define NVT_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>

#define NVT_TRANSFER_LEN (63 * 1024)     // largest data write accepted by the vendor driver (NT36XXX_TRANSFER_LEN)
#define NVT_READ_MAX 8192                // largest single read (NVT_READ_LEN_MAX in the vendor driver)
#define NVT_MAX_PARTITIONS 64

// Classic NT36xxx THP frame (TODO: confirm from a hardware capture; see the file header). The DT also declares four
// key channels (IC_KEY_CFG_SIZE, AIN_KEY); this frame length covers only the 18x36 matrix, so a trailing key block or
// checksum, if the classic frame has one, is not consumed.
#define NVT_THP_HEADER_LEN 0x20u
#define NVT_MATRIX_COLUMNS 18u           // IC_X_CFG_SIZE (picasso.dts:24503)
#define NVT_MATRIX_ROWS 36u              // IC_Y_CFG_SIZE (picasso.dts:24504)
#define NVT_MATRIX_NODES (NVT_MATRIX_COLUMNS * NVT_MATRIX_ROWS)
#define NVT_THP_MATRIX_OFFSET NVT_THP_HEADER_LEN
#define NVT_THP_MATRIX_LEN (NVT_MATRIX_NODES * 2u)
#define NVT_THP_FRAME_LEN (NVT_THP_HEADER_LEN + NVT_THP_MATRIX_LEN)
#define NVT_THP_PEEK_LEN 32u

// picasso's stock kernel embeds seven firmware objects of 0x22000 bytes each: a bare 0x20000-byte Novatek image
// followed by a 0x2000-byte vendor trailer. kallsyms confirms this: each `_fw_<name>_bin_bin` symbol points at the
// image header (00 01 00 00 ...), and the next object (or `_fw_end`) is exactly +0x22000. The image ends with the
// "NVT"/"MOD" block at +0x20000, so the vendor write-size scan stops there and the trailer is never downloaded.
//
// A 0x22000 carve taken backward from that end marker starts 0x2000 early: its first 0x2000 bytes are the *previous*
// object's trailer, and its last 0x20000 bytes are the image. The true array starts at carve + 0x2000.
// nvt_fw_parse_stock() accepts a bare image, the true object (image + trailer), or the shifted carve.
//
// Observed trailer layout (opaque, semantics unknown, not consumed by this driver):
//   +0x000 u32 magic 0x425a6666 in all seven trailers
//   +0x004 f32 200.0 or 171.2; +0x008 f32 17.5 or 6.227; +0x00c f32 5.71875
//   +0x01c u32 10 or 5; +0x030 four u32 parameters (0x922400 or 0x922000, 0, 0x18b5020, 0x5027b90)
//   +0x040 nine-byte records; +0x100 and +0x1c0 small tables; the rest is zero
// Trailers are identical within two groups: fw01/mp01/fw02/mp02/ts_fw (group A) and fw03/mp03 (group B). The 0x2000
// block before the first object is not a trailer and is not used.
#define NVT_STOCK_TRAILER_SIZE 0x2000u
#define NVT_IMAGE_SCAN_MAX 0x8000u

#define NVT_ERR_BUS (-5)
#define NVT_ERR_PARAM (-22)
#define NVT_ERR_FW (-8)
#define NVT_ERR_CHIP (-19)
#define NVT_ERR_TIMEOUT (-110)

// Transport: full-duplex SPI mode 0, one transfer, chip select held for the whole transfer.
struct nvt_bus {
    void *ctx;
    int (*xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*delay_ms)(void *ctx, unsigned ms);
};

// NT36672C/NT36675 register bank. Sources: sm7150-mainline nt36xxx_mem_map.h and nt36xxx.h. The address layout is
// shared with NT36675, which is why the trim entry for NT36672C selects the NT36675 map.
struct nvt_map {
    const char *name;
    uint32_t event_buf;                  // MMAP_EVENT_BUF_ADDR, 0x22D00 for NT36675/NT36672C
    uint32_t boot_rdy;                   // MMAP_BOOT_RDY_ADDR
    uint32_t spi_rd_fast;                // MMAP_SPI_RD_FAST_ADDR (DT: novatek,spi-rd-fast-addr)
    uint32_t ilm_des, ilm_length, dlm_des, dlm_length;
    uint32_t g_ilm_checksum, g_dlm_checksum, r_ilm_checksum, r_dlm_checksum;
    uint32_t bld_ilm_dlm_crc, bld_crc_en, dma_crc_flag;
    uint32_t eng_rst;                    // MMAP_ENG_RST_ADDR
    uint32_t chip_info;                  // MMAP_MAGIC_NUMBER_0X1F64E_ADDR (trim id lives at +1)
    uint32_t swrst_sif;                  // MMAP_SWRST_N8_ADDR (DT: novatek,swrst-n8-addr)
};

struct nvt_chip {
    const struct nvt_bus *bus;
    uint8_t *scratch;                    // caller supplied, at least NVT_TRANSFER_LEN + 8 bytes
    uint8_t *rxbuf;                      // caller supplied, at least NVT_READ_MAX + 2 bytes
    const struct nvt_map *map;
    uint8_t id[6];
    unsigned hw_crc;                     // 2 for NT36672C: the boot ROM checks a 3-byte CRC bank (TODO semantics)
    unsigned reset_retries;              // reset-state polls before giving up (0 = the vendor's 10); one every 10 ms
    // Download diagnostics, one stage per line of the boot log.
    unsigned dl_attempt, dl_stage;       // 1 reset, 2 CRC setup, 3 partition write, 4 CRC enable, 5 boot ready, 6 state
    unsigned dl_part;                    // partition and address of the failed write (stage 3)
    uint32_t dl_sram, dl_left;
    int dl_rc;
    uint8_t dl_reset_state[6];
    unsigned dl_polls;
    uint8_t crc_flags;                   // bit2 crc_done, bit0 ilm ok, bit1 dlm ok (vendor layout)
    uint32_t crc_bin[2], crc_golden[2], crc_result[2];
    int crc_valid;
    int sram_valid;                      // the small read-back windows matched the image
    uint8_t sram_ok[3];                  // 0 ILM start, 1 ILM end, 2 DLM start
    uint8_t fwinfo_raw[17];              // last FWINFO event block read, for the failure line
};

struct nvt_fw_partition {
    char name[12];
    uint32_t bin_addr, sram_addr, size;
    uint32_t crc;                        // golden checksum the boot ROM compares: ILM/DLM at 0x18/0x1C, sections at +12
    uint32_t crc_calc;                   // NT36532E word-CRC diagnostic; not the hw_crc golden value (see nvt_fw_parse)
};
struct nvt_fw {
    const uint8_t *data;
    size_t size;
    unsigned count;
    size_t write_size;                   // end of the last 4 KiB block marked "NVT" or "MOD"
    uint8_t fw_ver, fw_ver_bar;          // at write_size - 4096; their sum must be 0xFF
    size_t source_offset;                // bytes skipped from the supplied buffer by nvt_fw_parse_stock()
    struct nvt_fw_partition part[NVT_MAX_PARTITIONS];
};

struct nvt_fw_info {
    uint8_t fw_ver;
    uint8_t x_num, y_num;
    uint16_t abs_x_max, abs_y_max;
    int defaulted;                       // info block was inconsistent after retries
};

// Host-side word-wise CRC used for ILM/DLM (polynomial 0x04c11db7, initial 0xffffffff, no final xor, over
// (len+4)/4 little-endian 32-bit words). Bytes beyond avail read as 0.
uint32_t nvt_crc32_words(const uint8_t *data, size_t len, size_t avail);

int nvt_set_page(struct nvt_chip *c, uint32_t addr);
int nvt_write(struct nvt_chip *c, uint32_t addr, const uint8_t *data, size_t len);   // sets the page first
int nvt_write_byte(struct nvt_chip *c, uint32_t addr, uint8_t value);
int nvt_read(struct nvt_chip *c, uint32_t addr, uint8_t *out, size_t len);

// Parse the vendor image: header size at +0, ILM/DLM descriptors at 0/12, CRCs at 0x18/0x1C, info sections at
// 0x30, overlays after the DLM. Checks the "NVT"/"MOD" 4 KiB marker and FW_VER + FW_VER_BAR == 0xFF.
int nvt_fw_parse(struct nvt_fw *fw, const uint8_t *data, size_t size);

// Parse either a bare vendor image or one of picasso's stock-kernel firmware objects. Tries the buffer directly,
// then at +NVT_STOCK_TRAILER_SIZE (the marker-anchored carve case), then scans the first NVT_IMAGE_SCAN_MAX bytes for
// an offset whose remainder nvt_fw_parse() accepts. On success fw->data covers exactly the bare image and
// fw->source_offset is the skipped amount (0 for a bare image or a true image+trailer object).
// The caller passes the blob as-is: no copy, no firmware in this repository.
int nvt_fw_parse_stock(struct nvt_fw *fw, const uint8_t *data, size_t size);

// Engine reset, read the trim id at chip_info+1 (7 bytes, six meaningful) and select the NT36675/NT36672C map.
int nvt_identify(struct nvt_chip *c);

// Bootloader reset, write all partitions, program descriptors/checksums, enable HW CRC, boot-ready, then wait
// for reset state >= 0xa0. Returns 0 on success.
int nvt_download_firmware(struct nvt_chip *c, const struct nvt_fw *fw);

// Read the bootloader CRC flags and golden/result checksums (printed when a download fails).
int nvt_read_bld_crc(struct nvt_chip *c, const struct nvt_fw *fw);
// Compare 32-byte windows of the downloaded ILM/DLM in SRAM with the image.
int nvt_verify_sram(struct nvt_chip *c, const struct nvt_fw *fw);

// After the download: read the firmware info block, then optionally send the NT36532E-derived idle/doze command
// sequence. Those three command values are not confirmed for NT36672C (TODO); the probe only sends them when
// explicitly asked to.
int nvt_get_fw_info(struct nvt_chip *c, struct nvt_fw_info *info);
int nvt_set_custom_cmd(struct nvt_chip *c, uint8_t cmd, uint16_t value);
int nvt_set_doze_delay(struct nvt_chip *c, uint16_t value);
int nvt_start_scanning(struct nvt_chip *c, struct nvt_fw_info *info, int legacy_commands);

// Touch report. The vendor driver (drivers/input/touchscreen/nt36672c/nt36xxx.c: nvt_ts_work_func) reads
// POINT_DATA_LEN = 65 bytes at the event buffer and decodes ten 6-byte records at 1 + 6*i: byte 0 of a record is
// (finger id << 3) | state with state 1 = down and 2 = move, x = (r[1] << 4) | (r[3] >> 4), y = (r[2] << 4) |
// (r[3] & 0x0f). There is no header, counter or checksum; an idle panel simply has no record with a valid id.
#define NVT_REPORT_LEN 65u
#define NVT_REPORT_POINTS 10u
int nvt_read_report(struct nvt_chip *c, uint8_t *report);   // NVT_REPORT_LEN bytes from the event buffer

// THP frame access. frame[0] is chip byte 0 of the event buffer (there is no dummy byte in NT36xxx reads).
int nvt_read_frame(struct nvt_chip *c, uint8_t *frame);   // NVT_THP_FRAME_LEN bytes
void nvt_frame_matrix(const uint8_t *frame, int16_t *matrix);   // NVT_MATRIX_NODES int16, row-major
int nvt_frame_header_valid(const uint8_t *frame);
unsigned nvt_frame_type(const uint8_t *frame);
unsigned nvt_frame_counter(const uint8_t *frame);

// Cheap 32-byte header window: valid integrity words, counter and type tell whether a full frame is new.
int nvt_peek_frame_header(struct nvt_chip *c, uint8_t *hdr);   // NVT_THP_PEEK_LEN bytes
int nvt_peek_valid(const uint8_t *hdr);
unsigned nvt_peek_counter(const uint8_t *hdr);
unsigned nvt_peek_type(const uint8_t *hdr);

// Event offsets and handshake values (NT36xxx nt36xxx.h: NT36XXX_EVT_*).
#define NVT_EVENT_HOST_CMD 0x50
#define NVT_EVENT_RESET_COMPLETE 0x60
#define NVT_EVENT_FWINFO 0x78
#define NVT_RESET_STATE_INIT 0xa0
#define NVT_RESET_STATE_MAX 0xaf
#define NVT_HOST_CMD_ENABLE_FW_CRC 0xae
// NT36532E-derived (reference NvtProtocol.h); TODO: confirm the NT36672C doze-delay register.
#define NVT_DOZE_DELAY_ADDR 0x113718
#endif
