// SPDX-License-Identifier: BSD-2-Clause-Patent
// picasso (Qualcomm SM7250 "lito") Novatek NT36672C board facts and the pure helpers around them. Every constant
// states its source. Nothing here touches hardware; TouchProbe.cpp owns the MMIO. Host-tested by Test/main.c.
#ifndef NATIVE_NOVATEK_TOUCH_BOARD_H
#define NATIVE_NOVATEK_TOUCH_BOARD_H
#include <stdint.h>
#include "NvtTouchCore.h"

// ---------------------------------------------------------------------------------------------------------------
// Device tree. /tmp/picasso.dts is the decompiled picasso DT used for bring-up; line numbers are from that file.
// ---------------------------------------------------------------------------------------------------------------

// QUPv3 wrapper 1: qupv3_1: qcom,qupv3_1_geni_se@9c0000, reg = <0x9c0000 0x2000> (picasso.dts:14476).
#define TB_QUP_WRAPPER_BASE 0x009c0000ULL
#define TB_QUP_WRAPPER_SIZE 0x2000ULL
// Serial engine 7: qupv3_se7_spi: spi@984000, reg = <0x984000 0x4000> (picasso.dts:14842). This is wrapper 1's
// fourth engine (its SE6..SE11 are at 0x980000..0x994000); see the clock mapping below.
#define TB_SE_BASE 0x00984000ULL
#define TB_SE_SIZE 0x4000ULL

// DMA path (not used by the FIFO-mode probe). qupv3_1 has iommus = <&apps_smmu 0x23 0> (picasso.dts:14481):
// apps_smmu: apps-smmu@15000000, reg = <0x15000000 0x100000 ...> (picasso.dts:10710). Stream ID 0x23 is constant.
#define TB_APPS_SMMU_BASE 0x15000000ULL
#define TB_APPS_SMMU_SIZE 0x100000ULL
#define TB_QUP_SMMU_SID 0x23

// Interrupts. spi@984000 has interrupts = <0 0x162 4> (picasso.dts:14853): GIC SPI 354, level high, not used by the
// GPIO-driven frame loop. The touch IRQ is TLMM GPIO5, whose pin interrupts are summarised as GIC SPI 208
// (lito-pinctrl.dtsi: "interrupts = <GIC_SPI 208 IRQ_TYPE_LEVEL_HIGH>"); the summary interrupt is therefore
// 32 + 208 = 240.
#define TB_SE_IRQ_SPI 354
#define TB_TLMM_SUMMARY_SPI 208
#define TB_TLMM_SUMMARY_INTID (32 + TB_TLMM_SUMMARY_SPI)

// GCC: gcc: qcom,gcc@100000, reg = <0x100000 0x1f0000> (picasso.dts:3427). Clock IDs below are the DT's clock
// specifiers for spi@984000: se-clk = 0x4b, m-ahb = 0x57, s-ahb = 0x58 (picasso.dts:14849).
#define TB_GCC_BASE 0x00100000ULL
#define TB_GCC_SIZE 0x001f0000ULL
#define TB_CLOCK_SE_CLK 0x4b
#define TB_CLOCK_M_AHB 0x57
#define TB_CLOCK_S_AHB 0x58

