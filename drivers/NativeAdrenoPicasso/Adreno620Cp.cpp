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
#include "Adreno620Firmware.h"
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
#include <IOKit/IOBufferMemoryDescriptor.h>
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
#include <string.h>

/* The driver's client-BO reader falls back to a board DRAM reader when a BO has no kernel mapping (system BOs). This port had none
 * installed, so a stall's IB dump was all 0xdeadbeef. Reads one word through a temporary kernel mapping of its page. */
extern "C" int (*native_adreno_dram_read32)(uint64_t physical, uint32_t *value);
extern "C" int (*native_adreno_cached_read32)(uint64_t physical, uint32_t *value);
extern "C" int (*native_adreno_dram_read_words)(uint64_t physical, uint32_t *out, size_t count);
/* One fresh mapping per page, kIOMapInhibitCache: a default-cache mapping of a physical page is cacheable, so it would both read a
 * stale cache line and leave one behind for the next reader (the earlier form omitted the flag and could report a zero the CPU had
 * cached, which is exactly the diagnostic this reader exists to answer). */
static int dramReadPage(uint64_t physical, uint32_t *out, size_t count)
{
    if (!out || !count) return -1;
    const uint64_t page = physical & ~UINT64_C(0xfff);
    const size_t offset = size_t(physical & 0xfff);
    if (offset + count * sizeof(uint32_t) > 4096) count = (4096 - offset) / sizeof(uint32_t);
    IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withPhysicalAddress(page, 4096, kIODirectionIn);
    if (!descriptor) return -1;
    IOMemoryMap *map = descriptor->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly | kIOMapInhibitCache);
    descriptor->release();
    if (!map) return -1;
    const uint8_t *base = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(map->getAddress()));
    for (size_t i = 0; i < count; ++i) out[i] = *reinterpret_cast<volatile const uint32_t *>(base + offset + 4 * i);
    map->release();
    return int(count);
}
static int dramRead32(uint64_t physical, uint32_t *value)
{return dramReadPage(physical, value, 1) == 1 ? 0 : -1;}
static int dramReadWords(uint64_t physical, uint32_t *out, size_t count)
{return dramReadPage(physical, out, count);}
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
constexpr uint32_t kGmuSptpracPowerControl = 0x1A881;   /* GPU_GMU_GX_SPTPRAC_POWER_CONTROL: 0x778000 powers SP/TP */
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
 * qcom,highest-bank-bit = <14>). KGSL encodes mode 3 as field 0 (not UBWC 1.0);
 * hbb14 gives hbb_lo=1, hbb_hi=0. Mainline additionally sets amsbc (bit 4) for
 * UBWC >= 3.0; that is a separate pending change, so this boot keeps 0x2 to
 * isolate the SQE firmware bump. */
constexpr uint32_t kRbNcModeCntl = 0x00000002u;
constexpr uint32_t kTpl1NcModeCntl = 0x00000002u;
constexpr uint32_t kSpNcModeCntl = 0x00000022u;         /* uavflagprd_inv=2 for a650 family */
constexpr uint32_t kUcheModeCntl = 0x00200000u;         /* hbb_lo << 21 */

constexpr uint32_t kCpMeInitOpcode = 0x48;
constexpr uint32_t kCpSetSecureModeOpcode = 0x66;       /* adreno_pm4.xml CP_SET_SECURE_MODE */
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

/* wf130: NativeAdrenoTimer's CP source hands back CP_ALWAYS_ON_COUNTER's dword
 * indices (0x980/0x981); the GPU window is already mapped and powered here. */
IOReturn gpuTimerRead32(void *, uint32_t dword, uint32_t *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = gpuRead(dword);
    return kIOReturnSuccess;
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

/* wf103: SCM (secure channel manager) probe. The GPU boots in secure mode and, per the
 * zap-shader documentation, "can only render to protected buffers" until a signed zap is
 * loaded through TZ and the CP is switched out of secure mode. Loading that zap needs the
 * Qualcomm PAS SCM calls; this helper is the conduit those calls will use. It is the same
 * instruction the platform's PSCI driver uses (drivers/ArmCpu/ArmCpu.cpp). */
struct ScmReply { uint32_t a0, a1, a2, a3; uint64_t a6; };
ScmReply scmSipCall(uint32_t function, uint32_t arginfo, uint64_t arg0,
    uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0,
    uint64_t arg6 = 0)
{
    register uint64_t x0 __asm__("x0") = function;
    register uint64_t x1 __asm__("x1") = arginfo;
    register uint64_t x2 __asm__("x2") = arg0;
    register uint64_t x3 __asm__("x3") = arg1;
    register uint64_t x4 __asm__("x4") = arg2;
    register uint64_t x5 __asm__("x5") = arg3;
    /* x6 is the ARM_SMCCC_QUIRK_QCOM_A6 in/out register. Linux initialises it
     * to zero, passes it as the 7th SMC argument, and — after the call — stores
     * the value the firmware left in it as the session id of an interrupted
     * secure call: "the retry must pass back the session ID ... calls hang on
     * retry without it" (82bcd087 + Sricharan R's follow-up). wf106-wf109
     * hung exactly there: the first PAS call returns 1 (INTERRUPTED) and our
     * retry, with x6 forced back to 0, wedged the PIL session. */
    register uint64_t x6 __asm__("x6") = arg6;
    register uint64_t x7 __asm__("x7") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
        "+r"(x6), "+r"(x7)
        : "r"(x4), "r"(x5)
        : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16",
          "x17", "memory");
    return ScmReply{uint32_t(x0), uint32_t(x1), uint32_t(x2), uint32_t(x3), x6};
}

