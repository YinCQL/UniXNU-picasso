// SPDX-License-Identifier: BSD-2-Clause-Patent
// picasso's Novatek NT36672C touch probe. `sheng-touch=<n>`:
//   1  read-only snapshot of the GCC branches, the SE7 DFS levels and the touch pins
//   2  vote the QUP clocks, wait for them, configure the pins, read the GENI engine identity
//   3  initialize GENI SPI, choose the serial clock, reset the touch bootloader, read the chip id
//   4  download the caller-supplied firmware, start scanning, decode THP frames and publish contacts
// Every stage reports through native_novatek_touch_* globals and a PICASSO_NVT line; the first failure stops the
// sequence. Board constants and their sources are in TouchBoard.h. No hardware run has validated this sequence yet.
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>          // nanoseconds_to_deadline for the timed wait in the frame loop
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "GeniSpi.h"
#include "NvtProtocol.h"
#include "NvtTouchCore.h"
#include "TouchBoard.h"
#include "TouchPublish.h"
}

static_assert(NVT_MATRIX_COLUMNS == NVT_COLUMNS && NVT_MATRIX_ROWS == NVT_ROWS,
              "protocol frame geometry and core matrix geometry differ");

extern "C" {
// The HID digitizer reads this snapshot under the seqlock. The names and array sizes are the contract used by
// drivers/NativeHidInput/NativeHidInput.cpp.
volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile int native_touch_xs[NVT_SLOTS], native_touch_ys[NVT_SLOTS], native_touch_ids[NVT_SLOTS];

// Entry points declared before their definitions (XNU builds with -Wmissing-prototypes).
// The sink storage is owned by PicassoBootProgressStubs.c; the probe only notifies.
void native_touch_notify(void) __attribute__((weak_import));
static void notifyThunk(void *) { if (native_touch_notify) native_touch_notify(); }
void native_novatek_touch_set_firmware(const uint8_t *data, size_t size);
void native_novatek_touch_probe(void);
void native_novatek_touch_boot_report(unsigned now);

// Probe diagnostics for the boot monitor.
volatile uint32_t native_novatek_touch_stage_req, native_novatek_touch_stage, native_novatek_touch_step;
volatile uint32_t native_novatek_touch_frames, native_novatek_touch_valid, native_novatek_touch_contacts;
volatile int native_novatek_touch_error;
volatile int native_novatek_touch_last_x = -1, native_novatek_touch_last_y = -1;
volatile uint32_t native_novatek_touch_id[6];
volatile uint32_t native_novatek_touch_dfshz[TB_DFS_LEVELS];
volatile uint32_t native_novatek_touch_irq_stats[3];   // [0] 0 not armed, 1 armed, 2 attach failed, [1] handled

// Firmware is supplied by the integrator (weak symbols or the setter below); no vendor blob is stored in this tree.
extern const uint8_t native_novatek_touch_firmware[] __attribute__((weak_import));
extern const uint32_t native_novatek_touch_firmware_size __attribute__((weak_import));
}

namespace {
static const uint8_t *gFirmware;
static size_t gFirmwareSize;
// sheng-touch-notlmm=1: the TLMM pages of the touch pins raise a synchronous external abort on this boot
// (GPIO6 tested), so leave the pins exactly as the bootloader left them and do not touch TLMM at all.
static bool gSkipTlmm;


struct Region {
    OSSharedPtr<IOMemoryMap> keep;
    uintptr_t va;
};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
// Warm-reset marker. IMEM keeps its contents across the reset that a fatal device access triggers, so the next
// boot can name the step that died even when the CDC console (saturated by the UI trace once userland starts)
// loses the last lines. The USB console owns the restart reason at 0x65c; 0x800 is a separate word.
#define TB_IMEM_BASE 0x146ab000ULL
#define TB_IMEM_MARKER 0x800
// Plain statics, not a Region: a function-local static of a class type would need a guard variable and a
// constructor, and the marker has to be usable from any thread at any time. The map is deliberately leaked.
static uintptr_t gMarkerVa;
static bool markerMap(void) {
    if (gMarkerVa) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(TB_IMEM_BASE, 0x1000);
    if (!memory) return false;
    IOMemoryMap *map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (!map) return false;
    gMarkerVa = static_cast<uintptr_t>(map->getVirtualAddress());
    return gMarkerVa != 0;
}
static void marker(uint32_t code) {
    if (!gMarkerVa && !markerMap()) return;
    *reinterpret_cast<volatile uint32_t *>(gMarkerVa + TB_IMEM_MARKER) = 0x4e560000u | (code & 0xffffu);
    __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t rd32(uintptr_t base, uint32_t reg) { return *reinterpret_cast<volatile uint32_t *>(base + reg); }
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

void ioDelayUs(void *, unsigned us) {
    if (us > 50) { IODelay(us); return; }
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : TB_XO_HZ) * us / 1000000 + 1;
    do { __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now)); } while (now - start < ticks);
}

struct Session {
    Region gccLow, gccQuv, gccCore, gccVote, tlmm, tlmmWest, tlmmFar, wrapper, se;
    struct geni_spi spi;
    uint8_t *tx, *rx, *frame;
    int16_t *matrix;
    struct nvt_chip chip;
    struct nvt_bus bus;
    struct nvt_touch_core *core;
    uint64_t levelHz[TB_DFS_LEVELS];
    unsigned level, div, modeUsed;
    uint64_t actualHz;
    uint32_t info[18];
    int identifyRc[4];
    uint8_t modeId[4][6];
};

