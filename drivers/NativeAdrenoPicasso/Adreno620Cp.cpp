/* SPDX-License-Identifier: MIT */
/* Stage 4 for the picasso A620: hw_init, SQE upload and the CP ring.
 *
 * The sequence is mainline's a6xx_hw_init() for an a650-family device (a620 is
 * ADRENO_6XX_GEN3 in the catalog), with the values this chip's vendor driver
 * (a6xx_core_a620) uses where the two differ: PC_DBG_ECO_CNTL bit 9
 * (disable_tseskip), the a620 hang-detect span, the UBWC 3.0/hbb14 config
 * (SM7250's DT values), and the a650 GBIF QSB/QOS list.
 *
 * What is deliberately absent:
 *   - ZAP. Mainline loads a620_zap.mbn through the secure PIL and then makes
 *     the CP switch out of secure mode (CP_SET_SECURE_MODE). This port has no
 *     PIL/SCM path, so the CP is left in the mode the boot chain left it in
 *     and no secure-mode packet is sent. The no-op IB below shows whether the
 *     CP runs at all; if it does not, that is the next piece.
 *   - Preemption. One ring, so a6xx_preempt_hw_init() is a no-op and no yield
 *     packet is emitted.
 *   - The SCM "GPU SMMU aperture" call that adreno_hw_init() makes on kernels
 *     that have it; older drivers and the vendor driver work without it.
 *
 * All CPU access to the GPU context buffers goes through uncached aliases
 * obtained by mapping their physical extents, with cache maintenance by
 * physical address around every hand-off, exactly like Adreno620Gmu.cpp.
 * Nothing here frees or remaps a buffer the GPU may still read. */
#include "Adreno620Probe.h"
#include "Adreno620ProbeCore.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoA620HwInit.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoKernelTime.h"
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoUserClient.h"
#include "AdrenoPM4.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <stddef.h>
#include <stdint.h>

/* The driver's client-BO reader falls back to a board DRAM reader when a BO has no kernel mapping (system BOs). This port had none
 * installed, so a stall's IB dump was all 0xdeadbeef. Reads one word through a temporary kernel mapping of its page. */
extern "C" int (*native_adreno_dram_read32)(uint64_t physical, uint32_t *value);
extern "C" int (*native_adreno_cached_read32)(uint64_t physical, uint32_t *value);
static int dramRead32(uint64_t physical, uint32_t *value)
{
    if (!value) return -1;
    IOMemoryDescriptor *page = IOMemoryDescriptor::withPhysicalAddress(physical & ~UINT64_C(0xfff), 4096, kIODirectionIn);
    if (!page) return -1;
    IOMemoryMap *map = page->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly);
    page->release();
    if (!map) return -1;
    const uint8_t *base = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(map->getAddress()));
    *value = *reinterpret_cast<volatile const uint32_t *>(base + (physical & 0xfff));
    map->release();
    return 0;
}
/* The kernel's cacheable physical aperture (the direct map), which is how the driver's line audit sees a client's dirty cache lines:
 * an IO mapping of the same page takes the page's IO attribute and would read DRAM instead. */
extern "C" vm_offset_t phystokv(uint64_t pa);
extern "C" uint64_t gPhysBase, gPhysSize;
static int cachedRead32(uint64_t physical, uint32_t *value)
{
    if (!value || physical < gPhysBase || physical - gPhysBase >= gPhysSize) return -1;
    const vm_offset_t alias = phystokv(physical & ~UINT64_C(0xfff));
    if (!alias) return -1;
    *value = *reinterpret_cast<volatile const uint32_t *>(static_cast<uintptr_t>(alias) + (physical & 0xfff));
    return 0;
}

/* NativeAdrenoPublish.cpp has no header: these are its exported entry points. */
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *,
    NativeAdrenoRing *, const NativeAdrenoRingClock *, const NativeAdrenoTimer *,
    const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" IOService *native_adreno_published_service(void);

/* The staged AGXAccelerator (drivers/NativeAdreno/NativeAdrenoAccelerator.cpp).
 * build-native-adreno-accelerator.py removes its initializer from
 * __mod_init_func and exports it, because the class must be registered after
 * the original IOGPUFamily kext has published IOAcceleratorES. */
extern "C" void native_adreno_accelerator_static_init(void)
    __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp") __attribute__((weak));
extern "C" IOService *native_adreno_accelerator_allocate(void) __attribute__((weak));
extern "C" unsigned native_adreno_accelerator_instance_size(void) __attribute__((weak));
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;

/* The boot addon's progress (bsd/kern/kern_exec.c): extensions mount, daemons
 * mount, wallpaper overlay, each stage/status. Read by the report thread below
 * once launchd has run the first exec that triggers the mount. */
extern "C" volatile int native_gpu_addon_stage, native_gpu_addon_status;
extern "C" volatile int native_gpu_addon2_stage, native_gpu_addon2_status;
extern "C" volatile int native_gpu_addon3_stage, native_gpu_addon3_status;
/* Submissions and IBs the production submit object has seen: the plugin's
 * arrival is visible here even though its own report is not printed. */
extern "C" volatile uint32_t native_adreno_submit_ibs[4];
/* The plugin's own report lines and connection counters (NativeAdrenoPublish). */
extern "C" const char *native_adreno_metal_report(void);
extern "C" const char *native_adreno_metal_report2(void);
extern "C" uint32_t native_adreno_metal_opens(void);
extern "C" uint32_t native_adreno_metal_foreign_opens(void);
extern "C" uint32_t native_adreno_metal_last(uint32_t *selector, uint32_t *status);
extern "C" uint32_t native_adreno_metal_marks(uint32_t *out, uint32_t capacity);
/* Why the latest user connection's mount failed ([0] 1 no address space,
 * 2 the core's open, 3 the window scope; [1] its IOReturn; [3] count), the
 * table-switch proof and the CP switch check. */
extern "C" volatile uint32_t native_adreno_mount_fail[4];
extern "C" volatile uint32_t native_adreno_table_switches[8];
extern "C" volatile uint32_t native_adreno_cp_aperture[4];
extern "C" volatile uint32_t native_adreno_cp_check[6];