void scmProbe()
{
    constexpr uint32_t kSipOwner = 2u;              /* ARM_SMCCC_OWNER_SIP */
    constexpr uint32_t kOwnerShift = 24;
    constexpr uint32_t kFastCall = 1u << 31;        /* ARM_SMCCC_FAST_CALL */
    constexpr uint32_t kSmc64 = 1u << 30;           /* ARM_SMCCC_SMC_64 */
    constexpr uint32_t kSvcInfo = 0x06u, kIsCallAvail = 0x01u;
    constexpr uint32_t kSvcPil = 0x02u, kPasInitImage = 0x01u;
    constexpr uint32_t kPasMemSetup = 0x02u, kPasAuthAndReset = 0x05u;
    const uint32_t avail = (kFastCall | kSmc64 | (kSipOwner << kOwnerShift) |
        (kSvcInfo << 8) | kIsCallAvail);
    const uint64_t selector = (uint64_t(kSipOwner) << kOwnerShift) |
        uint64_t((kSvcPil << 8) | kPasInitImage);
    const ScmReply init = scmSipCall(avail, 1, selector);
    const uint64_t setupSelector = (uint64_t(kSipOwner) << kOwnerShift) |
        uint64_t((kSvcPil << 8) | kPasMemSetup);
    const ScmReply setup = scmSipCall(avail, 1, setupSelector);
    const uint64_t resetSelector = (uint64_t(kSipOwner) << kOwnerShift) |
        uint64_t((kSvcPil << 8) | kPasAuthAndReset);
    const ScmReply reset = scmSipCall(avail, 1, resetSelector);
    printf("PICASSO_GPU scm: is-call-avail init a0=%08x a1=%08x a2=%08x a3=%08x\n",
        init.a0, init.a1, init.a2, init.a3);
    printf("PICASSO_GPU scm: is-call-avail memsetup a0=%08x a1=%08x; auth %08x/%08x\n",
        setup.a0, setup.a1, reset.a0, reset.a1);
    /* wf107: the PAS calls in loadZap() are STD (blocking) SMCs, as
     * qcom_scm-smc.c sends every non-atomic call. This one is the same
     * read-only SVC_INFO query over STD instead of FAST: if it answers, the
     * STD path above EL1 works and a PAS hang is specific to the PIL service;
     * if it does not, no PAS call can ever return. */
    const uint32_t availStd = (kSmc64 | (kSipOwner << kOwnerShift) |
        (kSvcInfo << 8) | kIsCallAvail);
    printf("PICASSO_GPU scm: calling std is-call-avail\n");
    const ScmReply stdInfo = scmSipCall(availStd, 1, selector);
    printf("PICASSO_GPU scm: std is-call-avail a0=%08x a1=%08x\n",
        stdInfo.a0, stdInfo.a1);
    /* wf110: does the PIL service answer at all for this platform? PAS_IS_SUPPORTED (SVC_PIL 0x02,
     * cmd 0x07, ARGS(1) = pas id) reads no memory and touches no GPU state. If it answers for the
     * GPU's pas id 0x0d, the PIL service is live and the wf106-wf109 hang is specific to
     * PAS_INIT_IMAGE's metadata handling; if it hangs too, the PIL service is not usable from this
     * boot environment at all. Pas id 1 (modem on this platform) is the control. */
    {
        constexpr uint32_t kStdSipSmc64 = 0x40000000u | 0x02000000u;
        printf("PICASSO_GPU scm: calling pas-supported(13)\n");
        const ScmReply gpu = scmSipCall(kStdSipSmc64 | (0x02u << 8) | 0x07u,
            0x01u, 0x0du);
        printf("PICASSO_GPU scm: pas-supported(13) a0=%08x a1=%08x\n", gpu.a0, gpu.a1);
        printf("PICASSO_GPU scm: calling pas-supported(1)\n");
        const ScmReply modem = scmSipCall(kStdSipSmc64 | (0x02u << 8) | 0x07u,
            0x01u, 0x01u);
        printf("PICASSO_GPU scm: pas-supported(1) a0=%08x a1=%08x\n", modem.a0, modem.a1);
    }
    /* wf115 measured what happens when the AP touches the crypto engine while
     * its NoC path has no vote: the qcrypto@1de0000 register read took an
     * SError ("Unhandled implementation specific error") and killed the boot.
     * The liveness read therefore belongs after rpmhVoteCryptoPath() (wf116),
     * never before the vote, and it is not part of this read-only probe. */
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

    /* wf103: can this kernel reach the QCOM secure monitor, and does the firmware expose the
     * PAS calls the zap needs? (No state is changed; the reply is the measurement.) */
    scmProbe();

    /* a6xx_sptprac_enable() (a6xx_gmu.c). This device is a legacy GMU (gmu_is_wrapper is not set in
     * a6xx_catalog, and the DT property is absent), and on that path mainline powers the SP/TP rail
     * itself: it writes GPU_GMU_GX_SPTPRAC_POWER_CONTROL = 0x778000 and polls
     * GMU_SPTPRAC_PWR_CLK_STATUS until (status & 0x38) == 0x28. The port booted the GMU through the
     * HFI/OOB GPU_SET handshake but never wrote this register, and the measured result was a CP that
     * runs while no shader work happens: every draw, blit and compute left the client's memory
     * untouched and every fence retired (wf90-wf92). */
    {
        const uint32_t before = gmuRead(kGmuGxPwrStatus);
        gmuWrite(kGmuSptpracPowerControl, 0x778000u);
        uint32_t status = before;
        for (unsigned poll = 0; poll < 100; ++poll) {
            status = gmuRead(kGmuGxPwrStatus);
            if ((status & 0x38u) == 0x28u) break;
            IODelay(1);
        }
        printf("PICASSO_GPU cp: sptprac power: status %08x -> %08x (want &38==28)\n", before, status);
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

    /* RBBM_SECVID_TRUST_CNTL is read only (wf80: reading it returns the bootloader's 1, but writing the
     * 0 that a6xx hw_init's no-zap fallback writes hangs GPU init at the write, before the CP starts).
     * It is TZ's register on this device; the port leaves it alone and prints the value for the record. */
    printf("PICASSO_GPU cp: RBBM_SECVID_TRUST_CNTL=%08x (TZ-owned, not written)\n",
        gpuRead(A620_RBBM_SECVID_TRUST_CNTL));

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

    /* No GMEM VA range for this chip: a6xx_hw_init writes UCHE_GMEM_RANGE_MIN/MAX only outside the
     * a650 family/a702/a730 set, and msm reports MSM_PARAM_GMEM_BASE as 0 from ADRENO_6XX_GEN3 on. The
     * GPU decodes TILE6_2 (GMEM-tiled) addresses as offsets into its own 512 KiB tile memory; a window
     * here would move Turnip's tile addresses out of that space (wf77 programmed 0x40000000 and only
     * added UCHE OOB accesses). The trap/write-through window above is the only UCHE range this chip takes. */

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

/* wf100: mainline a6xx_gpu_init sends CP_SET_SECURE_MODE(0) once the zap shader
 * has loaded, then flushes and waits for the ring to idle. The GPU powers on in
 * secure mode; this packet is what switches the CP (and through it the GPU) to
 * non-secure operation. The port has no PIL/SCM to run the zap, never sent this
 * packet, and measures RBBM_SECVID_TRUST_CNTL = 1 — mainline's no-zap fallback
 * writes that register to 0 from the CPU, which this port measured as a bus hang
 * (wf80). This is the TZ-sanctioned alternative, and it is the only GPU state
 * known to differ from a rendering-clean target. */
bool leaveSecureMode()
{
    uint32_t words[2] = {};
    const uint32_t payload[1] = {0};
    if (!packet(words, 2, 0, kCpSetSecureModeOpcode, payload, 1)) return false;
    uint32_t end = 0;
    if (!submitRing(words, 2, &end)) return false;
    printf("PICASSO_GPU cp: CP_SET_SECURE_MODE(0) submitted wptr=%u\n", end);
    if (!waitIdle(end, kMeInitTimeoutUs)) {
        printCpStatus("secure-mode");
        return false;
    }
    printf("PICASSO_GPU cp: CP_SET_SECURE_MODE(0) idle rptr=%u\n", gpuRead(A620_CP_RB_RPTR));
    return true;
}

/* wf116: the crypto path's RPMh votes. The vendor cannot run PAS_INIT_IMAGE or
 * PAS_AUTH_AND_RESET until it has voted the crypto engine's NoC path:
 * subsys-pil-tz.c scm_pas_enable_bw() registers an msm_bus client with
 * src = MSM_BUS_MASTER_CRYPTO_CORE_0 (0x7d), dst = EBI (0x200) and 393.6 MB/s,
 * and calls it around every PAS call, because TZ authenticates the image on the
 * qcrypto engine, whose NoC master port is clock-gated by the BCM machinery
 * until that vote exists. wf115 measured the same gating from the AP side: a
 * read of qcrypto@1de0000 took an SError. The path in this device's bus
 * topology (mas_qxm_crypto -> slv_qns_a2noc_snoc -> mas_qnm_aggre2_noc ->
 * slv_qns_gemnoc_sf -> mas_qnm_snoc_sf -> slv_qns_llcc -> mas_llcc_mc ->
 * slv_ebi) names the BCMs CE0, SN6, SN0, SH0, MC0 and ACV; slv_qhs_crypto0_cfg
 * (the register window whose read faulted) names CN0. The downstream bus
 * driver sends these as BCM_TCS_CMD(commit, valid, vote_x, vote_y) commands in
 * the apps RSC's active TCS group, with the vote clamped to the 14-bit field
 * and the VCD's last command carrying commit (msm_bus_fabric_rpmh.c
 * tcs_cmd_gen/tcs_cmd_list_gen, msm_bus_arb_rpmh.c bcm_update_bus_req).
 * Register layout and offsets are rpmh-rsc.c's v2.7/v3.0 tables, chosen by the
 * DRV_ID major version; the TCS block offset is the DT's qcom,tcs-offset; the
 * ACTIVE group is first in qcom,tcs-config <2 2 ...> (type ACTIVE = 2, count 2).
 * Addresses and per-BCM aux data (unit, width, VCD) come from the AOP command
 * DB, read the same way as drivers/soc/qcom/cmd-db.c. */
constexpr uint64_t kAppsRscDrv2Base = 0x18220000u;   /* DT reg drv-2 (qcom,drv-id 2) */
constexpr uint64_t kAppsRscBytes = 0x10000u;
constexpr uint64_t kRscTcsOffset = 0xd00u;           /* DT qcom,tcs-offset */
constexpr uint64_t kRpmhCmdDbBase = 0x80860000u;     /* DT cmd-db memory region */
constexpr uint64_t kRpmhCmdDbBytes = 0x20000u;
constexpr uint32_t kRscAmcModeEnable = 1u << 16;
constexpr uint32_t kRscAmcModeTrigger = 1u << 24;
constexpr uint32_t kRscCmdMsgidWrite = 8u | (1u << 8) | (1u << 16);  /* LEN|RESP|WRITE */
constexpr uint32_t kRscCmdStatusCompl = 1u << 16;
constexpr uint32_t kBcmVoteMax = 0x3fffu;            /* BCM_TCS_CMD_VOTE_MASK */
constexpr uint32_t kBcmAcvApps = 0x8;                /* BCM_TCS_CMD_ACV_APPS */

struct RscLayout {
    uint32_t tcsStride, cmdStride, irqStatus, irqClear, waitForCmpl, control,
        status, cmdEnable, cmdMsgid, cmdAddr, cmdData, cmdStatus;
};

RscLayout rscLayout(uint32_t drvId)
{
    /* rpmh-rsc.c rpmh_rsc_reg_offset_ver_3_0 / ver_2_7. */
    if (((drvId >> 16) & 0xffu) >= 3u)
        return RscLayout{672, 24, 0x04, 0x08, 0x20, 0x24, 0x28, 0x2c, 0x34, 0x38, 0x3c, 0x40};
    return RscLayout{672, 20, 0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 0x3c};
}

uint16_t rpmhLe16(const volatile uint8_t *p)
{
    return uint16_t(p[0] | (p[1] << 8));
}

uint32_t rpmhLe32(const volatile uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
        (uint32_t(p[3]) << 24);
}

struct CmdDbBcm {
    uint32_t address, unit;
    uint16_t width;
    uint8_t vcd;
    bool found;
};

/* cmd_db_read_addr() + cmd_db_read_aux_data(struct bcm_db). */
CmdDbBcm cmdDbBcmLookup(const Mapping &db, const char *name)
{
    CmdDbBcm out = {};
    if (!db.base || db.bytes < 144 || rpmhLe32(db.base + 4) != 0x0c0330dbu)
        return out;
    uint8_t query[8] = {};
    for (unsigned i = 0; i < 8 && name[i]; ++i) query[i] = uint8_t(name[i]);
    for (unsigned resource = 0; resource < 8; ++resource) {
        const volatile uint8_t *slave = db.base + 8 + resource * 16;
        if (!rpmhLe16(slave)) break;
        const uint64_t headers = 144u + rpmhLe16(slave + 2);
        const uint64_t data = 144u + rpmhLe16(slave + 4);
        const unsigned count = rpmhLe16(slave + 6);
        for (unsigned entry = 0; entry < count; ++entry) {
            const uint64_t at = headers + uint64_t(entry) * 24;
            if (at + 24 > db.bytes) return out;
            bool match = true;
            for (unsigned byte = 0; byte < 8; ++byte)
                if (db.base[at + byte] != query[byte]) { match = false; break; }
            if (!match) continue;
            out.address = rpmhLe32(db.base + at + 16);
            const unsigned auxLength = rpmhLe16(db.base + at + 20);
            const uint64_t aux = data + rpmhLe16(db.base + at + 22);
            if (auxLength >= 8 && aux + 8 <= db.bytes) {
                out.unit = rpmhLe32(db.base + aux);
                out.width = rpmhLe16(db.base + aux + 4);
                out.vcd = db.base[aux + 6];
            }
            out.found = out.address != 0;
            return out;
        }
    }
    return out;
}

/* Votes the crypto path's BCMs through the apps RSC's active TCSs. Returns the
 * number of BCMs voted. Every wait is bounded: a stuck RSC must not stop the
 * boot, and the PAS result is what the caller measures. */
unsigned rpmhVoteCryptoPath()
{
    Mapping rsc = {};
    Mapping db = {};
    if (!mapPhysical(kAppsRscDrv2Base, kAppsRscBytes, rsc)) {
        printf("PICASSO_GPU rpmh: RSC at %08x did not map\n",
            uint32_t(kAppsRscDrv2Base));
        return 0;
    }
    const bool dbMapped = mapPhysical(kRpmhCmdDbBase, kRpmhCmdDbBytes, db);
    if (!dbMapped)
        printf("PICASSO_GPU rpmh: cmd-db at %08x did not map\n",
            uint32_t(kRpmhCmdDbBase));

    const uint32_t drvId = read32(rsc, 0);
    const RscLayout layout = rscLayout(drvId);
    const uint64_t tcsBase = kRscTcsOffset;
    const uint32_t irqEnable = read32(rsc, tcsBase + 0x00);
    printf("PICASSO_GPU rpmh: drv-id=%08x tcs-stride=%u cmd-stride=%u irq-enable=%08x\n",
        drvId, layout.tcsStride, layout.cmdStride, irqEnable);

    /* The TX-done line is a GIC SPI this kernel has no handler for. Mask only
     * the two TCSs this vote uses for the duration of the vote and restore the
     * register afterwards; IRQ_STATUS still records completion. */
    write32(rsc, tcsBase + 0x00, irqEnable & ~3u);
    barrier();

    struct Vote {
        const char *name;
        bool acv;
    };
    const Vote votes[] = {
        {"CE0", false}, {"SN6", false}, {"SN0", false}, {"SH0", false},
        {"MC0", false}, {"ACV", true}, {"CN0", false},
    };

    auto waitIdle = [&](uint64_t tcsAt) {
        uint32_t status = 0;
        for (unsigned spin = 0; spin < 200; ++spin) {
            status = read32(rsc, tcsAt + layout.status);
            if (status) break;
            IODelay(10);
        }
        return status;
    };

    auto sendBcm = [&](unsigned tcs, uint32_t address, uint32_t data) {
        const uint64_t tcsAt = tcsBase + uint64_t(layout.tcsStride) * tcs;
        waitIdle(tcsAt);
        /* __tcs_set_trigger(false): clear the AMC trigger, then the mode. */
        uint32_t control = read32(rsc, tcsAt + layout.control);
        control &= ~kRscAmcModeTrigger;
        write32(rsc, tcsAt + layout.control, control);
        control &= ~kRscAmcModeEnable;
        write32(rsc, tcsAt + layout.control, control);
        write32(rsc, tcsAt + layout.cmdEnable, 0);
        barrier();
        write32(rsc, tcsAt + layout.cmdMsgid, kRscCmdMsgidWrite);
        write32(rsc, tcsAt + layout.cmdAddr, address);
        write32(rsc, tcsAt + layout.cmdData, data);
        write32(rsc, tcsAt + layout.cmdEnable, 1u);
        barrier();
        /* __tcs_set_trigger(true), with the synchronous readbacks rpmh-rsc.c
         * relies on between the mode enable and the trigger. */
        write32(rsc, tcsAt + layout.control, kRscAmcModeEnable);
        (void)read32(rsc, tcsAt + layout.control);
        write32(rsc, tcsAt + layout.control, kRscAmcModeEnable | kRscAmcModeTrigger);
        (void)read32(rsc, tcsAt + layout.control);
        uint32_t irqStatus = 0, status = 0, cmdStatus = 0;
        unsigned spins = 0;
        for (; spins < 500; ++spins) {
            irqStatus = read32(rsc, tcsBase + layout.irqStatus);
            status = read32(rsc, tcsAt + layout.status);
            cmdStatus = read32(rsc, tcsAt + layout.cmdStatus);
            if ((irqStatus & (1u << tcs)) || (cmdStatus & kRscCmdStatusCompl))
                break;
            IODelay(10);
        }
        write32(rsc, tcsBase + layout.irqClear, 1u << tcs);
        write32(rsc, tcsAt + layout.cmdEnable, 0);
        control = read32(rsc, tcsAt + layout.control);
        control &= ~kRscAmcModeTrigger;
        write32(rsc, tcsAt + layout.control, control);
        control &= ~kRscAmcModeEnable;
        write32(rsc, tcsAt + layout.control, control);
        barrier();
        const bool done = (irqStatus & (1u << tcs)) ||
            (cmdStatus & kRscCmdStatusCompl);
        printf("PICASSO_GPU rpmh: tcs=%u irq=%08x status=%08x cmd-status=%08x "
               "spins=%u\n", tcs, irqStatus, status, cmdStatus, spins);
        return done;
    };

    unsigned voted = 0;
    for (unsigned index = 0; index < sizeof(votes) / sizeof(votes[0]); ++index) {
        const CmdDbBcm bcm = cmdDbBcmLookup(db, votes[index].name);
        if (!bcm.found) {
            printf("PICASSO_GPU rpmh: cmd-db has no %s\n", votes[index].name);
            continue;
        }
        /* Downstream clamps any bandwidth vote to the 14-bit field; ACV is not a
         * bandwidth vote but a VCD bitmask (bcm_update_acv_req). */
        const uint32_t data = votes[index].acv
            ? ((1u << 30) | (1u << 29) | kBcmAcvApps)
            : ((1u << 30) | (1u << 29) | (kBcmVoteMax << 14) | kBcmVoteMax);
        const unsigned tcs = index & 1u;
        const bool done = sendBcm(tcs, bcm.address, data);
        printf("PICASSO_GPU rpmh: %s addr=%x unit=%u width=%u vcd=%u data=%08x tcs=%u done=%d\n",
            votes[index].name, bcm.address, bcm.unit, unsigned(bcm.width),
            unsigned(bcm.vcd), data, tcs, int(done));
        ++voted;
    }
    write32(rsc, tcsBase + 0x00, irqEnable);
    barrier();
    if (dbMapped && db.keep) db.keep->release();
    if (rsc.keep) rsc.keep->release();
    printf("PICASSO_GPU rpmh: crypto path voted %u/%u\n", voted,
        unsigned(sizeof(votes) / sizeof(votes[0])));
    return voted;
}

/* wf119: the probe thread blocks inside EL3 once the interrupted PAS session is
 * resumed, but the rest of the machine keeps running. This helper runs on
 * another CPU and dumps the TZ/hypervisor log ring (DT qcom,tz-log@146ab720,
 * "hyplog-enabled", address and size in the u32 at +0x410/+0x414, the layout of
 * the vendor tz_log driver) so whatever QTEE logged around the stall reaches
 * the console. */
void dumpTextRuns(const char *tag, const Mapping &m, uint64_t offset,
    uint64_t bytes, unsigned maxLines)
{
    char line[97];
    unsigned length = 0, lines = 0;
    for (uint64_t i = offset; i < offset + bytes && i < m.bytes; ++i) {
        const uint8_t byte = m.base[i];
        if (byte >= 0x20 && byte < 0x7f) {
            if (length < sizeof line - 1) line[length++] = char(byte);
            continue;
        }
        if (length >= 8) {
            line[length] = 0;
            printf("PICASSO_GPU %s: %s\n", tag, line);
            if (++lines >= maxLines) return;
        }
        length = 0;
    }
    if (length >= 8 && lines < maxLines) {
        line[length] = 0;
        printf("PICASSO_GPU %s: %s\n", tag, line);
    }
}

/* wf120: the TZ log is binary (wf119 found no printable runs in either ring),
 * so dump the raw bytes as 64-character windows with non-printables as '.'.
 * That keeps magic words and ring headers visible without depending on a
 * format this project has not confirmed. */
void dumpBytes(const char *tag, const Mapping &m, uint64_t offset,
    uint64_t bytes)
{
    char line[65];
    const uint64_t end = offset + bytes;
    for (uint64_t at = offset; at < end && at < m.bytes; at += 64) {
        unsigned n = 0;
        for (unsigned i = 0; i < 64 && at + i < end && at + i < m.bytes; ++i) {
            const uint8_t byte = m.base[at + i];
            line[n++] = (byte >= 0x20 && byte < 0x7f) ? char(byte) : '.';
        }
        line[n] = 0;
        printf("PICASSO_GPU %s: %s\n", tag, line);
    }
}

void tzLogDump()
{
    Mapping base = {};
    if (!mapPhysical(UINT64_C(0x146ab720), 0x3000, base)) {
        printf("PICASSO_GPU tzlog: base map failed\n");
        return;
    }
    const uint32_t address = read32(base, 0x410);
    const uint32_t size = read32(base, 0x414);
    printf("PICASSO_GPU tzlog: hyplog address=%08x size=%08x magic=%08x "
           "%08x %08x\n", address, size, read32(base, 0x0), read32(base, 0x4),
        read32(base, 0x400));
    dumpTextRuns("tzlog", base, 0, 0x3000, 12);
    dumpBytes("tzlog-raw", base, 0, 0x100);
    if (base.keep) base.keep->release();
    if (!address || address == 0xffffffffu) return;
    const uint64_t page = address & ~UINT64_C(0xfff);
    const uint64_t offset = address - page;
    uint64_t bytes = size ? size : 0x4000;
    if (bytes > 0x40000) bytes = 0x40000;
    Mapping hyp = {};
    if (!mapPhysical(page, bytes + offset, hyp)) {
        printf("PICASSO_GPU tzlog: hyplog map failed pa=%08x\n", address);
        return;
    }
    /* The ring keeps its newest text at the end for this firmware; the head
     * is dumped as well because a wrap splits the newest lines. */
    dumpTextRuns("hyplog-head", hyp, offset, bytes < 0x800 ? bytes : 0x800, 8);
    dumpBytes("hyplog-raw", hyp, offset, bytes < 0x200 ? bytes : 0x200);
    if (bytes > 0x400)
        dumpBytes("hyplog-tail", hyp, offset + bytes - 0x400, 0x400);
    if (hyp.keep) hyp.keep->release();
}

void tzLogThread(void *, wait_result_t)
{
    IOSleep(5000);
    tzLogDump();
    IOSleep(15000);
    tzLogDump();
}

/* wf125: a print-only heartbeat. Every fatal post-zap boot stops mid-console
 * without the CDC panic marker, so the open question is whether the machine
 * froze or only the PID1 thread stopped printing. This thread touches no
 * hardware: one line every five seconds keeps that distinction measurable. */
void heartbeatThread(void *, wait_result_t)
{
    for (unsigned beat = 1;; ++beat) {
        IOSleep(5000);
        printf("PICASSO_GPU beat %u\n", beat);
    }
}

/* wf104: the zap shader. Mainline hw_init() loads it right after a6xx_cp_init()
 * and then sends CP_SET_SECURE_MODE(0) (a6xx_gpu.c); adreno_zap_shader_load()
 * runs the MDT through the QCOM PAS calls. The two pieces:
 *   - metadata: qcom_mdt_read_metadata()'s blob, ELF header + program headers
 *     followed by the hash segment with no padding. For this split .mdt that
 *     is the whole file: phdrs[0].p_filesz 0x94 + phdrs[1].p_filesz 0x1a38 =
 *     0x1acc = 6860 bytes. It goes to PAS_INIT_IMAGE from a physically
 *     contiguous 4K-aligned buffer (qcom_scm_pas_init_image turns memory
 *     protection on for it).
 *   - region: qcom_mdt_get_size() over loadable segments only (the hash
 *     segment is type 2 and never loadable). The single PT_LOAD is
 *     a620_zap.b02: p_paddr 0x5000, filesz=memsz=0x68c, so the span is
 *     ALIGN(0x568c) - 0x5000 = 0x1000 and qcom_mdt_load_no_init() copies it to
 *     region + (p_paddr - min_paddr) = offset 0.
 * The region is the one the device tree reserves for exactly this: pil_gpu_mem
 * "pil_gpu_region@8bc15400", no-map, size 0x2000, with qcom,kgsl-hyp carrying
 * pas-id 0x0d and firmware-name a620_zap. PAS_AUTH_AND_RESET then starts the
 * zap, after which the CP accepts CP_SET_SECURE_MODE(0).
 * SMC transport: qcom_scm-smc.c fills x0=CALL_VAL(type, SMC_64, OWNER_SIP,
 * fnid), x1=arginfo, x2..=args; PAS runs as STD_CALL, and x0==1 means
 * QCOM_SCM_INTERRUPTED (retry), as __scm_smc_do_quirk does. */
bool loadZap()
{
    /* wf116: vote the crypto engine's NoC path before TZ runs PAS, the way
     * subsys-pil-tz.c's scm_pas_enable_bw() does around every PAS call. */
    rpmhVoteCryptoPath();

    /* wf117: the interrupted-SMC quirk requires the normal world to take the
     * interrupt that ended the secure call before the session is resumed.
     * Record DAIF at the SMC boundary, and (behind sheng-gpu-ce=1) probe the CE
     * registers the vote above is meant to un-gate: wf115 faulted on exactly
     * this read before any vote existed. */
    {
        uint64_t daif = 0;
        __asm__ volatile("mrs %0, daif" : "=r"(daif));
        printf("PICASSO_GPU zap: daif=%llx before pas\n",
            static_cast<unsigned long long>(daif));
    }
    {
        uint32_t probe = 0;
        if (PE_parse_boot_argn("sheng-gpu-ce", &probe, sizeof probe) && probe) {
            Mapping ce = {};
            if (mapPhysical(UINT64_C(0x1de0000), 0x20000, ce)) {
                printf("PICASSO_GPU ce: version=%08x status=%08x engines=%08x\n",
                    read32(ce, 0x1A000), read32(ce, 0x1A100),
                    read32(ce, 0x1A108));
                if (ce.keep) ce.keep->release();
            } else {
                printf("PICASSO_GPU ce: map failed\n");
            }
        }
    }

    constexpr uint32_t kPasSvc = 0x02;
    constexpr uint32_t kPasInitImageCmd = 0x01;
    constexpr uint32_t kPasMemSetupCmd = 0x02;
    constexpr uint32_t kPasAuthAndResetCmd = 0x05;
    constexpr uint32_t kArgInfoInitImage = 0x82;    /* QCOM_SCM_ARGS(2, VAL, RW) */
    constexpr uint32_t kArgInfoMemSetup = 0x03;     /* QCOM_SCM_ARGS(3) */
    constexpr uint32_t kArgInfoAuth = 0x01;         /* QCOM_SCM_ARGS(1) */
    constexpr uint32_t kScmSipStd = 0x42000000u;    /* STD_CALL | SMC_64 | OWNER_SIP */
    constexpr uint32_t kGpuPasId = 0x0d;
    constexpr size_t kRegionCapacity = 0x2000;
    constexpr uint32_t kMdtTypeMask = 0x7000000u;
    constexpr uint32_t kMdtTypeHash = 0x2000000u;

    const uint8_t *mdt = nullptr;
    size_t mdtBytes = 0;
    const uint8_t *segments[3] = {};
    size_t segmentBytes[3] = {};
    if (native_adreno620_zap_firmware(&mdt, &mdtBytes, segments,
            segmentBytes) != 0) {
        printf("PICASSO_GPU zap: no firmware staged\n");
        return false;
    }
    Adreno620MdtImage image{};
    const Adreno620FwStatus parsed = Adreno620MdtParse(mdt, mdtBytes,
        segments, segmentBytes, 3, &image);
    if (parsed != Adreno620FwOk || !image.segments) {
        printf("PICASSO_GPU zap: mdt parse status=%d\n", int(parsed));
        return false;
    }

    /* qcom_mdt_read_metadata(): phdrs[0].p_filesz + the hash segment. */
    if (image.segment[0].filesz > mdtBytes) {
        printf("PICASSO_GPU zap: mdt header exceeds file\n");
        return false;
    }
    unsigned hashIndex = 0;
    for (unsigned index = 1; index < image.segments; ++index) {
        if ((image.segment[index].flags & kMdtTypeMask) == kMdtTypeHash) {
            hashIndex = index;
            break;
        }
    }
    if (!hashIndex) {
        printf("PICASSO_GPU zap: mdt has no hash segment\n");
        return false;
    }
    const size_t metadataBytes = size_t(image.segment[0].filesz +
        image.segment[hashIndex].filesz);
    if (metadataBytes > mdtBytes) {
        printf("PICASSO_GPU zap: metadata exceeds mdt bytes=%x\n",
            uint32_t(mdtBytes));
        return false;
    }

    /* qcom_mdt_get_size() over the loadable segments. */
    uint64_t minAddress = UINT64_MAX, maxAddress = 0;
    for (unsigned index = 0; index < image.segments; ++index) {
        const Adreno620MdtSegment &segment = image.segment[index];
        if (!segment.loadable) continue;
        if (segment.vaddr < minAddress) minAddress = segment.vaddr;
        const uint64_t end = (segment.vaddr + segment.memsz +
            UINT64_C(0xfff)) & ~UINT64_C(0xfff);
        if (end > maxAddress) maxAddress = end;
    }
    if (minAddress >= maxAddress || maxAddress - minAddress > kRegionCapacity) {
        printf("PICASSO_GPU zap: loadable span invalid min=%x max=%x\n",
            uint32_t(minAddress), uint32_t(maxAddress));
        return false;
    }
    const size_t regionBytes = size_t(maxAddress - minAddress);

    /* Metadata: physically contiguous, 4K aligned, and — like the vendor
     * pil_init_image_trusted() — written through a strongly ordered (uncached)
     * mapping. The vendor asks dma_alloc_attrs for DMA_ATTR_STRONGLY_ORDERED
     * before the very same PAS_INIT_IMAGE call; TZ enables memory protection
     * over this blob during the call, so a cacheable alias is the remaining
     * difference from stock. XNU's kIOMapInhibitCache makes the pages IO
     * (uncached) for every mapping, and the bytes are written through that
     * mapping with byte stores (device memory rejects DC ZVA and unaligned
     * STP, wf104/wf105). */
    const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
        static_cast<IOOptionBits>(kIOMemoryMapperNone) |
        static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    IOBufferMemoryDescriptor *metadata =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options,
            metadataBytes, UINT64_C(0xfffff000));
    if (!metadata) {
        printf("PICASSO_GPU zap: metadata allocation failed bytes=%x\n",
            uint32_t(metadataBytes));
        return false;
    }
    if (metadata->prepare() != kIOReturnSuccess) {
        metadata->release();
        printf("PICASSO_GPU zap: metadata prepare failed\n");
        return false;
    }
    IOByteCount metadataLength = 0;
    const uint64_t metadataPhys = metadata->getPhysicalSegment(0,
        &metadataLength, kIOMemoryMapperNone);
    IOMemoryMap *metadataMap = metadata->map(kIOMapAnywhere | kIOMapInhibitCache);
    uint8_t *metadataImage = metadataMap ? reinterpret_cast<uint8_t *>(
        static_cast<uintptr_t>(metadataMap->getAddress())) : nullptr;
    if (!metadataImage || !metadataPhys || metadataLength < metadataBytes) {
        if (metadataMap) metadataMap->release();
        metadata->complete();
        metadata->release();
        printf("PICASSO_GPU zap: metadata mapping unavailable\n");
        return false;
    }
    for (size_t index = 0; index < metadataBytes; ++index)
        metadataImage[index] = mdt[index];
    __asm__ volatile("dsb sy" ::: "memory");
    printf("PICASSO_GPU zap: metadata bytes=%x phys=%08x/%08x first=%08x\n",
        uint32_t(metadataBytes), uint32_t(metadataPhys),
        uint32_t(metadataPhys >> 32),
        *reinterpret_cast<volatile const uint32_t *>(metadataImage));
    /* wf108: can the secure world read this buffer at all? qcom_scm_io_readl()
     * (SVC_IO 0x05 / IO_READ 0x01, ARGS(1) = physical address, value in a1) is
     * an atomic FAST call, so it cannot wait on the PIL service. If it returns
     * the ELF magic the metadata memory is reachable from TZ and a PAS hang is
     * specific to PIL; if it hangs too, TZ cannot read our DRAM at all. */
    {
        constexpr uint32_t kSvcIo = 0x05u, kIoRead = 0x01u;
        constexpr uint32_t kFastSipSmc64 = 0x80000000u | 0x40000000u | 0x02000000u;
        const uint64_t ioSelector = (uint64_t(2) << 24) |
            uint64_t((kSvcIo << 8) | kIoRead);
        const ScmReply available = scmSipCall(kFastSipSmc64 | (0x06u << 8) | 0x01u,
            0x01u, ioSelector);
        printf("PICASSO_GPU zap: svc-io available a0=%08x a1=%08x\n",
            available.a0, available.a1);
        if (!available.a0 && available.a1) {
            printf("PICASSO_GPU zap: calling svc-io read phys=%08x\n",
                uint32_t(metadataPhys));
            const ScmReply ioRead = scmSipCall(kFastSipSmc64 | (kSvcIo << 8) |
                kIoRead, 0x01u, metadataPhys);
            printf("PICASSO_GPU zap: svc-io read a0=%08x a1=%08x want=464c457f\n",
                ioRead.a0, ioRead.a1);
        }
    }

    /* wf121: the segment region must satisfy the PT_LOAD's p_align.
     * The zap's single loadable segment has p_align 0x100000, and the vendor's
     * pil_setup_region()/pil_alloc_region() allocates the relocatable region
     * with that alignment (peripheral-loader.c). wf120 proved the DT's
     * pil_gpu_mem carveout at 0x8bc15400 — only 0x400-aligned — is rejected by
     * PAS_MEM_SETUP with 0xffcfffba, so allocate a physically contiguous
     * 1 MiB-aligned buffer and stage the segments there (uncached, byte
     * stores, exactly as before). */
    const IOOptionBits regionOptions =
        static_cast<IOOptionBits>(kIODirectionInOut) |
        static_cast<IOOptionBits>(kIOMemoryMapperNone) |
        static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous);
    IOBufferMemoryDescriptor *regionBuffer =
        IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            regionOptions, regionBytes, UINT64_C(0xfffffffffff00000));
    if (!regionBuffer || regionBuffer->prepare() != kIOReturnSuccess) {
        if (regionBuffer) regionBuffer->release();
        metadataMap->release();
        metadata->complete();
        metadata->release();
        printf("PICASSO_GPU zap: region allocation failed\n");
        return false;
    }
    IOByteCount regionLength = 0;
    const uint64_t regionPhys = regionBuffer->getPhysicalSegment(0,
        &regionLength, kIOMemoryMapperNone);
    IOMemoryMap *regionMap = regionBuffer->map(kIOMapAnywhere | kIOMapInhibitCache);
    if (!regionMap || !regionPhys || regionLength < regionBytes) {
        if (regionMap) regionMap->release();
        regionBuffer->complete();
        regionBuffer->release();
        metadataMap->release();
        metadata->complete();
        metadata->release();
        printf("PICASSO_GPU zap: region mapping failed\n");
        return false;
    }
    uint8_t *region = reinterpret_cast<uint8_t *>(
        static_cast<uintptr_t>(regionMap->getAddress()));
    printf("PICASSO_GPU zap: region phys=%08x/%08x span=%x va=%08x/%08x\n",
        uint32_t(regionPhys), uint32_t(regionPhys >> 32),
        uint32_t(regionBytes),
        uint32_t(reinterpret_cast<uintptr_t>(region)),
        uint32_t(uint64_t(reinterpret_cast<uintptr_t>(region)) >> 32));
    /* No bzero()/memcpy() here: XNU's bzero uses DC ZVA and its memcpy ends
     * with unaligned 16-byte stores, and both fault on the kIOMapInhibitCache
     * (device-memory) mapping — measured in wf104 (DC ZVA) and wf105 (STP at a
     * non-16-aligned address). Byte stores are always aligned for device
     * memory. The reserved region is no-map and private to PIL; upstream
     * zeroes only a segment's memsz-filesz tail, as the loop below does. */
    bool copied = true;
    for (unsigned index = 0; index < image.segments; ++index) {
        const Adreno620MdtSegment &segment = image.segment[index];
        if (!segment.loadable) continue;
        const uint64_t offset = segment.vaddr - minAddress;
        if (!segment.data || segment.bytes < segment.filesz ||
            offset + segment.memsz > regionBytes) {
            copied = false;
            break;
        }
        for (uint64_t byte = 0; byte < segment.memsz; ++byte) {
            const uint8_t value = byte < segment.filesz ? segment.data[byte] : 0;
            *reinterpret_cast<volatile uint8_t *>(region + offset + byte) = value;
        }
        printf("PICASSO_GPU zap: segment %u offset=%x filesz=%x memsz=%x\n",
            index, uint32_t(offset), uint32_t(segment.filesz),
            uint32_t(segment.memsz));
    }
    /* The region mapping is device memory (no cache lines), so the stores above
     * are already in DRAM; the DSB orders them before TZ reads the region. */
    __asm__ volatile("dsb sy" ::: "memory");
    if (!copied) {
        regionMap->release();
        regionBuffer->complete();
        regionBuffer->release();
        metadataMap->release();
        metadata->complete();
        metadata->release();
        printf("PICASSO_GPU zap: segment outside region\n");
        return false;
    }

    /* A Qualcomm STD SMC may return before the operation completes:
     * QCOM_SCM_INTERRUPTED (a0 = 1) plus a session id in x6. The retry must
     * carry that session id and x0 = 1 — exactly arm_smccc_smc_quirk() and
     * qcom_scm-smc.c's `if (res->a0 == QCOM_SCM_INTERRUPTED) a0 = res->a0`
     * (commit 82bcd087 and Sricharan R's follow-up; without x6 "calls hang on
     * retry"). wf112 measured this first-hand: PAS_INIT returned a0=1, and the
     * old retry, which re-zeroed x6, wedged the PIL session (wf106-wf109). */
    /* wf119: which interrupt ends the secure call? The interrupted-SMC protocol
     * exists so the normal world can take a pending IRQ before resuming the
     * session; the resume then hangs, so record what is pending at that moment.
     * wf118 (both runs) reset the board ~15 s after the first PAS call without
     * printing anything after it; the wf118 dump read ICC_HPPIR0_EL1, a
     * Secure-group register that can trap to EL3 from Non-secure EL1 and wedge
     * the very monitor whose session is suspended. This version prints first
     * and reads only NS-safe state: ICC_HPPIR1/RPR/CTLR, and the distributor's
     * pending/enable/group bits for SPI 272 (the crypto BAM, DT qcrypto
     * interrupts = <0 0x110 4>; bank 8, bit 16 in each GICD bitmap). */
    auto dumpGic = [] {
        printf("PICASSO_GPU gic: enter\n");
        uint64_t hppir1 = 0, rpr = 0, ctlr = 0, daif = 0;
        __asm__ volatile("mrs %0, icc_hppir1_el1" : "=r"(hppir1));
        __asm__ volatile("mrs %0, icc_rpr_el1" : "=r"(rpr));
        __asm__ volatile("mrs %0, icc_ctlr_el1" : "=r"(ctlr));
        __asm__ volatile("mrs %0, daif" : "=r"(daif));
        printf("PICASSO_GPU gic: daif=%llx hppir1=%llx rpr=%llx ctlr=%llx\n",
            static_cast<unsigned long long>(daif),
            static_cast<unsigned long long>(hppir1),
            static_cast<unsigned long long>(rpr),
            static_cast<unsigned long long>(ctlr));
        Mapping gic = {};
        if (!mapPhysical(UINT64_C(0x17a00000), 0x10000, gic)) {
            printf("PICASSO_GPU gic: distributor map failed\n");
            return;
        }
        printf("PICASSO_GPU gic: dist ctlr=%08x typer=%08x pend8=%08x "
               "en8=%08x grp8=%08x cfg272=%08x\n",
            read32(gic, 0x00), read32(gic, 0x04), read32(gic, 0x2a0),
            read32(gic, 0x120), read32(gic, 0x1a0), read32(gic, 0xc44));
        if (gic.keep) gic.keep->release();
    };
    auto pasCall = [&dumpGic](uint32_t function, uint32_t arginfo, uint64_t arg0,
        uint64_t arg1, uint64_t arg2, bool maskFirst) {
        uint64_t begin = 0, end = 0;
        uint64_t pmr = 0, daif = 0;
        if (maskFirst) {
            /* wf120: hold back Non-secure interrupt delivery for the first
             * attempt only (PMR=0 masks the CPU interface; DAIF.I masks the
             * PSTATE). The retry below always runs unmasked, because the
             * interrupted-call protocol requires the normal world to take the
             * pending interrupt before resuming the secure session. */
            __asm__ volatile("mrs %0, icc_pmr_el1" : "=r"(pmr));
            __asm__ volatile("mrs %0, daif" : "=r"(daif));
            __asm__ volatile("msr daifset, #2" ::: "memory");
            __asm__ volatile("msr icc_pmr_el1, %0" :: "r"(UINT64_C(0)) : "memory");
            __asm__ volatile("isb" ::: "memory");
        }
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(begin));
        ScmReply reply = scmSipCall(function, arginfo, arg0, arg1, arg2);
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(end));
        if (maskFirst) {
            __asm__ volatile("msr icc_pmr_el1, %0" :: "r"(pmr) : "memory");
            __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
            __asm__ volatile("isb" ::: "memory");
        }
        printf("PICASSO_GPU zap: call mask=%d a0=%08x a1=%08x ticks=%llu\n",
            int(maskFirst), reply.a0, reply.a1,
            static_cast<unsigned long long>(end - begin));
        for (unsigned attempt = 1; reply.a0 == 1u && attempt < 64; ++attempt) {
            printf("PICASSO_GPU zap: interrupted session=%08x/%08x attempt=%u\n",
                uint32_t(reply.a6), uint32_t(reply.a6 >> 32), attempt);
            if (attempt == 1)
                dumpGic();
            reply = scmSipCall(1u, arginfo, arg0, arg1, arg2, reply.a6);
        }
        return reply;
    };
    /* wf119: dump the TZ log from another core while this thread sits in EL3. */
    {
        uint32_t tzlog = 0;
        if (PE_parse_boot_argn("sheng-gpu-tzlog", &tzlog, sizeof tzlog) && tzlog) {
            thread_t thread = nullptr;
            if (kernel_thread_start(tzLogThread, nullptr, &thread) == KERN_SUCCESS)
                thread_deallocate(thread);
            else
                printf("PICASSO_GPU tzlog: thread failed\n");
        }
    }
    /* wf125: independent liveness tick, no hardware touched. */
    {
        uint32_t heartbeat = 0;
        if (PE_parse_boot_argn("sheng-gpu-heartbeat", &heartbeat,
                sizeof heartbeat) && heartbeat) {
            thread_t thread = nullptr;
            if (kernel_thread_start(heartbeatThread, nullptr, &thread) == KERN_SUCCESS)
                thread_deallocate(thread);
            else
                printf("PICASSO_GPU beat: thread failed\n");
        }
    }
    /* wf114: the full PAS session with the metadata at the 4K-aligned carveout
     * address (reserved_pil 0x8bc17400 + 0xC00, inside the 0x3e8c00-byte
     * region). wf112 measured the first INIT returning QCOM_SCM_INTERRUPTED
     * here; with the session-id retry in place, this separates "TZ stalls on the
     * AP's DMA memory/XPU" (the carveout completes) from "TZ stalls in the
     * authentication itself" (it hangs here too). */
    constexpr uint64_t kAltMetadataPhysical = UINT64_C(0x8bc18000);
    IOMemoryDescriptor *altDescriptor = IOMemoryDescriptor::withPhysicalAddress(
        kAltMetadataPhysical, (metadataBytes + 0xfffu) & ~UINT64_C(0xfff),
        kIODirectionInOut);
    IOMemoryMap *altMap = altDescriptor ? altDescriptor->createMappingInTask(
        kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (altDescriptor) altDescriptor->release();
    ScmReply init{};
    bool ok = false;
    if (altMap) {
        uint8_t *alt = reinterpret_cast<uint8_t *>(
            static_cast<uintptr_t>(altMap->getAddress()));
        for (size_t index = 0; index < metadataBytes; ++index)
            alt[index] = mdt[index];
        __asm__ volatile("dsb sy" ::: "memory");
        printf("PICASSO_GPU zap: calling pas init at carveout phys=%08x\n",
            uint32_t(kAltMetadataPhysical));
        init = pasCall(kScmSipStd | (kPasSvc << 8) | kPasInitImageCmd,
            kArgInfoInitImage, kGpuPasId, kAltMetadataPhysical, 0, true);
        printf("PICASSO_GPU zap: pas init a0=%08x a1=%08x a2=%08x a3=%08x\n",
            init.a0, init.a1, init.a2, init.a3);
        ok = init.a0 == 0 && init.a1 == 0;
    }
    ScmReply setup{};
    ScmReply auth{};
    if (ok) {
        printf("PICASSO_GPU zap: calling pas mem-setup region=%08x span=%x\n",
            uint32_t(regionPhys), uint32_t(regionBytes));
        setup = pasCall(kScmSipStd | (kPasSvc << 8) | kPasMemSetupCmd,
            kArgInfoMemSetup, kGpuPasId, regionPhys, regionBytes, true);
        printf("PICASSO_GPU zap: pas mem-setup a0=%08x a1=%08x\n",
            setup.a0, setup.a1);
        ok = setup.a0 == 0 && setup.a1 == 0;
    }
    if (ok) {
        printf("PICASSO_GPU zap: calling pas auth-and-reset\n");
        auth = pasCall(kScmSipStd | (kPasSvc << 8) | kPasAuthAndResetCmd,
            kArgInfoAuth, kGpuPasId, 0, 0, true);
        printf("PICASSO_GPU zap: pas auth-and-reset a0=%08x a1=%08x\n",
            auth.a0, auth.a1);
        ok = auth.a0 == 0 && auth.a1 == 0;
    }

    /* wf125: the vendor never returns the relocatable region to the allocator
     * — pil_alloc_region() caches priv->region and only pil_release_region() /
     * pil_free_memory() free it on error or shutdown. PAS_MEM_SETUP hands TZ
     * this physical range for the auth read, so it is the one kernel allocation
     * TZ knows about. wf124 measured the consequence of freeing it: PID1 blocks
     * in a dyld-cache pread the moment the page cache reuses those pages, and
     * wf126 (same kernel, region retained) hashes all 64 caches and reaches
     * launchd's "Early boot complete". Retention is therefore the default;
     * sheng-gpu-keep-region=0 restores the freeing path for comparison. */
    uint32_t keepRegion = 1;
    const bool retainRegion = !PE_parse_boot_argn("sheng-gpu-keep-region",
        &keepRegion, static_cast<int>(sizeof keepRegion)) || keepRegion;
    if (retainRegion) {
        printf("PICASSO_GPU zap: region retained phys=%08x span=%x\n",
            uint32_t(regionPhys), uint32_t(regionBytes));
    } else {
        regionMap->release();
        regionBuffer->complete();
        regionBuffer->release();
    }
    if (altMap) altMap->release();
    metadataMap->release();
    metadata->complete();
    metadata->release();
    printf("PICASSO_GPU zap: loaded=%d\n", int(ok));
    return ok;
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
    info.gmem_base = 0;                      /* msm MSM_PARAM_GMEM_BASE: 0 from ADRENO_6XX_GEN3 on (no UCHE window; TILE6_2 is an offset) */
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
    /* wf130: CP_ALWAYS_ON_COUNTER is the userspace timestamp mainline exposes
     * (a6xx_get_timestamp reads it through gpu_read64); without a bound timer
     * the client's GetTimestamp answers Unsupported. Attach once, from the GPU
     * window this driver already maps. */
    static NativeAdrenoTimer gpuTimer;
    static bool gpuTimerAttached = false;
    if (!gpuTimerAttached) {
        NativeAdrenoTimeIo timerIo = { nullptr, gpuTimerRead32 };
        if (gpuTimer.attach(&timerIo, NativeAdrenoTimeCp) == kIOReturnSuccess) {
            uint64_t ticks = 0;
            gpuTimerAttached = gpuTimer.read(&ticks) == kIOReturnSuccess;
            printf("PICASSO_GPU cp: gpu timer attached=%d ticks=%llu\n",
                int(gpuTimerAttached),
                static_cast<unsigned long long>(ticks));
        }
    }
    const IOReturn published = native_adreno_publish(vm, &gSubmit, ring, &clock,
        gpuTimerAttached ? &gpuTimer : nullptr, &info, ias, 40);
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
    /* wf128: the first two samples land before the render server exists (backboardd opens the
     * accelerator after the dyld-cache phase). This third sample, three minutes in, shows
     * whether client submissions keep arriving instead of stopping after the first burst. */
    IOSleep(120000);
    printf("PICASSO_GPU submits final: submissions=%u ibs=%u max=%u many=%u\n",
        native_adreno_submit_ibs[0], native_adreno_submit_ibs[1],
        native_adreno_submit_ibs[2], native_adreno_submit_ibs[3]);
    printf("PICASSO_GPU metal final: opens=%u foreign=%u report=%s report2=%s\n",
        native_adreno_metal_opens(), native_adreno_metal_foreign_opens(),
        native_adreno_metal_report(), native_adreno_metal_report2());
    /* wf133: the UI comes up minutes after the third sample (wf132 saw the lock screen with the
     * +180 s counter still at the five setup submissions), so keep reporting: whether the
     * compositor's later frames are GPU work shows up here as the counter growing. */
    for (unsigned round = 1;; ++round) {
        IOSleep(30000);
        printf("PICASSO_GPU submits live %u: submissions=%u ibs=%u max=%u many=%u "
               "opens=%u report=%s\n", round,
            native_adreno_submit_ibs[0], native_adreno_submit_ibs[1],
            native_adreno_submit_ibs[2], native_adreno_submit_ibs[3],
            native_adreno_metal_opens(), native_adreno_metal_report());
    }
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
    native_adreno_dram_read_words = dramReadWords;
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
    /* mainline hw_init(): load the zap through TZ, then switch the CP out of
     * secure mode. The GPU powers on in secure mode and there "can only render
     * to protected buffers" (freedreno zap documentation) — the measured
     * signature of every submit so far. wf100 sent CP_SET_SECURE_MODE(0)
     * without a zap and the SQE answered CP_OPCODE_ERROR, so the packet is only
     * sent once the zap has authenticated. sheng-gpu-zap=0 keeps the old path
     * for a controlled comparison. */
    uint32_t zapEnabled = 1;
    if (PE_parse_boot_argn("sheng-gpu-zap", &zapEnabled,
            static_cast<int>(sizeof zapEnabled)) && !zapEnabled) {
        printf("PICASSO_GPU zap: disabled by boot arg\n");
    } else if (!loadZap()) {
        printf("PICASSO_GPU zap: not loaded; keeping secure mode\n");
    } else {
        const uint32_t trustBefore = gpuRead(A620_RBBM_SECVID_TRUST_CNTL);
        const bool left = leaveSecureMode();
        const uint32_t trustAfter = gpuRead(A620_RBBM_SECVID_TRUST_CNTL);
        printf("PICASSO_GPU cp: secure mode left=%d trust %08x -> %08x\n",
            int(left), trustBefore, trustAfter);
    }
    /* wf125: the CP was started in secure mode and its RBBM interrupt sources
     * are enabled by hw_init (RBBM_INT_0_MASK). After the zap the GPU is the
     * only block whose state changed, and an asserted line this kernel has no
     * handler for would freeze every CPU that takes it. sheng-gpu-mask-irqs=1
     * records what is pending at the secure-mode boundary and masks the block. */
    uint32_t maskIrqs = 0;
    if (PE_parse_boot_argn("sheng-gpu-mask-irqs", &maskIrqs,
            static_cast<int>(sizeof maskIrqs)) && maskIrqs) {
        const uint32_t irqStatus = gpuRead(A620_RBBM_INT_0_STATUS);
        gpuWrite(A620_RBBM_INT_0_STATUS, irqStatus);
        gpuWrite(A620_RBBM_INT_0_MASK, 0);
        printf("PICASSO_GPU cp: irqs masked status=%08x mask=%08x\n",
            irqStatus, gpuRead(A620_RBBM_INT_0_MASK));
    }
    uint32_t fence = 0;
    if (!submitNoop(&fence)) return -1;
    if (!waitFence(fence, kFenceTimeoutUs, true)) return -1;

    /* The GPU is initialized: a6xx_gmu_clear_oob(GPU_SET), the last step of
     * mainline hw_init. */
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
