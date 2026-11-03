// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests for the NT36672C protocol, core and publication path. No hardware: the bus below models the chip's
// page-select SPI framing and enough of its register file to check identification, firmware parsing and download.
#include "NvtProtocol.h"
#include "NvtTouchCore.h"
#include "TouchBoard.h"
#include "TouchPublish.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

// ---------------------------------------------------------------------------------------------------------------
// Fake NT36xxx SPI target.
// ---------------------------------------------------------------------------------------------------------------
#define MEM_SIZE 0x40000
#define FAKE_EVENT_BUF 0x22D00
static uint8_t mem[MEM_SIZE];
static uint32_t page;
static unsigned engine_resets, bootloader_resets, boot_rdy_writes, boot_ready_rejects, bus_delays;
static int crc_enabled;
static const uint8_t *expect_image;
static uint32_t expect_ilm_bin, expect_ilm_sram, expect_ilm_size;
static uint32_t expect_dlm_bin, expect_dlm_sram, expect_dlm_size;

static uint32_t rd4(uint32_t a) {
    return (uint32_t)mem[a] | ((uint32_t)mem[a + 1] << 8) | ((uint32_t)mem[a + 2] << 16) |
           ((uint32_t)mem[a + 3] << 24);
}
static void put32(uint8_t *d, size_t at, uint32_t v) {
    d[at] = (uint8_t)v;
    d[at + 1] = (uint8_t)(v >> 8);
    d[at + 2] = (uint8_t)(v >> 16);
    d[at + 3] = (uint8_t)(v >> 24);
}
static int boot_ready_ok(void) {
    if (!crc_enabled || !expect_image) return 0;
    const uint32_t g_ilm = rd4(0x3F100), g_dlm = rd4(0x3F104);
    if (g_ilm != nvt_crc32_words(mem + expect_ilm_sram, expect_ilm_size, MEM_SIZE - expect_ilm_sram)) return 0;
    if (g_dlm != nvt_crc32_words(mem + expect_dlm_sram, expect_dlm_size, MEM_SIZE - expect_dlm_sram)) return 0;
    if (memcmp(mem + expect_ilm_sram, expect_image + expect_ilm_bin, expect_ilm_size)) return 0;
    if (memcmp(mem + expect_dlm_sram, expect_image + expect_dlm_bin, expect_dlm_size)) return 0;
    return 1;
}
static int fake_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    (void)ctx;
    if (len == 3 && tx[0] == 0xff) {   // page select
        page = ((uint32_t)tx[1] << 8) | tx[2];
        return 0;
    }
    const uint32_t addr = (page << 7) | (tx[0] & 0x7f);
    if (tx[0] & 0x80) {
        if (addr == 0x7FFF80) {         // engine reset is outside the modelled memory
            if (len == 2 && tx[1] == 0x5a) ++engine_resets;
            return 0;
        }
        const size_t data = len - 1;
        if (data && (size_t)addr + data > MEM_SIZE) return -1;
        if (data) memcpy(mem + addr, tx + 1, data);
        if (data && addr <= 0x3F0FE && 0x3F0FE < addr + data && mem[0x3F0FE] == 0x69) {
            ++bootloader_resets;
            crc_enabled = 0;
            mem[FAKE_EVENT_BUF + 0x60] = 0;
        }
        if (data && addr <= 0x3F30E && 0x3F30E < addr + data && (mem[0x3F30E] & 0x80)) crc_enabled = 1;
        if (data && addr <= 0x3F10D && 0x3F10D < addr + data && mem[0x3F10D] == 1) {
            ++boot_rdy_writes;
            if (boot_ready_ok()) mem[FAKE_EVENT_BUF + 0x60] = 0xa0;
            else ++boot_ready_rejects;
        }
        return 0;
    }
    // Address and dummy cycles precede data. Distinct canaries catch a shifted read.
    if (len < 3 || tx[1] != 0) return -1;
    const size_t data = len - 2;
    if (!rx || !data || (size_t)addr + data > MEM_SIZE) return -1;
    rx[0] = 0xee;
    rx[1] = 0xdd;
    memcpy(rx + 2, mem + addr, data);
    return 0;
}
static void fake_delay(void *ctx, unsigned ms) { (void)ctx; (void)ms; ++bus_delays; }
static const struct nvt_bus fake_bus = {NULL, fake_xfer, fake_delay};
static uint8_t scratch[NVT_TRANSFER_LEN + 8], rxbuf[NVT_READ_MAX + 2];
static struct nvt_chip chip;