namespace {

constexpr uint64_t kGpuWindowBase = 0x3d00000u;
constexpr uint64_t kGpuWindowBytes = 0x40000u;
constexpr uint64_t kGmuWindowBase = 0x3d6a000u;
constexpr uint64_t kGmuWindowBytes = 0x30000u;
constexpr uint64_t kGmuDownshift = kGmuWindowBase - kGpuWindowBase;   /* 0x6a000 */

/* GPU-window dwords used by hw_init (a6xx.xml, A6XX variants). The generated
 * table covers the rest; these are the ones it does not name. */
constexpr uint32_t kGpuUcheWriteRangeMax = 0x0E05;   /* reg64 */
constexpr uint32_t kGpuUcheWriteThruBase = 0x0E07;   /* reg64 */
constexpr uint32_t kGpuUcheTrapBase = 0x0E09;        /* reg64 */
constexpr uint32_t kGpuTpl1BicubicWeights = 0xB608;  /* array, 5 entries */
constexpr uint32_t kGpuCpPerfctrCpSel0 = 0x08D0;
constexpr uint32_t kGpuUcheFilterCntl = 0x0E18;
constexpr uint32_t kGpuCpRoqThresholds1 = 0x08C1;
constexpr uint32_t kGpuCpRoqThresholds2 = 0x08C2;
constexpr uint32_t kGpuCpMemPoolSize = 0x08C3;
constexpr uint32_t kGpuSpNcModeCntl = 0x0AE02;
constexpr uint32_t kGpuRbbmNcModeCntl = 0x00534;
constexpr uint32_t kGpuRbbmSecvidTsbAddrModeCntl = 0x0F810;
constexpr uint32_t kGpuGbifQsbSide0 = 0x3C03;        /* side 0..3 contiguous */
constexpr uint32_t kGpuCpAddrModeCntl = 0x00842;
constexpr uint32_t kGpuVscAddrModeCntl = 0x00C01;
constexpr uint32_t kGpuGrasAddrModeCntl = 0x08601;
constexpr uint32_t kGpuRbAddrModeCntl = 0x08E05;
constexpr uint32_t kGpuPcAddrModeCntl = 0x09E01;
constexpr uint32_t kGpuHlsqAddrModeCntl = 0x0BE05;
constexpr uint32_t kGpuVfdAddrModeCntl = 0x0A601;
constexpr uint32_t kGpuVpcAddrModeCntl = 0x09601;
constexpr uint32_t kGpuUcheAddrModeCntl = 0x00E00;
constexpr uint32_t kGpuSpAddrModeCntl = 0x0AE01;
constexpr uint32_t kGpuTpl1AddrModeCntl = 0x0B601;

/* GMU-window dwords (a6xx_gmu.xml). */
constexpr uint32_t kGmuSptpracClockControl = 0x1A880;
constexpr uint32_t kGmuCgcModeCntl = 0x23B09;
constexpr uint32_t kGmuCgcDelayCntl = 0x23B0A;
constexpr uint32_t kGmuCgcHystCntl = 0x23B0B;
constexpr uint32_t kGmuCxBusyMask = 0x23B0E;
constexpr uint32_t kGmuAhbFenceStatus = 0x23B13;
constexpr uint32_t kGmuAhbFenceStatusClr = 0x23B14;
constexpr uint32_t kGmuGxPwrStatus = 0x1F8D0;
constexpr uint32_t kGmuPowerCounterSelect0 = 0x1F841;
constexpr uint32_t kGmuPowerCounterEnable = 0x1F840;

/* Values from mainline hw_init()/a6xx_set_hwcg() for a 6XX_GEN3 (a650 family)
 * device, or from the vendor a620 core entry where noted. */
constexpr uint32_t kUcheTrapStartLo = 0xFFFFF000u;
constexpr uint32_t kUcheTrapStartHi = 0x0001FFFFu;
constexpr uint32_t kUcheTrapRangeMaxLo = 0xFFFFFFC0u;
constexpr uint32_t kUcheTrapRangeMaxHi = 0x0001FFFFu;
constexpr uint32_t kRbbmHangIntCntl = (1u << 30) | 0x3FFFFu;   /* a620 hang_detect_cycles */
constexpr uint32_t kPcDbgEcoCntl = 0x00010000u | (1u << 9);    /* prim fifo 0x10000, disable_tseskip */
constexpr uint32_t kRbbmClockCntlOn = 0x8AA8AA82u;
constexpr uint32_t kGmuCgcMode = 0x00020200u;           /* mainline a620 catalog */
constexpr uint32_t kGmuCgcDelay = 0x00010111u;
constexpr uint32_t kGmuCgcHyst = 0x00005555u;
constexpr uint32_t kGxOffBits = (1u << 6) | (1u << 7);  /* GDSC power off, clock off */
constexpr uint32_t kRbCntl = 0x0000020Cu;               /* BUFSZ(32K) | BLKSZ(32) */
constexpr uint32_t kIntMask = 0x03D0E242u;              /* A6XX_INT_MASK for a620 */
constexpr uint32_t kApriv = 0x0000006Eu;                /* BIT(6)|5|3|2|1 */
constexpr uint32_t kCpProtectCntl = 0x0000000Bu;        /* PROT_EN|FAULT_ON_VIOL|INF_SPAN */

/* UBWC 3.0, highest bank bit 14 (SM7250 GPU DT: qcom,ubwc-mode = <3>,
 * qcom,highest-bank-bit = <14>; the same values the upstream SM7250 UBWC patch
 * records). KGSL encodes mode 3 as feild 0 (not UBWC 1.0), amsbc=1 is only for
 * A640/A680, and hbb14 gives hbb_lo=1, hbb_hi=0. */
constexpr uint32_t kRbNcModeCntl = 0x00000002u;
constexpr uint32_t kTpl1NcModeCntl = 0x00000002u;
constexpr uint32_t kSpNcModeCntl = 0x00000022u;         /* uavflagprd_inv=2 for a650 family */
constexpr uint32_t kUcheModeCntl = 0x00200000u;         /* hbb_lo << 21 */

constexpr uint32_t kCpMeInitOpcode = 0x48;
constexpr uint32_t kCpNopOpcode = 0x10;
constexpr uint32_t kCpIndirectBufferOpcode = 0x3F;
constexpr uint32_t kCpEventWriteOpcode = 0x46;
constexpr uint32_t kCacheFlushTs = 0x04;
constexpr uint32_t kEventWriteIrq = 1u << 31;
constexpr uint32_t kMeInitDwords = 9;                   /* header + 8 payload */
constexpr uint32_t kIbSubmitDwords = 9;                 /* IB packet + fence packet */
constexpr uint32_t kRingDwords = GpuRingBytes / 4;
constexpr unsigned kMeInitTimeoutUs = 2000000;          /* KGSL spends 2 s here */
constexpr unsigned kFenceTimeoutUs = 2000000;
constexpr unsigned kFencedWriteTries = 200;
constexpr unsigned kIdleReportUs = 250000;              /* one idle poll line per 250 ms */

struct Mapping {
    IOMemoryMap *keep;
    volatile uint8_t *base;
    uint64_t bytes;
    uint64_t physical;
};

struct CpState {
    Mapping gpu, gmu;
    Mapping sqe, ring, memptrs, scratch;
    NativeAdrenoGpuBuffer buffer[GpuBufferCount];
    bool buffers;
    uint32_t next;          /* ring dword index for the next submission */
    uint32_t sequence;      /* last fence value submitted */
    bool started;
};

CpState *gState;

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

bool mapPhysical(uint64_t base, uint64_t bytes, Mapping &out)
{
    if (!base || !bytes) return false;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, static_cast<IOByteCount>(bytes));
    if (!memory) return false;
    out.keep = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (!out.keep) return false;
    out.base = reinterpret_cast<volatile uint8_t *>(out.keep->getVirtualAddress());
    out.bytes = bytes;
    out.physical = base;
    return out.base != nullptr;
}

uint32_t gpuRead(uint32_t dword)
{
    return read32(gState->gpu, uint64_t(dword) * 4);
}

void gpuWrite(uint32_t dword, uint32_t value)
{
    write32(gState->gpu, uint64_t(dword) * 4, value);
}

uint32_t gmuRead(uint32_t dword)
{
    return read32(gState->gmu, uint64_t(dword) * 4 - kGmuDownshift);
}

void gmuWrite(uint32_t dword, uint32_t value)
{
    write32(gState->gmu, uint64_t(dword) * 4 - kGmuDownshift, value);
}

void gmuUpdate(uint32_t dword, uint32_t mask, uint32_t value)
{
    const uint32_t old = gmuRead(dword);
    gmuWrite(dword, (old & ~mask) | (value & mask));
}

void clean(const Mapping &m)
{
    if (m.base && m.bytes) NativeAdrenoMaintainPhysicalPages(m.physical, m.bytes, NativeAdrenoClean);
}

void invalidate(const Mapping &m)
{
    if (m.base && m.bytes) NativeAdrenoMaintainPhysicalPages(m.physical, m.bytes, NativeAdrenoInvalidate);
}

/* The ring buffer page: a 64-bit write of the same value to +0 and +4 makes a
 * 64-bit register write that a compiler cannot split into two reordered stores. */
void gpuWrite64(uint32_t dword, uint64_t value)
{
    gpuWrite(dword, uint32_t(value));
    gpuWrite(dword + 1, uint32_t(value >> 32));
}