// GCC register offsets for SM7250/lito, from the in-review upstream "clk: qcom: Add Global Clock Controller (GCC)
// driver for SM7250" (Sreeshankar K, 2026; the driver is not merged yet, so these are proposal-stage facts).
// QUPv3 wrapper 1: core 0x23138 (vote bit 19), core_2x 0x23140 (bit 18), m_ahb 0x18004 (bit 20),
// s_ahb 0x18008 (bit 21); wrapper-1 engine s1 (= global SE7, whose DT clock ID is 0x4b) branch 0x1813c (bit 23).
// The SE7 = s1 mapping is inferred from the DT clock IDs (SE6 = 0x49 = s0, SE7 = 0x4b = s1) and the wrapper's
// SE6..SE11 ordering, not from a public lito table.
// The wrapper vote register is 0x52008; GPLL0 uses enable register 0x52010 bit 0 and mode register 0x0 bit 31
// (the mode register offset follows the SM8250 driver; the SM7250 patch excerpt did not print it: TODO verify).
#define TB_GCC_GPLL0_MODE 0x00000000
#define TB_GCC_GPLL0_ENABLE 0x0052010
#define TB_GCC_VOTE_ENABLE 0x0052008
#define TB_GCC_BRANCH_M_AHB 0x00018004
#define TB_GCC_BRANCH_S_AHB 0x00018008
#define TB_GCC_BRANCH_S1 0x0001813c
#define TB_GCC_BRANCH_CORE 0x00023138
#define TB_GCC_BRANCH_CORE_2X 0x00023140
#define TB_GCC_DFS_S1_RCG 0x00018140

#define TB_VOTE_CORE_2X (1u << 18)
#define TB_VOTE_CORE (1u << 19)
#define TB_VOTE_M_AHB (1u << 20)
#define TB_VOTE_S_AHB (1u << 21)
#define TB_VOTE_S1 (1u << 23)
#define TB_VOTE_ALL (TB_VOTE_CORE_2X | TB_VOTE_CORE | TB_VOTE_M_AHB | TB_VOTE_S_AHB | TB_VOTE_S1)
#define TB_CBCR_CLK_OFF (1u << 31)
#define TB_GPLL0_LOCK (1u << 31)
// The probe maps these 4 KiB GCC pages; the register constants below are offsets in them.
#define TB_GCC_PAGE_SIZE 0x1000
#define TB_GCC_LOW_PAGE 0x0               // GPLL0 mode
#define TB_GCC_QUPV3_PAGE 0x18000         // m_ahb/s_ahb/s1 branches and the s1 DFS RCG
#define TB_GCC_CORE_PAGE 0x23000          // core and core_2x branches
#define TB_GCC_VOTE_PAGE 0x52000          // GPLL0 and QUP branch votes

// SPI sanity limit and the reset pulse timing (TODO: confirm the NT36672C reset timing).
#define TB_SPI_MIN_HZ 100000u
#define TB_RESET_LOW_US 10000u
#define TB_RESET_SETTLE_MS 50u

// RCG/DFS register layout (clk-rcg2.c: cmd, cfg, M, N, D, DFSR_CMD, then per-level CFG/M/N tables).
#define TB_DFS_LEVELS 8
#define TB_XO_HZ 19200000ULL                  // BI_TCXO; also the DFS level-0 bypass source
#define TB_RCG_CMD 0x0
#define TB_RCG_CFG 0x4
#define TB_RCG_M 0x8
#define TB_RCG_N 0xc
#define TB_RCG_DFSR_CMD 0x14                  // bit0 DFS enable, bits 4:1 current level
#define TB_RCG_PERF_CFG(l) (0x1c + 4 * (l))
#define TB_RCG_PERF_M(l) (0x5c + 4 * (l))
#define TB_RCG_PERF_N(l) (0x9c + 4 * (l))

// TLMM: tlmm: pinctrl@f000000 (picasso.dts:7394); lito-pinctrl.dtsi gives reg = <0x0f000000 0x1000000>.
// pinctrl-msm: control register = 0x1000 * gpio, io register = +4, interrupt config = +8, status = +0xc.
// The pins are split over four tiles inside that one window: pinctrl-lito.c defines NORTH 0x0, SOUTH 0x100000,
// WEST 0x500000, EAST 0x900000 and gives every PINGROUP its tile, so a pin's register is tile_base + 0x1000*gpio.
// The touch pins are 4 and 5 in EAST, 6..10 in WEST (pinctrl-lito.c lito_pins). Reading them at NORTH + 0x1000*gpio
// touches unimplemented tile pages: on this boot GPIO6's NORTH page raised a synchronous external abort (ESR
// 0x96000010), and the whole NORTH window reads zero and drops writes.
#define TB_TLMM_BASE 0x0f000000ULL
#define TB_TLMM_NORTH 0x0000000ULL
#define TB_TLMM_SOUTH 0x0100000ULL
#define TB_TLMM_WEST 0x0500000ULL
#define TB_TLMM_EAST 0x0900000ULL
#define TB_GPIO_STRIDE 0x1000
#define TB_TLMM_IO_OFFSET 0x4
#define TB_TLMM_INTR_CFG_OFFSET 0x8
#define TB_TLMM_INTR_STATUS_OFFSET 0xc

