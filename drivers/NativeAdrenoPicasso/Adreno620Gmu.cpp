/* SPDX-License-Identifier: MIT */
/* Adreno 620 GMU cold start and HFI handshake.
 *
 * This is the A6xx GEN3 (SM7250/lito) sequence of the vendor msm-4.19 driver (Xiaomi picasso-r-oss,
 * drivers/gpu/msm/adreno_a6xx_gmu.c: a6xx_gmu_fw_start, a6xx_load_rsc_ucode, a6xx_load_pdc_ucode,
 * load_gmu_fw, a6xx_gmu_start, a6xx_gmu_hfi_start) with the validated containers and transports of this
 * tree: AdrenoFirmwarePlanGmu for the TLV image, NativeAdrenoGmuContextPayloads for the coherent GMU
 * buffers of the context the SMMU stage already built, AdrenoHfi for the queues and AdrenoHfiStartup for
 * the perf/bandwidth/core-start messages. Nothing here invents a value: every register offset/value is
 * cited at its use, and the HFI vote indices come from the device's own command DB.
 *
 * The two weak hooks in Adreno620Hooks.cpp are replaced by these strong definitions; the probe calls the
 * GMU one in stage 3 step 2 and the HFI one in stage 3 step 3. The SMMU stage must have completed: the
 * GMU's own context bank translates its icache/dummy/log/HFI buffers, and this file writes those buffers
 * through non-cacheable CPU aliases of the same physical pages. */
#include "Adreno620ProbeCore.h"
#include "Adreno620Board.h"
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoKernelTime.h"
#include "AdrenoFirmware.h"
#include "AdrenoHfi.h"
#include "AdrenoHfiStartup.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <string.h>

namespace {

/* GMU CSR dword offsets (adreno_a6xx_gmu.c / a6xx_reg.h) are GPU-window register indices: a GMU CSR at
 * dword N lives in the kgsl_gmu_reg window at byte N*4 - gmu2gpu_offset*4, where the vendor computes
 * gmu2gpu_offset = (0x3d6a000 - 0x3d00000) >> 2 = 0x1A800 (kgsl_gmu.c). The kgsl_gmu_reg window starts at
 * 0x3d6a000, so its byte offset is N*4 - 0x6a000. */
constexpr uint64_t kGmuWindowBase = 0x3d6a000u;
constexpr uint64_t kGmuDownshift = kGmuWindowBase - 0x3d00000u;   /* 0x6a000 */

/* GMU CSR registers, dword offsets. */
constexpr uint32_t kCm3ItcmStart = 0x1B400;
constexpr uint32_t kCm3DtcmStart = 0x1C400;
constexpr uint32_t kCm3SysReset = 0x1F800;
constexpr uint32_t kCm3BootConfig = 0x1F801;
constexpr uint32_t kCm3FwBusy = 0x1F81A;
constexpr uint32_t kCm3FwInitResult = 0x1F81C;
constexpr uint32_t kCm3Cfg = 0x1F82D;
constexpr uint32_t kGmuIcacheConfig = 0x1F400;
constexpr uint32_t kGmuDcacheConfig = 0x1F401;
constexpr uint32_t kGmuSysBusConfig = 0x1F40F;
constexpr uint32_t kGmuRpmhCtrl = 0x1F8E8;
constexpr uint32_t kGmuPwrColInterFrameCtrl = 0x1F8C0;
constexpr uint32_t kGmuCxFalIntf = 0x1F8F0;
constexpr uint32_t kGmuCxFalNextIntf = 0x1F8F1;
constexpr uint32_t kGmuPwrColCpMsg = 0x1F900;
constexpr uint32_t kGmuPwrColCpResp = 0x1F901;
constexpr uint32_t kGmuHfiCtrlStatus = 0x1F980;
constexpr uint32_t kGmuHfiSfrAddr = 0x1F982;
constexpr uint32_t kGmuHfiQtblInfo = 0x1F984;
constexpr uint32_t kGmuHfiQtblAddr = 0x1F985;
constexpr uint32_t kGmuHfiCtrlInit = 0x1F986;
constexpr uint32_t kGmuGmu2HostIntrClr = 0x1F991;
constexpr uint32_t kGmuGmu2HostIntrInfo = 0x1F992;
constexpr uint32_t kGmuGmu2HostIntrMask = 0x1F993;
constexpr uint32_t kGmuHost2GmuIntrSet = 0x1F994;
constexpr uint32_t kGmuGeneral7 = 0x1F9CC;
constexpr uint32_t kGmuAoHostIntrClr = 0x23B04;
constexpr uint32_t kGmuAoHostIntrStatus = 0x23B05;
constexpr uint32_t kGmuAoHostIntrMask = 0x23B06;
constexpr uint32_t kGmuAhbFenceRange0 = 0x23B11;

/* GPU-window registers (gpu reg[0], 0x3d00000). */
constexpr uint32_t kGbifQsbSide0 = 0x3C03;      /* A6XX_GBIF_QSB_SIDE0, side 0..3 contiguous */
constexpr uint32_t kRbbmGbifClientQosCntl = 0x11;
constexpr uint32_t kGbifHalt = 0x3C45;

/* Values from the vendor driver for this exact chip. */
constexpr uint32_t kGmuFenceRange = 0x80A008A0u;       /* GMU_FENCE_RANGE_MASK in adreno_a6xx_gmu.c */
constexpr uint32_t kGmuCm3CfgReset = 0x4052u;          /* CM3 reset value, prevents NMI boot */
constexpr uint32_t kGmuInterFrameCtrl = 0x09C40400u;
constexpr uint32_t kRpmhEnableMask = 0x00000F11u;      /* interface|LLC|DDR|MX|CX|GFX (kgsl_gmu.h) */
constexpr uint32_t kGbifQosValue = 0x00071620u;        /* a650_gbif_regs, shared by a620 */
constexpr uint32_t kGbifClientQos = 3u;
constexpr uint32_t kChipIdCoreMask = 0xFFFF0000u;      /* chip id core/major kept in HFI_SFR_ADDR */
constexpr uint32_t kInvalidVlvl = 0xFFFFu;

/* Clock programming (gpucc-lito.c): gmu_clk_src is an RCG2 at 0x1120 whose parent 6 is
 * gcc_gpu_gpll0_div_clk_src (GPLL0/2 = 300 MHz); the 200 MHz table entry is parent/1.5, encoded as
 * SRC_SEL=6, HID_DIV=2 in the CFG word (clk-rcg2.c: rate = parent*2/(div+1)). */
constexpr uint32_t kGmuRcgr = 0x1120;
constexpr uint32_t kRcgrRootEn = 1u << 1;
constexpr uint32_t kRcgrRootOff = 1u << 31;
constexpr uint32_t kRcgrUpdate = 1u << 0;
constexpr uint32_t kRcgrCfgGmu = (6u << 8) | 2u;
constexpr uint32_t kRcgrCfgClear = 0x1Fu | (7u << 8) | (3u << 12) | (1u << 20);
constexpr uint32_t kGmuBranch = 0x1098;                 /* GPU_CC_CX_GMU_CLK CBCR */
constexpr uint32_t kBcbrEnable = 1u;
constexpr uint32_t kBcbrOff = 1u << 31;
constexpr uint32_t kGpuccPllVote = 0x52000;             /* gcc_gpu_gpll0[_div]_clk_src branches */
constexpr uint32_t kGccGpll0Enable = 0x52010;           /* gpll0 enable bit 0 */
constexpr uint32_t kGccGpll0Status = 0x30;              /* LUCID PLL_STATUS */
constexpr uint32_t kPllLockDet = 1u << 31;
constexpr uint32_t kCxoHz = 19200000u;
constexpr uint32_t kGmuHz = 200000000u;

/* RSCC (0x3de0000) dword offsets, adreno_a6xx_gmu.c a6xx_load_rsc_ucode. */
constexpr uint32_t kRscStatus0Drv0 = 0x04;
constexpr uint32_t kRscPdcSeqStartAddr = 0x08;
constexpr uint32_t kRscPdcMatchValueLo = 0x09;
constexpr uint32_t kRscPdcMatchValueHi = 0x0A;
constexpr uint32_t kRscPdcSlaveIdDrv0 = 0x0B;
constexpr uint32_t kRscHiddenTcsCmd0Addr = 0x0D;
constexpr uint32_t kRscHiddenTcsCmd0Data = 0x0E;
constexpr uint32_t kRscOverrideStartAddr = 0x100;
constexpr uint32_t kRscSeqBusyDrv0 = 0x101;
constexpr uint32_t kRscSeqMem0Drv0 = 0x180;

/* PDC config/sequencer dword offsets (a6xx_reg.h). */
constexpr uint32_t kPdcEnable = 0x1140;
constexpr uint32_t kPdcSeqStartAddr = 0x1148;
constexpr uint32_t kPdcTcs1Control = 0x1572;
constexpr uint32_t kPdcTcs1CmdEnable = 0x1573;
constexpr uint32_t kPdcTcs1CmdWait = 0x1574;
constexpr uint32_t kPdcTcs1Cmd0MsgId = 0x1575;
constexpr uint32_t kPdcTcs3Control = 0x15D6;
constexpr uint32_t kPdcTcs3CmdEnable = 0x15D7;
constexpr uint32_t kPdcTcs3CmdWait = 0x15D8;
constexpr uint32_t kPdcTcs3Cmd0MsgId = 0x15D9;
constexpr uint32_t kPdcCmdOffset = 4;
constexpr uint32_t kPdcMsgId = 0x10108;

struct Mapping {
    IOMemoryMap *keep;
    volatile uint8_t *base;
    uint64_t bytes;
};

/* One GMU buffer: the allocator's physical extent, its fixed GMU IOVA and the non-cacheable CPU alias this
 * file writes and reads. The GMU context maps the same pages Normal non-cacheable, so every access that
 * crosses the CPU/GMU boundary is paired with cache maintenance by physical address. */
struct PayloadBuffer {
    uint64_t physical;
    uint64_t bytes;
    uint64_t iova;
    Mapping alias;
};

struct CmdDb {
    bool valid;
    uint16_t gfx[32], mx[32], cx[32];
    size_t gfxCount, mxCount, cxCount, scratchCount;
    uint32_t mxAddress, cxAddress, xoAddress, vrmAddress;
};

struct GmuState {
    Mapping gmu, gpu, gpucc, gcc, rscc, pdcCfg, pdcSeq, smmu;
    PayloadBuffer buffer[5];      /* icache, dummy, debug, log, hfi */
    bool buffersMapped;
    bool booted;
    AdrenoHfi hfi;
    bool hfiInitialized;
    bool hfiStarted;
    uint32_t firmwareCore, firmwarePwr, firmwareHfi;
    uint32_t chipIdCompressed;
    CmdDb cmdDb;
    uint32_t speedBin;
    uint32_t votesAvailable;
};

GmuState *gState;

uint32_t read32(const Mapping &m, uint64_t offset)
{
    if (!m.base || offset + 4 > m.bytes) return 0;
    return *reinterpret_cast<volatile uint32_t *>(m.base + offset);
}

void write32(const Mapping &m, uint64_t offset, uint32_t value)
{
    if (!m.base || offset + 4 > m.bytes) return;
    *reinterpret_cast<volatile uint32_t *>(m.base + offset) = value;
}

void barrier()
{
    __asm__ volatile("dsb sy" ::: "memory");
}

bool mapRange(uint64_t base, uint64_t bytes, Mapping &out)
{
    if (!base || !bytes) return false;
    IODeviceMemory *memory = IODeviceMemory::withRange(base,
        static_cast<IOByteCount>(bytes));
    if (!memory) return false;
    out.keep = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (!out.keep) return false;
    out.base = reinterpret_cast<volatile uint8_t *>(out.keep->getVirtualAddress());
    out.bytes = bytes;
    return out.base != nullptr;
}

uint32_t coreRead(uint32_t dword)
{
    return read32(gState->gmu, uint64_t(dword) * 4 - kGmuDownshift);
}

void coreWrite(uint32_t dword, uint32_t value)
{
    write32(gState->gmu, uint64_t(dword) * 4 - kGmuDownshift, value);
}

uint32_t gpuRead(uint32_t dword)
{
    return read32(gState->gpu, uint64_t(dword) * 4);
}

void gpuWrite(uint32_t dword, uint32_t value)
{
    write32(gState->gpu, uint64_t(dword) * 4, value);
}

uint32_t rsccRead(uint32_t dword)
{
    return read32(gState->rscc, uint64_t(dword) * 4);
}

void rsccWrite(uint32_t dword, uint32_t value)
{
    write32(gState->rscc, uint64_t(dword) * 4, value);
}

/* Poll until (read & mask) == expected, bounded by attempts of delayUs; 1 on timeout, -1 on unmapped. */
int pollCore(uint32_t dword, uint32_t mask, uint32_t expected, unsigned attempts,
    unsigned delayUs, uint32_t *observed)
{
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        const uint32_t value = coreRead(dword);
        if (observed) *observed = value;
        if ((value & mask) == expected) return 0;
        if (attempt + 1 < attempts) IODelay(delayUs);
    }
    return 1;
}