/* CP_RB_WPTR is a fenced register: the GMU drops a write that arrives while the
 * GPU domain is not ready and reports it in GMU_AHB_FENCE_STATUS bit 0. Rewrite
 * the pointer when that happens, as a6xx_fenced_write() does. */
bool fencedWptr(uint32_t value)
{
    gpuWrite(A620_CP_RB_WPTR, value);
    barrier();
    for (unsigned tries = 0; tries < kFencedWriteTries; ++tries) {
        const uint32_t status = gmuRead(kGmuAhbFenceStatus);
        if (!(status & 1u)) return true;
        IODelay(10);
        gpuWrite(A620_CP_RB_WPTR, value);
        barrier();
    }
    return false;
}

/* One TYPE7 packet into a word buffer. */
bool packet(uint32_t *words, size_t capacity, size_t at, uint32_t opcode,
    const uint32_t *payload, uint32_t count)
{
    if (at + 1 + count > capacity) return false;
    if (AdrenoPM4Type7(opcode, count, &words[at]) != AdrenoPM4Success) return false;
    for (uint32_t i = 0; i < count; ++i) words[at + 1 + i] = payload ? payload[i] : 0;
    return true;
}

void printCpStatus(const char *where)
{
    /* A6XX CP/RBBM fault and progress registers: 0x210 RBBM_STATUS, 0x213
     * RBBM_STATUS3, 0x201 RBBM_INT_0_STATUS, 0x821 CP_HW_FAULT, 0x823
     * CP_INTERRUPT_STATUS, 0x908/0x909 CP_SQE_STAT address/data. */
    gpuWrite(A620_CP_SQE_STAT_ADDR, 0);
    printf("PICASSO_GPU cp: %s rptr=%u wptr=%u status=%08x status3=%08x int=%08x "
           "fault=%08x cpint=%08x sqe_stat=%08x\n",
        where, gpuRead(A620_CP_RB_RPTR), gpuRead(A620_CP_RB_WPTR),
        gpuRead(A620_RBBM_STATUS), gpuRead(A620_RBBM_STATUS3),
        gpuRead(A620_RBBM_INT_0_STATUS), gpuRead(A620_CP_HW_FAULT),
        gpuRead(A620_CP_INTERRUPT_STATUS), gpuRead(A620_CP_SQE_STAT_DATA));
}

/* For a stalled client fence (NativeAdrenoSubmit's ring state carries no CP_INT): the SQE error code, the hardware fault register, the
 * register-protection status (0x824) and one SQE state sample. Reads only. */
extern "C" uint32_t native_adreno_cp_fault_regs(uint32_t *out, uint32_t count)
{
    if (!out || count < 4) return 0;
    gpuWrite(A620_CP_SQE_STAT_ADDR, 0);
    out[0] = gpuRead(A620_CP_INTERRUPT_STATUS);
    out[1] = gpuRead(A620_CP_HW_FAULT);
    out[2] = gpuRead(0x0824);   /* CP_PROTECT_STATUS (a6xx.xml) */
    out[3] = gpuRead(A620_CP_SQE_STAT_DATA);
    return 1;
}

/* Wait until the CP's read pointer shadow reaches `target` and no submit is in
 * flight. HW_APRIV means the hardware writes the shadow into memptrs.rptr, so
 * that word is the authority; CP_RB_RPTR is printed for the diagnostics only. */
bool waitIdle(uint32_t target, unsigned timeoutUs)
{
    unsigned lastReport = 0;
    for (unsigned waited = 0;; waited += 100) {
        invalidate(gState->memptrs);
        const uint32_t shadow = read32(gState->memptrs, 0);
        if (shadow == target) return true;
        if (waited >= kIdleReportUs && waited - lastReport >= kIdleReportUs) {
            lastReport = waited;
            printf("PICASSO_GPU cp: idle poll waited=%u target=%u shadow=%u "
                   "rptr=%u wptr=%u status=%08x\n",
                waited, target, shadow, gpuRead(A620_CP_RB_RPTR),
                gpuRead(A620_CP_RB_WPTR), gpuRead(A620_RBBM_STATUS));
        }
        if (waited >= timeoutUs) {
            printf("PICASSO_GPU cp: idle timeout target=%u shadow=%u\n", target, shadow);
            return false;
        }
        IODelay(100);
    }
}

/* hw_init: the ordered register list of mainline a6xx_hw_init() for a650,
 * plus the vendor a620 details noted at each step. */
bool hardwareInit()
{
    /* The GMU holds GX on through the GPU_SET OOB taken in stage 3. */
    const uint32_t gx = gmuRead(kGmuGxPwrStatus);
    if (gx & kGxOffBits) {
        printf("PICASSO_GPU cp: GX is off before hw_init (status=%08x)\n", gx);
        return false;
    }

    /* Clear GBIF halt in case the GX domain was not collapsed. */
    gpuWrite(A620_GBIF_HALT, 0);
    (void)gpuRead(A620_GBIF_HALT);
    gpuWrite(A620_RBBM_GBIF_HALT, 0);
    (void)gpuRead(A620_RBBM_GBIF_HALT);

    gpuWrite(A620_RBBM_SECVID_TSB_CNTL, 0);
    /* Disable the trusted memory range. */
    gpuWrite64(A620_RBBM_SECVID_TSB_TRUSTED_BASE, 0);
    gpuWrite(A620_RBBM_SECVID_TSB_TRUSTED_SIZE, 0);

    /* Turn on 64-bit addressing for all blocks. */
    const uint32_t addrMode[] = {
        kGpuCpAddrModeCntl, kGpuVscAddrModeCntl, kGpuGrasAddrModeCntl, kGpuRbAddrModeCntl,
        kGpuPcAddrModeCntl, kGpuHlsqAddrModeCntl, kGpuVfdAddrModeCntl, kGpuVpcAddrModeCntl,
        kGpuUcheAddrModeCntl, kGpuSpAddrModeCntl, kGpuTpl1AddrModeCntl,
        kGpuRbbmSecvidTsbAddrModeCntl,
    };
    for (size_t i = 0; i < sizeof(addrMode) / sizeof(addrMode[0]); ++i) gpuWrite(addrMode[i], 1);

    /* a6xx_set_hwcg(true): the GMU AO clock-gating values, the A620 hwcg list,
     * and clock control last. */
    gmuWrite(kGmuCgcModeCntl, kGmuCgcMode);
    gmuWrite(kGmuCgcDelayCntl, kGmuCgcDelay);
    gmuWrite(kGmuCgcHystCntl, kGmuCgcHyst);
    gmuUpdate(kGmuSptpracClockControl, 1u, 0u);   /* SP clock off while programming */
    for (unsigned i = 0; i < NATIVE_ADRENO_A620_HWCG_COUNT; ++i)
        gpuWrite(NativeAdrenoA620Hwcg[i][0], NativeAdrenoA620Hwcg[i][1]);
    gmuUpdate(kGmuSptpracClockControl, 1u, 1u);
    gpuWrite(A620_RBBM_CLOCK_CNTL, kRbbmClockCntlOn);

    /* The a620 GBIF list (vendor a650_gbif_regs, used by both a620 and a650). */
    for (uint32_t side = 0; side < 4; ++side) gpuWrite(kGpuGbifQsbSide0 + side, 0x00071620u);
    gpuWrite(A620_RBBM_GBIF_CLIENT_QOS_CNTL, 3);

    /* Make all blocks contribute to the GPU BUSY perf counter. */
    gpuWrite(A620_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xFFFFFFFFu);

    /* Disable L2 bypass in the UCHE: trap base, write-through base and the
     * range maximum (adreno_gpu->uche_trap_base = 0x1fffffffff000). */
    gpuWrite(kGpuUcheWriteRangeMax, kUcheTrapRangeMaxLo);
    gpuWrite(kGpuUcheWriteRangeMax + 1, kUcheTrapRangeMaxHi);
    gpuWrite(kGpuUcheTrapBase, kUcheTrapStartLo);
    gpuWrite(kGpuUcheTrapBase + 1, kUcheTrapStartHi);
    gpuWrite(kGpuUcheWriteThruBase, kUcheTrapStartLo);
    gpuWrite(kGpuUcheWriteThruBase + 1, kUcheTrapStartHi);

    /* a650 family: no programmed GMEM range. */
    gpuWrite(kGpuUcheFilterCntl, 0x804);
    gpuWrite(A620_UCHE_CACHE_WAYS, 4);

    gpuWrite(kGpuCpRoqThresholds2, 0x02000140);
    gpuWrite(kGpuCpRoqThresholds1, 0x8040362C);
    gpuWrite(kGpuCpMemPoolSize, 128);
    gpuWrite(0x09E00, kPcDbgEcoCntl);
    gpuWrite(A620_CP_AHB_CNTL, 1);
    gpuWrite(A620_RBBM_PERFCTR_CNTL, 1);
    gpuWrite(kGpuCpPerfctrCpSel0, 0);   /* PERF_CP_ALWAYS_COUNT */

    /* UBWC: SM7250 is UBWC 3.0 with highest bank bit 14. */
    gpuWrite(A620_RB_NC_MODE_CNTL, kRbNcModeCntl);
    gpuWrite(A620_TPL1_NC_MODE_CNTL, kTpl1NcModeCntl);
    gpuWrite(kGpuSpNcModeCntl, kSpNcModeCntl);
    gpuWrite(A620_UCHE_MODE_CNTL, kUcheModeCntl);
    gpuWrite(kGpuRbbmNcModeCntl, 0);

    gpuWrite(A620_RBBM_INTERFACE_HANG_INT_CNTL, kRbbmHangIntCntl);
    gpuWrite(A620_UCHE_CLIENT_PF, (1u << 7) | 1u);

    const uint32_t bicubic[5] = {0, 0x3FE05FF4u, 0x3FA0EBEEu, 0x3F5193EDu, 0x3F0243F0u};
    for (uint32_t i = 0; i < 5; ++i) gpuWrite(kGpuTpl1BicubicWeights + i, bicubic[i]);

    /* GMU busy counter and the power counter. */
    gmuWrite(kGmuCxBusyMask, 0xFF000000u);
    gmuUpdate(kGmuPowerCounterSelect0, 0xFFu, 1u << 5);
    gmuWrite(kGmuPowerCounterEnable, 1);

    /* Protect registers from the CP. The generated table already carries the
     * final CP_PROTECT(47) entry with the infinite span. */
    gpuWrite(A620_CP_PROTECT_CNTL, kCpProtectCntl);
    for (unsigned i = 0; i < NATIVE_ADRENO_A620_PROTECT_COUNT; ++i)
        gpuWrite(A620_CP_PROTECT + NativeAdrenoA620Protect[i][0], NativeAdrenoA620Protect[i][1]);

    gpuWrite(A620_CP_APRIV_CNTL, kApriv);
    gpuWrite(A620_RBBM_INT_0_MASK, kIntMask);
    return true;
}