// Pins. picasso.dts:9241 qupv3_se7_spi_active muxes gpio6/7/8 to function "qup11"; the task's board facts add CS
// gpio9 "qup11". The DT does not give the numeric TLMM function selector for "qup11". TODO: read it from the
// lito/SM7250 pinctrl function table; 1 is the selector of the first non-gpio function on SM7150's qup11 pins and
// is the value used here (unverified on lito). Reset is novatek,reset-gpio = <&tlmm 4 0> and IRQ is
// novatek,irq-gpio = <&tlmm 5 0x2001> (picasso.dts:24462-24463); both are plain GPIOs (selector 0).
#define TB_GPIO_SPI_CLK 6
#define TB_GPIO_SPI_MISO 7
#define TB_GPIO_SPI_MOSI 8
#define TB_GPIO_SPI_CS 9
#define TB_GPIO_RESET 4
#define TB_GPIO_IRQ 5
#define TB_FUNC_GPIO 0
#define TB_FUNC_QUP11 1                       // pinctrl-lito.c lito_pins: pin 6 starts with qup11, so its slot is 1
#define TB_PULL_NONE 0
#define TB_PULL_DOWN 1
#define TB_PULL_KEEPER 2
#define TB_PULL_UP 3
#define TB_DRIVE_6MA 2                        // drive-strength = <6> encoded as (mA / 2) - 1

// pinctrl-msm TLMM interrupt config bits (pinctrl-msm.c): enable, polarity, detection 0 level / 1 rising /
// 2 falling / 3 both, raw status, target 3 = application processor.
#define TB_TLMM_INTR_ENABLE (1u << 0)
#define TB_TLMM_INTR_POL_HIGH (1u << 1)
#define TB_TLMM_INTR_DETECT_RISING (1u << 2)
#define TB_TLMM_INTR_RAW_STATUS (1u << 4)
#define TB_TLMM_INTR_TARGET_APPS (3u << 5)
#define TB_TLMM_INTR_CFG (TB_TLMM_INTR_ENABLE | TB_TLMM_INTR_POL_HIGH | TB_TLMM_INTR_DETECT_RISING | \
                          TB_TLMM_INTR_RAW_STATUS | TB_TLMM_INTR_TARGET_APPS)

// SPI. The touch node gives spi-max-frequency = <0x4c4b40> = 5 MHz (picasso.dts:24461) and no spi-cpha/spi-cpol,
// so mode 0. The sm7150-mainline NT36672C patch raises the controller limit to 8 MHz, but this board's DT says 5 MHz.
#define TB_SPI_MODE 0
#define TB_SPI_DEFAULT_HZ 5000000u
#define TB_SPI_MAX_HZ 5000000u

// Panel. g7a 37_02_0b and g7a 36_02_0c are 0x438 x 0x960 = 1080 x 2400 (picasso.dts:22817 and 22935). The touch
// matrix is 18 channels across the 1080-pixel axis and 36 channels along the 2400-pixel axis.
#define TB_PANEL_WIDTH_PX 1080
#define TB_PANEL_HEIGHT_PX 2400
#define TB_PANEL_SHORT_TENTHS (TB_PANEL_WIDTH_PX * 10)     // core coordinates are 0.1 panel pixels
#define TB_PANEL_LONG_TENTHS (TB_PANEL_HEIGHT_PX * 10)