// The touch pins live in two TLMM tiles, so there are two windows: EAST starts at TB_GPIO_RESET, WEST at
// TB_GPIO_SPI_CLK. Each accessor is relative to the first pin of its window.
uintptr_t tlmmPin(const Session &s, unsigned gpio) {
    if (gpio <= TB_GPIO_IRQ) {
        return s.tlmm.va + tb_tlmm_ctl(gpio) - tb_tlmm_ctl(TB_GPIO_RESET);
    }
    return s.tlmmWest.va + tb_tlmm_ctl(gpio) - tb_tlmm_ctl(TB_GPIO_SPI_CLK);
}

// pinctrl-msm changes one field at a time; the bits outside TB_PIN_CFG_MASK (bit 0 is set on every pin here) survive.
void pinConfig(Session &s, unsigned gpio, uint32_t cfg) {
    const uintptr_t pin = tlmmPin(s, gpio);
    wr32(pin, 0, (rd32(pin, 0) & ~TB_PIN_CFG_MASK) | cfg);
}

int busXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Session *s = static_cast<Session *>(ctx);
    return geni_spi_transfer(&s->spi, tx, rx, len, 0);
}
void busDelay(void *, unsigned ms) { IOSleep(ms); }

uint32_t ioRd(void *ctx, uint32_t reg) { return rd32(reinterpret_cast<uintptr_t>(ctx), reg); }
void ioWr(void *ctx, uint32_t reg, uint32_t value) { wr32(reinterpret_cast<uintptr_t>(ctx), reg, value); }

void fail(unsigned step, int code) {
    native_novatek_touch_step = step;
    if (!native_novatek_touch_error) native_novatek_touch_error = code ? code : -1;
    printf("PICASSO_NVT fail step=%u rc=%d\n", step, code);
}
void setStage(unsigned stage) { native_novatek_touch_stage = stage; }

// A branch with hardware clock gating (bit 1) may show CLK_OFF while idle; the others must be on.
bool swBranchRunning(uint32_t cbcr) { return !(cbcr & TB_CBCR_CLK_OFF) || (cbcr & 2u); }
bool clocksRunning(Session &s) {
    const uint32_t m_ahb = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_BRANCH_M_AHB));
    const uint32_t s_ahb = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_BRANCH_S_AHB));
    const uint32_t core = rd32(s.gccCore.va, tb_gcc_offset(TB_GCC_BRANCH_CORE));
    const uint32_t core2x = rd32(s.gccCore.va, tb_gcc_offset(TB_GCC_BRANCH_CORE_2X));
    const uint32_t s1 = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_BRANCH_S1));
    return swBranchRunning(m_ahb) && swBranchRunning(s_ahb) && !(core & TB_CBCR_CLK_OFF) &&
           !(core2x & TB_CBCR_CLK_OFF) && !(s1 & TB_CBCR_CLK_OFF);
}