/* Copy the SQE image (the vendor file without its 4-byte header) into the
 * context's SQE buffer. One 32-bit store per word: the alias is uncached and
 * libc's wide stores are not appropriate here. */
bool uploadSqe()
{
    const uint8_t *bytes = nullptr;
    size_t length = 0;
    if (native_adreno620_sqe_firmware(&bytes, &length) != 0 || !bytes || length <= 4) {
        printf("PICASSO_GPU cp: no SQE image staged\n");
        return false;
    }
    const size_t payload = length - 4;
    if (payload > gState->sqe.bytes) {
        printf("PICASSO_GPU cp: SQE image %zu exceeds the %llu-byte buffer\n",
            payload, static_cast<unsigned long long>(gState->sqe.bytes));
        return false;
    }
    volatile uint32_t *dest = reinterpret_cast<volatile uint32_t *>(gState->sqe.base);
    const uint8_t *source = bytes + 4;
    for (size_t i = 0; i < payload / 4; ++i)
        dest[i] = uint32_t(source[i * 4]) | (uint32_t(source[i * 4 + 1]) << 8) |
            (uint32_t(source[i * 4 + 2]) << 16) | (uint32_t(source[i * 4 + 3]) << 24);
    clean(gState->sqe);
    printf("PICASSO_GPU cp: sqe payload=%zu first=%08x\n", payload, dest[0]);
    return true;
}

/* Program SQE base, ring base/control and the RPTR shadow address. */
bool programCp()
{
    gpuWrite64(A620_CP_SQE_INSTR_BASE, GpuSqeIova);
    gpuWrite64(A620_CP_RB_BASE, GpuRingIova);
    gpuWrite(A620_CP_RB_CNTL, kRbCntl);
    /* HW_APRIV: the CP maintains the read-pointer shadow itself. */
    gpuWrite64(A620_CP_RB_RPTR_ADDR, GpuMemptrsIova);
    gpuWrite(A620_CP_RB_WPTR, 0);
    barrier();
    return true;
}

/* Append `words` to the ring and kick it; returns the new write pointer. */
bool submitRing(const uint32_t *words, uint32_t count, uint32_t *end)
{
    CpState &s = *gState;
    if (count + kMeInitDwords + kIbSubmitDwords > kRingDwords) return false;
    const uint32_t mask = kRingDwords - 1;
    volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(s.ring.base);
    for (uint32_t i = 0; i < count; ++i) ring[(s.next + i) & mask] = words[i];
    const uint32_t next = (s.next + count) & mask;
    clean(s.ring);
    barrier();
    if (!fencedWptr(next)) {
        printf("PICASSO_GPU cp: WPTR %u was dropped\n", next);
        return false;
    }
    s.next = next;
    if (end) *end = next;
    return true;
}

bool cpInit()
{
    uint32_t words[kMeInitDwords] = {};
    const uint32_t meInit[8] = {0x0000002F, 0x00000003, 0x20000000, 0, 0, 0, 0, 0};
    if (!packet(words, kMeInitDwords, 0, kCpMeInitOpcode, meInit, 8)) return false;
    uint32_t end = 0;
    if (!submitRing(words, kMeInitDwords, &end)) return false;
    printf("PICASSO_GPU cp: me_init submitted wptr=%u shadow=%u rptr=%u\n", end,
        read32(gState->memptrs, 0), gpuRead(A620_CP_RB_RPTR));
    if (!waitIdle(end, kMeInitTimeoutUs)) {
        printCpStatus("me_init");
        return false;
    }
    printf("PICASSO_GPU cp: me_init idle\n");
    return true;
}

/* A one-dword IB of CP_NOP in the scratch page, ended by an A6XX
 * CACHE_FLUSH_TS event write to memptrs.fence. (NativeAdrenoRing's fence is
 * the A7XX CACHE_CLEAN shape and is not used here.) */
bool submitNoop(uint32_t *fence)
{
    uint32_t nop = 0;
    if (AdrenoPM4Type7(kCpNopOpcode, 0, &nop) != AdrenoPM4Success) return false;
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gState->scratch.base);
    scratch[0] = nop;
    clean(gState->scratch);

    const uint64_t fenceAddress = GpuMemptrsIova + 4;
    const uint32_t sequence = gState->sequence + 1;
    uint32_t words[kIbSubmitDwords] = {};
    const uint32_t ib[3] = {uint32_t(GpuScratchIova), uint32_t(GpuScratchIova >> 32), 1};
    const uint32_t event[4] = {kCacheFlushTs | kEventWriteIrq,
        uint32_t(fenceAddress), uint32_t(fenceAddress >> 32), sequence};
    if (!packet(words, kIbSubmitDwords, 0, kCpIndirectBufferOpcode, ib, 3)) return false;
    if (!packet(words, kIbSubmitDwords, 4, kCpEventWriteOpcode, event, 4)) return false;

    invalidate(gState->memptrs);
    uint32_t end = 0;
    if (!submitRing(words, kIbSubmitDwords, &end)) return false;
    gState->sequence = sequence;
    printf("PICASSO_GPU cp: noop ib submitted wptr=%u fence=%u\n", end, sequence);
    if (fence) *fence = sequence;
    return true;
}