void maintain(const PayloadBuffer &buffer, NativeAdrenoCacheOperation operation)
{
    if (!buffer.physical || !buffer.bytes) return;
    NativeAdrenoMaintainPhysicalPages(buffer.physical, buffer.bytes, operation);
}

uint8_t *aliasBytes(const Mapping &m)
{
    return reinterpret_cast<uint8_t *>(const_cast<uint8_t *>(m.base));
}

uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
        (uint32_t(p[3]) << 24);
}

uint16_t le16(const uint8_t *p)
{
    return uint16_t(p[0] | (p[1] << 8));
}

/* Command DB: XBL's reserved region at 0x80860000 (picasso.dts memory-region for cmd-db), format from the
 * vendor drivers/soc/qcom/cmd-db.c. The ARC vote indices of kgsl_gmu.c are not constants; they are entries
 * of the gfx.lvl/mx.lvl/cx.lvl tables stored here by the boot firmware. */
constexpr uint64_t kCmdDbBase = 0x80860000u;
constexpr uint64_t kCmdDbBytes = 0x20000u;
constexpr size_t kCmdDbHeaderBytes = 144;   /* version, magic, 8 rsc_hdr, checksum, reserved */

bool cmdDbAux(const volatile uint8_t *data, size_t dataBytes, const char *id,
    const uint16_t hdr[8][8], uint16_t *values, size_t capacity, size_t *count,
    uint32_t *address)
{
    uint8_t query[8] = {};
    for (size_t i = 0; i < 8 && id[i]; ++i) query[i] = static_cast<uint8_t>(id[i]);
    for (unsigned resource = 0; resource < 8; ++resource) {
        const uint16_t *header = hdr[resource];
        const uint16_t slave = header[0];
        if (!slave) break;
        const size_t headerOffset = header[1];
        const size_t dataOffset = header[2];
        const size_t entries = header[3];
        for (size_t index = 0; index < entries; ++index) {
            const size_t at = headerOffset + index * 24;
            if (at + 24 > dataBytes) return false;
            bool match = true;
            for (size_t byte = 0; byte < 8; ++byte)
                if (data[at + byte] != query[byte]) { match = false; break; }
            if (!match) continue;
            const uint32_t addr = uint32_t(data[at + 16]) | (uint32_t(data[at + 17]) << 8) |
                (uint32_t(data[at + 18]) << 16) | (uint32_t(data[at + 19]) << 24);
            const uint16_t length = uint16_t(data[at + 20] | (data[at + 21] << 8));
            const uint16_t offset = uint16_t(data[at + 22] | (data[at + 23] << 8));
            if (address) *address = addr;
            if (!values) { *count = 0; return true; }
            size_t words = length / 2;
            if (words > capacity) return false;
            const size_t aux = dataOffset + offset;
            if (aux + words * 2 > dataBytes) return false;
            for (size_t word = 0; word < words; ++word)
                values[word] = uint16_t(data[aux + word * 2] | (data[aux + word * 2 + 1] << 8));
            /* kgsl_gmu.c rpmh_arc_cmds: trailing zero padding ends the table. */
            while (words > 1 && values[words - 1] == 0 && values[words - 2] != 0) --words;
            *count = words;
            return true;
        }
    }
    return false;
}

bool readCmdDb(CmdDb &cmd)
{
    cmd = {};
    Mapping mapping = {};
    if (!mapRange(kCmdDbBase, kCmdDbBytes, mapping)) {
        printf("PICASSO_GPU gmu: command DB at %08llx did not map\n",
            static_cast<unsigned long long>(kCmdDbBase));
        return false;
    }
    const volatile uint8_t *data = mapping.base;
    const uint8_t magic[4] = {0xdb, 0x30, 0x03, 0x0c};
    bool magicOk = true;
    for (unsigned byte = 0; byte < 4; ++byte)
        if (data[4 + byte] != magic[byte]) magicOk = false;
    if (!magicOk) {
        printf("PICASSO_GPU gmu: command DB magic absent\n");
        return false;
    }
    uint16_t header[8][8] = {};
    for (unsigned resource = 0; resource < 8; ++resource) {
        const size_t at = 8 + resource * 16;
        for (unsigned word = 0; word < 8; ++word)
            header[resource][word] = uint16_t(data[at + word * 2] |
                (data[at + word * 2 + 1] << 8));
    }
    /* Every value is read from the mapped region directly; the helpers take byte counts, so the size used is
     * what the header could reference (the region is 128 KiB and offsets are 16-bit). */
    cmd.valid = true;
    cmd.valid = cmdDbAux(data, kCmdDbBytes - kCmdDbHeaderBytes, "gfx.lvl",
        (const uint16_t (*)[8])header, cmd.gfx, 32, &cmd.gfxCount, nullptr) && cmd.valid;
    cmd.valid = cmdDbAux(data, kCmdDbBytes - kCmdDbHeaderBytes, "mx.lvl",
        (const uint16_t (*)[8])header, cmd.mx, 32, &cmd.mxCount, &cmd.mxAddress) && cmd.valid;
    cmd.valid = cmdDbAux(data, kCmdDbBytes - kCmdDbHeaderBytes, "cx.lvl",
        (const uint16_t (*)[8])header, cmd.cx, 32, &cmd.cxCount, &cmd.cxAddress) && cmd.valid;
    bool xoFound = false, vrmFound = false;
    xoFound = cmdDbAux(data, kCmdDbBytes - kCmdDbHeaderBytes, "xo.lvl",
        (const uint16_t (*)[8])header, nullptr, 0, &cmd.scratchCount, &cmd.xoAddress);
    vrmFound = cmdDbAux(data, kCmdDbBytes - kCmdDbHeaderBytes, "vrm.soc",
        (const uint16_t (*)[8])header, nullptr, 0, &cmd.scratchCount, &cmd.vrmAddress);
    printf("PICASSO_GPU gmu: cmd-db xo.lvl=%d addr=%x vrm.soc=%d addr=%x\n",
        xoFound, cmd.xoAddress, vrmFound, cmd.vrmAddress);
    /* If a required rail is missing, name every entry whose id starts with its first letter so the DB's own
     * identifier spelling is on the record. */
    if (!xoFound || !vrmFound) {
        for (unsigned resource = 0; resource < 8; ++resource) {
            const uint16_t *h = header[resource];
            if (!h[0]) break;
            for (size_t index = 0; index < h[3]; ++index) {
                const size_t at = h[1] + index * 24;
                if (at + 24 > kCmdDbBytes - kCmdDbHeaderBytes) break;
                if (data[at] != 'x' && data[at] != 'v') continue;
                char id[9] = {};
                for (unsigned byte = 0; byte < 8; ++byte) {
                    const uint8_t c = data[at + byte];
                    id[byte] = c >= 0x20 && c < 0x7F ? char(c) : '?';
                }
                printf("PICASSO_GPU gmu: cmd-db id '%s' addr=%02x%02x%02x%02x len=%u\n", id,
                    data[at + 19], data[at + 18], data[at + 17], data[at + 16],
                    unsigned(data[at + 20] | (data[at + 21] << 8)));
            }
        }
    }
    /* The mapped alias is kept for the boot; nothing unmaps a region the GMU may later read. */
    return cmd.valid && cmd.gfxCount && cmd.mxCount && cmd.cxCount;
}