void readIdentity(Session &s) {
    s.spi.se = (struct geni_io){reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    if (geni_spi_read_info(&s.spi)) return;
    s.info[12] = s.spi.info.qup_hw_version;
    s.info[13] = s.spi.info.fw_revision;
    s.info[14] = s.spi.info.if_disable;
    s.info[15] = s.spi.info.hw_param0;
}

// `sub` walks stage 1 one group at a time (sheng-touch-sub): 1 runs nothing, 2 adds the maps, 3 the GCC reads,
// 4 the TLMM reads, 5 the SE maps and the identity. The console lags behind a reset, so survival is the signal.
bool snapshot(Session &s, unsigned sub) {
    native_novatek_touch_step = 1;
    printf("PICASSO_NVT sub=%u map gcc\n", sub);
    if (sub < 2) { printf("PICASSO_NVT sub=%u done\n", sub); return true; }
    if (sub == 50 || sub == 51 || sub == 60 || sub == 61) {
        // TLMM calibration. Earlier probes read NORTH + 0x1000*gpio, which is a tile none of the touch pins live in:
        // the whole NORTH window returns zeros and drops writes, and its GPIO9 page raises a synchronous external
        // abort (ESR 0x96000010). pinctrl-lito.c gives each pin its tile, so read the touch pins at their own tile.
        // The write/readback on WEST pin 120 (an unreferenced pin) proves the window is live before trusting values.
        if (!mapRegion(TB_TLMM_BASE + TB_TLMM_WEST + tb_tlmm_ctl(120), TB_GPIO_STRIDE, s.tlmmFar)) { fail(1, -12); return false; }
        const uint32_t ctl120 = rd32(s.tlmmFar.va, 0);
        wr32(s.tlmmFar.va, 0, 0x000001c0u);
        const uint32_t rb = rd32(s.tlmmFar.va, 0);
        wr32(s.tlmmFar.va, 0, ctl120);
        printf("PICASSO_NVT tlmm west120 ctl=%08x rb=%08x back=%08x\n", ctl120, rb, rd32(s.tlmmFar.va, 0));
        static const unsigned pins[] = {TB_GPIO_RESET, TB_GPIO_IRQ, TB_GPIO_SPI_CLK, TB_GPIO_SPI_MISO,
                                        TB_GPIO_SPI_MOSI, TB_GPIO_SPI_CS};
        for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
            if (!mapRegion(tb_tlmm_pa(pins[i]), TB_GPIO_STRIDE, s.tlmmFar)) { fail(1, -12); return false; }
            printf("PICASSO_NVT tlmm pin=%u ctl=%08x io=%08x icfg=%08x istat=%08x\n", pins[i],
                   rd32(s.tlmmFar.va, 0), rd32(s.tlmmFar.va, TB_TLMM_IO_OFFSET),
                   rd32(s.tlmmFar.va, TB_TLMM_INTR_CFG_OFFSET), rd32(s.tlmmFar.va, TB_TLMM_INTR_STATUS_OFFSET));
        }
        printf("PICASSO_NVT sub=%u done\n", sub);
        return true;
    }
    marker(0x14);
    if (!mapRegion(TB_GCC_BASE + TB_GCC_LOW_PAGE, TB_GCC_PAGE_SIZE, s.gccLow)) { fail(1, -12); return false; }
    marker(0x15);
    if (!mapRegion(TB_GCC_BASE + TB_GCC_QUPV3_PAGE, TB_GCC_PAGE_SIZE, s.gccQuv)) { fail(1, -12); return false; }
    marker(0x16);
    if (!mapRegion(TB_GCC_BASE + TB_GCC_CORE_PAGE, TB_GCC_PAGE_SIZE, s.gccCore)) { fail(1, -12); return false; }
    marker(0x17);
    if (!mapRegion(TB_GCC_BASE + TB_GCC_VOTE_PAGE, TB_GCC_PAGE_SIZE, s.gccVote)) { fail(1, -12); return false; }
    marker(0x18);
    if (!gSkipTlmm &&
        (!mapRegion(TB_TLMM_BASE + TB_TLMM_EAST + tb_tlmm_ctl(TB_GPIO_RESET),
                    (TB_GPIO_IRQ - TB_GPIO_RESET + 1) * TB_GPIO_STRIDE, s.tlmm) ||
         !mapRegion(TB_TLMM_BASE + TB_TLMM_WEST + tb_tlmm_ctl(TB_GPIO_SPI_CLK),
                    (TB_GPIO_SPI_CS - TB_GPIO_SPI_CLK + 1) * TB_GPIO_STRIDE, s.tlmmWest))) {
        fail(1, -12);
        return false;
    }
    marker(0x19);
    printf("PICASSO_NVT map gcc ok g=%08x q=%08x c=%08x v=%08x te=%08x tw=%08x\n",
           (unsigned)(s.gccLow.va >> 12), (unsigned)(s.gccQuv.va >> 12), (unsigned)(s.gccCore.va >> 12),
           (unsigned)(s.gccVote.va >> 12), (unsigned)(s.tlmm.va >> 12), (unsigned)(s.tlmmWest.va >> 12));
    if (sub < 3) { printf("PICASSO_NVT sub=%u done\n", sub); return true; }
    printf("PICASSO_NVT read gcc\n");
    marker(0x1a);
    uint32_t *v = s.info;
    v[0] = rd32(s.gccVote.va, tb_gcc_offset(TB_GCC_VOTE_ENABLE));
    v[1] = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_BRANCH_M_AHB));
    v[2] = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_BRANCH_S_AHB));
    v[3] = rd32(s.gccCore.va, tb_gcc_offset(TB_GCC_BRANCH_CORE));
    v[4] = rd32(s.gccCore.va, tb_gcc_offset(TB_GCC_BRANCH_CORE_2X));
    v[5] = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_BRANCH_S1));
    v[6] = rd32(s.gccLow.va, tb_gcc_offset(TB_GCC_GPLL0_MODE));
    v[7] = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_DFS_S1_RCG + TB_RCG_DFSR_CMD));
    v[8] = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_DFS_S1_RCG + TB_RCG_CFG));
    for (unsigned l = 0; l < TB_DFS_LEVELS; ++l) {
        const uint32_t cfg = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_DFS_S1_RCG + TB_RCG_PERF_CFG(l)));
        const uint32_t m = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_DFS_S1_RCG + TB_RCG_PERF_M(l)));
        const uint32_t n = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_DFS_S1_RCG + TB_RCG_PERF_N(l)));
        s.levelHz[l] = tb_dfs_level_hz(cfg, m, n);
        native_novatek_touch_dfshz[l] = static_cast<uint32_t>(s.levelHz[l]);
    }
    marker(0x1b);
    printf("PICASSO_NVT read gcc ok\n");
    if (sub < 4) { printf("PICASSO_NVT sub=%u done\n", sub); return true; }
    printf("PICASSO_NVT read tlmm notlmm=%u\n", gSkipTlmm ? 1u : 0u);
    if (gSkipTlmm) {
        printf("PICASSO_NVT tlmm skipped\n");
    } else if (sub >= 40) {
        // Progressive TLMM probes 40..44: a page far from the touch pins first, then one touch pin per step.
        if (!mapRegion(TB_TLMM_BASE + 100 * TB_GPIO_STRIDE, TB_GPIO_STRIDE, s.tlmmFar)) { fail(1, -12); return false; }
        printf("PICASSO_NVT tlmm100=%08x\n", rd32(s.tlmmFar.va, 0));
        if (sub == 40) { printf("PICASSO_NVT sub=%u done\n", sub); return true; }
        v[9] = rd32(tlmmPin(s, TB_GPIO_SPI_CLK), 0);
        printf("PICASSO_NVT tlmm6=%08x\n", v[9]);
        if (sub == 41) { printf("PICASSO_NVT sub=%u done\n", sub); return true; }
        v[10] = rd32(tlmmPin(s, TB_GPIO_SPI_MISO), 0);
        printf("PICASSO_NVT tlmm7=%08x\n", v[10]);
        if (sub == 42) { printf("PICASSO_NVT sub=%u done\n", sub); return true; }
        v[11] = rd32(tlmmPin(s, TB_GPIO_SPI_MOSI), 0);
        printf("PICASSO_NVT tlmm8=%08x\n", v[11]);
        if (sub == 43) { printf("PICASSO_NVT sub=%u done\n", sub); return true; }
        v[16] = rd32(tlmmPin(s, TB_GPIO_IRQ), TB_TLMM_INTR_CFG_OFFSET);
        printf("PICASSO_NVT tlmm5irq=%08x\n", v[16]);
        printf("PICASSO_NVT sub=%u done\n", sub);
    } else {
        v[9] = rd32(tlmmPin(s, TB_GPIO_SPI_CLK), 0);
        v[10] = rd32(tlmmPin(s, TB_GPIO_SPI_MISO), 0);
        v[11] = rd32(tlmmPin(s, TB_GPIO_SPI_MOSI), 0);
        v[16] = rd32(tlmmPin(s, TB_GPIO_IRQ), TB_TLMM_INTR_CFG_OFFSET);
        printf("PICASSO_NVT read tlmm ok %08x %08x %08x irq=%08x\n", v[9], v[10], v[11], v[16]);
    }
    if (sub < 5) { printf("PICASSO_NVT sub=%u done\n", sub); return true; }
    printf("PICASSO_NVT map se\n");
    marker(0x1c);
    if (!mapRegion(TB_QUP_WRAPPER_BASE, TB_QUP_WRAPPER_SIZE, s.wrapper)) { fail(1, -12); return false; }
    marker(0x1d);
    if (!mapRegion(TB_SE_BASE, TB_SE_SIZE, s.se)) { fail(1, -12); return false; }
    marker(0x1e);
    printf("PICASSO_NVT map se ok w=%08x s=%08x\n", (unsigned)(s.wrapper.va >> 12),
           (unsigned)(s.se.va >> 12));
    if (clocksRunning(s)) {
        marker(0x1f);
        printf("PICASSO_NVT clocks running\n");
        readIdentity(s);
        marker(0x20);
    }
    marker(0x21);
    printf("PICASSO_NVT stage1 vote=%08x m_ahb=%08x s_ahb=%08x core=%08x core2x=%08x s1=%08x gpll0=%08x "
           "dfs=%08x qh=%08x fw=%08x if=%08x hw=%08x\n",
           v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], s.info[12], s.info[13], s.info[14], s.info[15]);
    return true;
}

