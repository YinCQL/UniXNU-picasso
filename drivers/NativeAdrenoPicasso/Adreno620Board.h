/* SPDX-License-Identifier: MIT */
#ifndef ADRENO620_BOARD_H
#define ADRENO620_BOARD_H
#include <stddef.h>
#include <stdint.h>

/* Platform inputs for the Adreno 620 in the Redmi K30 5G ("picasso", Qualcomm
 * SM7250). Every value below is copied from one of:
 *   - /tmp/picasso.dts (decompiled stock device tree; line numbers in the
 *     comments), or
 *   - the pinned Linux/msm sources under UniXNU/.cache/adreno-source, or
 *   - the lito clock bindings in the stock picasso kernel tree.
 * This file is a description, not a driver: it maps nothing, powers nothing and
 * touches no register. The staged probe and the main NativeAdreno driver consume
 * these tables; a caller that cannot trace a value to its comment must not use
 * it. The DTS reg properties are big-endian cells; parseReg/parseIommus mirror
 * NativeAdrenoBoard's semantics so a board can be built from the live property
 * bytes instead of the constants when the provider offers them. */
namespace adreno620 {

/* ---------------------------------------------------------------------- */
/* MMIO windows                                                            */
/* ---------------------------------------------------------------------- */

struct Window {
    const char *name;
    uint64_t base;
    uint64_t bytes;
};

enum WindowId : unsigned {
    /* msm_gpu reg/reg-names, DTS line 13510. */
    WindowGpu = 0,          /* kgsl_3d0_reg_memory */
    WindowCxDbgc,           /* cx_dbgc */
    WindowRscc,             /* rscc: RSCC / RSC controller */
    WindowIsense,           /* isense_cntl */
    /* gmu reg/reg-names, DTS line 13914. */
    WindowGmu,              /* kgsl_gmu_reg (GMU core) */
    WindowGmuPdcCfg,        /* kgsl_gmu_pdc_cfg */
    WindowGmuPdcSeq,        /* kgsl_gmu_pdc_seq */
    /* kgsl_smmu reg/reg-names, DTS line 10670. */
    WindowSmmu,             /* base */
    WindowSmmuTcu,          /* tcu-base */
    /* Clock controllers that carry the GPU branches and GDSCs. */
    WindowGpucc,            /* DTS line 3479 */
    WindowGcc,              /* DTS line 3431 */
    WindowCount
};

const Window *windows(size_t *count);
const Window &window(WindowId id);

/* ---------------------------------------------------------------------- */
/* Interrupts                                                              */
/* ---------------------------------------------------------------------- */

struct Interrupt {
    const char *name;
    uint32_t spi;           /* DTS interrupt number cell (type 0 = SPI) */
    uint32_t flags;         /* DTS trigger cell: 4 = level, active high */
};

enum InterruptId : unsigned {
    InterruptGpu = 0,       /* msm_gpu, DTS line 13516: <0 0x12c 4> */
    InterruptHfi,           /* gmu, DTS line 13919: <0 0x130 4> */
    InterruptGmu,           /* gmu, DTS line 13919: <0 0x131 4> */
    InterruptSmmuGlobal,    /* kgsl_smmu first (global) interrupt, 0x2a1 */
    InterruptSmmuCbFirst,   /* first context-bank interrupt, 0x2a6; 8 follow */
    InterruptTbu0,          /* gfx_0_tbu, 0x2af */
    InterruptTbu1,          /* gfx_1_tbu, 0x2b0 */
    InterruptCount
};

const Interrupt *interrupts(size_t *count);
const Interrupt &interrupt(InterruptId id);

/* GIC INTID of an SPI as used by XNU's interrupt controllers. */
constexpr uint32_t gicIntId(uint32_t spi) { return spi + 32u; }

/* ---------------------------------------------------------------------- */
/* Clocks                                                                  */
/* ---------------------------------------------------------------------- */

enum class ClockController : uint8_t { Gcc, Gpucc };

struct Clock {
    const char *name;
    ClockController controller;
    uint32_t id;            /* dt-bindings clock id */
    uint32_t cbcr_offset;   /* CBCR inside the controller window, or NoOffset */
};

enum ClockId : unsigned {
    ClockGpuccAhb = 0,      /* GPU_CC_AHB_CLK = 0 */
    ClockGpuccCxGmu,        /* GPU_CC_CX_GMU_CLK = 3 */
    ClockGpuccCxo,          /* GPU_CC_CXO_CLK = 6 */
    ClockGpuccSmmuVote,     /* GPU_CC_HLOS1_VOTE_GPU_SMMU_CLK = 12 */
    ClockGccDdrssGpuAxi,    /* GCC_DDRSS_GPU_AXI_CLK = 13 ("mem_clk") */
    ClockGccGpuMemnocGfx,   /* GCC_GPU_MEMNOC_GFX_CLK = 35 */
    ClockGccGpuSnocDvmGfx,  /* GCC_GPU_SNOC_DVM_GFX_CLK = 36 */
    ClockGccGpuCfgAhb,      /* GCC_GPU_CFG_AHB_CLK = 31 */
    ClockCount
};

constexpr uint32_t NoOffset = 0xffffffffu;
const Clock *clocks(size_t *count);
const Clock &clock(ClockId id);

/* GPUCC/GCC branch offsets used by the read/write stage. They are the
 * SM7250/lito offsets the existing NativeAdrenoService.cpp already probes
 * (drivers/NativeAdreno/NativeAdrenoService.cpp, logProbeGpucc/logProbeGcc),
 * plus gcc_ddrss_gpu_axi_clk at 0x71154 from gcc-lito.c (the "axi_clk" of the
 * gmu node's clock list). */

constexpr uint64_t CxoHz = 19200000u;  /* SM7250 XO; confirm at run time from GPUCC_CXO */

/* ---------------------------------------------------------------------- */
/* Power                                                                   */
/* ---------------------------------------------------------------------- */

struct Gdsc {
    const char *name;
    uint32_t control_offset; /* GDSCR inside WindowGpucc */
    uint32_t status_offset;  /* GDS hardware status inside WindowGpucc */
    uint32_t timeout_us;     /* qcom,gds-timeout of the sibling node */
};

/* gpu_cx_gdsc, DTS line 10585: reg 0x3d9106c (GPUCC + 0x106c),
 * hw-ctrl-addr 0xa0, qcom,gds-timeout 0x1f4. The GDS status register used by
 * the existing service is GPUCC + 0x1540 (GX_GDS_HW_CTRL is the sibling). */
const Gdsc &cxGdsc();
/* gpu_gx_gdsc, DTS line 10613: reg 0x3d9100c (GPUCC + 0x100c),
 * status GPUCC + 0x1010. The node has no timeout; the CX sibling's 500 us is
 * used. GX is normally powered by the GMU firmware, not by the CPU. */
const Gdsc &gxGdsc();

/* GPU OPP bins from qcom,gpu-pwrlevel-bins, DTS lines 13520-13870. */
struct Pwrlevel {
    uint64_t frequency_hz;
    uint32_t bus_index;     /* index into gpuBusKbps() */
    uint32_t bus_min;
    uint32_t bus_max;
    uint32_t acd_level;
};

struct PwrlevelBin {
    uint32_t speed_bin;     /* qcom,speed-bin; 0 is the DT default bin */
    uint32_t initial_level;
    uint32_t ca_target_level;
    uint32_t throttle_level;
    size_t count;
    const Pwrlevel *levels;
};

const PwrlevelBin *pwrlevelBins(size_t *count);
const PwrlevelBin *pwrlevelBinForSpeed(uint32_t speed_bin);
/* The bin marked qcom,speed-bin = 0. Never an invented fallback. */
const PwrlevelBin &defaultPwrlevelBin();

/* Fuse cells selected by msm_gpu nvmem-cells, defined at DTS lines 7349-7361:
 * speed_bin (0x1ea, bit 5, 8 bits), gaming_bin (0x212, bit 0, 1 bit) and
 * isense_slope (0x45c8, 4 bytes). The probe cannot read fuses; the main
 * session uses speed_bin to select a PwrlevelBin. */
struct NvmemCell {
    const char *name;
    uint32_t address;
    uint16_t bit;
    uint16_t bits;
};
const NvmemCell *nvmemCells(size_t *count);

/* Interconnect paths (qcom,msm-bus vectors-KBps). */
struct BusPath { uint32_t src; uint32_t dst; };
const BusPath &gpuBusPath();     /* gpubw src-dst-ports <0x1a 0x200> */
const BusPath &gmuCnocPath();    /* gmu <0x1a 0x2734> */
const BusPath &smmuBusPath();    /* kgsl_smmu <0x9b 0x200> */

/* grp3d bandwidth table, DTS line 13541: 13 cases, KB/s. */
size_t gpuBusLevelCount();
uint32_t gpuBusKbps(uint32_t level);
/* gmu cnoc table, DTS line 13924: 0 and 0x64 KB/s. */
uint32_t gmuCnocKbps(uint32_t level);
/* kgsl_smmu table: 0 and 0x3e8 KB/s. */
uint32_t smmuBusKbps(uint32_t level);

/* ---------------------------------------------------------------------- */
/* SMMU                                                                    */
/* ---------------------------------------------------------------------- */

struct SmmuStream {
    const char *name;
    uint16_t sid;           /* first #iommu-cells specifier cell */
    uint16_t mask;          /* second cell: arm-smmu SMR mask */
};

/* kgsl_smmu, DTS line 10670: compatible qcom,qsmmu-v500,
 * #iommu-cells = 2, qcom,skip-init, qcom,use-3-lvl-tables,
 * qcom,no-dynamic-asid, #global-interrupts = 1.
 * The iommus entries are gfx3d_user <0 0x401>, gfx3d_secure <2 0x400>,
 * gmu_user <4 0x400>, gmu_kernel <5 0x400> (DTS lines 13899-13943).
 * The second cell is the SMR mask (arm-smmu.c arm_smmu_of_xlate), not a
 * context bank. No context bank number is present in the DT; downstream
 * assigns one dynamically and queries DOMAIN_ATTR_CONTEXT_BANK. The plan
 * below is the native driver's fixed choice, with ASID = bank + 1 because
 * qcom,no-dynamic-asid forces cfg->asid = cbndx + 1 in downstream arm-smmu.c. */
const SmmuStream *smmuStreams(size_t *count);

struct SmmuBank {
    const char *name;
    uint32_t bank;
    uint32_t asid;
    uint16_t sid;
    uint16_t mask;
    bool bringup;           /* part of the first bring-up sequence */
};

const SmmuBank *smmuBanks(size_t *count);
constexpr uint32_t smmuAsidForBank(uint32_t bank) { return bank + 1u; }

struct SmmuConfig {
    uint64_t base;
    uint64_t bytes;
    uint64_t tcu_base;
    uint64_t tcu_bytes;
    uint32_t iommu_cells;
    uint32_t global_interrupts;
    uint32_t actlr_sid;     /* qcom,actlr <0x0 0x3ff 0x32b ...> */
    uint32_t actlr_mask;
    uint32_t actlr_value;
};

const SmmuConfig &smmu();

/* ---------------------------------------------------------------------- */
/* Firmware                                                                */
/* ---------------------------------------------------------------------- */

struct FirmwareFile {
    const char *name;
    uint64_t bytes;
    uint32_t crc32;         /* IEEE 802.3, whole file */
    uint8_t sha256[32];
};

/* /firmware of the vendor image (vendor.img, ext4). These are hashes and
 * sizes only; no blob is stored in this repository. Stage the files in a
 * ramdisk and hand them to the probe through native_adreno_set_firmware(). */
const FirmwareFile &zapMdt();
/* b00, b01, b02 in ELF program-header order. */
size_t zapSegmentCount();
const FirmwareFile &zapSegment(unsigned index);
const FirmwareFile &zapElf();
/* The vendor image ships a630_sqe.fw together with a650_sqe.fw. The stock
 * kernel's firmware-name strings introduce only "a620_zap" for A620 and reuse
 * the already-listed a630/a650 names, which is the downstream pattern for
 * A620 = a630_sqe.fw + a650_gmu.bin. The a650_sqe.fw alternative is exposed
 * because the pinned mainline catalog uses it for the A650 family; see
 * README.md and choose at integration time, never silently.
 * The pinned alternative is linux-firmware's a650_sqe.fw (version 0x112,
 * 31964 bytes), whose version clears mainline's 0x095 floor; the vendor image's
 * 0x087 file (31460 bytes) is kept out of tree as a650_sqe_vendor_0x87.fw. */
const FirmwareFile &sqe();
const FirmwareFile *sqeAlternate();
const FirmwareFile &gmu();

/* GMU memory layout, from a6xx_gmu_memory_probe for the A650 family (A620 is
 * ADRENO_6XX_GEN3, downstream adreno_is_a650_family). The DTCM base is
 * 0x10004000 for the A650 family; the firmware container's metadata blocks
 * confirm it (block at 0x10004000). */
struct GmuLayout {
    uint64_t itcm_base;
    uint64_t itcm_bytes;
    uint64_t dtcm_base;
    uint64_t dtcm_bytes;
    uint64_t icache_iova;
    uint64_t icache_bytes;  /* SZ_16M - SZ_16K at 0x4000 */
    uint64_t dummy_iova;
    uint64_t dummy_bytes;   /* SZ_4K for the A650 family, not A740's 16K */
    uint64_t log_iova;
    uint64_t log_bytes;     /* 16K */
    uint64_t hfi_iova;
    uint64_t hfi_bytes;     /* 16K */
    uint64_t uncached_end;  /* 0x80000000, end of the GMU uncached range */
};

const GmuLayout &gmuLayout();

/* Load addresses. The ZAP program headers (validated by Adreno620MdtParse)
 * place the code segment at 0x5000 with e_entry = 0x5000 and the hash segment
 * at 0x6000. The SQE image is one contiguous blob; CP_SQE_INSTR_BASE takes its
 * 4 KiB-aligned IOVA (NativeAdrenoCpInit refuses anything else). The GMU has no
 * entry word in the container: the CM3 fetches its initial SP and reset vector
 * from ITCM address 0, which the container's first data block covers. */
constexpr uint32_t ZapEntry = 0x5000u;
constexpr uint64_t ZapCodeVaddr = 0x5000u;
constexpr uint64_t ZapHashVaddr = 0x6000u;
constexpr uint64_t SqeIovaAlignment = 0x1000u;
constexpr uint32_t GmuEntry = 0u;

/* ---------------------------------------------------------------------- */
/* Identity                                                                */
/* ---------------------------------------------------------------------- */

/* msm_gpu qcom,chipid, DTS line 13519. Downstream adreno.h decodes this as
 * core 6, major 2, minor 0, patch 1 -> revn 620. The pinned mainline catalog
 * (a6xx_catalog.c) has no 0x06020001 entry; its A620/A621 line uses
 * 0x06020100 with zapfw a620_zap.mbn and ADRENO_6XX_GEN3. A6xx has no
 * chip-ID MMIO register in either tree (downstream reads qcom,chipid), so the
 * probe publishes both numbers instead of pretending a register read proves
 * the ID. */
constexpr uint32_t ChipId = 0x06020001u;
constexpr uint32_t ChipCore = 6u;
constexpr uint32_t ChipMajor = 2u;
constexpr uint32_t ChipMinor = 0u;
constexpr uint32_t ChipPatch = 1u;
constexpr uint32_t AdrenoRevn = 620u;
constexpr uint32_t MainlineCatalogChipId = 0x06020100u;

/* ---------------------------------------------------------------------- */
/* Pure helpers                                                            */
/* ---------------------------------------------------------------------- */

enum ParseResult : int {
    ParseOk = 0,
    ParseBadArgument = -1,
    ParseNoSpace = -2
};

struct IommuEntry {
    uint16_t sid;
    uint16_t mask;
};

/* Same acceptance rules as NativeAdrenoBoard: big-endian cells, 1..2
 * address/size cells, the 2/2-cell little-endian ADT quirk, no zero-length
 * window, no overflow. Output is unchanged on failure. */
int parseReg(const uint8_t *cells, size_t length, uint32_t address_cells,
    uint32_t size_cells, Window *out, size_t capacity, size_t *count);

/* One phandle cell followed by cells_per_entry specifier cells. The first
 * specifier cell is the stream ID; the optional second is the SMR mask.
 * Output is unchanged on failure. */
int parseIommus(const uint8_t *cells, size_t length, uint32_t cells_per_entry,
    IommuEntry *out, size_t capacity, size_t *count);

/* Expand (sid, mask) into the exact SIDs the native ArmSmmu4K kit can attach.
 * Only masks with at most 4 set bits (16 SIDs) are accepted; returns the
 * required count, writes min(required, capacity) entries. Returns 0 for an
 * unsupported mask. */
size_t expandStreamMask(uint16_t sid, uint16_t mask, uint16_t *out, size_t capacity);

/* Qualcomm root-clock-generator divider: output = parent / (2 * (m + 1)). */
uint64_t rcgRateHz(uint64_t parent_hz, uint32_t m);
/* Exact inverse. Returns 0 and writes m when parent == target * 2 * (m + 1),
 * with m <= max_m; otherwise returns -1 and writes nothing. */
int rcgMForRate(uint64_t parent_hz, uint64_t target_hz, uint32_t max_m, uint32_t *m);

/* Counter arithmetic used for bounded fence/timeout waits. usToTicks rounds
 * up; both return 0 on overflow. */
uint64_t usToTicks(uint64_t hz, uint64_t us);
uint64_t ticksToUs(uint64_t ticks, uint64_t hz);

} // namespace adreno620

#endif /* ADRENO620_BOARD_H */