/* setup_volt_dependency_tbl: first primary entry >= vlvl, first secondary entry >= that primary value. */
bool arcVote(const CmdDb &cmd, const uint16_t *primary, size_t primaryCount,
    const uint16_t *secondary, size_t secondaryCount, uint16_t vlvl, uint32_t *vote)
{
    size_t primaryIndex = 0;
    while (primaryIndex < primaryCount && primary[primaryIndex] < vlvl) ++primaryIndex;
    if (primaryIndex == primaryCount) return false;
    const uint16_t current = primary[primaryIndex];
    size_t secondaryIndex = 0;
    while (secondaryIndex + 1 < secondaryCount && secondary[secondaryIndex] < current)
        ++secondaryIndex;
    if (secondary[secondaryIndex] < current) return false;
    *vote = (uint32_t(current) << 16) | (uint32_t(secondaryIndex & 0xFF) << 8) |
        uint32_t(primaryIndex & 0xFF);
    return true;
}

/* GPU OPP VLVL codes, picasso.dts gpu-opp-table opp-microvolt. The bin's frequencies index this table. */
uint16_t vlvlForFrequency(uint64_t hz)
{
    switch (hz) {
    case 750000000ull: return 0x180;
    case 700000000ull: return 0x140;
    case 670000000ull: return 0x140;
    case 625000000ull: return 0x100;
    case 540000000ull: return 0x100;
    case 500000000ull: return 0x0C0;
    case 400000000ull: return 0x080;
    case 275000000ull: return 0x040;
    default: return 0;
    }
}

/* msm_gpu nvmem-cells speed_bin: qfprom@780000 gpu_speed_bin@1ea bits 5..12 (picasso.dts). */
uint32_t readSpeedBin()
{
    Mapping qfprom = {};
    if (!mapRange(0x780000u, 0x7000u, qfprom)) {
        printf("PICASSO_GPU gmu: QFPROM did not map; default speed bin\n");
        return 0;
    }
    const uint16_t word = uint16_t(qfprom.base[0x1ea] | (qfprom.base[0x1eb] << 8));
    return (word >> 5) & 0xFFu;
}

/* gmu_clk_src: ROOT_EN, parent/divider, UPDATE, then release the force enable (clk-rcg2.c). */
bool setGmuClock(uint32_t sel, uint32_t div)
{
    uint32_t command = read32(gState->gpucc, kGmuRcgr);
    write32(gState->gpucc, kGmuRcgr, command | kRcgrRootEn);
    bool rootOn = false;
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        command = read32(gState->gpucc, kGmuRcgr);
        if (!(command & kRcgrRootOff)) { rootOn = true; break; }
        IODelay(1);
    }
    if (!rootOn) {
        printf("PICASSO_GPU gmu: gmu_clk_src root did not come on cmd=%08x\n",
            read32(gState->gpucc, kGmuRcgr));
        return false;
    }
    uint32_t cfg = read32(gState->gpucc, kGmuRcgr + 4);
    write32(gState->gpucc, kGmuRcgr + 4,
        (cfg & ~kRcgrCfgClear) | (sel << 8) | div);
    command = read32(gState->gpucc, kGmuRcgr);
    write32(gState->gpucc, kGmuRcgr, command | kRcgrUpdate);
    bool updated = false;
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        command = read32(gState->gpucc, kGmuRcgr);
        if (!(command & kRcgrUpdate)) { updated = true; break; }
        IODelay(1);
    }
    write32(gState->gpucc, kGmuRcgr, command & ~kRcgrRootEn);
    cfg = read32(gState->gpucc, kGmuRcgr + 4);
    printf("PICASSO_GPU gmu: gmu_clk_src sel=%u div=%u cfg=%08x updated=%d cmd=%08x\n",
        sel, div, cfg, updated, read32(gState->gpucc, kGmuRcgr));
    return updated && (cfg & ((7u << 8) | 0x1Fu)) == ((sel << 8) | div);
}

/* Programming clock tree: GPLL0 -> gcc_gpu_gpll0_div_clk_src (300 MHz) -> gmu_clk_src /1.5 (200 MHz). */
bool clocksOn()
{
    uint32_t value = read32(gState->gcc, kGccGpll0Enable);
    if (!(value & 1u)) {
        write32(gState->gcc, kGccGpll0Enable, value | 1u);
        bool locked = false;
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            if (read32(gState->gcc, kGccGpll0Status) & kPllLockDet) { locked = true; break; }
            IODelay(10);
        }
        if (!locked) printf("PICASSO_GPU gmu: GPLL0 did not lock\n");
    }
    /* GPLL0 is a LUCID alpha PLL: MODE 0x0 (OUTCTRL bit 0, VOTE_FSM_ENA bit 20), OPMODE 0x38
     * (RUN = 1), USER_CTL 0x0c (OUT_MASK bits 0..2), STATUS 0x30 (LOCK_DET bit 31). The enable_reg at
     * 0x52010 bit 0 is the FSM soft vote only; in the non-FSM mode the PLL is started through OPMODE. */
    uint32_t status = read32(gState->gcc, kGccGpll0Status);
    const uint32_t mode = read32(gState->gcc, 0x0);
    if (!(status & kPllLockDet) && !(mode & (1u << 20))) {
        write32(gState->gcc, 0x38, 1);
        bool locked = false;
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            status = read32(gState->gcc, kGccGpll0Status);
            if (status & kPllLockDet) { locked = true; break; }
            IODelay(10);
        }
        if (locked) {
            write32(gState->gcc, 0x0C, read32(gState->gcc, 0x0C) | 0x7u);
            write32(gState->gcc, 0x0, read32(gState->gcc, 0x0) | 0x1u);
        }
        printf("PICASSO_GPU gmu: GPLL0 started mode=%08x opmode=%08x status=%08x locked=%d\n",
            read32(gState->gcc, 0x0), read32(gState->gcc, 0x38), status, locked);
    }
    /* gcc_gpu_gpll0_clk_src (bit 15) and gcc_gpu_gpll0_div_clk_src (bit 16) enable the two GPU-side outputs. */
    value = read32(gState->gcc, kGpuccPllVote);
    write32(gState->gcc, kGpuccPllVote, value | (1u << 15) | (1u << 16));

    const bool configured = setGmuClock(6, 2);
    /* Recommended WAKEUP/SLEEP settings for gpu_cc_cx_gmu_clk (gpucc-lito.c probe). */
    uint32_t branch = read32(gState->gpucc, kGmuBranch);
    write32(gState->gpucc, kGmuBranch,
        branch | (0xFu << 8) | (0xFu << 4) | kBcbrEnable);
    bool running = false;
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        branch = read32(gState->gpucc, kGmuBranch);
        if (!(branch & kBcbrOff)) { running = true; break; }
        IODelay(1);
    }
    /* gpll0 mode/opmode/user_ctl/status: LUCID at offset 0 (OUTCTRL is MODE bit 0, RUN is OPMODE bit 0). */
    printf("PICASSO_GPU gmu: clock branch=%08x running=%d gpll0 enable=%08x mode=%08x opmode=%08x "
           "user=%08x status=%08x\n",
        branch, running, read32(gState->gcc, kGccGpll0Enable), read32(gState->gcc, 0x0),
        read32(gState->gcc, 0x38), read32(gState->gcc, 0x0C), read32(gState->gcc, kGccGpll0Status));
    return configured && running;
}