bool enableClocks(Session &s) {
    native_novatek_touch_step = 2;
    marker(0x31);
    // GPLL0 feeds the DFS levels: a shared vote, then wait for lock.
    wr32(s.gccVote.va, tb_gcc_offset(TB_GCC_GPLL0_ENABLE),
         rd32(s.gccVote.va, tb_gcc_offset(TB_GCC_GPLL0_ENABLE)) | 1u);
    bool locked = false;
    for (unsigned i = 0; i < 10000 && !locked; ++i) {
        locked = (rd32(s.gccLow.va, tb_gcc_offset(TB_GCC_GPLL0_MODE)) & TB_GPLL0_LOCK) != 0;
        if (!locked) IODelay(10);
    }
    marker(0x32);
    wr32(s.gccVote.va, tb_gcc_offset(TB_GCC_VOTE_ENABLE),
         rd32(s.gccVote.va, tb_gcc_offset(TB_GCC_VOTE_ENABLE)) | TB_VOTE_ALL);
    bool running = false;
    for (unsigned i = 0; i < 2000 && !running; ++i) {
        running = clocksRunning(s);
        if (!running) IODelay(10);
    }
    marker(0x33);
    s.info[0] = rd32(s.gccVote.va, tb_gcc_offset(TB_GCC_VOTE_ENABLE));
    s.info[1] = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_BRANCH_M_AHB));
    s.info[2] = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_BRANCH_S_AHB));
    s.info[3] = rd32(s.gccCore.va, tb_gcc_offset(TB_GCC_BRANCH_CORE));
    s.info[4] = rd32(s.gccCore.va, tb_gcc_offset(TB_GCC_BRANCH_CORE_2X));
    s.info[5] = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_BRANCH_S1));
    printf("PICASSO_NVT stage2 gpll0=%08x running=%u\n", s.info[6], running ? 1u : 0u);
    if (!locked || !running) { fail(2, -110); return false; }
    marker(0x34);
    // QUP SPI pins and the two touch GPIOs. Reset is driven high here; the probe pulses it in stage 3.
    // The far TLMM pages raise a synchronous external abort on this boot (see gSkipTlmm), so leave the pins
    // exactly as the bootloader configured them when sheng-touch-notlmm is set.
    if (!gSkipTlmm) {
        // The bootloader leaves the SPI pins as plain GPIO inputs (ctl 0x1) and drives the reset line low
        // (ctl 0x201, io 0), so the controller answers nothing until the pins are muxed and reset is released.
        pinConfig(s, TB_GPIO_SPI_CLK, TB_PIN_CFG_SPI);
        pinConfig(s, TB_GPIO_SPI_MISO, TB_PIN_CFG_SPI);
        pinConfig(s, TB_GPIO_SPI_MOSI, TB_PIN_CFG_SPI);
        pinConfig(s, TB_GPIO_SPI_CS, TB_PIN_CFG_SPI_CS);
        pinConfig(s, TB_GPIO_RESET, TB_PIN_CFG_RESET);
        pinConfig(s, TB_GPIO_IRQ, TB_PIN_CFG_IRQ);
        wr32(tlmmPin(s, TB_GPIO_RESET), TB_TLMM_IO_OFFSET, 1);
        s.info[9] = rd32(tlmmPin(s, TB_GPIO_SPI_CLK), 0);
        s.info[10] = rd32(tlmmPin(s, TB_GPIO_SPI_MISO), 0);
        s.info[11] = rd32(tlmmPin(s, TB_GPIO_SPI_MOSI), 0);
        s.info[16] = rd32(tlmmPin(s, TB_GPIO_IRQ), TB_TLMM_INTR_CFG_OFFSET);
    }
    readIdentity(s);
    printf("PICASSO_NVT stage2 pins=%08x %08x %08x irq=%08x se=%08x\n",
           s.info[9], s.info[10], s.info[11], s.info[16], s.info[12]);
    return true;
}

