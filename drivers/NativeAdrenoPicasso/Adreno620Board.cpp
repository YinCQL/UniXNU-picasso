/* SPDX-License-Identifier: MIT */
#include "Adreno620Board.h"

namespace adreno620 {

namespace {

/* DTS line numbers refer to /tmp/picasso.dts as decompiled for this port. */
const Window kWindows[WindowCount] = {
    {"kgsl_3d0_reg_memory", 0x3d00000u, 0x40000u},   /* msm_gpu reg[0] */
    {"cx_dbgc",             0x3d61000u, 0x800u},     /* msm_gpu reg[1] */
    {"rscc",                0x3de0000u, 0x1000u},    /* msm_gpu reg[2] */
    {"isense_cntl",         0x3d8b000u, 0x2000u},    /* msm_gpu reg[3] */
    {"kgsl_gmu_reg",        0x3d6a000u, 0x30000u},   /* gmu reg[0] */
    {"kgsl_gmu_pdc_cfg",    0xb290000u, 0x10000u},   /* gmu reg[1] */
    {"kgsl_gmu_pdc_seq",    0xb490000u, 0x10000u},   /* gmu reg[2] */
    {"smmu_base",           0x3da0000u, 0x10000u},   /* kgsl_smmu reg[0] */
    {"smmu_tcu_base",       0x3dc2000u, 0x20u},      /* kgsl_smmu reg[1] */
    {"gpucc",               0x3d90000u, 0x9000u},    /* gpucc reg */
    {"gcc",                 0x100000u,  0x1f0000u},  /* gcc reg */
};

const Interrupt kInterrupts[InterruptCount] = {
    {"kgsl_3d0_irq",     0x12cu, 4u},
    {"kgsl_hfi_irq",     0x130u, 4u},
    {"kgsl_gmu_irq",     0x131u, 4u},
    {"smmu_global",      0x2a1u, 4u},
    {"smmu_context_cb0", 0x2a6u, 4u},
    {"gfx_0_tbu",        0x2afu, 4u},
    {"gfx_1_tbu",        0x2b0u, 4u},
};

/* Clock ids: include/dt-bindings/clock/qcom,gpucc-lito.h and qcom,gcc-lito.h
 * in the stock picasso kernel tree. CBCR offsets: NativeAdrenoService.cpp. */
const Clock kClocks[ClockCount] = {
    {"gpu_cc_ahb",             ClockController::Gpucc, 0u,  0x1078u},
    {"gmu_clk",                ClockController::Gpucc, 3u,  0x1098u}, /* GPU_CC_CX_GMU_CLK */
    {"cxo_clk",                ClockController::Gpucc, 6u,  0x109cu},
    {"smmu_vote",              ClockController::Gpucc, 12u, 0x5000u},
    {"mem_clk",                ClockController::Gcc,   13u, 0x71154u}, /* GCC_DDRSS_GPU_AXI_CLK (gcc-lito.c) */
    {"mem_iface_clk",          ClockController::Gcc,   35u, 0x7100cu},
    {"gcc_gpu_snoc_dvm_gfx",   ClockController::Gcc,   36u, 0x71018u},
    {"gcc_gpu_cfg_ahb",        ClockController::Gcc,   31u, 0x71004u},
};

const Gdsc kCxGdsc = {"gpu_cx_gdsc", 0x106cu, 0x1540u, 0x1f4u};
const Gdsc kGxGdsc = {"gpu_gx_gdsc", 0x100cu, 0x1010u, 0x1f4u};

/* qcom,gpu-pwrlevel@N order is preserved. acd_level is the DT qcom,acd-level
 * for the level; the off level carries none. */
const Pwrlevel kBin0[] = {
    {750000000u, 10u,  9u, 12u, 0xa02c5ffdu},
    {670000000u, 10u,  9u, 12u, 0xa02c5ffdu},
    {625000000u, 10u,  9u, 12u, 0x802d5ffdu},
    {500000000u,  8u,  7u, 10u, 0xa02d5ffdu},
    {400000000u,  7u,  5u,  8u, 0x802e5ffdu},
    {275000000u,  5u,  5u,  7u, 0x802f5ffdu},
    {0u,          0u,  0u,  0u, 0u},
};
const Pwrlevel kBin1[] = {
    {625000000u, 10u,  9u, 12u, 0x802d5ffdu},
    {500000000u,  8u,  7u, 10u, 0xa02d5ffdu},
    {400000000u,  7u,  5u,  8u, 0x802e5ffdu},
    {275000000u,  5u,  5u,  7u, 0x802f5ffdu},
    {0u,          0u,  0u,  0u, 0u},
};
const Pwrlevel kBin3[] = {
    {540000000u,  8u,  7u, 12u, 0x802d5ffdu},
    {400000000u,  7u,  5u,  8u, 0x802e5ffdu},
    {275000000u,  5u,  5u,  7u, 0x802f5ffdu},
    {0u,          0u,  0u,  0u, 0u},
};

const PwrlevelBin kBins[] = {
    {0x00u, 5u, 4u, 0u, sizeof(kBin0) / sizeof(kBin0[0]), kBin0},
    {0x84u, 3u, 2u, 0u, sizeof(kBin1) / sizeof(kBin1[0]), kBin1},
    {0x73u, 2u, 1u, 0u, sizeof(kBin3) / sizeof(kBin3[0]), kBin3},
    {0x9eu, 5u, 4u, 0u, sizeof(kBin0) / sizeof(kBin0[0]), kBin0},
    {0xa5u, 3u, 2u, 0u, sizeof(kBin1) / sizeof(kBin1[0]), kBin1},
};

const NvmemCell kNvmem[] = {
    {"speed_bin",    0x1eau,  5u, 8u},
    {"gaming_bin",   0x212u, 0u, 1u},
    {"isense_slope", 0x45c8u, 0u, 32u},
};

const BusPath kGpuBus = {0x1au, 0x200u};
const BusPath kGmuCnoc = {0x1au, 0x2734u};
const BusPath kSmmuBus = {0x9bu, 0x200u};

/* grp3d qcom,msm-bus,vectors-KBps, 13 cases in DTS order. */
const uint32_t kGpuBusKbps[] = {
    0u, 400000u, 800000u, 1200000u, 1804800u, 2188000u, 2724000u,
    3072000u, 4068000u, 5412000u, 6220000u, 7216000u, 8371200u,
};
const uint32_t kGmuCnocKbps[] = {0u, 100u};
const uint32_t kSmmuBusKbps[] = {0u, 1000u};

/* kgsl_smmu iommus, DTS lines 13899/13908/13936/13943. */
const SmmuStream kStreams[] = {
    {"gfx3d_user",   0x000u, 0x401u},
    {"gfx3d_secure", 0x002u, 0x400u},
    {"gmu_user",     0x004u, 0x400u},
    {"gmu_kernel",   0x005u, 0x400u},
};

/* Fixed native context plan. Downstream leaves the bank assignment to the
 * arm-smmu allocator; a native driver that owns the SMMU fixes it. ASID is
 * bank + 1 because qcom,no-dynamic-asid. Only the user and GMU kernel banks
 * are part of the first bring-up; the secure bank needs ZAP/TZ. */
const SmmuBank kBanks[] = {
    {"gfx3d_user",   0u, 1u, 0x000u, 0x401u, true},
    {"gfx3d_secure", 1u, 2u, 0x002u, 0x400u, false},
    {"gmu_user",     2u, 3u, 0x004u, 0x400u, false},
    {"gmu_kernel",   3u, 4u, 0x005u, 0x400u, true},
};

const SmmuConfig kSmmu = {
    0x3da0000u, 0x10000u, 0x3dc2000u, 0x20u,
    2u, 1u,
    0x000u, 0x3ffu, 0x32bu,   /* qcom,actlr <0x0 0x3ff 0x32b ...> */
};

const FirmwareFile kZapMdt = {
    "a620_zap.mdt", 6860u, 0xae7b941cu,
    {0xf7,0x16,0x18,0xf0,0x30,0x58,0xf3,0x6e,0x48,0x39,0x0a,0x14,0xa5,0x5a,0xd2,0x6d,
     0xea,0x45,0x2e,0x40,0xaa,0x54,0xf7,0xd8,0xa1,0x56,0x89,0x10,0x38,0xba,0x6d,0xfe},
};
const FirmwareFile kZapB00 = {
    "a620_zap.b00", 148u, 0x4629d3f3u,
    {0xec,0x9b,0x9d,0x5a,0x67,0x45,0x63,0x84,0x80,0x96,0x24,0xb1,0x4a,0x00,0xd1,0x5d,
     0x36,0x29,0x7e,0x5f,0x2e,0x75,0x72,0x85,0x04,0xcd,0x87,0x2b,0xc9,0x5e,0x09,0x47},
};
const FirmwareFile kZapB01 = {
    "a620_zap.b01", 6712u, 0x9220d541u,
    {0xb0,0xed,0x36,0x93,0x72,0xc0,0xdd,0x23,0x3f,0x6d,0x2f,0x13,0xd2,0x64,0xda,0x69,
     0xca,0x5c,0xc0,0x69,0x62,0x82,0x21,0x77,0x69,0x6d,0x9f,0xea,0xf2,0x8d,0xc2,0x05},
};
const FirmwareFile kZapB02 = {
    "a620_zap.b02", 1676u, 0xb439bc29u,
    {0x5a,0x4d,0xe8,0xad,0x88,0x1b,0x57,0xd6,0x4c,0x36,0x97,0xb1,0x07,0x8c,0xe0,0x12,
     0xfb,0x47,0x30,0x0d,0x91,0x4a,0x32,0xc5,0x05,0x22,0xda,0x76,0x2e,0x2f,0x2b,0xd8},
};
const FirmwareFile kZapElf = {
    "a620_zap.elf", 13964u, 0x8cbed8d9u,
    {0xa6,0x64,0x2a,0x6d,0x7b,0x71,0x9f,0x8d,0xad,0x6c,0xb8,0x2e,0x0b,0x0c,0xcf,0x33,
     0xb5,0x86,0x5b,0x27,0xcf,0xc1,0x93,0x3c,0x32,0x5d,0xbb,0xb6,0xbc,0xb5,0xf1,0x15},
};
const FirmwareFile kSqe = {
    "a630_sqe.fw", 32008u, 0x8b648790u,
    {0xa0,0x98,0x9f,0x6d,0x2b,0x2b,0x37,0x1b,0xfe,0xdb,0xfa,0xfe,0xef,0x40,0xa4,0x92,
     0x93,0xb7,0x12,0x8f,0xa7,0x4b,0xe9,0xf2,0xaf,0x25,0xab,0x9f,0x59,0x59,0x97,0xe0},
};
/* wf101: the SQE shipped in this device's own firmware partition (a650_sqe.fw
 * version 0x087, the image the stock Android KMD runs on this CP) instead of the
 * pinned linux-firmware a650_sqe 0x112. The pinned image executes every simple
 * CP packet but answered CP_SET_SECURE_MODE with CP_OPCODE_ERROR (wf100), which
 * points at SQE/CP generation mismatch; the vendor image is the device's own.
 * The mainline 0x095 security floor is for a650 phones; this is a bring-up
 * measurement on the image the device ships. */
const FirmwareFile kSqeAlt = {
    "a650_sqe.fw", 31460u, 0x59c1d422u,
    {0xa7,0x06,0x74,0x44,0xf8,0xaf,0x6c,0xe2,0xe8,0x56,0xe5,0xb3,0xde,0x42,0xb1,0x20,
     0xa3,0xbd,0xa3,0xa2,0x15,0xf8,0x28,0x27,0x60,0x35,0x72,0x3b,0x2e,0x9f,0x8d,0x81},
};
const FirmwareFile kGmu = {
    "a650_gmu.bin", 41308u, 0xc190814du,
    {0x78,0xe7,0x92,0xd3,0x5b,0x64,0x0b,0xc8,0xd2,0x3c,0x89,0xb2,0x04,0xe7,0x28,0xee,
     0x66,0x32,0xed,0x62,0xfb,0xff,0x4b,0x5f,0x1c,0xf4,0x2d,0xd7,0x28,0x5e,0xf6,0xc5},
};
const GmuLayout kGmuLayout = {
    0x0u, 0x4000u,             /* ITCM */
    0x10004000u, 0x4000u,      /* DTCM, A650 family base */
    0x4000u, 0xffc000u,        /* icache: SZ_16M - SZ_16K */
    0x60000000u, 0x1000u,      /* dummy: SZ_4K */
    0x60001000u, 0x4000u,      /* log: first free page after dummy */
    0x60005000u, 0x4000u,      /* hfi: after log */
    0x80000000u,               /* uncached range end */
};

constexpr uint32_t be32(const uint8_t *p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
        (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

constexpr uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool decodeWindow(const uint8_t *at, uint32_t address_cells, uint32_t size_cells,
    Window *out)
{
    uint64_t base = 0, bytes = 0;
    bool adt_le = false;
    if (address_cells == 2 && size_cells == 2) {
        const uint32_t w1_le = le32(at + 4);
        const uint32_t w3_le = le32(at + 12);
        const uint32_t w0_be = be32(at);
        const uint32_t w1_be = be32(at + 4);
        if (w1_le == 0 && w3_le == 0 && w0_be != 0 && w1_be == 0)
            adt_le = true;
    }
    if (adt_le) {
        base = uint64_t(le32(at)) | (uint64_t(le32(at + 4)) << 32);
        bytes = uint64_t(le32(at + 8)) | (uint64_t(le32(at + 12)) << 32);
    } else {
        for (uint32_t cell = 0; cell < address_cells; ++cell) {
            if (base >> 32) return false;
            base = (base << 32) | be32(at + cell * 4);
        }
        for (uint32_t cell = 0; cell < size_cells; ++cell) {
            if (bytes >> 32) return false;
            bytes = (bytes << 32) | be32(at + (address_cells + cell) * 4);
        }
    }
    if (!bytes || base + bytes < base) return false;
    out->name = nullptr;
    out->base = base;
    out->bytes = bytes;
    return true;
}

bool decodeIommu(const uint8_t *at, uint32_t cells_per_entry, IommuEntry *out)
{
    const uint32_t ph_be = be32(at);
    const uint32_t ph_le = le32(at);
    if (!ph_be && !ph_le) return false;
    const bool is_le = ph_le < ph_be;
    const uint32_t sid = is_le ? le32(at + 4) : be32(at + 4);
    if (sid > 0xffffu) return false;
    uint32_t mask = 0;
    if (cells_per_entry >= 2) {
        mask = is_le ? le32(at + 8) : be32(at + 8);
        if (mask > 0xffffu) return false;
    }
    out->sid = static_cast<uint16_t>(sid);
    out->mask = static_cast<uint16_t>(mask);
    return true;
}

} // namespace

const Window *windows(size_t *count)
{
    if (count) *count = WindowCount;
    return kWindows;
}

const Window &window(WindowId id)
{
    return kWindows[id < WindowCount ? id : WindowGpu];
}

const Interrupt *interrupts(size_t *count)
{
    if (count) *count = InterruptCount;
    return kInterrupts;
}

const Interrupt &interrupt(InterruptId id)
{
    return kInterrupts[id < InterruptCount ? id : InterruptGpu];
}

const Clock *clocks(size_t *count)
{
    if (count) *count = ClockCount;
    return kClocks;
}

const Clock &clock(ClockId id)
{
    return kClocks[id < ClockCount ? id : ClockGpuccAhb];
}

const Gdsc &cxGdsc() { return kCxGdsc; }
const Gdsc &gxGdsc() { return kGxGdsc; }

const PwrlevelBin *pwrlevelBins(size_t *count)
{
    if (count) *count = sizeof(kBins) / sizeof(kBins[0]);
    return kBins;
}

const PwrlevelBin *pwrlevelBinForSpeed(uint32_t speed_bin)
{
    for (const PwrlevelBin &bin : kBins)
        if (bin.speed_bin == speed_bin) return &bin;
    return nullptr;
}

const PwrlevelBin &defaultPwrlevelBin()
{
    return kBins[0];
}

const NvmemCell *nvmemCells(size_t *count)
{
    if (count) *count = sizeof(kNvmem) / sizeof(kNvmem[0]);
    return kNvmem;
}

const BusPath &gpuBusPath() { return kGpuBus; }
const BusPath &gmuCnocPath() { return kGmuCnoc; }
const BusPath &smmuBusPath() { return kSmmuBus; }

size_t gpuBusLevelCount() { return sizeof(kGpuBusKbps) / sizeof(kGpuBusKbps[0]); }

uint32_t gpuBusKbps(uint32_t level)
{
    return level < gpuBusLevelCount() ? kGpuBusKbps[level] : 0u;
}

uint32_t gmuCnocKbps(uint32_t level)
{
    return level < sizeof(kGmuCnocKbps) / sizeof(kGmuCnocKbps[0]) ? kGmuCnocKbps[level] : 0u;
}

uint32_t smmuBusKbps(uint32_t level)
{
    return level < sizeof(kSmmuBusKbps) / sizeof(kSmmuBusKbps[0]) ? kSmmuBusKbps[level] : 0u;
}

const SmmuStream *smmuStreams(size_t *count)
{
    if (count) *count = sizeof(kStreams) / sizeof(kStreams[0]);
    return kStreams;
}

const SmmuBank *smmuBanks(size_t *count)
{
    if (count) *count = sizeof(kBanks) / sizeof(kBanks[0]);
    return kBanks;
}

const SmmuConfig &smmu() { return kSmmu; }

const FirmwareFile &zapMdt() { return kZapMdt; }

size_t zapSegmentCount() { return 3; }

const FirmwareFile &zapSegment(unsigned index)
{
    switch (index) {
    case 0: return kZapB00;
    case 1: return kZapB01;
    default: return kZapB02;
    }
}

const FirmwareFile &zapElf() { return kZapElf; }
const FirmwareFile &sqe() { return kSqe; }
const FirmwareFile *sqeAlternate() { return &kSqeAlt; }
const FirmwareFile &gmu() { return kGmu; }
const GmuLayout &gmuLayout() { return kGmuLayout; }

int parseReg(const uint8_t *cells, size_t length, uint32_t address_cells,
    uint32_t size_cells, Window *out, size_t capacity, size_t *count)
{
    if (!count) return ParseBadArgument;
    *count = 0;
    if (!cells || !length || address_cells < 1 || address_cells > 2 ||
        size_cells < 1 || size_cells > 2)
        return ParseBadArgument;
    const size_t per_window = (address_cells + size_cells) * sizeof(uint32_t);
    if (length % per_window) return ParseBadArgument;
    const size_t total = length / per_window;
    if (!out || total > capacity) return ParseNoSpace;
    /* Validate every window before writing any, so a failure leaves the
     * caller's array untouched. */
    for (size_t index = 0; index < total; ++index) {
        Window scratch{};
        if (!decodeWindow(cells + index * per_window, address_cells,
            size_cells, &scratch))
            return ParseBadArgument;
    }
    for (size_t index = 0; index < total; ++index)
        (void)decodeWindow(cells + index * per_window, address_cells,
            size_cells, &out[index]);
    *count = total;
    return ParseOk;
}

int parseIommus(const uint8_t *cells, size_t length, uint32_t cells_per_entry,
    IommuEntry *out, size_t capacity, size_t *count)
{
    if (!count) return ParseBadArgument;
    *count = 0;
    if (!cells || !length || cells_per_entry < 1 || cells_per_entry > 4)
        return ParseBadArgument;
    const size_t per_entry = (1 + cells_per_entry) * sizeof(uint32_t);
    if (length % per_entry) return ParseBadArgument;
    const size_t total = length / per_entry;
    if (!out || total > capacity) return ParseNoSpace;
    for (size_t index = 0; index < total; ++index) {
        IommuEntry scratch{};
        if (!decodeIommu(cells + index * per_entry, cells_per_entry, &scratch))
            return ParseBadArgument;
    }
    for (size_t index = 0; index < total; ++index)
        (void)decodeIommu(cells + index * per_entry, cells_per_entry, &out[index]);
    *count = total;
    return ParseOk;
}

size_t expandStreamMask(uint16_t sid, uint16_t mask, uint16_t *out, size_t capacity)
{
    unsigned bits = 0;
    for (unsigned bit = 0; bit < 16; ++bit)
        if (mask & (1u << bit)) ++bits;
    if (bits > 4) return 0;
    const size_t required = size_t(1) << bits;
    if (!out && capacity) return 0;
    uint16_t positions[4] = {0, 0, 0, 0};
    unsigned used = 0;
    for (unsigned bit = 0; bit < 16; ++bit)
        if (mask & (1u << bit)) positions[used++] = static_cast<uint16_t>(bit);
    const uint16_t base = static_cast<uint16_t>(sid & ~mask);
    for (size_t index = 0; index < required && index < capacity; ++index) {
        uint16_t value = base;
        for (unsigned bit = 0; bit < used; ++bit)
            if (index & (size_t(1) << bit))
                value = static_cast<uint16_t>(value | (1u << positions[bit]));
        out[index] = value;
    }
    return required;
}

uint64_t rcgRateHz(uint64_t parent_hz, uint32_t m)
{
    const uint64_t divider = 2ull * (uint64_t(m) + 1ull);
    if (!parent_hz || !divider) return 0;
    return parent_hz / divider;
}

int rcgMForRate(uint64_t parent_hz, uint64_t target_hz, uint32_t max_m, uint32_t *m)
{
    if (!parent_hz || !target_hz || !m) return -1;
    if (parent_hz % target_hz) return -1;
    const uint64_t quotient = parent_hz / target_hz;
    if (quotient < 2 || (quotient & 1u)) return -1;
    const uint64_t candidate = quotient / 2 - 1;
    if (candidate > max_m || candidate > UINT32_MAX) return -1;
    *m = static_cast<uint32_t>(candidate);
    return 0;
}

uint64_t usToTicks(uint64_t hz, uint64_t us)
{
    if (!hz || !us) return 0;
    if (us > UINT64_MAX / hz) return 0;
    const uint64_t product = hz * us;
    if (product > UINT64_MAX - 999999u) return 0;
    return (product + 999999u) / 1000000u;
}

uint64_t ticksToUs(uint64_t ticks, uint64_t hz)
{
    if (!hz || !ticks) return 0;
    if (ticks > UINT64_MAX / 1000000u) return 0;
    return (ticks * 1000000u) / hz;
}

} // namespace adreno620