bool mapGmuState()
{
    if (!gState) gState = new GmuState;
    if (!gState) return false;
    GmuState &s = *gState;
    if (!s.gmu.base && !mapRange(adreno620::window(adreno620::WindowGmu).base,
            adreno620::window(adreno620::WindowGmu).bytes, s.gmu)) return false;
    if (!s.gpu.base && !mapRange(adreno620::window(adreno620::WindowGpu).base,
            adreno620::window(adreno620::WindowGpu).bytes, s.gpu)) return false;
    if (!s.gpucc.base && !mapRange(adreno620::window(adreno620::WindowGpucc).base,
            adreno620::window(adreno620::WindowGpucc).bytes, s.gpucc)) return false;
    if (!s.gcc.base && !mapRange(adreno620::window(adreno620::WindowGcc).base,
            adreno620::window(adreno620::WindowGcc).bytes, s.gcc)) return false;
    if (!s.rscc.base && !mapRange(adreno620::window(adreno620::WindowRscc).base,
            adreno620::window(adreno620::WindowRscc).bytes, s.rscc)) return false;
    if (!s.pdcCfg.base && !mapRange(adreno620::window(adreno620::WindowGmuPdcCfg).base,
            adreno620::window(adreno620::WindowGmuPdcCfg).bytes, s.pdcCfg)) return false;
    if (!s.pdcSeq.base && !mapRange(adreno620::window(adreno620::WindowGmuPdcSeq).base,
            adreno620::window(adreno620::WindowGmuPdcSeq).bytes, s.pdcSeq)) return false;
    /* Diagnostic only: the GMU bank's fault status when firmware execution stops. */
    if (!s.smmu.base && !mapRange(adreno620::window(adreno620::WindowSmmu).base,
            adreno620::window(adreno620::WindowSmmu).bytes, s.smmu)) return false;
    return true;
}

/* The board plan gives the GMU two streams: gmu_user (SID 4) and gmu_kernel (SID 5), each with the DT
 * stream mask 0x400, so the hardware may name them 4/5 or 0x404/0x405. The SMMU stage attaches exact SIDs
 * only, so a GMU port that emits a masked SID matches no SMR and falls through to the default S2CR. A
 * store into the icache aperture then aborts without a context-bank fault, which is what the CM3 HardFault
 * at its BSS clear shows. Give the kernel SMR the DT mask and route the user SID through a free slot.
 * GR0 SMR[n] = VALID|MASK<<16|SID, S2CR[n] = TYPE<<16|CBNDX (TRANSLATE is type 0). GR1 (0x1000) holds
 * CBFRSYNRA. The GPU contexts own the SMR slots the SMMU stage used, so only a free slot is claimed. */
void routeGmuStreams()
{
    Mapping &smmu = gState->smmu;
    if (!smmu.base) return;
    for (unsigned row = 0; row < 12; row += 4) {
        printf("PICASSO_GPU gmu: smmu smr%u-%u", row, row + 3);
        for (unsigned n = row; n < row + 4; ++n) printf(" %08x", read32(smmu, 0x800 + 4 * n));
        printf(" s2cr");
        for (unsigned n = row; n < row + 4; ++n) printf(" %08x", read32(smmu, 0xc00 + 4 * n));
        printf("\n");
    }
    /* Kernel stream: keep SID 5 matching and add the DT mask (SMR_MASK lives in bits 31:16, VALID in
     * bit 31, so mask 0x400 is 0x04000000 on top of 0x80000000|id = 0x84000005). */
    const uint32_t kernelSmr = read32(smmu, 0x800);
    if (kernelSmr == 0x80000005u) write32(smmu, 0x800, 0x84000005u);
    /* User stream: first free SMR slot at or after 3. */
    unsigned slot = 0;
    for (unsigned n = 3; n < 12; ++n) {
        if (!(read32(smmu, 0x800 + 4 * n) & 0x80000000u)) { slot = n; break; }
    }
    if (slot) {
        write32(smmu, 0x800 + 4 * slot, 0x84000004u);
        write32(smmu, 0xc00 + 4 * slot, 3u);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    /* Drop any cached miss for the context: CB3 ASID 4 TLBI, then wait for the TLB to quiesce. */
    write32(smmu, 0xB000 + 0x610, 4u);
    write32(smmu, 0xB000 + 0x7f0, 0xFFFFFFFFu);
    for (unsigned spin = 0; spin < 1000; ++spin) {
        if (!(read32(smmu, 0xB000 + 0x7f4) & 1u)) break;
        IOSleep(1);
    }
    printf("PICASSO_GPU gmu: stream kernel smr=%08x->%08x slot=%u user smr=%08x s2cr=%08x\n",
        kernelSmr, read32(smmu, 0x800), slot,
        slot ? read32(smmu, 0x800 + 4 * slot) : 0, slot ? read32(smmu, 0xc00 + 4 * slot) : 0);
    /* CPU-side proof that the icache page behind the CM3's faulting BSS store is backed and writable: IOVA
     * 0x1573f8 is the byte the firmware died on (PC 0x448); 0x1573f8 - 0x4000 is its offset in the object. */
    volatile uint32_t *probe = reinterpret_cast<volatile uint32_t *>(
        aliasBytes(gState->buffer[0].alias) + 0x1533F8);
    const uint32_t saved = *probe;
    *probe = 0xA5A5A5A5u;
    __asm__ volatile("dsb sy" ::: "memory");
    const uint32_t readback = *probe;
    *probe = saved;
    printf("PICASSO_GPU gmu: icache bss probe iova=1573f8 read=%08x wrote=a5a5a5a5 readback=%08x\n",
        saved, readback);
}

/* arm-smmu iova_to_phys: write the IOVA to CB_ATS1PR (0x800, 64-bit), wait for CB_ATSR.ACTIVE to
 * clear, then read PAR (0x50, 64-bit; bit 0 = fault, PA in bits 39:12). This runs the context bank's
 * own stage-1 walk, so it answers whether the CM3's failing IOVA is translatable and where it lands. */
uint64_t probeIova(size_t cb, uint64_t iova)
{
    Mapping &smmu = gState->smmu;
    write32(smmu, cb + 0x800, uint32_t(iova));
    write32(smmu, cb + 0x804, uint32_t(iova >> 32));
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned spin = 0; spin < 200; ++spin) {
        if (!(read32(smmu, cb + 0x8F0) & 1u)) break;
        IODelay(5);
    }
    const uint64_t lo = read32(smmu, cb + 0x50);
    const uint64_t hi = read32(smmu, cb + 0x54);
    return (hi << 32) | lo;
}

void probePayloadTranslations()
{
    Mapping &smmu = gState->smmu;
    if (!smmu.base) return;
    const size_t cb = 0xB000;   /* bank 3, the GMU's context */
    printf("PICASSO_GPU gmu: bank3 ttbr0=%08x%08x tcr=%08x sctlr=%08x\n",
        read32(smmu, cb + 0x24), read32(smmu, cb + 0x20),
        read32(smmu, cb + 0x30), read32(smmu, cb));
    const uint64_t probes[] = {
        0x4000, 0x157000, 0x1573F8, 0x157408, 0x158000,
        0x60000000, 0x60000800, 0x60004000, 0x60008000,
    };
    for (unsigned index = 0; index < sizeof(probes) / sizeof(probes[0]); ++index) {
        const uint64_t par = probeIova(cb, probes[index]);
        printf("PICASSO_GPU gmu: probe iova=%08x par=%08x%08x fault=%u pa=%08x\n",
            uint32_t(probes[index]), uint32_t(par >> 32), uint32_t(par), unsigned(par & 1u),
            uint32_t((par >> 12) & 0x0FFFFFFFu) << 12);
    }
}

/* The firmware's three init-table entries clear DTCM 0x10004ec0+0x1de4, DTCM 0x10007f80+4 and the icache
 * BSS at IOVA 0x157398+0x84a0 (loader image ends at 0x157398; TLV block 13). Filling the icache BSS with a
 * pattern before release turns the CM3's progress into a memory observation: the first word it did not
 * overwrite is the exact store it stopped at (or none, if the clear completed and it stopped later). The
 * icache object starts at IOVA 0x4000, so the byte offset is iova - 0x4000. */
constexpr uint32_t kBssFirst = 0x157398u;
constexpr uint32_t kBssBytes = 0x84A0u;
constexpr uint32_t kBssPattern = 0xDEADBEEFu;

void patternIcacheBss()
{
    volatile uint32_t *words = reinterpret_cast<volatile uint32_t *>(
        aliasBytes(gState->buffer[0].alias) + (kBssFirst - 0x4000u));
    for (unsigned index = 0; index < kBssBytes / 4; ++index) words[index] = kBssPattern;
    __asm__ volatile("dsb sy" ::: "memory");
    /* Immediate readback: distinguishes "the firmware cleared the pattern" from "the CPU write never
     * reached this page", which an end-of-boot read alone cannot. */
    printf("PICASSO_GPU gmu: bss pattern write first=%08x mid=%08x last=%08x\n",
        words[0], words[kBssBytes / 8], words[kBssBytes / 4 - 1]);
}