bool waitFence(uint32_t fence, unsigned timeoutUs, bool verbose)
{
    if (!fence) return false;
    for (unsigned waited = 0;; waited += 100) {
        invalidate(gState->memptrs);
        const uint32_t value = read32(gState->memptrs, 4);
        if (value >= fence) return true;
        if (waited >= timeoutUs) {
            if (verbose) {
                printf("PICASSO_GPU cp: fence timeout wanted=%u have=%u\n", fence, value);
                printCpStatus("fence");
            }
            return false;
        }
        IODelay(100);
    }
}

/* Ring callbacks for the production NativeAdrenoRing: the same GPU window and
 * GMU fence status the boot stage uses. The cache callbacks are no-ops because
 * every CPU access to the ring and memptrs goes through the uncached aliases
 * mapped above (no cache line of them can exist). */
int ringWrite(void *, uint32_t reg, uint32_t value)
{
    gpuWrite(reg, value);
    return 0;
}

int ringRead(void *, uint32_t reg, uint32_t *value)
{
    if (!value) return 1;
    *value = gpuRead(reg);
    return 0;
}

int ringBarrier(void *)
{
    barrier();
    return 0;
}

int ringFenceStatus(void *, int clear, uint32_t *status)
{
    if (clear) {
        gmuWrite(kGmuAhbFenceStatusClr, 0x7u);
        return 0;
    }
    if (!status) return 1;
    *status = gmuRead(kGmuAhbFenceStatus);
    return 0;
}

int ringPublish(void *, const volatile uint32_t *, size_t)
{
    return 0;
}

int ringAcquire(void *, volatile uint32_t *, size_t)
{
    return 0;
}

uint64_t ringNowUs(void *)
{
    uint64_t microseconds = 0;
    return NativeAdrenoKernelTime::now_us(nullptr, &microseconds) ? 0 : microseconds;
}

/* The live ring as the production class sees it: adopted once, with the A6xx
 * fence profile. Returns null when the board buffers are gone or the adopt
 * was refused. */
NativeAdrenoRing *ringObject()
{
    static NativeAdrenoRing ring;
    static bool ready = false, failed = false;
    if (ready) return &ring;
    if (failed) return nullptr;
    CpState &s = *gState;
    if (!s.ring.base || !s.memptrs.base) {
        failed = true;
        return nullptr;
    }
    const NativeAdrenoRingIo io = {nullptr, ringWrite, ringRead, ringBarrier,
        nullptr, ringFenceStatus};
    const NativeAdrenoRingCache cache = {nullptr, ringPublish, ringAcquire};
    const IOReturn adopted = ring.adopt(&io, &cache,
        const_cast<uint8_t *>(s.ring.base), size_t(s.ring.bytes), GpuRingIova,
        const_cast<uint8_t *>(s.memptrs.base), size_t(s.memptrs.bytes), GpuMemptrsIova,
        s.next, s.sequence);
    if (adopted) {
        printf("PICASSO_GPU cp: ring adopt failed %08x\n", unsigned(adopted));
        failed = true;
        return nullptr;
    }
    ring.setA6xxFence(true);
    ready = true;
    return &ring;
}

/* Take the live ring into the production ring class with the A6xx fence
 * profile and submit one CP_INDIRECT_BUFFER through it. This is the first
 * submission through the same code the Metal path will use. */
bool ringSmoke()
{
    CpState &s = *gState;
    NativeAdrenoRing *ring = ringObject();
    if (!ring) return false;
    NativeAdrenoRingClock clock;
    clock.cookie = nullptr;
    clock.now_us = ringNowUs;
    clock.idle = nullptr;

    uint32_t words[4] = {};
    const uint32_t ib[3] = {uint32_t(GpuScratchIova), uint32_t(GpuScratchIova >> 32), 1};
    if (!packet(words, 4, 0, kCpIndirectBufferOpcode, ib, 3)) return false;
    uint32_t fence = 0;
    const IOReturn committed = ring->submit(words, 4, &fence);
    uint32_t observed = 0;
    const NativeAdrenoRingFence waited = committed ? NativeAdrenoRingFencePoisoned :
        ring->wait(fence, &clock, kFenceTimeoutUs, &observed);
    printf("PICASSO_GPU cp: ring smoke submit=%08x fence=%u observed=%u result=%d\n",
        unsigned(committed), fence, observed, int(waited));
    if (committed || waited != NativeAdrenoRingFenceComplete) return false;

    /* Keep the boot-stage ring cursor in step with the class: the submission
     * above wrote exactly kIbSubmitDwords words (IB + A6xx fence). */
    s.next = (s.next + kIbSubmitDwords) & (kRingDwords - 1);
    s.sequence = fence;
    return true;
}

/* The production submit object and the VM it serves, attached by publishGpu()
 * and used by the client test and by every user connection from then on. */
NativeAdrenoSubmit gSubmit;
NativeAdrenoVm *gPublishedVm = nullptr;
bool gServicePublished = false;

/* Publish the live GPU context through the production service: the same VM,
 * ring and submit object the A740 board hands over, with this chip's device
 * information. No user client can submit yet (the Metal plugin is not part of
 * this image), so the published ring stays idle. */
bool publishGpu()
{
    NativeAdrenoRing *ring = ringObject();
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    const uint32_t ias = NativeAdrenoGpuContextInputBits();
    if (!ring || !vm || ias < 33 || ias > 48) {
        printf("PICASSO_GPU cp: publish refused ring=%p vm=%p ias=%u\n", ring, vm, ias);
        return false;
    }
    gSubmit.setA6xxPlain(true);
    const IOReturn attached = gSubmit.attach(vm, ring);
    if (attached) {
        printf("PICASSO_GPU cp: submit attach failed %08x\n", unsigned(attached));
        return false;
    }
    static NativeAdrenoAbiDeviceInfo info;
    info = {};
    info.chip_id = 0x06020001u;               /* the ADT's qcom,chipid for this GPU */
    info.gmem_bytes = 512u * 1024u;
    info.gmem_base = 0;
    info.uche_trap_base = 0x1fffffffff000ull;
    info.va_start = GpuClientVaStart;
    info.va_size = (uint64_t(1) << ias) - GpuClientVaStart;
    info.highest_bank_bit = 14;               /* SM7250: UBWC 3.0, hbb 14 */
    info.ubwc_swizzle = 0x6;                  /* levels 2 and 3 (qcom_ubwc_swizzle) */
    info.macrotile_mode = 0;
    info.ubwc_version = 3;
    /* The client chooses its BO addresses inside the window mount() gave it and BoMap validates every one; Turnip requires the flag to
     * initialize the address heap that darwin_bo_init allocates from (without it the heap stays zeroed and the first allocation walks a
     * NULL hole: SIGSEGV in every process, after GetDeviceInfo). */
    info.flags = NativeAdrenoDeviceClientIova;
    NativeAdrenoRingClock clock;
    clock.cookie = nullptr;
    clock.now_us = ringNowUs;
    clock.idle = nullptr;
    const IOReturn published = native_adreno_publish(vm, &gSubmit, ring, &clock,
        nullptr, &info, ias, 40);
    printf("PICASSO_GPU cp: publish status=%08x service=%p va_size=%llx\n",
        unsigned(published), native_adreno_published_service(),
        static_cast<unsigned long long>(info.va_size));
    if (published == kIOReturnSuccess) {
        gPublishedVm = vm;
        gServicePublished = true;
    }
    return published == kIOReturnSuccess;
}