static void model_reset(void) {
    memset(mem, 0, sizeof(mem));
    page = 0;
    engine_resets = bootloader_resets = boot_rdy_writes = boot_ready_rejects = bus_delays = 0;
    crc_enabled = 0;
    expect_image = NULL;
    // Trim id starts at chip_info after the two SPI lead-in bytes.
    const uint8_t trim[6] = {0x0c, 0xaa, 0xbb, 0x72, 0x66, 0x03};
    memcpy(mem + 0x1F64E, trim, sizeof(trim));
    // FWINFO: fw_ver, ~fw_ver, 18 x 36, abs_x_max 1080, abs_y_max 2400 (big-endian).
    const uint8_t info[8] = {0x2a, 0xd5, 18, 36, 0x04, 0x38, 0x09, 0x60};
    memcpy(mem + FAKE_EVENT_BUF + 0x78, info, sizeof(info));
    chip = (struct nvt_chip){.bus = &fake_bus, .scratch = scratch, .rxbuf = rxbuf, .reset_retries = 3};
}
static void model_expect(const struct nvt_fw *fw) {
    expect_image = fw->data;
    expect_ilm_bin = fw->part[0].bin_addr;
    expect_ilm_sram = fw->part[0].sram_addr;
    expect_ilm_size = fw->part[0].size;
    expect_dlm_bin = fw->part[1].bin_addr;
    expect_dlm_sram = fw->part[1].sram_addr;
    expect_dlm_size = fw->part[1].size;
}