/* The CM3's real exception vectors (VTOR 0x200) cannot say which exception fired: the stacked frame only
 * has the interrupted PC. Install tiny ITCM stubs for vectors 2..15 (NMI, faults, SVCall, PendSV,
 * SysTick) that store their exception number at DTCM 0x10007fd0 and park, so one boot names the vector.
 * The stubs live in ITCM above the loader image (which ends at 0x3af8): movs r0,#n; ldr r1,[pc,#4];
 * str r0,[r1]; b .; .word 0x10007fd0. */
constexpr uint32_t kStubBase = 0x3B00u;
constexpr uint32_t kStubReport = 0x10007FD0u;

void installExceptionStubs()
{
    /* Only the fault vectors: NMI/SVCall/PendSV/SysTick belong to the firmware's own runtime (gmu13
     * proved it takes PendSV normally once its init starts), so they stay untouched. */
    for (unsigned vector = 3; vector <= 6; ++vector) {
        const uint32_t at = kStubBase + (vector - 2) * 0x10;
        /* Halfwords: movs r0,#vector; ldr r1,[pc,#4]; str r0,[r1]; b .; .word kStubReport */
        coreWrite(kCm3ItcmStart + at / 4, (0x4901u << 16) | (0x2000u | vector));
        coreWrite(kCm3ItcmStart + at / 4 + 1, (0xE7FEu << 16) | 0x6008u);
        coreWrite(kCm3ItcmStart + at / 4 + 2, kStubReport);
        coreWrite(kCm3ItcmStart + 0x200u / 4 + vector, at | 1u);   /* VTOR entry */
    }
    /* Clear the report and flush the ITCM writes before the CM3 can take an exception. */
    coreWrite(kCm3DtcmStart + (kStubReport - 0x10004000u) / 4, 0);
    __asm__ volatile("dsb sy" ::: "memory");
    printf("PICASSO_GPU gmu: vectors hf=%08x svc=%08x pendsv=%08x systick=%08x stub3=%08x%08x\n",
        coreRead(kCm3ItcmStart + 0x200u / 4 + 3), coreRead(kCm3ItcmStart + 0x200u / 4 + 11),
        coreRead(kCm3ItcmStart + 0x200u / 4 + 14), coreRead(kCm3ItcmStart + 0x200u / 4 + 15),
        coreRead(kCm3ItcmStart + (kStubBase + 0x10) / 4),
        coreRead(kCm3ItcmStart + (kStubBase + 0x10) / 4 + 1));
}

void checkExceptionStub()
{
    const uint32_t vector = coreRead(kCm3DtcmStart + (kStubReport - 0x10004000u) / 4);
    printf("PICASSO_GPU gmu: exception stub vector=%u (0 = no exception taken)\n", vector);
}

void checkIcacheBss()
{
    volatile uint32_t *words = reinterpret_cast<volatile uint32_t *>(
        aliasBytes(gState->buffer[0].alias) + (kBssFirst - 0x4000u));
    unsigned first = kBssBytes / 4;
    for (unsigned index = 0; index < kBssBytes / 4; ++index) {
        if (words[index] != kBssPattern) { first = index; break; }
    }
    printf("PICASSO_GPU gmu: bss clear %s", first == kBssBytes / 4 ? "left the pattern intact (no store ran)" : "");
    if (first != kBssBytes / 4) {
        printf("iova=%08x word=%08x (index %u of %u) ",
            kBssFirst + first * 4, words[first], first, kBssBytes / 4);
        printf("around:");
        const unsigned from = first >= 4 ? first - 4 : 0;
        for (unsigned index = from; index < from + 12 && index < kBssBytes / 4; ++index)
            printf(" %08x", words[index]);
        printf(" tail=%08x", words[kBssBytes / 4 - 1]);
    }
    printf("\n");
}

void printFences()
{
    printf("PICASSO_GPU gmu: fence allow=%08x status=%08x rscc status0=%08x control_req=%08x ack=%08x\n",
        coreRead(0x23B10), coreRead(0x23B13), rsccRead(kRscStatus0Drv0),
        coreRead(0x23B07), coreRead(0x23B08));
}

void printSmmuFault()
{    Mapping &smmu = gState->smmu;
    if (!smmu.base) return;
    /* Context banks are (pages + bank) * stride with ID1.PAGES = 8 and a 4 KiB stride, so bank n is at
     * 0x8000 + n*0x1000. Bank 0 is gfx3d_user, bank 2 gmu_user (SID 4), bank 3 gmu_kernel (SID 5). */
    for (unsigned bank = 0; bank < 5; ++bank) {
        const size_t cb = 0x8000 + bank * 0x1000;
        printf("PICASSO_GPU gmu: smmu bank%u fsr=%08x far=%08x%08x fsynr=%08x synra=%08x gfsr=%08x\n",
            bank, read32(smmu, cb + 0x58), read32(smmu, cb + 0x64), read32(smmu, cb + 0x60),
            read32(smmu, cb + 0x68), read32(smmu, 0x1000 + 0x400 + 4 * bank), read32(smmu, 0x48));
    }
}

bool mapPayloads()
{
    NativeAdrenoGmuPayload payload[5] = {};
    const IOReturn status = NativeAdrenoGmuContextPayloads(payload);
    if (status) {
        printf("PICASSO_GPU gmu: the SMMU context's GMU payloads are not ready status=%08x\n",
            (unsigned)status);
        return false;
    }
    const adreno620::GmuLayout &layout = adreno620::gmuLayout();
    /* The fixed GMU addresses this firmware and the kernel's SMMU/VM plan assume. Log and HFI have no fixed
     * address (the allocator places them after the dummy); their IOVAs are only passed to registers below. */
    if (payload[0].iova != layout.icache_iova || payload[0].bytes != layout.icache_bytes) {
        printf("PICASSO_GPU gmu: icache payload iova=%llx bytes=%llx, want %llx/%llx\n",
            static_cast<unsigned long long>(payload[0].iova),
            static_cast<unsigned long long>(payload[0].bytes),
            static_cast<unsigned long long>(layout.icache_iova),
            static_cast<unsigned long long>(layout.icache_bytes));
        return false;
    }
    if (payload[1].iova != layout.dummy_iova || payload[1].bytes < layout.dummy_bytes) {
        printf("PICASSO_GPU gmu: dummy payload iova=%llx bytes=%llx, want %llx/%llx\n",
            static_cast<unsigned long long>(payload[1].iova),
            static_cast<unsigned long long>(payload[1].bytes),
            static_cast<unsigned long long>(layout.dummy_iova),
            static_cast<unsigned long long>(layout.dummy_bytes));
        return false;
    }
    /* debug has no board field: the allocator's 0x7000 (NativeAdrenoGmuDefaultSizes) is the extent it maps. */
    if (payload[2].iova != 0x60400000u || payload[2].bytes < 0x7000u) {
        printf("PICASSO_GPU gmu: debug payload iova=%llx bytes=%llx\n",
            static_cast<unsigned long long>(payload[2].iova),
            static_cast<unsigned long long>(payload[2].bytes));
        return false;
    }
    for (unsigned index = 0; index < 5; ++index) {
        PayloadBuffer &buffer = gState->buffer[index];
        buffer.physical = payload[index].physical;
        buffer.bytes = payload[index].bytes;
        buffer.iova = payload[index].iova;
        if (!mapRange(payload[index].physical, payload[index].bytes, buffer.alias)) {
            printf("PICASSO_GPU gmu: payload %u at %llx did not map\n", index,
                static_cast<unsigned long long>(payload[index].physical));
            return false;
        }
        /* The allocator zeroed through a cacheable mapping. Push those lines back and drop them before this
         * file's non-cacheable alias is used, so no stale line can be written over the firmware later. */
        maintain(buffer, NativeAdrenoCleanInvalidate);
    }
    printf("PICASSO_GPU gmu: payloads icache=%llx dummy=%llx log=%llx hfi=%llx debug=%llx\n",
        static_cast<unsigned long long>(payload[0].iova),
        static_cast<unsigned long long>(payload[1].iova),
        static_cast<unsigned long long>(payload[3].iova),
        static_cast<unsigned long long>(payload[4].iova),
        static_cast<unsigned long long>(payload[2].iova));
    gState->buffersMapped = true;
    return true;
}

int hfiNotify(void *)
{
    /* The queues were just written through the CPU alias; make them visible to the GMU before the doorbell. */
    maintain(gState->buffer[4], NativeAdrenoClean);
    barrier();
    coreWrite(kGmuHost2GmuIntrSet, 1);
    barrier();
    return 0;
}