bool openBus(Session &s, unsigned hz) {
    native_novatek_touch_step = 3;
    // Reset pulse: low, TB_RESET_LOW_US, high, TB_RESET_SETTLE_MS.
    // Without TLMM the reset line cannot be pulsed; the panel keeps whatever the bootloader left (released).
    if (!gSkipTlmm) {
        wr32(tlmmPin(s, TB_GPIO_RESET), TB_TLMM_IO_OFFSET, 0);
        IODelay(TB_RESET_LOW_US);
        wr32(tlmmPin(s, TB_GPIO_RESET), TB_TLMM_IO_OFFSET, 1);
        IOSleep(TB_RESET_SETTLE_MS);
    }
    marker(0x41);

    s.spi.poll_limit = 400000;
    s.spi.poll_only = 1;                 // status registers stay pollable; no unhandled engine IRQ reaches the GIC
    if (s.spi.info.fifo_disabled) {
        // GPI DMA is not implemented here. The SMMU stream id and bases are in TouchBoard.h for the integrator.
        fail(30, GENI_ERR_NOTSUP);
        return false;
    }
    int r = geni_spi_init(&s.spi);
    if (r) { fail(30, r); return false; }
    marker(0x42);

    for (unsigned l = 0; l < TB_DFS_LEVELS; ++l) {
        const uint32_t cfg = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_DFS_S1_RCG + TB_RCG_PERF_CFG(l)));
        const uint32_t m = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_DFS_S1_RCG + TB_RCG_PERF_M(l)));
        const uint32_t n = rd32(s.gccQuv.va, tb_gcc_offset(TB_GCC_DFS_S1_RCG + TB_RCG_PERF_N(l)));
        s.levelHz[l] = tb_dfs_level_hz(cfg, m, n);
        native_novatek_touch_dfshz[l] = static_cast<uint32_t>(s.levelHz[l]);
    }
    struct tb_clock_choice choice = {};
    bool fallback = false;
    if (tb_choose_clock(s.levelHz, TB_DFS_LEVELS, s.spi.oversampling, hz, &choice)) {
        // DFS registers not in the expected shape: run the engine from level 0 at whatever rate it reports.
        const uint64_t sclk = s.levelHz[0] ? s.levelHz[0] : TB_XO_HZ;
        choice.level = 0;
        choice.div = static_cast<unsigned>((sclk + (uint64_t)s.spi.oversampling * hz - 1) /
                                           ((uint64_t)s.spi.oversampling * hz));
        if (!choice.div) choice.div = 1;
        choice.sclk_hz = sclk;
        choice.actual_hz = sclk / (s.spi.oversampling * choice.div);
        fallback = true;
    }
    s.level = choice.level;
    s.div = choice.div;
    s.actualHz = choice.actual_hz;
    r = geni_spi_set_clock(&s.spi, hz, static_cast<unsigned>(choice.sclk_hz), choice.level);
    if (r) { fail(31, r); return false; }
    marker(0x43);
    r = geni_spi_setup(&s.spi, TB_SPI_MODE, 0);
    if (r) { fail(31, r); return false; }
    marker(0x44);

    s.tx = static_cast<uint8_t *>(IOMalloc(NVT_TRANSFER_LEN + 8));
    s.rx = static_cast<uint8_t *>(IOMalloc(NVT_READ_MAX + 2));
    s.frame = static_cast<uint8_t *>(IOMalloc(NVT_THP_FRAME_LEN));
    s.matrix = static_cast<int16_t *>(IOMalloc(NVT_MATRIX_NODES * sizeof(int16_t)));
    s.core = static_cast<struct nvt_touch_core *>(IOMalloc(sizeof(struct nvt_touch_core)));
    if (!s.tx || !s.rx || !s.frame || !s.matrix || !s.core) { fail(32, -12); return false; }
    nvt_touch_reset(s.core);

    s.bus = (struct nvt_bus){static_cast<void *>(&s), busXfer, busDelay};
    s.chip = (struct nvt_chip){};
    s.chip.bus = &s.bus;
    s.chip.scratch = s.tx;
    s.chip.rxbuf = s.rx;
    s.chip.reset_retries = 60;           // vendor: 10 polls of 10 ms; longer here to tell "slow" from "never"

    unsigned forced = 4;
    PE_parse_boot_argn("sheng-touch-mode", &forced, sizeof forced);
    static const unsigned order[4] = {0, 3, 1, 2};
    int rc = NVT_ERR_CHIP;
    for (unsigned i = 0; i < 4 && rc; ++i) {
        const unsigned mode = forced < 4 ? forced : order[i];
        geni_spi_setup(&s.spi, ((mode & 1) ? GENI_SPI_CPHA : 0) | ((mode & 2) ? GENI_SPI_CPOL : 0), 0);
        s.chip.map = nullptr;
        s.chip.hw_crc = 0;
        memset(s.chip.id, 0, sizeof(s.chip.id));
        rc = nvt_identify(&s.chip);
        memcpy(s.modeId[i], s.chip.id, sizeof(s.chip.id));
        s.identifyRc[i] = rc;
        s.modeUsed = mode;
        if (forced < 4) break;
    }
    for (unsigned i = 0; i < sizeof(s.chip.id); ++i) native_novatek_touch_id[i] = s.chip.id[i];
    printf("PICASSO_NVT stage3 sclk=%uHz div=%u level=%u fallback=%u id=%02x%02x%02x%02x%02x%02x mode=%u rc=%d "
           "hw=%08x/%08x fifo=%u/%u\n",
           static_cast<unsigned>(s.actualHz), s.div, s.level, fallback ? 1u : 0u,
           s.chip.id[0], s.chip.id[1], s.chip.id[2], s.chip.id[3], s.chip.id[4], s.chip.id[5],
           s.modeUsed, rc, s.spi.info.hw_param0, s.spi.info.hw_param1, s.spi.info.tx_fifo_depth,
           s.spi.info.rx_fifo_depth);
    marker(0x45);
    if (rc) { fail(33, rc); return false; }
    return true;
}