/* Register the staged AGXAccelerator for the original Metal discovery: load
 * the original IOGPUFamily, find its IOAcceleratorES, run the staged class's
 * initializer under preModLoad/postModLoad (the sequence in the reference
 * board probe), then publish an accelerator carrying the Metal plugin keys.
 * Stages: 1 class present and kext load attempted, 2 parent metatype found,
 * 3 class registered, 4 object created and initialised, 5 published. */
bool publishMetal()
{
    volatile unsigned stage = 1;
    kern_return_t status = kIOReturnSuccess;
    if (!native_adreno_accelerator_static_init ||
        !native_adreno_accelerator_allocate ||
        !native_adreno_accelerator_instance_size) {
        printf("PICASSO_GPU cp: metal stage=1 status=%08x staged class missing\n",
            unsigned(kIOReturnUnsupported));
        return false;
    }
    const kern_return_t loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (loaded) {
        printf("PICASSO_GPU cp: metal stage=1 status=%08x kext=%d\n",
            unsigned(loaded), int(loaded));
        return false;
    }
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) {
        printf("PICASSO_GPU cp: metal stage=1 status=%08x parent=%p size=%u\n",
            unsigned(kIOReturnNotFound), parent,
            parent ? parent->getClassSize() : 0u);
        return false;
    }
    stage = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        printf("PICASSO_GPU cp: metal stage=2 status=%08x premod refused\n",
            unsigned(kIOReturnNoResources));
        return false;
    }
    native_adreno_accelerator_static_init();
    status = OSMetaClass::postModLoad(handle);
    if (status) {
        printf("PICASSO_GPU cp: metal stage=2 status=%08x postmod\n", unsigned(status));
        return false;
    }
    if (native_adreno_accelerator_instance_size() != 136) {
        printf("PICASSO_GPU cp: metal stage=2 status=%08x instance=%u\n",
            unsigned(kIOReturnInvalid), native_adreno_accelerator_instance_size());
        return false;
    }
    stage = 3;
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator) {
        printf("PICASSO_GPU cp: metal stage=3 status=%08x allocate\n",
            unsigned(kIOReturnNoMemory));
        return false;
    }
    if (!accelerator->init()) {
        accelerator->release();
        printf("PICASSO_GPU cp: metal stage=3 status=%08x init\n",
            unsigned(kIOReturnError));
        return false;
    }
    /* The two keys the original _MTLRegisterDevices helper reads, plus the
     * process names this kernel's grant honours in place of the sandbox
     * verdict (this upstream's native_sandbox_grant matches names literally;
     * names are the first 16 characters proc_selfname reports). */
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("NativeSandboxGrantProcess",
        "backboardd,SpringBoard,mediaserverd,WidgetRenderer_D,"
        "iconservicesagent,posterboardd,Wallpaper");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    stage = 4;
    if (!accelerator->attach(IOService::getPlatform())) {
        accelerator->release();
        printf("PICASSO_GPU cp: metal stage=4 status=%08x attach\n",
            unsigned(kIOReturnNotAttached));
        return false;
    }
    accelerator->registerService();
    stage = 5;
    printf("PICASSO_GPU cp: metal stage=%u status=%08x registered=1\n",
        stage, unsigned(kIOReturnSuccess));
    return true;
}

/* The boot addon's report (sheng-gpu-addon=1): launchd's first exec runs the
 * mount, so read the three stage/status pairs from a thread after it. Stages:
 * 1 source probed, 2 mounted, 3.. the visible paths checked; status is the
 * last error (0 mounted). */
void addonReport(void *, wait_result_t)
{
    IOSleep(20000);
    printf("PICASSO_GPU addon: extensions=%d/%x daemons=%d/%x wallpaper=%d/%x\n",
        native_gpu_addon_stage, native_gpu_addon_status,
        native_gpu_addon2_stage, native_gpu_addon2_status,
        native_gpu_addon3_stage, native_gpu_addon3_status);
    printf("PICASSO_GPU submits: submissions=%u ibs=%u max=%u many=%u\n",
        native_adreno_submit_ibs[0], native_adreno_submit_ibs[1],
        native_adreno_submit_ibs[2], native_adreno_submit_ibs[3]);
    printf("PICASSO_GPU metal diag: opens=%u foreign=%u report=%s report2=%s\n",
        native_adreno_metal_opens(), native_adreno_metal_foreign_opens(),
        native_adreno_metal_report(), native_adreno_metal_report2());
    printf("PICASSO_GPU mount: fail=%u/%x count=%u ttbr=%u/%u cp=%x/%x check=%x/%x/%x/%x/%x/%x\n",
        native_adreno_mount_fail[0], native_adreno_mount_fail[1], native_adreno_mount_fail[3],
        native_adreno_table_switches[6], native_adreno_table_switches[7],
        native_adreno_cp_aperture[2], native_adreno_cp_aperture[3],
        native_adreno_cp_check[0], native_adreno_cp_check[1], native_adreno_cp_check[2],
        native_adreno_cp_check[3], native_adreno_cp_check[4], native_adreno_cp_check[5]);
    IOSleep(40000);
    uint32_t selector = 0, status = 0;
    const uint32_t calls = native_adreno_metal_last(&selector, &status);
    uint32_t marks[8] = {};
    const uint32_t total = native_adreno_metal_marks(marks, 8);
    char list[64] = {};
    size_t at = 0;
    for (uint32_t i = 0; i < 8 && i < total && at < sizeof list - 8; ++i)
        at += snprintf(list + at, sizeof list - at, " %x", marks[i]);
    printf("PICASSO_GPU client: calls=%u last=%u/%x marks=%u%s\n",
        calls, selector, status, total, list);
    printf("PICASSO_GPU submits later: submissions=%u ibs=%u max=%u many=%u\n",
        native_adreno_submit_ibs[0], native_adreno_submit_ibs[1],
        native_adreno_submit_ibs[2], native_adreno_submit_ibs[3]);
    printf("PICASSO_GPU metal later: opens=%u foreign=%u report=%s report2=%s\n",
        native_adreno_metal_opens(), native_adreno_metal_foreign_opens(),
        native_adreno_metal_report(), native_adreno_metal_report2());
}

/* One entry of the production user-client dispatch, with the record sizes the
 * class declares (the same pair the IOKit wrapper copies for a real caller). */
IOReturn clientCall(NativeAdrenoUserClient &client, uint32_t method,
    const void *input, void *output, const NativeAdrenoRingClock *clock)
{
    const size_t in = NativeAdrenoUserClient::inputSize(method);
    const size_t out = NativeAdrenoUserClient::outputSize(method);
    return client.dispatch(method, in ? input : nullptr, in,
        out ? output : nullptr, out, clock);
}

/* Open one client of the published service through the production user-client
 * class and submit one IB through NativeAdrenoSubmit's A620 profile: allocate
 * an IB and a target in the GPU context's aperture, write three CP_MEM_WRITE
 * packets, submit, wait on the CP timestamp fence and read the target back
 * (sheng-gpu-client-test=1). This is the path the Metal plugin will use, with
 * the IOKit wrapper and sandbox in front of it. */