void rscAndPdc()
{
    GmuState &s = *gState;
    /* a6xx_load_rsc_ucode: RSCC sequencer ucode for sleep/wakeup (a650-family values, shared by a620). */
    rsccWrite(kRscStatus0Drv0, 1u << 24);
    rsccWrite(kRscPdcSlaveIdDrv0, 1);
    rsccWrite(kRscHiddenTcsCmd0Data, 0);
    rsccWrite(kRscHiddenTcsCmd0Addr, 0);
    rsccWrite(kRscHiddenTcsCmd0Data + 2, 0);
    rsccWrite(kRscHiddenTcsCmd0Addr + 2, 0);
    rsccWrite(kRscHiddenTcsCmd0Data + 4, 0x80000000u);
    rsccWrite(kRscHiddenTcsCmd0Addr + 4, 0);
    rsccWrite(kRscOverrideStartAddr, 0);
    rsccWrite(kRscPdcSeqStartAddr, 0x4520);
    rsccWrite(kRscPdcMatchValueLo, 0x4510);
    rsccWrite(kRscPdcMatchValueHi, 0x4514);
    const uint32_t sequence[5] = {0xEAAAE5A0u, 0xE1A1EBABu, 0xA2E0A581u, 0xECAC82E2u, 0x0020EDADu};
    for (unsigned index = 0; index < 5; ++index)
        rsccWrite(kRscSeqMem0Drv0 + index, sequence[index]);

    /* a650 family (a620 included): mainline's a6xx_gmu_rpmh_init sets pdc_in_aop for this family, so the AOP owns
     * the PDC sequencer and TCS: it skips the sequencer ucode and every TCS write, and only starts the sequencer and
     * enables the PDC. The PDC must not be left disabled while the HFI is live: the GMU's power requests are
     * sequenced through it, and an unserviced PDC handshake wedges the GMU's RSC path. */
    Mapping &cfg = s.pdcCfg;
    write32(cfg, kPdcSeqStartAddr * 4, 0);
    write32(cfg, kPdcEnable * 4, 0x80000001u);
    barrier();
    printf("PICASSO_GPU gmu: PDC enabled (AOP-owned TCS; mx=%x cx=%x xo=%x vrm=%x)\n",
        s.cmdDb.mxAddress, s.cmdDb.cxAddress, s.cmdDb.xoAddress, s.cmdDb.vrmAddress);
}

int hfiHealthy(void *)
{
    const uint32_t reset = coreRead(kCm3SysReset);
    const uint32_t status = coreRead(kGmuHfiCtrlStatus);
    const uint32_t ao = coreRead(kGmuAoHostIntrStatus);
    if ((ao & 0x29u) || reset != 0 || !(status & 1)) {
        printf("PICASSO_GPU hfi: unhealthy reset=%08x hfi=%08x ao=%08x\n", reset, status, ao);
        return 1;
    }
    return 0;
}

int hfiWait(void *, unsigned us)
{
    /* The GMU writes its ACK into the HFI queue through its non-cacheable mapping; drop any line this CPU
     * could have left before the next read. */
    maintain(gState->buffer[4], NativeAdrenoInvalidate);
    if (us) IOSleep((us + 999) / 1000);
    return 0;
}

/* Load the validated TLV image exactly as load_gmu_fw does: ITCM/DTCM data through the CM3 windows, icache
 * blocks into the mapped icache buffer, and every payload word read back before the reset is released. */
int loadFirmware(const uint8_t *gmu, size_t bytes)
{
    GmuState &s = *gState;
    const adreno620::GmuLayout &layout = adreno620::gmuLayout();
    const AdrenoFirmwareRegion regions[3] = {
        {uint32_t(layout.itcm_base), layout.itcm_bytes, 1},
        {uint32_t(layout.icache_iova), layout.icache_bytes, 2},
        {uint32_t(layout.dtcm_base), layout.dtcm_bytes, 3},
    };
    AdrenoFirmwareBlock blocks[32] = {};
    size_t count = 0;
    if (AdrenoFirmwarePlanGmu(gmu, bytes, regions, 3, blocks, 32, &count) !=
        AdrenoFirmwareSuccess) {
        printf("PICASSO_GPU gmu: firmware TLV plan refused\n");
        return -1;
    }
    uint64_t dataBytes = 0;
    for (size_t index = 0; index < count; ++index) dataBytes += blocks[index].bytes;
    printf("PICASSO_GPU gmu: TLV blocks=%lu data=%llu\n", static_cast<unsigned long>(count),
        static_cast<unsigned long long>(dataBytes));

    /* The allocator already zeroed every buffer through its cacheable mapping and mapPayloads() wrote those
     * zeroes back and dropped the lines, so the prealloc holes are zero in DRAM without touching them here.
     * All accesses to the non-cacheable alias are single 32-bit words: libc memset/memcpy use wide vector
     * stores and DC ZVA, which fault on this kernel's Device-nGnRnE mapping of DRAM. */
    PayloadBuffer &icache = s.buffer[0];
    for (size_t index = 0; index < count; ++index) {
        const AdrenoFirmwareBlock &block = blocks[index];
        if (!block.bytes) continue;
        if (block.region_identifier == 2) {
            if (block.address < layout.icache_iova ||
                uint64_t(block.address) - layout.icache_iova + block.bytes > icache.bytes) {
                printf("PICASSO_GPU gmu: icache block %x+%x out of range\n",
                    block.address, block.bytes);
                return -1;
            }
            volatile uint32_t *destination = reinterpret_cast<volatile uint32_t *>(
                aliasBytes(icache.alias) + (block.address - layout.icache_iova));
            for (uint32_t at = 0; at < block.bytes; at += 4)
                destination[at / 4] = le32(gmu + block.payload_offset + at);
        } else {
            /* load_gmu_fw: (block address - region base)/4 + the CM3 window's start dword. */
            const uint32_t region = block.region_identifier == 1 ?
                uint32_t(layout.itcm_base) : uint32_t(layout.dtcm_base);
            const uint32_t start = (block.region_identifier == 1 ?
                kCm3ItcmStart : kCm3DtcmStart) + (block.address - region) / 4;
            for (uint32_t at = 0; at < block.bytes; at += 4)
                coreWrite(start + at / 4, le32(gmu + block.payload_offset + at));
        }
    }
    barrier();
    maintain(icache, NativeAdrenoClean);

    /* Read back every payload word before releasing the CM3, exactly as the pinned startup does. */
    for (size_t index = 0; index < count; ++index) {
        const AdrenoFirmwareBlock &block = blocks[index];
        if (!block.bytes) continue;
        uint32_t value = 0;
        if (block.region_identifier == 2) maintain(icache, NativeAdrenoInvalidate);
        for (uint32_t at = 0; at < block.bytes; at += 4) {
            const uint32_t want = le32(gmu + block.payload_offset + at);
            if (block.region_identifier == 2) {
                value = *reinterpret_cast<volatile uint32_t *>(
                    aliasBytes(icache.alias) + (block.address - layout.icache_iova) + at);
            } else {
                const uint32_t region = block.region_identifier == 1 ?
                    uint32_t(layout.itcm_base) : uint32_t(layout.dtcm_base);
                const uint32_t start = (block.region_identifier == 1 ?
                    kCm3ItcmStart : kCm3DtcmStart) + (block.address - region) / 4;
                value = coreRead(start + at / 4);
            }
            if (value != want) {
                printf("PICASSO_GPU gmu: payload mismatch block=%zu at=%x have=%08x want=%08x\n",
                    index, at, value, want);
                return -1;
            }
        }
    }
    return 0;
}

/* a6xx_gmu_start: assert CM3 reset, release it, then wait for the firmware's init word. */
int releaseAndPoll(uint32_t mask, uint32_t ready, uint32_t *observed, unsigned milliseconds)
{
    coreWrite(kCm3SysReset, 1);
    barrier();
    coreWrite(kCm3SysReset, 0);
    barrier();
    for (unsigned attempt = 0; attempt < milliseconds; ++attempt) {
        *observed = coreRead(kCm3FwInitResult);
        if ((*observed & mask) == ready) return 0;
        IOSleep(1);
    }
    return 1;
}

/* Mainline a6xx_gmu_fw_start calls a6xx_gmu_gfx_rail_on and a6xx_sptprac_enable only for the legacy (a630-family)
 * GMU. a620 is an a650-family GPU: those OOB bits do not exist there and the GMU firmware owns the GX rail, so the
 * modern AOP-owned PDC path in rscAndPdc() is the whole board-side power contract. */