static volatile uint32_t gIrqEvents;
static volatile uint32_t *gIrqPin;
static void touchIrq(void *) {
    if (!gIrqPin) return;
    if (!(gIrqPin[TB_TLMM_INTR_STATUS_OFFSET / 4] & 1)) {
        native_novatek_touch_irq_stats[2] = native_novatek_touch_irq_stats[2] + 1;
        return;
    }
    gIrqPin[TB_TLMM_INTR_STATUS_OFFSET / 4] = 0;
    native_novatek_touch_irq_stats[1] = native_novatek_touch_irq_stats[1] + 1;
    __atomic_fetch_add(&gIrqEvents, 1, __ATOMIC_RELEASE);
    thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gIrqEvents)));
}
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *)
    __attribute__((weak_import));
bool armTouchIrq(Session &s) {
    if (gSkipTlmm) { native_novatek_touch_irq_stats[0] = 3; return false; }   // no TLMM: poll only
    volatile uint32_t *pin = reinterpret_cast<volatile uint32_t *>(tlmmPin(s, TB_GPIO_IRQ));
    gIrqPin = pin;
    pin[TB_TLMM_INTR_CFG_OFFSET / 4] = TB_TLMM_INTR_CFG & ~TB_TLMM_INTR_ENABLE;
    pin[TB_TLMM_INTR_STATUS_OFFSET / 4] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    if (!armcpu_spi_attach) { native_novatek_touch_irq_stats[0] = 2; return false; }
    const int rc = armcpu_spi_attach(TB_TLMM_SUMMARY_INTID, false, touchIrq, nullptr, nullptr, nullptr);
    if (rc) { native_novatek_touch_irq_stats[0] = 2; return false; }
    pin[TB_TLMM_INTR_CFG_OFFSET / 4] = TB_TLMM_INTR_CFG;   // enable last
    native_novatek_touch_irq_stats[0] = 1;
    return true;
}

const uint8_t *probeFirmware(size_t &size) {
    if (gFirmware && gFirmwareSize) { size = gFirmwareSize; return gFirmware; }
    if (&native_novatek_touch_firmware && &native_novatek_touch_firmware_size &&
        native_novatek_touch_firmware_size) {
        size = native_novatek_touch_firmware_size;
        return native_novatek_touch_firmware;
    }
    size = 0;
    return nullptr;
}

void publish(const struct nvt_contact *out, int n) {
    int xs[NVT_SLOTS], ys[NVT_SLOTS], ids[NVT_SLOTS];
    for (int i = 0; i < n && i < NVT_SLOTS; ++i) {
        const struct tb_hid_point point = tb_hid_from_panel(out[i].x, out[i].y);
        xs[i] = point.x;
        ys[i] = point.y;
        ids[i] = out[i].tracking_id;
    }
    struct nvt_touch_publication pub;
    pub.seq = &native_touch_seq;
    pub.count = &native_touch_count;
    pub.xs = native_touch_xs;
    pub.ys = native_touch_ys;
    pub.ids = native_touch_ids;
    pub.slots = NVT_SLOTS;
    pub.sink = &notifyThunk;
    pub.sink_context = nullptr;
    nvt_touch_publish(&pub, xs, ys, ids, static_cast<unsigned>(n));
}

// Decode the panel's ten 6-byte records (vendor nvt_ts_work_func) into contacts in the publication convention
// (0.1 panel pixels). An idle panel simply has no record with a valid finger id.
void parseReport(const uint8_t *report, struct nvt_contact *out, int &n) {
    n = 0;
    for (unsigned i = 0; i < NVT_REPORT_POINTS; ++i) {
        const unsigned pos = 6 * i;
        const unsigned id = report[pos] >> 3;
        const unsigned state = report[pos] & 0x07u;
        if (!id || id > NVT_REPORT_POINTS || (state != 1 && state != 2)) continue;
        const int x = (report[pos + 1] << 4) | (report[pos + 3] >> 4);
        const int y = (report[pos + 2] << 4) | (report[pos + 3] & 0x0f);
        if (x > TB_PANEL_WIDTH_PX || y > TB_PANEL_HEIGHT_PX) continue;
        struct nvt_contact &c = out[n++];
        bzero(&c, sizeof(c));
        c.slot = static_cast<int>(i);
        c.tracking_id = static_cast<int>(id);
        c.x = x * 10;
        c.y = y * 10;
    }
}