bool clientTest()
{
    NativeAdrenoRing *ring = ringObject();
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    const uint32_t ias = NativeAdrenoGpuContextInputBits();
    if (!ring || !vm || !gServicePublished) {
        printf("PICASSO_GPU cp: client test refused ring=%p vm=%p published=%d\n",
            ring, vm, gServicePublished ? 1 : 0);
        return false;
    }
    static NativeAdrenoUserClient client;
    NativeAdrenoRingClock clock;
    clock.cookie = nullptr;
    clock.now_us = ringNowUs;
    clock.idle = nullptr;
    const IOReturn opened = client.open(NativeAdrenoAbiVersion, vm, &gSubmit, ring,
        ias, 40);
    printf("PICASSO_GPU cp: client open status=%08x ias=%u\n", unsigned(opened), ias);
    if (opened) return false;

    NativeAdrenoAbiParams params = {};
    IOReturn status = clientCall(client, NativeAdrenoMethodQuery, nullptr, &params, &clock);
    printf("PICASSO_GPU cp: client query status=%08x abi=%u in=%u out=%u\n",
        unsigned(status), params.abi_version, params.input_bits, params.output_bits);
    if (status) return false;

    /* System BOs have no kernel mapping until a client view maps them, so
     * the self-test asks for one contiguous run, which allocates through the
     * kernel-mapped path (the Metal plugin itself uses system BOs with its own
     * views). */
    const NativeAdrenoAbiBoAlloc alloc = {4096, 12, 1};
    NativeAdrenoAbiBoAllocReply ib = {}, target = {};
    status = clientCall(client, NativeAdrenoMethodBoAlloc, &alloc, &ib, &clock);
    if (status) {
        printf("PICASSO_GPU cp: client test ib alloc %08x\n", unsigned(status));
        return false;
    }
    status = clientCall(client, NativeAdrenoMethodBoAlloc, &alloc, &target, &clock);
    if (status) {
        printf("PICASSO_GPU cp: client test target alloc %08x\n", unsigned(status));
        return false;
    }
    const uint64_t ibIova = GpuClientVaStart;
    const uint64_t targetIova = GpuClientVaStart + 0x1000;
    const NativeAdrenoAbiBoMap mapIb = {ib.handle, ibIova, 1, 0, 2, 0};
    const NativeAdrenoAbiBoMap mapTarget = {target.handle, targetIova, 3, 0, 2, 0};
    NativeAdrenoAbiBoMapReply reply = {};
    if (clientCall(client, NativeAdrenoMethodBoMap, &mapIb, &reply, &clock)) return false;
    if (clientCall(client, NativeAdrenoMethodBoMap, &mapTarget, &reply, &clock)) return false;

    NativeAdrenoMemory *ibMemory = client.memoryOf(ib.handle);
    NativeAdrenoMemory *targetMemory = client.memoryOf(target.handle);
    if (!ibMemory || !targetMemory) {
        printf("PICASSO_GPU cp: client test memory lookup failed\n");
        return false;
    }
    volatile uint32_t *ibWords = static_cast<volatile uint32_t *>(ibMemory->mapping());
    volatile uint32_t *targetWords = static_cast<volatile uint32_t *>(targetMemory->mapping());
    if (!ibWords || !targetWords) {
        printf("PICASSO_GPU cp: client test memory mapping ib=%p target=%p\n",
            ibWords, targetWords);
        return false;
    }
    for (unsigned i = 0; i < 1024; ++i) targetWords[i] = 0xdeadbeef;
    const uint32_t values[3] = {0x5e9a7400u, 0xa1658bffu, 0x600d0001u};
    uint32_t words[12] = {};
    for (unsigned i = 0; i < 3; ++i) {
        if (AdrenoPM4Type7(0x3d, 3, &words[i * 4]) != AdrenoPM4Success) return false;
        words[i * 4 + 1] = uint32_t(targetIova + i * 4);
        words[i * 4 + 2] = uint32_t(targetIova >> 32);
        words[i * 4 + 3] = values[i];
    }
    for (unsigned i = 0; i < 12; ++i) ibWords[i] = words[i];
    if (ibMemory->publishForDevice(0, 4096, false) ||
        targetMemory->publishForDevice(0, 4096, true)) {
        printf("PICASSO_GPU cp: client test publish failed\n");
        return false;
    }

    NativeAdrenoAbiSubmit request = {};
    request.buffer_count = 1;
    request.buffers[0] = {ib.handle, ibIova, sizeof words};
    NativeAdrenoAbiSubmitReply submitted = {};
    status = clientCall(client, NativeAdrenoMethodSubmit, &request, &submitted, &clock);
    printf("PICASSO_GPU cp: client submit status=%08x fence=%llu\n",
        unsigned(status), static_cast<unsigned long long>(submitted.fence));
    int matched = 0;
    if (!status && submitted.fence) {
        NativeAdrenoAbiWaitFence wait = {submitted.fence, 200000000ull};
        const IOReturn waited = clientCall(client, NativeAdrenoMethodWaitFence, &wait,
            nullptr, &clock);
        if (!waited && !targetMemory->acquireDeviceWrites(0, 4096)) {
            matched = (targetWords[0] == values[0]) + (targetWords[1] == values[1]) +
                (targetWords[2] == values[2]) + (targetWords[3] == 0xdeadbeef);
        }
        printf("PICASSO_GPU cp: client fence wait=%08x matched=%d/4\n",
            unsigned(waited), matched);
    }

    const NativeAdrenoAbiBoFree freeIb = {ib.handle}, freeTarget = {target.handle};
    (void)clientCall(client, NativeAdrenoMethodBoFree, &freeIb, nullptr, &clock);
    (void)clientCall(client, NativeAdrenoMethodBoFree, &freeTarget, nullptr, &clock);
    const IOReturn closed = client.close();
    printf("PICASSO_GPU cp: client close=%08x\n", unsigned(closed));
    return !status && matched == 4 && !closed;
}

bool acquireBuffers()
{
    CpState &s = *gState;
    if (NativeAdrenoGpuContextBuffers(s.buffer) != kIOReturnSuccess) {
        printf("PICASSO_GPU cp: the GPU context buffers are not available\n");
        return false;
    }
    if (!mapPhysical(s.buffer[0].physical, s.buffer[0].bytes, s.sqe) ||
        !mapPhysical(s.buffer[1].physical, s.buffer[1].bytes, s.ring) ||
        !mapPhysical(s.buffer[2].physical, s.buffer[2].bytes, s.memptrs) ||
        !mapPhysical(s.buffer[4].physical, s.buffer[4].bytes, s.scratch)) {
        printf("PICASSO_GPU cp: cannot map the GPU context buffers\n");
        return false;
    }
    return true;
}

} // namespace