int startCm3()
{
    GmuState &s = *gState;
    const PayloadBuffer &log = s.buffer[3];
    const PayloadBuffer &hfi = s.buffer[4];
    coreWrite(kCm3FwInitResult, 0);
    coreWrite(kCm3BootConfig, 2);                    /* TCM_BOOT (kgsl_gmu.h) */
    coreWrite(kGmuHfiQtblAddr, uint32_t(hfi.iova));
    coreWrite(kGmuHfiQtblInfo, 1);
    coreWrite(kGmuAhbFenceRange0, kGmuFenceRange);
    coreWrite(kCm3Cfg, kGmuCm3CfgReset);
    coreWrite(kGmuHfiSfrAddr, s.chipIdCompressed);
    /* LOGMEM_SIZE is SZ_4K in kgsl_gmu.h; the field is (4K units - 1) below the address. */
    coreWrite(kGmuPwrColCpMsg, uint32_t(log.iova) & 0xFFFFF000u);
    coreWrite(kGmuSysBusConfig, 1);
    coreWrite(kGmuIcacheConfig, 1);
    coreWrite(kGmuDcacheConfig, 1);
    coreWrite(kGmuPwrColInterFrameCtrl, kGmuInterFrameCtrl);
    uint32_t rpmh = coreRead(kGmuRpmhCtrl);
    coreWrite(kGmuRpmhCtrl, rpmh | kRpmhEnableMask);
    for (uint32_t side = 0; side < 4; ++side)
        gpuWrite(kGbifQsbSide0 + side, kGbifQosValue);
    gpuWrite(kRbbmGbifClientQosCntl, kGbifClientQos);
    gpuWrite(kGbifHalt, 0);
    coreWrite(kGmuPwrColCpResp, 0);
    barrier();

    /* The firmware boot handshake: core version measured from the container's DTCM block (0x20010006) selects
     * the mask; anything <= 0x20010004 expects the legacy 0xBABEFACE word (a6xx_gmu_start). Mainline asserts
     * reset before releasing it; the poll budget is mainline's 10 s (100 us x 100000), slept so a boot that
     * never signals cannot peg a CPU. */
    const uint32_t resultMask = s.firmwareCore <= 0x20010004u ? 0xFFFFFFFFu : 0x1FFu;
    const uint32_t resultReady = s.firmwareCore <= 0x20010004u ? 0xBABEFACEu : 0x100u;
    printf("PICASSO_GPU gmu: config reset=%08x boot=%08x qtbl=%08x/%08x fence=%08x cfg=%08x sfr=%08x "
           "msg=%08x rpmh=%08x general7=%08x sysbus=%08x icache=%08x dcache=%08x frame=%08x\n",
        coreRead(kCm3SysReset), coreRead(kCm3BootConfig), coreRead(kGmuHfiQtblAddr),
        coreRead(kGmuHfiQtblInfo), coreRead(kGmuAhbFenceRange0), coreRead(kCm3Cfg),
        coreRead(kGmuHfiSfrAddr), coreRead(kGmuPwrColCpMsg), coreRead(kGmuRpmhCtrl),
        coreRead(kGmuGeneral7), coreRead(kGmuSysBusConfig), coreRead(kGmuIcacheConfig),
        coreRead(kGmuDcacheConfig), coreRead(kGmuPwrColInterFrameCtrl));
    /* The CM3's reset handler increments the word at DTCM 0x3FDC before it clears the stack and jumps to the
     * main image. A released, clocked CM3 must change it; a CM3 whose clock is dead leaves it untouched, which
     * distinguishes "no bus to the GMU" from "firmware stopped later". */
    const uint32_t bootWordBefore = coreRead(kCm3DtcmStart + 0xFF7);
    patternIcacheBss();
    installExceptionStubs();
    uint32_t observed = 0;
    int poll = releaseAndPoll(resultMask, resultReady, &observed, 10000);
    uint32_t bootWord = coreRead(kCm3DtcmStart + 0xFF7);
    printf("PICASSO_GPU gmu: CM3 released core=%08x result=%08x want=%08x poll=%d reset=%08x hfi=%08x "
           "ao=%08x intr=%08x busy=%08x seqbusy=%08x bootword=%08x->%08x\n",
        s.firmwareCore, observed, resultReady, poll, coreRead(kCm3SysReset),
        coreRead(kGmuHfiCtrlStatus), coreRead(kGmuAoHostIntrStatus),
        coreRead(kGmuGmu2HostIntrInfo), coreRead(kCm3FwBusy), rsccRead(kRscSeqBusyDrv0),
        bootWordBefore, bootWord);
    if (poll) {
        /* No instruction has run: the CM3's clock is the suspect. Try the other GPLL0 output (600 MHz / 3 is
         * also 200 MHz) and then plain XO, so one boot identifies which source the GMU can start from. */
        if (bootWord == bootWordBefore) {
            printf("PICASSO_GPU gmu: CM3 never executed; retrying gmu_clk_src sel=5 div=5\n");
            setGmuClock(5, 5);
            observed = 0;
            poll = releaseAndPoll(resultMask, resultReady, &observed, 10000);
            bootWord = coreRead(kCm3DtcmStart + 0xFF7);
            printf("PICASSO_GPU gmu: CM3 retry result=%08x poll=%d bootword=%08x\n",
                observed, poll, bootWord);
        }
        if (poll && bootWord == bootWordBefore) {
            printf("PICASSO_GPU gmu: CM3 still never executed; retrying at XO sel=0 div=0\n");
            setGmuClock(0, 0);
            observed = 0;
            poll = releaseAndPoll(resultMask, resultReady, &observed, 3000);
            bootWord = coreRead(kCm3DtcmStart + 0xFF7);
            printf("PICASSO_GPU gmu: CM3 XO retry result=%08x poll=%d bootword=%08x\n",
                observed, poll, bootWord);
        }
    }
    if (poll) {
        printSmmuFault();
        printFences();
        checkExceptionStub();
        checkIcacheBss();
        /* The HardFault handler at 0x4a0 dumps 0x100 bytes from DTCM 0x100054a0 to the dummy buffer at
         * 0x60000800 (stage marker 0x13). The exception frame starts there; read it with 32-bit accesses. */
        volatile uint32_t *frame = reinterpret_cast<volatile uint32_t *>(
            aliasBytes(s.buffer[1].alias) + 0x800);
        for (unsigned base = 0; base < 64; base += 16) {
            printf("PICASSO_GPU gmu: fault frame +%02x", base * 4);
            for (unsigned index = base; index < base + 16; ++index)
                printf(" %08x", frame[index]);
            printf("\n");
        }
        /* The same report through the DTCM register window: if the dummy copy above is empty but this is
         * not, the handler ran and its store through the GMU data port failed (stream routing), not the
         * fault capture. */
        const uint32_t dtcmFrame = kCm3DtcmStart + (0x100054a0u - 0x10004000u) / 4;
        for (unsigned base = 0; base < 64; base += 16) {
            printf("PICASSO_GPU gmu: dtcm report +%02x", base * 4);
            for (unsigned index = base; index < base + 16; ++index)
                printf(" %08x", coreRead(dtcmFrame + index));
            printf("\n");
        }
        /* The C runtime switches to a stack at DTCM 0x10006ca0 (0xc78 loads 0x10006c40 and adds 0x60);
         * a parked firmware still has its call chain there. */
        const uint32_t dtcmStack = kCm3DtcmStart + (0x10006900u - 0x10004000u) / 4;
        for (unsigned base = 0; base < 0xE8; base += 16) {
            printf("PICASSO_GPU gmu: dtcm stack +%03x", 0x2900 + base * 4);
            for (unsigned index = base; index < base + 16; ++index)
                printf(" %08x", coreRead(dtcmStack + index));
            printf("\n");
        }
        /* The reset handler runs on MSP = 0x10007f80 and the init-table loop still uses it; any exception
         * taken in that phase stacks R0..xPSR at 0x10007f60..0x10007f80, naming the interrupted PC. */
        const uint32_t dtcmMsp = kCm3DtcmStart + (0x10007F00u - 0x10004000u) / 4;
        for (unsigned base = 0; base < 0x100; base += 16) {
            printf("PICASSO_GPU gmu: dtcm msp +%03x", 0x3F00 + base * 4);
            for (unsigned index = base; index < base + 16; ++index)
                printf(" %08x", coreRead(dtcmMsp + index));
            printf("\n");
        }
        /* The GMU log and HFI queues live in the two buffers the firmware was given. */
        for (unsigned which = 3; which <= 4; ++which) {
            volatile uint32_t *words = reinterpret_cast<volatile uint32_t *>(
                aliasBytes(s.buffer[which].alias));
            printf("PICASSO_GPU gmu: buffer%u iova=%llx", which,
                static_cast<unsigned long long>(s.buffer[which].iova));
            for (unsigned index = 0; index < 16; ++index) printf(" %08x", words[index]);
            printf("\n");
        }
        return -1;
    }
    coreWrite(kGmuHfiCtrlInit, 1);
    barrier();
    observed = 0;
    poll = pollCore(kGmuHfiCtrlStatus, 1, 1, 1000, 100, &observed);
    printf("PICASSO_GPU gmu: HFI controller status=%08x poll=%d\n", observed, poll);
    if (poll) return -1;
    return 0;
}

} // namespace