void frameLoop(Session &s) {
    native_novatek_touch_step = 5;
    bool irqArmed = false;
    unsigned pollOnly = 0;
    PE_parse_boot_argn("sheng-touch-poll", &pollOnly, sizeof pollOnly);
    if (!pollOnly) irqArmed = armTouchIrq(s);
    printf("PICASSO_NVT stage4 loop irq=%u\n", irqArmed ? 1u : 0u);
    uint32_t seen = gIrqEvents;
    unsigned printed = 0, changes = 0;
    struct nvt_contact last[NVT_REPORT_POINTS];
    int lastN = 0;
    for (;;) {
        if (irqArmed) {
            // Interrupt driven, with a timed fallback: a pin-interrupt polarity the panel does not use would park
            // this thread forever, so the wake-up also happens every 20 ms and reads the panel anyway.
            uint64_t deadline = 0;
            nanoseconds_to_deadline(20000000ULL, &deadline);
            assert_wait_deadline(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gIrqEvents)), THREAD_UNINT, deadline);
            if (gIrqEvents != seen) clear_wait(current_thread(), THREAD_AWAKENED);
            else thread_block(THREAD_CONTINUE_NULL);
            seen = gIrqEvents;
        }
        // The panel's report format (vendor nvt_ts_work_func): ten 6-byte records at 1 + 6*i.
        uint8_t report[NVT_REPORT_LEN];
        if (nvt_read_report(&s.chip, report)) { fail(50, NVT_ERR_BUS); return; }
        ++native_novatek_touch_valid;
        struct nvt_contact out[NVT_REPORT_POINTS];
        int n = 0;
        parseReport(report, out, n);
        if (n != lastN || memcmp(out, last, sizeof(out[0]) * static_cast<size_t>(n)) != 0) {
            // A sample that disagrees with a second read was taken while the firmware rewrote the buffer. Drop it;
            // publishing it would show up as a phantom press or lift, and UIKit work is expensive on this port.
            uint8_t again[NVT_REPORT_LEN];
            struct nvt_contact out2[NVT_REPORT_POINTS];
            int n2 = 0;
            if (nvt_read_report(&s.chip, again)) { fail(50, NVT_ERR_BUS); return; }
            parseReport(again, out2, n2);
            if (n2 != n || memcmp(out, out2, sizeof(out[0]) * static_cast<size_t>(n)) != 0) {
                continue;   // torn: the next wake reads a settled report
            }
            memcpy(last, out, sizeof(out[0]) * static_cast<size_t>(n));
            lastN = n;
            ++native_novatek_touch_frames;
            native_novatek_touch_contacts = static_cast<uint32_t>(n);
            if (n > 0) {
                const struct tb_hid_point point = tb_hid_from_panel(out[0].x, out[0].y);
                native_novatek_touch_last_x = point.x;
                native_novatek_touch_last_y = point.y;
            }
            publish(out, n);
            ++changes;
            if (printed < 6) {
                printf("PICASSO_NVT report contacts=%d%s%s\n", n, n > 0 ? "" : " (lift)",
                       n > 0 ? " first" : "");
                ++printed;
            }
            if (n > 0 && printed <= 6) {
                printf("PICASSO_NVT point id=%d x=%d y=%d\n", out[0].tracking_id, out[0].x, out[0].y);
                ++printed;
            }
        }
        if (!irqArmed) IOSleep(pollOnly ? 4 : 10);
    }
}

bool runStage4(Session &s, unsigned stage) {
    native_novatek_touch_step = 4;
    size_t fwSize = 0;
    const uint8_t *firmware = probeFirmware(fwSize);
    if (!firmware || !fwSize) {
        printf("PICASSO_NVT no firmware: link native_novatek_touch_firmware[]/native_novatek_touch_firmware_size "
               "or call native_novatek_touch_set_firmware() before the probe (vendor files "
               "novatek_nt36672c_g7a_fw01/02/03.bin)\n");
        fail(40, -2);
        return false;
    }
    struct nvt_fw fw;
    // Accepts a bare 0x20000-byte image, the true 0x22000-byte stock object (image + 0x2000 trailer), or a carve
    // taken from the image's end marker (which starts 0x2000 early).
    int rc = nvt_fw_parse_stock(&fw, firmware, fwSize);
    if (rc) { fail(40, rc); return false; }
    printf("PICASSO_NVT fw in=%u skip=%u size=%u write=%u ver=%02x/%02x parts=%u ilm=%u@%x dlm=%u@%x\n",
           static_cast<unsigned>(fwSize), static_cast<unsigned>(fw.source_offset),
           static_cast<unsigned>(fw.size), static_cast<unsigned>(fw.write_size), fw.fw_ver, fw.fw_ver_bar,
           fw.count, fw.part[0].size, fw.part[0].bin_addr, fw.part[1].size, fw.part[1].bin_addr);
    rc = nvt_download_firmware(&s.chip, &fw);
    if (rc) {
        printf("PICASSO_NVT download failed rc=%d try=%u stage=%u state=%02x crc=%02x sram=%u/%u/%u "
               "part=%u at=%x left=%u\n", rc, s.chip.dl_attempt, s.chip.dl_stage, s.chip.dl_reset_state[1],
               s.chip.crc_flags, s.chip.sram_ok[0], s.chip.sram_ok[1], s.chip.sram_ok[2], s.chip.dl_part,
               s.chip.dl_sram, s.chip.dl_left);
        if (s.spi.snap_valid) {
            printf("PICASSO_NVT spi fail irq=%08x st=%08x txfs=%08x rxfs=%08x len=%u txrem=%u rxrem=%u\n",
                   s.spi.snap_irq, s.spi.snap_status, s.spi.snap_txfs, s.spi.snap_rxfs,
                   s.spi.snap_len, s.spi.snap_tx_rem, s.spi.snap_rx_rem);
        }
        fail(41, rc);
        return false;
    }
    unsigned legacy = 0;
    PE_parse_boot_argn("sheng-touch-legacy-start", &legacy, sizeof legacy);
    struct nvt_fw_info info;
    rc = nvt_start_scanning(&s.chip, &info, legacy != 0);
    printf("PICASSO_NVT fwinfo rc=%d ver=%u %ux%u max=%ux%u\n", rc, info.fw_ver, info.x_num, info.y_num,
           info.abs_x_max, info.abs_y_max);
    // Did the image actually land in SRAM, and what does the boot ROM's CRC bank say about it?
    (void)nvt_read_bld_crc(&s.chip, &fw);
    (void)nvt_verify_sram(&s.chip, &fw);
    printf("PICASSO_NVT sram ok=%u/%u/%u rst=%02x crc=%02x g=%08x/%08x r=%08x/%08x b=%08x/%08x\n",
           s.chip.sram_ok[0], s.chip.sram_ok[1], s.chip.sram_ok[2], s.chip.dl_reset_state[1], s.chip.crc_flags,
           s.chip.crc_golden[0], s.chip.crc_golden[1], s.chip.crc_result[0], s.chip.crc_result[1],
           s.chip.crc_bin[0], s.chip.crc_bin[1]);
    if (rc || info.defaulted) {
        printf("PICASSO_NVT fwinfo raw=%02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x%02x\n",
               s.chip.fwinfo_raw[0], s.chip.fwinfo_raw[1], s.chip.fwinfo_raw[2], s.chip.fwinfo_raw[3],
               s.chip.fwinfo_raw[4], s.chip.fwinfo_raw[5], s.chip.fwinfo_raw[6], s.chip.fwinfo_raw[7],
               s.chip.fwinfo_raw[8], s.chip.fwinfo_raw[9], s.chip.fwinfo_raw[10], s.chip.fwinfo_raw[11],
               s.chip.fwinfo_raw[12], s.chip.fwinfo_raw[13], s.chip.fwinfo_raw[14], s.chip.fwinfo_raw[15],
               s.chip.fwinfo_raw[16]);
    }
    if (rc && rc != NVT_ERR_FW) { fail(42, rc); return false; }
    if (info.defaulted || info.x_num != NVT_MATRIX_COLUMNS || info.y_num != NVT_MATRIX_ROWS) {
        // A geometry mismatch would decode the matrix with the wrong stride; refuse rather than report garbage.
        fail(43, -22);
        return false;
    }
    setStage(4);
    if (stage >= 4) frameLoop(s);
    return true;
}
}  // namespace