// HID publication contract. NativeHidInput.cpp normalises xs by 30480 and ys by 20320, so the producer must keep
// that raw convention. On the NT36532E tablet xs was the long axis; on Picasso (portrait) xs is the 1080-pixel
// short axis and ys the 2400-pixel long axis. Mirror flags are bring-up switches, still unverified on hardware.
#define TB_HID_X_MAX 30480
#define TB_HID_Y_MAX 20320
#define TB_HID_MIRROR_X 0
#define TB_HID_MIRROR_Y 0

// TouchBoard.h supplies the board configuration for an 18x36 NT36672C panel; refuse to build against another core.
#if NVT_COLUMNS != 18 || NVT_ROWS != 36
#error "NT36672C: NvtTouchCore must be built for an 18x36 matrix"
#endif

static inline uint32_t tb_tlmm_ctl(unsigned gpio) { return (uint32_t)gpio * TB_GPIO_STRIDE; }
// Tile of a pin (pinctrl-lito.c lito_pins): the touch reset/IRQ GPIOs are EAST, the qup11 pins WEST.
static inline uint32_t tb_tlmm_tile(unsigned gpio) { return gpio <= TB_GPIO_IRQ ? TB_TLMM_EAST : TB_TLMM_WEST; }
static inline uintptr_t tb_tlmm_pa(unsigned gpio) { return TB_TLMM_BASE + tb_tlmm_tile(gpio) + tb_tlmm_ctl(gpio); }
static inline uint32_t tb_tlmm_io(unsigned gpio) { return tb_tlmm_ctl(gpio) + TB_TLMM_IO_OFFSET; }
static inline uint32_t tb_tlmm_intr_cfg(unsigned gpio) { return tb_tlmm_ctl(gpio) + TB_TLMM_INTR_CFG_OFFSET; }
static inline uint32_t tb_tlmm_intr_status(unsigned gpio) { return tb_tlmm_ctl(gpio) + TB_TLMM_INTR_STATUS_OFFSET; }

// One TLMM control value: function 2:0, pull 5:4, drive strength 8:6, output enable 9 (pinctrl-msm).
static inline uint32_t tb_pin_cfg(unsigned function, unsigned drive_code, unsigned pull, int output) {
    return ((function & 7u) << 2) | ((pull & 3u) << 4) | ((drive_code & 7u) << 6) | (output ? (1u << 9) : 0u);
}
// Every field tb_pin_cfg writes. pinctrl-msm changes one field at a time with a read-modify-write, so the bits
// outside this mask (bit 0 is set on every pin this board exposes) must be preserved.
#define TB_PIN_CFG_MASK (((7u) << 2) | ((3u) << 4) | ((7u) << 6) | (1u << 9))
#define TB_PIN_CFG_SPI (tb_pin_cfg(TB_FUNC_QUP11, TB_DRIVE_6MA, TB_PULL_NONE, 0))
#define TB_PIN_CFG_SPI_CS (tb_pin_cfg(TB_FUNC_QUP11, TB_DRIVE_6MA, TB_PULL_NONE, 0))
#define TB_PIN_CFG_RESET (tb_pin_cfg(TB_FUNC_GPIO, TB_DRIVE_6MA, TB_PULL_UP, 1))
#define TB_PIN_CFG_IRQ (tb_pin_cfg(TB_FUNC_GPIO, TB_DRIVE_6MA, TB_PULL_DOWN, 0))

// GCC page/offset split: register constants above are offsets from TB_GCC_BASE; the probe maps 4 KiB pages.
static inline uint64_t tb_gcc_page(uint32_t reg) { return TB_GCC_BASE + (reg & ~0xfffu); }
static inline uint32_t tb_gcc_offset(uint32_t reg) { return reg & 0xfffu; }