extern "C" int native_adreno620_gmu_boot(const uint8_t *gmu, size_t gmuBytes,
    const uint8_t *sqe, size_t sqeBytes)
{
    (void)sqe;
    (void)sqeBytes;
    if (!gmu || !gmuBytes) return -1;
    if (!gState) gState = new GmuState;
    GmuState &s = *gState;
    if (s.booted) return -1;
    printf("PICASSO_GPU gmu: cold start begin bytes=%zu\n", gmuBytes);

    if (!mapGmuState() || !mapPayloads()) return -1;
    routeGmuStreams();
    probePayloadTranslations();

    /* HFI queues first: the GMU must never see a queue while it is stopped. */
    if (AdrenoHfiInitializeStopped(&s.hfi,
            reinterpret_cast<volatile uint32_t *>(s.buffer[4].alias.base),
            size_t(s.buffer[4].bytes), s.buffer[4].iova, &s, hfiNotify) != AdrenoHfiSuccess) {
        printf("PICASSO_GPU gmu: HFI queue initialization refused\n");
        return -1;
    }
    s.hfiInitialized = true;
    maintain(s.buffer[4], NativeAdrenoClean);

    if (!clocksOn()) return -1;

    /* gmu_start: clear stale interrupts, mask everything the host does not service. HFI bit 0 is polled
     * directly (kgsl_hfi.h HFI_IRQ_MASK = 0x8006), so only the fault/debug sources raise the line. */
    coreWrite(kGmuGmu2HostIntrClr, 0xFFFFFFFFu);
    coreWrite(kGmuAoHostIntrClr, 0xFFFFFFFFu);
    coreWrite(kGmuGmu2HostIntrMask, ~0x8006u);
    coreWrite(kGmuAoHostIntrMask, ~0x29u);
    barrier();
    const uint32_t aoStatus = coreRead(kGmuAoHostIntrStatus);
    if (aoStatus & 0x29u) {
        printf("PICASSO_GPU gmu: AO fault status=%08x before start\n", aoStatus);
        return -1;
    }
    const uint32_t initialReset = coreRead(kCm3SysReset);
    if (initialReset != 1) {
        coreWrite(kCm3SysReset, 1);
        barrier();
        uint32_t held = 0;
        if (pollCore(kCm3SysReset, 1, 1, 100, 10, &held)) {
            printf("PICASSO_GPU gmu: CM3 reset would not assert status=%08x\n", held);
            return -1;
        }
    }
    printf("PICASSO_GPU gmu: entry reset=%08x fw_init=%08x hfi_ctl=%08x\n",
        initialReset, coreRead(kCm3FwInitResult), coreRead(kGmuHfiCtrlStatus));

    /* A6xx GEN3 cold boot (a6xx_gmu_fw_start, GMU_COLD_BOOT): veto FAL10 (both interfaces, as mainline does
     * for the a650 family), retain TCM, load RSC/PDC ucode. */
    coreWrite(kGmuCxFalNextIntf, 1);
    coreWrite(kGmuCxFalIntf, 1);
    coreWrite(kGmuGeneral7, 1);
    s.chipIdCompressed = (adreno620::ChipId & kChipIdCoreMask) |
        (adreno620::ChipMinor << 12) | (adreno620::ChipPatch << 8);
    s.cmdDb.valid = readCmdDb(s.cmdDb);
    printf("PICASSO_GPU gmu: chipid=%08x command-db=%d gfx=%zu mx=%zu cx=%zu\n",
        s.chipIdCompressed, s.cmdDb.valid, s.cmdDb.gfxCount, s.cmdDb.mxCount,
        s.cmdDb.cxCount);
    rscAndPdc();

    if (loadFirmware(gmu, gmuBytes)) return -1;

    /* The version block is part of the DTCM payload just loaded; read it before the CM3 runs (load_gmu_version_info). */
    s.firmwareCore = coreRead(kCm3DtcmStart + 0xFF8);
    s.firmwarePwr = coreRead(kCm3DtcmStart + 0xFFA);
    s.firmwareHfi = coreRead(kCm3DtcmStart + 0xFFC);
    printf("PICASSO_GPU gmu: firmware core=%08x pwr=%08x hfi=%08x\n",
        s.firmwareCore, s.firmwarePwr, s.firmwareHfi);
    if (!s.firmwareCore || !s.firmwareHfi) {
        printf("PICASSO_GPU gmu: firmware version block was not loaded\n");
        return -1;
    }

    if (startCm3()) return -1;
    s.booted = true;
    printf("PICASSO_GPU gmu: cold start complete\n");
    return 0;
}

extern "C" int native_adreno620_hfi_handshake(void)
{
    if (!gState || !gState->booted || !gState->hfiInitialized) {
        printf("PICASSO_GPU hfi: GMU is not running\n");
        return -1;
    }
    GmuState &s = *gState;
    if (s.hfiStarted) return -1;

    /* H2F_MSG_PERF_TBL + H2F_MSG_BW_VOTE_TBL + H2F_MSG_CORE_FW_START (kgsl_hfi.c hfi_start for the modern
     * wire profile; A620 has no AOP mailbox for the ACD table and the driver does not enable IFPC). The votes
     * are the kernel's own ARC encodings; when the command DB is readable they are exact, otherwise zero and
     * the GX power request that follows will say so. */
    AdrenoHfiStartupConfig config = {};
    const adreno620::PwrlevelBin *bin = nullptr;
    s.speedBin = readSpeedBin();
    bin = adreno620::pwrlevelBinForSpeed(s.speedBin);
    if (!bin) {
        printf("PICASSO_GPU hfi: speed bin %02x not in the board table; default bin\n", s.speedBin);
        bin = &adreno620::defaultPwrlevelBin();
    }
    if (!bin || bin->count < 2 || bin->count > 16) {
        printf("PICASSO_GPU hfi: unusable GPU power-level bin\n");
        return -1;
    }
    s.votesAvailable = 0;
    for (size_t index = 0; index < bin->count; ++index) {
        const uint64_t hz = bin->levels[bin->count - 1 - index].frequency_hz;
        const uint16_t vlvl = vlvlForFrequency(hz);
        uint32_t vote = 0;
        if (s.cmdDb.valid && arcVote(s.cmdDb, s.cmdDb.gfx, s.cmdDb.gfxCount,
                s.cmdDb.mx, s.cmdDb.mxCount, vlvl, &vote))
            ++s.votesAvailable;
        config.gpu[index].hz = hz;
        config.gpu[index].vote = vote;
    }
    config.gpu_count = unsigned(bin->count);
    config.gmu_count = 2;
    config.gmu[0].hz = 0;
    config.gmu[0].vote = 0;
    config.gmu[1].hz = kGmuHz;
    if (s.cmdDb.valid) {
        uint32_t vote = 0;
        if (arcVote(s.cmdDb, s.cmdDb.cx, s.cmdDb.cxCount, s.cmdDb.mx, s.cmdDb.mxCount, 48, &vote))
            config.gmu[1].vote = vote;
    }

    /* Mainline a6xx_build_bw_table: one "off" bandwidth level for the targets whose GMU does not scale bus
     * bandwidth itself (gmu_core_scales_bandwidth() is false before A640, which includes A620). */
    config.bandwidth_count = 1;
    config.cnoc_count = 3;
    config.ddr_count = 3;
    config.cnoc_wait_mask = 0x05;
    config.ddr_wait_mask = 0x07;
    const uint32_t cnocAddresses[3] = {0x50034, 0x5007C, 0x5004C};
    const uint32_t ddrAddresses[3] = {0x50000, 0x5005C, 0x5000C};
    for (unsigned index = 0; index < 3; ++index) {
        config.cnoc_addresses[index] = cnocAddresses[index];
        config.ddr_addresses[index] = ddrAddresses[index];
    }
    config.cnoc_data[0][0] = 0x40000000u;
    config.cnoc_data[0][1] = 0x00000000u;
    config.cnoc_data[0][2] = 0x40000000u;
    config.cnoc_data[1][0] = 0x60000001u;
    config.cnoc_data[1][1] = 0x20000001u;
    config.cnoc_data[1][2] = 0x60000001u;
    config.ddr_data[0][0] = 0x40000000u;
    config.ddr_data[0][1] = 0x40000000u;
    config.ddr_data[0][2] = 0x40000000u;
    config.enable_ifpc = 0;

    if (AdrenoHfiStartupValidate(&config) != AdrenoHfiStartupSuccess) {
        printf("PICASSO_GPU hfi: startup configuration refused (gpu levels=%u votes=%u)\n",
            config.gpu_count, s.votesAvailable);
        return -1;
    }
    printf("PICASSO_GPU hfi: perf gpu=%u gmu=%u votes=%u bin=%02x gmu-vote=%08x\n",
        config.gpu_count, config.gmu_count, s.votesAvailable, s.speedBin, config.gmu[1].vote);

    /* Cookie: NativeAdrenoKernelTime::now_us/wait_us are the production platform time source and reject a non-null
     * cookie (tests inject their own); hfiWait/hfiHealthy ignore it. Passing &s here made the very first now_us in
     * AdrenoHfiStartupRun fail with ClockError before any command was published. */
    AdrenoHfiStartupServices services = {
        nullptr, &NativeAdrenoKernelTime::now_us, &hfiWait, &hfiHealthy,
        2048, 100, 200000, 5000000,
    };
    if (AdrenoHfiStartupValidateServices(&services) != AdrenoHfiStartupSuccess) {
        printf("PICASSO_GPU hfi: startup services refused\n");
        return -1;
    }
    AdrenoHfiStartup startup = {};
    AdrenoHfiStartupResult result = {};
    const AdrenoHfiStartupStatus status = AdrenoHfiStartupRun(&startup, &s.hfi, &config,
        &services, &result);
    printf("PICASSO_GPU hfi: run status=%d stage=%u transport=%d fw=%u commands=%u polls=%u "
           "elapsed=%llu skipped=%u\n",
        status, unsigned(result.stage), int(result.transport_status), result.firmware_error,
        result.published_commands, result.poll_reads,
        static_cast<unsigned long long>(result.elapsed_us), s.hfi.skipped);
    if (status != AdrenoHfiStartupSuccess) {
        printf("PICASSO_GPU hfi: first mismatched response header=%08x ret=%08x code=%08x\n",
            s.hfi.skipped_header, s.hfi.skipped_ret, s.hfi.skipped_code);
        return -1;
    }
    s.hfiStarted = true;
    printf("PICASSO_GPU hfi: core firmware started\n");
    return 0;
}