extern "C" int native_adreno620_cp_start(void)
{
    /* The driver's diagnostic reader of client BOs reads DRAM through this hook, and the cacheable alias through the other. */
    native_adreno_dram_read32 = dramRead32;
    native_adreno_cached_read32 = cachedRead32;
    if (!gState) gState = new CpState;
    CpState &s = *gState;
    if (s.started) {
        printf("PICASSO_GPU cp: already started\n");
        return -1;
    }
    if (!mapPhysical(kGpuWindowBase, kGpuWindowBytes, s.gpu)) {
        printf("PICASSO_GPU cp: cannot map the GPU register window\n");
        return -1;
    }
    if (!mapPhysical(kGmuWindowBase, kGmuWindowBytes, s.gmu)) {
        printf("PICASSO_GPU cp: cannot map the GMU register window\n");
        return -1;
    }
    if (!acquireBuffers()) return -1;

    /* The ring and the memptrs are the CP's from here on; clear both through
     * the CPU aliases and make the ring visible before the first kick. */
    volatile uint32_t *memptrs = reinterpret_cast<volatile uint32_t *>(s.memptrs.base);
    for (uint64_t i = 0; i < s.memptrs.bytes / 4; ++i) memptrs[i] = 0;
    volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(s.ring.base);
    for (uint64_t i = 0; i < s.ring.bytes / 4; ++i) ring[i] = 0;
    clean(s.memptrs);
    clean(s.ring);
    s.next = 0;
    s.sequence = 0;

    printf("PICASSO_GPU cp: start sqe=%llx ring=%llx memptrs=%llx scratch=%llx\n",
        static_cast<unsigned long long>(GpuSqeIova),
        static_cast<unsigned long long>(GpuRingIova),
        static_cast<unsigned long long>(GpuMemptrsIova),
        static_cast<unsigned long long>(GpuScratchIova));

    /* Diagnostic bisect: sheng-gpu-cp-limit=<1..7> stops after that phase. */
    uint32_t limitValue = 0;
    const unsigned limit = (PE_parse_boot_argn("sheng-gpu-cp-limit",
        &limitValue, static_cast<int>(sizeof limitValue)) && limitValue)
        ? limitValue : 7u;
    if (limit <= 1) {
        printf("PICASSO_GPU cp: limit=%u reached before hw_init\n", limit);
        return -1;
    }

    if (!hardwareInit()) return -1;
    printf("PICASSO_GPU cp: hw_init done\n");
    if (limit <= 2) {
        printf("PICASSO_GPU cp: limit=%u reached after hw_init\n", limit);
        return -1;
    }
    if (!uploadSqe()) return -1;
    if (limit <= 3) {
        printf("PICASSO_GPU cp: limit=%u reached after sqe upload\n", limit);
        return -1;
    }
    if (!programCp()) return -1;
    if (limit <= 4) {
        printf("PICASSO_GPU cp: limit=%u reached after ring programming\n", limit);
        return -1;
    }
    printf("PICASSO_GPU cp: ring programmed base=%08x/%08x cntl=%08x rptr_addr=%08x/%08x\n",
        gpuRead(A620_CP_RB_BASE), gpuRead(A620_CP_RB_BASE + 1),
        gpuRead(A620_CP_RB_CNTL), gpuRead(A620_CP_RB_RPTR_ADDR),
        gpuRead(A620_CP_RB_RPTR_ADDR + 1));
    if (limit <= 5) {
        printf("PICASSO_GPU cp: limit=%u reached after register readback\n", limit);
        return -1;
    }

    /* Clear SQE_HALT to start the CP engine. */
    gpuWrite(A620_CP_SQE_CNTL, 1);
    barrier();
    printf("PICASSO_GPU cp: sqe started cntl=%08x status=%08x int=%08x fault=%08x\n",
        gpuRead(A620_CP_SQE_CNTL), gpuRead(A620_RBBM_STATUS),
        gpuRead(A620_CP_INTERRUPT_STATUS), gpuRead(A620_CP_HW_FAULT));
    if (limit <= 6) {
        printf("PICASSO_GPU cp: limit=%u reached after sqe start\n", limit);
        return -1;
    }

    if (!cpInit()) return -1;
    uint32_t fence = 0;
    if (!submitNoop(&fence)) return -1;
    if (!waitFence(fence, kFenceTimeoutUs, true)) return -1;

    /* The GPU is initialized: a6xx_gmu_clear_oob(GPU_SET), the last step of
     * mainline hw_init. (Zap is not loaded, so no CP_SET_SECURE_MODE.) */
    native_adreno620_gmu_release_gpu_set();
    s.started = true;
    printf("PICASSO_GPU cp: ready fence=%u\n", fence);

    /* Optional ring soak (sheng-gpu-cp-stress=<n>): repeated no-op
     * submissions after the OOB is released, spaced far enough apart for the
     * GMU to reach its idle level between them. A production submit path must
     * keep working across that boundary. */
    uint32_t stressValue = 0;
    const unsigned stress = (PE_parse_boot_argn("sheng-gpu-cp-stress",
        &stressValue, static_cast<int>(sizeof stressValue)) && stressValue)
        ? stressValue : 0u;
    for (unsigned index = 0; index < stress; ++index) {
        uint32_t soak = 0;
        if (!submitNoop(&soak)) return -1;
        if (!waitFence(soak, kFenceTimeoutUs, true)) {
            printf("PICASSO_GPU cp: stress failed at %u/%u gx=%08x\n",
                index + 1, stress, gmuRead(kGmuGxPwrStatus));
            return -1;
        }
        if ((index % 10) == 0)
            printf("PICASSO_GPU cp: stress %u/%u fence=%u gx=%08x\n",
                index + 1, stress, soak, gmuRead(kGmuGxPwrStatus));
        IODelay(50000);
    }
    if (stress) printf("PICASSO_GPU cp: stress done count=%u\n", stress);

    /* First submission through the production NativeAdrenoRing with the A6xx
     * fence profile (sheng-gpu-ring-smoke=1). */
    uint32_t smokeValue = 0;
    if (PE_parse_boot_argn("sheng-gpu-ring-smoke", &smokeValue,
        static_cast<int>(sizeof smokeValue)) && smokeValue) {
        if (!ringSmoke()) {
            printf("PICASSO_GPU cp: ring smoke failed\n");
            return -1;
        }
        printf("PICASSO_GPU cp: ring smoke ok next=%u seqno=%u\n", s.next, s.sequence);
    }

    uint32_t publishValue = 0;
    bool service = false;
    if (PE_parse_boot_argn("sheng-gpu-publish", &publishValue,
        static_cast<int>(sizeof publishValue)) && publishValue) {
        if (!publishGpu()) {
            printf("PICASSO_GPU cp: publish failed\n");
            return -1;
        }
        service = true;
        printf("PICASSO_GPU cp: published\n");
    }

    /* Register the accelerator for Metal (sheng-gpu-metal=1). The plugin's
     * user client opens the service published just above, so publishing is
     * implied when it was not requested separately. */
    uint32_t metalValue = 0;
    if (PE_parse_boot_argn("sheng-gpu-metal", &metalValue,
        static_cast<int>(sizeof metalValue)) && metalValue) {
        if (!service && !publishGpu()) {
            printf("PICASSO_GPU cp: metal publish failed\n");
            return -1;
        }
        if (!publishMetal()) {
            printf("PICASSO_GPU cp: metal failed\n");
            return -1;
        }
        printf("PICASSO_GPU cp: metal published\n");
    }

    /* One user client of the published service, submitting through the A620
     * plain profile (sheng-gpu-client-test=1). */
    uint32_t clientValue = 0;
    if (PE_parse_boot_argn("sheng-gpu-client-test", &clientValue,
        static_cast<int>(sizeof clientValue)) && clientValue) {
        if (!gServicePublished && !publishGpu()) {
            printf("PICASSO_GPU cp: client test publish failed\n");
            return -1;
        }
        if (!clientTest()) {
            printf("PICASSO_GPU cp: client test failed\n");
            return -1;
        }
        printf("PICASSO_GPU cp: client test ok\n");
    }

    /* Report the boot addon's mount stages once launchd has run them. */
    uint32_t addonValue = 0;
    if (PE_parse_boot_argn("sheng-gpu-addon", &addonValue,
        static_cast<int>(sizeof addonValue)) && addonValue) {
        thread_t report = nullptr;
        if (kernel_thread_start(addonReport, nullptr, &report) == KERN_SUCCESS)
            thread_deallocate(report);
        else
            printf("PICASSO_GPU addon: report thread failed\n");
    }
    return 0;
}

extern "C" int native_adreno620_cp_submit(uint32_t *fence)
{
    if (!gState || !gState->started) return adreno620::ProbeHookAbsent;
    if (!fence) return -1;
    if (!submitNoop(fence)) return -1;
    /* The published ring class must follow this submission (it was adopted
     * before it): otherwise the service's hangcheck sees a fence page ahead
     * of the class's newest number and poisons the ring, and the class's
     * write pointer stays behind the CP's, which makes every later submit
     * look like a full ring. */
    if (NativeAdrenoRing *ring = ringObject()) ring->noteExternalSubmit(gState->next, *fence);
    return 0;
}

extern "C" int native_adreno620_cp_wait(uint32_t fence)
{
    if (!gState || !gState->started) return adreno620::ProbeHookAbsent;
    return waitFence(fence, kFenceTimeoutUs, true) ? 0 : -1;
}