// ---------------------------------------------------------------------------------------------------------------
// Synthetic 3 x 4 KiB firmware image: header at 0x0, info sections at 0x30, ILM/DLM data after 0x60, the
// NVT/MOD marker in the last block and FW_VER/FW_VER_BAR at write_size - 4096.
// ---------------------------------------------------------------------------------------------------------------
static void build_firmware(uint8_t *d, size_t size, struct nvt_fw *fw) {
    memset(d, 0, size);
    put32(d, 0x00, 0x60);        // image header end; also ILM bin_addr
    put32(d, 0x04, 0x10000);     // ILM SRAM
    put32(d, 0x08, 0x40);        // ILM size
    put32(d, 0x0c, 0x100);       // DLM bin_addr
    put32(d, 0x10, 0x20000);     // DLM SRAM
    put32(d, 0x14, 0x20);        // DLM size
    for (unsigned i = 0; i < 0x40; ++i) d[0x60 + i] = (uint8_t)(i * 7 + 1);
    for (unsigned i = 0; i < 0x20; ++i) d[0x100 + i] = (uint8_t)(i * 3 + 2);
    put32(d, 0x18, nvt_crc32_words(d + 0x60, 0x40, size - 0x60));
    put32(d, 0x1c, nvt_crc32_words(d + 0x100, 0x20, size - 0x100));
    memcpy(d + size - 3, "NVT", 3);
    d[size - 4096] = 0x01;
    d[size - 4095] = 0xfe;
    CHECK(nvt_fw_parse(fw, d, size) == 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Board and clock helpers.
// ---------------------------------------------------------------------------------------------------------------
static void test_board(void) {
    CHECK(tb_tlmm_ctl(4) == 0x4000 && tb_tlmm_io(5) == 0x5004);
    CHECK(tb_tlmm_intr_cfg(5) == 0x5008 && tb_tlmm_intr_status(5) == 0x500c);
    CHECK(tb_pin_cfg(TB_FUNC_QUP11, TB_DRIVE_6MA, TB_PULL_NONE, 0) == 0x84);
    CHECK(TB_PIN_CFG_RESET == 0x2b0);      // function 0, pull-up, 6 mA, output enable
    CHECK(TB_PIN_CFG_IRQ == 0x90);         // function 0, pull-down, 6 mA, input
    CHECK(tb_gcc_page(TB_GCC_BRANCH_S1) == TB_GCC_BASE + 0x18000);
    CHECK(tb_gcc_offset(TB_GCC_BRANCH_S1) == 0x13c);
    CHECK(tb_gcc_offset(TB_GCC_VOTE_ENABLE) == 0x008);
    // DFS frequencies from the cited divider tables.
    CHECK(tb_dfs_level_hz((1u << 8) | 11, 0, 0) == 100000000ULL);   // GPLL0 main 600 / 6
    CHECK(tb_dfs_level_hz((6u << 8) | 7, 0, 0) == 75000000ULL);     // GPLL0 even 300 / 4
    CHECK(tb_dfs_level_hz(0, 0, 0) == 19200000ULL);                 // TCXO bypass
    CHECK(tb_dfs_level_hz((6u << 8) | (2u << 12), 4, (~21u) & 0xffff) == 48000000ULL);   // 300 * 4 / 25
    const uint64_t levels[TB_DFS_LEVELS] = {19200000ULL, 100000000ULL, 75000000ULL, 0, 0, 0, 0, 0};
    struct tb_clock_choice c;
    CHECK(tb_choose_clock(levels, TB_DFS_LEVELS, 1, 5000000, &c) == 0 && c.actual_hz == 5000000ULL && c.div == 15);
    CHECK(tb_choose_clock(levels, TB_DFS_LEVELS, 1, 4900000, &c) == 0 && c.level == 0 && c.div == 4 &&
          c.actual_hz == 4800000ULL);
    CHECK(tb_choose_clock(levels, TB_DFS_LEVELS, 1, 19200000, &c) == 0 && c.level == 0 && c.div == 1);
    const uint64_t none[TB_DFS_LEVELS] = {0};
    CHECK(tb_choose_clock(none, TB_DFS_LEVELS, 1, 5000000, &c) < 0);
    // HID raw convention: xs 0..30480 is the 1080-pixel short axis, ys 0..20320 the 2400-pixel long axis.
    CHECK(TB_PANEL_SHORT_TENTHS == 10800 && TB_PANEL_LONG_TENTHS == 24000);
    CHECK(tb_hid_x_from_short(0) == 0 && tb_hid_x_from_short(10799) == 30477);
    CHECK(tb_hid_y_from_long(0) == 0 && tb_hid_y_from_long(23999) == 20319);
    CHECK(tb_hid_x_from_short(5400) == 15240);
    CHECK(tb_hid_y_from_long(12000) == 10160);
    const struct tb_hid_point p = tb_hid_from_panel(5400, 12000);
    CHECK(p.x == 15240 && p.y == 10160);
}

// ---------------------------------------------------------------------------------------------------------------
// Publication seqlock.
// ---------------------------------------------------------------------------------------------------------------
static volatile uint32_t pub_seq, pub_count, pub_lock;
static volatile int pub_xs[12], pub_ys[12], pub_ids[12];
static unsigned sink_calls;
static void sink(void *ctx) {
    CHECK(ctx == (void *)0x1234 && pub_lock == 0 && !(pub_seq & 1));
    ++sink_calls;
}
static void test_publish(void) {
    struct nvt_touch_publication p = {&pub_seq, &pub_count, pub_xs, pub_ys, pub_ids, 12, sink, (void *)0x1234, &pub_lock};
    const int xs[2] = {100, 200}, ys[2] = {300, 400}, ids[2] = {3, 4};
    CHECK(nvt_touch_publish(&p, xs, ys, ids, 2) == 2);
    CHECK(pub_seq == 2 && pub_count == 2 && pub_xs[0] == 100 && pub_xs[1] == 200 && pub_ys[0] == 300 &&
          pub_ys[1] == 400 && pub_ids[0] == 3 && pub_ids[1] == 4);
    CHECK(sink_calls == 1);
    CHECK(nvt_touch_publish(&p, xs, ys, ids, 0) == 0);
    CHECK(pub_seq == 4 && pub_count == 0 && sink_calls == 2);
    int many_x[12], many_y[12], many_ids[12];
    for (unsigned i = 0; i < 12; ++i) { many_x[i] = (int)i; many_y[i] = (int)i; many_ids[i] = (int)i; }
    CHECK(nvt_touch_publish(&p, many_x, many_y, many_ids, 20) == 12 && pub_count == 12 && pub_seq == 6);
    CHECK(sink_calls == 3);
    // A busy writer keeps the previous contact and does not notify the sink.
    pub_lock = 1;
    CHECK(nvt_touch_publish(&p, xs, ys, ids, 2) == 0);
    CHECK(pub_seq == 6 && pub_count == 12 && sink_calls == 3 && pub_lock == 1);
    pub_lock = 0;
    CHECK(nvt_touch_publish(&p, NULL, ys, ids, 2) == 0);
    CHECK(pub_seq == 6 && pub_count == 12 && sink_calls == 3);
    CHECK(nvt_touch_publish(&p, NULL, NULL, NULL, 0) == 0);
    CHECK(pub_seq == 8 && pub_count == 0 && sink_calls == 4 && pub_lock == 0);
    pub_seq = UINT32_MAX - 1;
    CHECK(nvt_touch_publish(&p, xs, ys, ids, 2) == 2 && pub_seq == 0 && pub_count == 2);
    // After physical takeover, even a synthetic lift must not erase the real finger.
    unsigned takeover = 2;
    CHECK(nvt_touch_publish_if(&p, NULL, NULL, NULL, 0, &takeover) == 0);
    CHECK(pub_count == 2 && pub_seq == 0 && sink_calls == 5 && pub_lock == 0);
    takeover = 0;
    CHECK(nvt_touch_publish_if(&p, NULL, NULL, NULL, 0, &takeover) == 0);
    CHECK(pub_count == 0 && pub_seq == 2 && sink_calls == 6 && pub_lock == 0);
    // A failed physical writer cannot stop the synthetic producer before storing a contact.
    pub_lock = 1;
    CHECK(nvt_touch_publish_snapshot(&p, xs, ys, ids, 2, NULL, &takeover) == 0);
    CHECK(takeover == 0 && pub_seq == 2 && sink_calls == 6);
    pub_lock = 0;
    CHECK(nvt_touch_publish_snapshot(&p, xs, ys, ids, 2, NULL, &takeover) == 2);
    CHECK(takeover == 2 && pub_seq == 4 && pub_count == 2 && sink_calls == 7);
    CHECK(nvt_touch_publish_if(&p, NULL, NULL, NULL, 0, &takeover) == 0);
    CHECK(pub_seq == 4 && pub_count == 2 && sink_calls == 7 && pub_lock == 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Protocol: framing, firmware parse, identification, download, frames.
// ---------------------------------------------------------------------------------------------------------------
static void test_protocol(void) {
    CHECK(nvt_crc32_words((const uint8_t[]){1, 2, 3, 4}, 4, 4) == 0xade61088u);
    CHECK(nvt_crc32_words((const uint8_t[]){1, 2, 3}, 3, 3) == 0x615eab4au);   // the tail word is zero padded

    // Framing: a write sends offset|0x80; a read sends the offset and a dummy byte before data.
    model_reset();
    uint8_t pattern[300];
    for (unsigned i = 0; i < sizeof(pattern); ++i) pattern[i] = (uint8_t)(i * 7 + 3);
    CHECK(nvt_write(&chip, 0x1230, pattern, sizeof(pattern)) == 0);
    CHECK(!memcmp(mem + 0x1230, pattern, sizeof(pattern)));
    uint8_t back[300];
    CHECK(nvt_read(&chip, 0x1230, back, sizeof(back)) == 0 && !memcmp(back, pattern, sizeof(back)));
    CHECK(nvt_write(&chip, 0x10, pattern, 0) == NVT_ERR_PARAM);
    CHECK(nvt_read(&chip, 0x10, back, NVT_READ_MAX + 1) == NVT_ERR_PARAM);

    // Identification: one engine reset, five bootloader resets on failure, the NT36675 map on success.
    model_reset();
    CHECK(nvt_identify(&chip) == 0);
    CHECK(chip.map && chip.map->event_buf == 0x22D00 && chip.hw_crc == 2 && engine_resets == 1 &&
          bootloader_resets == 1);
    CHECK(!memcmp(chip.id, (const uint8_t[]){0x0c, 0xaa, 0xbb, 0x72, 0x66, 0x03}, 6));
    model_reset();
    mem[0x1F651] = 0x00;   // masked-in byte 0x72 wrong
    CHECK(nvt_identify(&chip) == NVT_ERR_CHIP && bootloader_resets == 5);

    // Firmware parser.
    uint8_t image[0x3000];
    struct nvt_fw fw;
    build_firmware(image, sizeof(image), &fw);
    CHECK(fw.count == 5 && fw.write_size == sizeof(image) && fw.fw_ver == 0x01 && fw.fw_ver_bar == 0xfe);
    CHECK(!strcmp(fw.part[0].name, "ILM") && !strcmp(fw.part[1].name, "DLM"));
    CHECK(!strcmp(fw.part[2].name, "Info-0") && fw.part[0].bin_addr == 0x60 && fw.part[0].sram_addr == 0x10000);
    CHECK(fw.part[0].crc == fw.part[0].crc_calc && fw.part[1].crc == fw.part[1].crc_calc);
    CHECK(nvt_fw_parse(&fw, image, 0x20) == NVT_ERR_FW);
    uint8_t copy[0x3000];
    memcpy(copy, image, sizeof(copy));
    memset(copy + sizeof(copy) - 3, 0, 3);
    CHECK(nvt_fw_parse(&fw, copy, sizeof(copy)) == NVT_ERR_FW);       // no NVT/MOD marker
    memcpy(copy, image, sizeof(copy));
    copy[sizeof(copy) - 4095] = 0x00;                                  // FW_VER + FW_VER_BAR != 0xFF
    CHECK(nvt_fw_parse(&fw, copy, sizeof(copy)) == NVT_ERR_FW);
    memcpy(copy, image, sizeof(copy));
    put32(copy, 0x08, 0x3000);                                         // ILM range beyond the image
    CHECK(nvt_fw_parse(&fw, copy, sizeof(copy)) == NVT_ERR_FW);

    // Stock-kernel layouts: bare image, true object (image + 0x2000 trailer), and a 0x22000 carve taken from the
    // image's end marker (which starts 0x2000 early, so its leading block belongs to the previous object).
    static uint8_t container[0x2000 + sizeof(image)];
    memset(container, 0, sizeof(container));
    put32(container, 0x00, 0x425a6666u);                               // observed trailer magic (previous object here)
    memcpy(container + 0x2000, image, sizeof(image));
    CHECK(nvt_fw_parse(&fw, container, sizeof(container)) == NVT_ERR_FW);
    CHECK(nvt_fw_parse_stock(&fw, container, sizeof(container)) == 0);
    CHECK(fw.source_offset == 0x2000 && fw.size == sizeof(image) && fw.data == container + 0x2000 && fw.count == 5);
    CHECK(fw.part[0].bin_addr == 0x60 && fw.part[1].bin_addr == 0x100);
    CHECK(nvt_fw_parse_stock(&fw, image, sizeof(image)) == 0 && fw.source_offset == 0);
    // True object: the image followed by its own trailer; the strict parser finds the marker at the image end.
    static uint8_t object[sizeof(image) + 0x2000];
    memcpy(object, image, sizeof(image));
    memset(object + sizeof(image), 0, 0x2000);
    CHECK(nvt_fw_parse_stock(&fw, object, sizeof(object)) == 0);
    CHECK(fw.source_offset == 0 && fw.size == sizeof(object) && fw.write_size == sizeof(image));
    // A differently sized leading block (and a 0x19 fill) is found by the scan fallback.
    static uint8_t alt[0x1000 + sizeof(image)];
    memset(alt, 0x19, 0x1000);
    memcpy(alt + 0x1000, image, sizeof(image));
    CHECK(nvt_fw_parse(&fw, alt, sizeof(alt)) == NVT_ERR_FW);
    CHECK(nvt_fw_parse_stock(&fw, alt, sizeof(alt)) == 0 && fw.source_offset == 0x1000);
    build_firmware(image, sizeof(image), &fw);

    // Download into the model: SRAM windows, descriptors, lengths and golden checksums.
    model_reset();
    model_expect(&fw);
    CHECK(nvt_identify(&chip) == 0);
    unsigned resets = bootloader_resets;
    CHECK(nvt_download_firmware(&chip, &fw) == 0);
    CHECK(bootloader_resets - resets == 1 && boot_rdy_writes == 1 && boot_ready_rejects == 0);
    CHECK(!memcmp(mem + 0x10000, image + 0x60, 0x40) && !memcmp(mem + 0x20000, image + 0x100, 0x20));
    CHECK(rd4(0x3F128) == 0x10000 && rd4(0x3F12C) == 0x20000);
    CHECK(rd4(0x3F118) == 0x40 && rd4(0x3F130) == 0x20);
    CHECK(rd4(0x3F100) == fw.part[0].crc && rd4(0x3F104) == fw.part[1].crc);
    CHECK(mem[FAKE_EVENT_BUF + 0x60] == 0xa0);
    // A boot ROM that never reports ready is retried three times and then fails.
    model_reset();
    model_expect(&fw);
    CHECK(nvt_identify(&chip) == 0);
    expect_ilm_size = 0x40;
    mem[FAKE_EVENT_BUF + 0x60] = 0;   // nothing sets it to 0xa0 once boot_ready_ok fails; force failure below
    expect_dlm_bin = 0x101;           // break the DLM comparison so boot_ready_ok returns false
    resets = bootloader_resets;
    CHECK(nvt_download_firmware(&chip, &fw) == NVT_ERR_TIMEOUT && bootloader_resets - resets == 3);
    CHECK(boot_ready_rejects == 3);

    // Firmware info block.
    model_reset();
    CHECK(nvt_identify(&chip) == 0);
    struct nvt_fw_info info;
    CHECK(nvt_get_fw_info(&chip, &info) == 0 && info.fw_ver == 0x2a && info.x_num == 18 && info.y_num == 36 &&
          info.abs_x_max == 1080 && info.abs_y_max == 2400 && !info.defaulted);
    mem[FAKE_EVENT_BUF + 0x79] = 0x00;
    CHECK(nvt_get_fw_info(&chip, &info) == NVT_ERR_FW && info.defaulted);

    // Frame header, counter/type, matrix decode and the peek window.
    model_reset();
    CHECK(nvt_identify(&chip) == 0);
    uint8_t frame[NVT_THP_FRAME_LEN], got[NVT_THP_FRAME_LEN];
    memset(frame, 0, sizeof(frame));
    frame[2] = 0x34;
    frame[3] = 0x12;
    frame[4] = 0x78;
    frame[5] = 0x56;
    frame[12] = 0x87;
    frame[13] = 0xa9;
    frame[8] = 0x4e;
    frame[9] = 0x54;
    frame[10] = 0x50;
    frame[11] = 0x31;
    frame[16] = 0xb1;
    frame[17] = 0xab;
    frame[18] = 0xaf;
    frame[19] = 0xce;
    frame[24] = 4;
    for (unsigned i = 0; i < NVT_MATRIX_NODES; ++i) {
        const int16_t v = (int16_t)(i * 3 - 500);
        frame[NVT_THP_MATRIX_OFFSET + i * 2] = (uint8_t)v;
        frame[NVT_THP_MATRIX_OFFSET + i * 2 + 1] = (uint8_t)((uint16_t)v >> 8);
    }
    CHECK(nvt_peek_valid(frame) && nvt_frame_header_valid(frame));
    CHECK(nvt_frame_counter(frame) == 0x1234 && nvt_frame_type(frame) == 4);
    frame[12] ^= 1;
    CHECK(!nvt_frame_header_valid(frame));
    frame[12] ^= 1;
    memcpy(mem + FAKE_EVENT_BUF, frame, sizeof(frame));
    CHECK(nvt_read_frame(&chip, got) == 0 && !memcmp(got, frame, sizeof(frame)));
    uint8_t peek[NVT_THP_PEEK_LEN];
    CHECK(nvt_peek_frame_header(&chip, peek) == 0 && !memcmp(peek, frame, sizeof(peek)));
    CHECK(nvt_peek_counter(peek) == 0x1234 && nvt_peek_type(peek) == 4);
    int16_t matrix[NVT_MATRIX_NODES];
    nvt_frame_matrix(got, matrix);
    for (unsigned i = 0; i < NVT_MATRIX_NODES; ++i) CHECK(matrix[i] == (int16_t)(i * 3 - 500));
}

// ---------------------------------------------------------------------------------------------------------------
// Core: baseline median, two blobs, tracking, thresholds, palm and edge rules.
// ---------------------------------------------------------------------------------------------------------------
static struct nvt_touch_core core;
static uint32_t rng = 12345;
static int noise(int amp) {
    rng = rng * 1664525u + 1013904223u;
    return (int)((rng >> 16) % (unsigned)(2 * amp + 1)) - amp;
}
static void base_frame(int16_t *m, int amp) {
    for (int i = 0; i < NVT_NODES; ++i) m[i] = (int16_t)(5000 + (i % 7) * 3 + noise(amp));
}
static void blob(int16_t *m, double row, double col, double amplitude, double sigma) {
    for (int r = 0; r < NVT_ROWS; ++r)
        for (int c = 0; c < NVT_COLUMNS; ++c) {
            const double d2 = (r - row) * (r - row) + (c - col) * (c - col);
            m[r * NVT_COLUMNS + c] =
                (int16_t)(m[r * NVT_COLUMNS + c] + (int)lround(amplitude * exp(-d2 / (2 * sigma * sigma))));
        }
}
static void settle(void) {
    int16_t frame[NVT_NODES];
    nvt_touch_reset(&core);
    for (unsigned i = 0; i < NVT_STARTUP_FRAMES; ++i) {
        base_frame(frame, 2);
        struct nvt_contact c[4];
        CHECK(nvt_touch_process(&core, frame, 4, c, 4) == -1);
    }
    CHECK(core.have_reference);
}
static int feed(int16_t *m, struct nvt_contact *out, int max) {
    return nvt_touch_process(&core, m, 4, out, max);
}
static void lift(void) {
    int16_t m[NVT_NODES];
    struct nvt_contact out[12];
    for (int k = 0; k < NVT_HOLD_FRAMES; ++k) { base_frame(m, 2); (void)feed(m, out, 12); }
    base_frame(m, 2);
    CHECK(feed(m, out, 12) == 0);
}

static void test_core(void) {
    int16_t f[NVT_NODES];
    struct nvt_contact c[12];
    settle();
    base_frame(f, 2);
    CHECK(feed(f, c, 12) == 0);
    for (int i = 0; i < NVT_NODES; ++i) CHECK(abs(core.reference[i] - (5000 + (i % 7) * 3)) <= 2);
    CHECK(nvt_touch_process(&core, f, 3, c, 12) == 0);   // wrong type ignored

    // One finger: reported from the second frame, near the expected centroid, same identifier.
    int id = 0;
    for (int frame = 0; frame < 4; ++frame) {
        base_frame(f, 2);
        blob(f, 18.0, 9.0, 900, 1.3);
        const int n = feed(f, c, 12);
        if (frame == 0) CHECK(n == 0);
        else {
            CHECK(n == 1);
            CHECK(abs(c[0].x - nvt_touch_column_center(9) * NVT_SUPER_RESOLUTION) < 120);
            CHECK(abs(c[0].y - nvt_touch_row_center(18) * NVT_SUPER_RESOLUTION) < 120);
            if (!id) id = c[0].tracking_id;
            CHECK(c[0].tracking_id == id && c[0].slot >= 0);
        }
    }
    // Sub-pixel position between columns 8 and 9.
    int n = 0;
    for (int frame = 0; frame < 40; ++frame) {
        base_frame(f, 2);
        blob(f, 18.0, 8.5, 900, 1.3);
        n = feed(f, c, 12);
    }
    CHECK(n == 1);
    const int lo = nvt_touch_column_center(8) * NVT_SUPER_RESOLUTION;
    const int hi = nvt_touch_column_center(9) * NVT_SUPER_RESOLUTION;
    CHECK(c[0].x > lo + 100 && c[0].x < hi - 100 && c[0].tracking_id == id);

    // Lift, then a new touch takes a new identifier and is not reported on its first frame.
    lift();
    base_frame(f, 2);
    blob(f, 8.0, 4.0, 900, 1.3);
    CHECK(feed(f, c, 12) == 0);
    base_frame(f, 2);
    blob(f, 8.0, 4.0, 900, 1.3);
    CHECK(feed(f, c, 12) == 1 && c[0].tracking_id != id);

    // Two fingers far apart: two contacts, distinct identifiers and slots.
    settle();
    for (int frame = 0; frame < 3; ++frame) {
        base_frame(f, 2);
        blob(f, 6.0, 4.0, 800, 1.3);
        blob(f, 28.0, 14.0, 700, 1.3);
        n = feed(f, c, 12);
        if (frame >= 1) {
            CHECK(n == 2 && c[0].tracking_id != c[1].tracking_id && c[0].slot != c[1].slot);
            CHECK(abs(c[0].x - c[1].x) > 1000);
        }
    }
    // A moving blob keeps its identifier.
    settle();
    int move_id = 0;
    for (int frame = 0; frame < 12; ++frame) {
        base_frame(f, 2);
        blob(f, 18.0, 4.0 + frame * 0.5, 900, 1.3);
        n = feed(f, c, 12);
        if (frame >= 1) {
            CHECK(n == 1);
            if (!move_id) move_id = c[0].tracking_id;
            CHECK(c[0].tracking_id == move_id);
        }
    }

    // Faint blobs never start a contact; a blob between the up and down thresholds keeps one alive.
    settle();
    for (int frame = 0; frame < 4; ++frame) {
        base_frame(f, 2);
        blob(f, 18, 9, 80, 1.3);
        CHECK(feed(f, c, 12) == 0);
    }
    for (int frame = 0; frame < 4; ++frame) {
        base_frame(f, 2);
        blob(f, 18, 9, 260, 1.3);
        CHECK(feed(f, c, 12) == 0);
    }
    for (int frame = 0; frame < 3; ++frame) {
        base_frame(f, 2);
        blob(f, 18, 9, 900, 1.3);
        (void)feed(f, c, 12);
    }
    base_frame(f, 2);
    blob(f, 18, 9, 260, 1.3);
    CHECK(feed(f, c, 12) == 1);
    // Palm-sized blobs are rejected by node count.
    settle();
    for (int frame = 0; frame < 4; ++frame) {
        base_frame(f, 2);
        blob(f, 18, 9, 900, 6.0);
        CHECK(feed(f, c, 12) == 0);
    }
    // The output limit is honoured.
    settle();
    for (int frame = 0; frame < 3; ++frame) {
        base_frame(f, 2);
        blob(f, 5, 3, 800, 1.2);
        blob(f, 5, 9, 800, 1.2);
        blob(f, 5, 15, 800, 1.2);
        blob(f, 28, 3, 800, 1.2);
        n = feed(f, c, 2);
        if (frame >= 1) CHECK(n == 2);
    }
    // Edge mapping formula and monotonicity on both axes.
    CHECK(nvt_touch_map_edge_axis(300, NVT_SHORT_AXIS_MAX, 1) == 600 - 483 - 1);
    CHECK(nvt_touch_map_edge_axis(300, NVT_LONG_AXIS_MAX, 0) == 600 - 483);
    CHECK(nvt_touch_map_edge_axis(5000, NVT_LONG_AXIS_MAX, 0) == 5000);
    int previous = -1;
    for (int v = 0; v < NVT_LONG_AXIS_MAX; v += 7) {
        const int mapped = nvt_touch_map_edge_axis(v, NVT_LONG_AXIS_MAX, 0);
        CHECK(mapped >= previous - 1);
        previous = mapped;
    }
    CHECK(nvt_touch_map_edge_axis(NVT_LONG_AXIS_MAX - 1, NVT_LONG_AXIS_MAX, 0) > NVT_LONG_AXIS_MAX - 600);
    // A type-2 baseline frame sets the reference immediately.
    nvt_touch_reset(&core);
    base_frame(f, 0);
    CHECK(nvt_touch_process(&core, f, 2, c, 12) == -1 && core.have_reference);
}

int main(void) {
    CHECK(NVT_MATRIX_COLUMNS == NVT_COLUMNS && NVT_MATRIX_ROWS == NVT_ROWS);
    test_board();
    test_publish();
    test_protocol();
    test_core();
    puts("PASS: NT36672C board/clock/HID helpers, publication seqlock, SPI framing, trim-id, firmware parse and "
         "model download, frame header/matrix decode, two-blob tracking, thresholds, palm and edge rules; synthetic "
         "data, no hardware");
    return 0;
}