// Optional explicit firmware supply; the weak symbols above remain the build-time path.
extern "C" void native_novatek_touch_set_firmware(const uint8_t *data, size_t size) {
    gFirmwareSize = size;
    __atomic_store_n(&gFirmware, data, __ATOMIC_RELEASE);
}

namespace {
// Shared body of the thread and the synchronous modes. `where` names the context in the log lines.
void probeBody(unsigned stage, const char *where) {
    marker(0x11);
    Session *s = static_cast<Session *>(IOMalloc(sizeof(Session)));
    if (!s) { fail(0, -12); return; }
    bzero(s, sizeof(*s));
    marker(0x12);
    unsigned sub = 5;
    PE_parse_boot_argn("sheng-touch-sub", &sub, sizeof sub);
    unsigned notlmm = 0;
    PE_parse_boot_argn("sheng-touch-notlmm", &notlmm, sizeof notlmm);
    gSkipTlmm = notlmm != 0;
    printf("PICASSO_NVT %s start stage=%u sub=%u notlmm=%u\n", where, stage, sub, gSkipTlmm ? 1u : 0u);
    unsigned hz = TB_SPI_DEFAULT_HZ;
    PE_parse_boot_argn("sheng-touch-hz", &hz, sizeof hz);
    if (hz < TB_SPI_MIN_HZ || hz > TB_SPI_MAX_HZ) hz = TB_SPI_DEFAULT_HZ;
    do {
        if (!snapshot(*s, sub)) break;
        setStage(1);
        if (stage < 2) break;
        if (!enableClocks(*s)) break;
        setStage(2);
        if (stage < 3) break;
        if (!openBus(*s, hz)) break;
        setStage(3);
        if (stage < 4) break;
        (void)runStage4(*s, stage);
    } while (0);
    printf("PICASSO_NVT %s end stage=%u step=%u error=%d\n", where, native_novatek_touch_stage,
           native_novatek_touch_step, native_novatek_touch_error);
}

void touchThread(void *, wait_result_t) {
    printf("PICASSO_NVT thread entry\n");
    thread_set_thread_name(current_thread(), "picasso-nvt");
    probeBody(native_novatek_touch_stage_req, "thread");
}
}  // namespace

// Callable from the boot monitor. `sheng-touch=<n>` selects the last stage to run; without it the probe does nothing.
extern "C" void native_novatek_touch_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("sheng-touch", &stage, sizeof stage) || stage < 1) return;
    if (native_novatek_touch_stage_req) return;
    if (stage > 4) stage = 4;
    marker(0x01);
    native_novatek_touch_stage_req = stage;
    // sheng-touch-sync=1 runs stage 1 in the caller (the boot monitor) instead of a new kernel thread. Stage 1
    // is bounded work, and this separates "the thread context is the problem" from "the MMIO is the problem".
    unsigned sync = 0;
    PE_parse_boot_argn("sheng-touch-sync", &sync, sizeof sync);
    if (sync) {
        if (stage > 1) stage = 1;
        probeBody(stage, "sync");
        return;
    }
    thread_t thread = nullptr;
    marker(0x02);
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_novatek_touch_error = -12;
    marker(0x03);
}

// Called by the boot monitor early in the pass; reports and clears the marker left by the previous boot. Runs
// before userland starts, while the CDC console is still quiet enough to keep the line.
extern "C" void native_novatek_touch_boot_report(unsigned now) {
    static unsigned reported;
    if (reported || now < 30) return;
    reported = 1;
    if (!markerMap()) { printf("PICASSO_NVT imem map failed\n"); return; }
    volatile uint32_t *slot = reinterpret_cast<volatile uint32_t *>(gMarkerVa + TB_IMEM_MARKER);
    const uint32_t previous = *slot;
    *slot = 0x4e56fffeu;
    __asm__ volatile("dsb sy" ::: "memory");
    const uint32_t readback = *slot;
    *slot = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    printf("PICASSO_NVT imem prev=%08x rb=%08x now=%u\n", previous, readback, now);
}