// Source clock rates by RCG source-select value for gcc_parent_map_0 (BI_TCXO = 0, GPLL0_OUT_MAIN = 1,
// GPLL0_OUT_EVEN = 6). SM7250 GPLL0 is 600 MHz, its even output 300 MHz; the GPLL0 rate follows from the driver's
// divider tables (100 MHz = main / 6, 75 MHz = even / 4) rather than an explicit documentation sentence.
static inline uint64_t tb_src_hz(unsigned sel) {
    return sel == 0 ? 19200000ULL : sel == 1 ? 600000000ULL : sel == 6 ? 300000000ULL : 0;
}
// clk-rcg2.c calc_rate: half-integer divider (hid), then M/N.
static inline uint64_t tb_calc_rate(uint64_t prate, uint32_t m, uint32_t n, unsigned mode, unsigned hid) {
    if (hid) { prate *= 2; prate /= hid + 1; }
    if (mode && n) { prate *= m; prate /= n; }
    return prate;
}
// Frequency of DFS level l from its CFG (source 10:8, mode 13:12, hid 4:0 on this 5-bit RCG), M and N. The N
// register stores the inverted value: n = ((~N) & mask) + m (mnd_width 16).
static inline uint64_t tb_dfs_level_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src = (cfg >> 8) & 7, mode = (cfg >> 12) & 3, hid = cfg & 0x1f;
    uint32_t m = 0, n = 0;
    if (mode) { m = mreg & 0xffff; n = ((~nreg) & 0xffff) + m; }
    return tb_calc_rate(tb_src_hz(src), m, n, mode, hid);
}
struct tb_clock_choice { unsigned level, div; uint64_t sclk_hz, actual_hz; };
// Choose the DFS level that gives the highest SPI rate not above target, with divider
// div = ceil(sclk / (oversampling * target)). Returns 0 on success, -1 when no level can reach the target.
static inline int tb_choose_clock(const uint64_t *level_hz, unsigned n, unsigned oversampling, unsigned target,
                                  struct tb_clock_choice *out) {
    int found = 0;
    if (!oversampling || !target) return -1;
    for (unsigned l = 0; l < n; ++l) {
        const uint64_t f = level_hz[l];
        if (!f) continue;
        uint64_t d = (f + (uint64_t)oversampling * target - 1) / ((uint64_t)oversampling * target);
        if (d < 1) d = 1;
        if (d > 0xfff) continue;
        const uint64_t actual = f / (oversampling * d);
        if (actual > target) continue;
        if (!found || actual > out->actual_hz || (actual == out->actual_hz && d < out->div)) {
            out->level = l; out->div = (unsigned)d; out->sclk_hz = f; out->actual_hz = actual; found = 1;
        }
    }
    return found ? 0 : -1;
}

// Core (x = short axis, y = long axis, both 0.1 panel pixels) to the HID raw convention used by NativeHidInput.cpp.
static inline int tb_clamp(int value, int low, int high) { return value < low ? low : value > high ? high : value; }
static inline int tb_hid_x_from_short(int short_tenths) {
    long long v = tb_clamp(short_tenths, 0, TB_PANEL_SHORT_TENTHS - 1);
    v = v * TB_HID_X_MAX / TB_PANEL_SHORT_TENTHS;
    if (TB_HID_MIRROR_X) v = TB_HID_X_MAX - 1 - v;
    return tb_clamp((int)v, 0, TB_HID_X_MAX - 1);
}
static inline int tb_hid_y_from_long(int long_tenths) {
    long long v = tb_clamp(long_tenths, 0, TB_PANEL_LONG_TENTHS - 1);
    v = v * TB_HID_Y_MAX / TB_PANEL_LONG_TENTHS;
    if (TB_HID_MIRROR_Y) v = TB_HID_Y_MAX - 1 - v;
    return tb_clamp((int)v, 0, TB_HID_Y_MAX - 1);
}
struct tb_hid_point { int x, y; };
static inline struct tb_hid_point tb_hid_from_panel(int short_tenths, int long_tenths) {
    const struct tb_hid_point point = {tb_hid_x_from_short(short_tenths), tb_hid_y_from_long(long_tenths)};
    return point;
}

#endif
