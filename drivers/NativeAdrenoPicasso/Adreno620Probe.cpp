/* SPDX-License-Identifier: MIT */
/* Picasso GPU probe kernel adapter. The stage machine lives in
 * Adreno620ProbeCore.cpp so it can be exercised on the host; this file owns the
 * real MMIO mappings, the read-only identity readback, the firmware checks and
 * the weak hooks that the main session implements when its GMU kit or GPU
 * context is available. Nothing here powers GX, loads firmware or submits work
 * unless the corresponding hook exists; a missing hook stops the stage with a
 * diagnostic instead of reporting success. */
#include "Adreno620Probe.h"
#include "Adreno620Board.h"
#include "Adreno620Firmware.h"
#include "Adreno620ProbeCore.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <libkern/c++/OSSharedPtr.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
volatile uint32_t native_adreno_stage_req;
volatile uint32_t native_adreno_stage;
volatile uint32_t native_adreno_step;
volatile int native_adreno_error;
volatile uint32_t native_adreno_identity[8];
volatile uint32_t native_adreno_identity_count;
volatile uint32_t native_adreno_gx_powered;
volatile uint32_t native_adreno_smmu_id[3];
volatile uint32_t native_adreno_smmu_banks;
volatile uint32_t native_adreno_smmu_sids;
volatile uint32_t native_adreno_fw_crc;
volatile uint32_t native_adreno_fw_bytes;
volatile uint32_t native_adreno_gmu_blocks;
volatile uint32_t native_adreno_gmu_bytes;
volatile uint32_t native_adreno_sqe_version;
volatile uint32_t native_adreno_sqe_security;
volatile uint32_t native_adreno_zap_segments;
volatile uint32_t native_adreno_fence;
volatile uint32_t native_adreno_ticks;
volatile uint32_t native_adreno_done;
}

/* Optional integration hooks. Each is declared weak, so a build without the
 * main session's GMU kit, GPU context or userland stack links unchanged and
 * the probe stops at the first stage whose hook is absent. The exact contract:
 *   smmu_attach  program the context banks in the plan and attach the exact
 *                SIDs; return 0 only after the routes read back.
 *   gmu_boot     upload the validated GMU image, start the CM3 and leave it
 *                running; no HFI exchange yet.
 *   hfi          run the FW_VERSION/power/startup handshake and return 0 after
 *                the final ACK.
 *   cp_start     attach the CP ring, program the ring registers and start the
 *                SQE from the validated microcode.
 *   cp_submit    submit one no-op IB and publish its fence.
 *   cp_wait      bounded wait for that fence; nonzero is a timeout/fault.
 *   draw         first draw/composite; exists only with NativeMetal/Turnip/IR3.
 */
extern "C" {
int native_adreno620_smmu_attach(const adreno620::ProbeSmmuPlan *plan)
    __attribute__((weak_import));
int native_adreno620_gmu_boot(const uint8_t *gmu, size_t gmu_bytes,
    const uint8_t *sqe, size_t sqe_bytes) __attribute__((weak_import));
int native_adreno620_hfi_handshake(void) __attribute__((weak_import));
int native_adreno620_cp_start(void) __attribute__((weak_import));
int native_adreno620_cp_submit(uint32_t *fence) __attribute__((weak_import));
int native_adreno620_cp_wait(uint32_t fence) __attribute__((weak_import));
int native_adreno620_draw(void) __attribute__((weak_import));
}

namespace {

using adreno620::Clock;
using adreno620::ClockController;
using adreno620::ClockId;
using adreno620::FirmwareFile;
using adreno620::Gdsc;
using adreno620::Probe;
using adreno620::ProbeOps;
using adreno620::ProbeSmmuPlan;
using adreno620::WindowId;

struct Mapping {
    OSSharedPtr<IOMemoryMap> keep;
    volatile uint8_t *base;
    size_t bytes;
};

struct ProbeIo {
    Mapping window[adreno620::WindowCount];
    uint32_t identity[8];
    size_t identity_count;
    uint32_t gx_powered;
    uint32_t smmu_id[3];
    uint32_t gmu_blocks;
    uint32_t gmu_bytes;
    uint32_t sqe_version;
    uint32_t sqe_security;
    uint32_t zap_segments;
};

ProbeIo *gIo;
Probe *gProbe;

/* Firmware is supplied by the integrator; no blob lives in this tree. The
 * pointers are copied under a full barrier and read only after the probe
 * thread starts, which orders them. */
const uint8_t *gFirmwareGmu;
size_t gFirmwareGmuBytes;
const uint8_t *gFirmwareSqe;
size_t gFirmwareSqeBytes;
const uint8_t *gFirmwareZapMdt;
size_t gFirmwareZapMdtBytes;
const uint8_t *gFirmwareZapSegment[3];
size_t gFirmwareZapSegmentBytes[3];

uint32_t read32(const Mapping &mapping, size_t offset)
{
    if (!mapping.base || offset + sizeof(uint32_t) > mapping.bytes) return 0;
    return *reinterpret_cast<volatile uint32_t *>(mapping.base + offset);
}

void write32(const Mapping &mapping, size_t offset, uint32_t value)
{
    if (!mapping.base || offset + sizeof(uint32_t) > mapping.bytes) return;
    *reinterpret_cast<volatile uint32_t *>(mapping.base + offset) = value;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

uint32_t readRegister(const Mapping &mapping, uint32_t dword)
{
    return read32(mapping, size_t(dword) * sizeof(uint32_t));
}

bool mapWindow(WindowId id, Mapping &mapping)
{
    const adreno620::Window &window = adreno620::window(id);
    if (!window.base || !window.bytes) return false;
    IODeviceMemory *memory = IODeviceMemory::withRange(window.base,
        static_cast<IOByteCount>(window.bytes));
    if (!memory) return false;
    mapping.keep = OSSharedPtr<IOMemoryMap>(
        memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    if (!mapping.keep) return false;
    mapping.base = reinterpret_cast<volatile uint8_t *>(
        mapping.keep->getVirtualAddress());
    mapping.bytes = static_cast<size_t>(window.bytes);
    return mapping.base != nullptr;
}

Mapping *ensureWindow(WindowId id)
{
    if (!gIo) return nullptr;
    Mapping &mapping = gIo->window[id];
    if (mapping.base && mapping.bytes) return &mapping;
    return mapWindow(id, mapping) ? &mapping : nullptr;
}

bool waitBits(const Mapping &mapping, size_t offset, uint32_t mask, bool set,
    unsigned attempts, unsigned delay_us)
{
    if (!mapping.base || !attempts || attempts > 10000) return false;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        const uint32_t value = read32(mapping, offset);
        if (((value & mask) != 0) == set) return true;
        if (attempt + 1 < attempts) IODelay(delay_us);
    }
    return false;
}

int opClocks(void *)
{
    Mapping *gpucc = ensureWindow(adreno620::WindowGpucc);
    Mapping *gcc = ensureWindow(adreno620::WindowGcc);
    if (!gpucc || !gcc) {
        printf("PICASSO_GPU clocks: GPUCC/GCC window did not map\n");
        return -1;
    }
    const Gdsc &cx = adreno620::cxGdsc();
    const uint32_t gdscr = read32(*gpucc, cx.control_offset);
    if (gdscr & 1u) write32(*gpucc, cx.control_offset, gdscr & ~1u);
    const unsigned attempts = cx.timeout_us / 100u + 1u;
    if (!waitBits(*gpucc, cx.status_offset, 1u << 31, true, attempts, 100)) {
        printf("PICASSO_GPU clocks: CX GDSC did not assert gdscr=%08x status=%08x\n",
            read32(*gpucc, cx.control_offset), read32(*gpucc, cx.status_offset));
        return -1;
    }
    for (unsigned id = 0; id < adreno620::ClockCount; ++id) {
        const Clock &clock = adreno620::clock(static_cast<ClockId>(id));
        if (clock.cbcr_offset == adreno620::NoOffset) {
            printf("PICASSO_GPU clocks: %s has no recorded CBCR; hardware vote required\n",
                clock.name);
            continue;
        }
        Mapping &controller = clock.controller == ClockController::Gpucc ?
            *gpucc : *gcc;
        const uint32_t value = read32(controller, clock.cbcr_offset);
        if (!(value & 1u)) write32(controller, clock.cbcr_offset, value | 1u);
        if (!waitBits(controller, clock.cbcr_offset, 1u << 31, false, 20, 100)) {
            printf("PICASSO_GPU clocks: %s did not run cbcr=%08x\n", clock.name,
                read32(controller, clock.cbcr_offset));
            return -1;
        }
    }
    printf("PICASSO_GPU clocks: cx_gdscr=%08x cx_status=%08x\n",
        read32(*gpucc, cx.control_offset), read32(*gpucc, cx.status_offset));
    /* GMU clock tree snapshot (gpucc-lito.c): the gmu_clk_src RCG2 CMD/CFG/M/N/D words, its cx_gmu branch CBCR and
     * GPU_CC_PLL1's user control word. The GMU cold start needs 200 MHz (GPLL0_OUT_MAIN_DIV / 1.5); this row tells
     * whether XBL already left that source programmed. Read-only. */
    printf("PICASSO_GPU clocks: gmuclk cmd=%08x cfg=%08x m=%08x n=%08x d=%08x branch=%08x pll1=%08x\n",
        read32(*gpucc, 0x1120), read32(*gpucc, 0x1124), read32(*gpucc, 0x1128),
        read32(*gpucc, 0x112c), read32(*gpucc, 0x1130),
        read32(*gpucc, adreno620::clock(adreno620::ClockGpuccCxGmu).cbcr_offset),
        read32(*gpucc, 0x100));
    return 0;
}

int opIdentity(void *, uint32_t *words, size_t capacity, size_t *count)
{
    Mapping *gpucc = ensureWindow(adreno620::WindowGpucc);
    Mapping *gpu = ensureWindow(adreno620::WindowGpu);
    if (!gpucc || !gpu || !gIo || !words || !count || capacity < 8) return -1;
    const Gdsc &cx = adreno620::cxGdsc();
    const Gdsc &gx = adreno620::gxGdsc();
    gIo->identity[0] = read32(*gpucc, cx.control_offset);
    gIo->identity[1] = read32(*gpucc, cx.status_offset);
    gIo->identity[2] = read32(*gpucc, gx.control_offset);
    gIo->identity[3] = read32(*gpucc, gx.status_offset);
    gIo->identity[4] = read32(*gpucc,
        adreno620::clock(adreno620::ClockGpuccAhb).cbcr_offset);
    gIo->identity[5] = read32(*gpucc,
        adreno620::clock(adreno620::ClockGpuccCxGmu).cbcr_offset);
    gIo->identity[6] = read32(*gpucc,
        adreno620::clock(adreno620::ClockGpuccSmmuVote).cbcr_offset);
    gIo->gx_powered = (gIo->identity[3] & (1u << 31)) ? 1u : 0u;
    gIo->identity[7] = 0;
    if (gIo->gx_powered) {
        /* RBBM_STATUS and VBIF_VERSION are read only while GX reports powered;
         * a read of a gated core can stall the interconnect. The chip id is not
         * an MMIO value on A6xx (downstream reads qcom,chipid), so this is a
         * health/identity readback, not an ID match. */
        gIo->identity[7] = readRegister(*gpu, 0x210);
        printf("PICASSO_GPU identity: rbbm_status=%08x rbbm_status3=%08x vbif_version=%08x "
               "cp_hw_fault=%08x\n",
            readRegister(*gpu, 0x210), readRegister(*gpu, 0x213),
            readRegister(*gpu, 0x3000), readRegister(*gpu, 0x821));
    } else {
        printf("PICASSO_GPU identity: GX is not powered; GPU register reads skipped\n");
    }
    gIo->identity_count = 8;
    for (size_t index = 0; index < gIo->identity_count; ++index)
        words[index] = gIo->identity[index];
    *count = gIo->identity_count;
    printf("PICASSO_GPU identity: chip_id=%08x (DT) catalog=%08x cx=%08x/%08x gx=%08x/%08x\n",
        adreno620::ChipId, adreno620::MainlineCatalogChipId, gIo->identity[0],
        gIo->identity[1], gIo->identity[2], gIo->identity[3]);
    return 0;
}

int opSmmu(void *, const ProbeSmmuPlan &plan)
{
    Mapping *smmu = ensureWindow(adreno620::WindowSmmu);
    if (!smmu || !gIo) return -1;
    gIo->smmu_id[0] = read32(*smmu, 0x20);   /* IDR0 */
    gIo->smmu_id[1] = read32(*smmu, 0x24);   /* IDR1 */
    gIo->smmu_id[2] = read32(*smmu, 0x28);   /* IDR2 */
    const uint32_t scr0 = read32(*smmu, 0x00);
    const uint32_t gfsr = read32(*smmu, 0x48);
    const uint32_t banks = gIo->smmu_id[1] & 0xffu;
    const uint32_t s2banks = (gIo->smmu_id[1] >> 16) & 0xffu;
    printf("PICASSO_GPU smmu: idr0=%08x idr1=%08x idr2=%08x scr0=%08x gfsr=%08x "
           "banks=%u s2=%u\n",
        gIo->smmu_id[0], gIo->smmu_id[1], gIo->smmu_id[2], scr0, gfsr, banks,
        s2banks);
    if (!banks || banks > 64) {
        printf("PICASSO_GPU smmu: unusable context-bank count\n");
        return -1;
    }
    for (size_t index = 0; index < plan.banks; ++index) {
        const adreno620::ProbeSmmuBank &bank = plan.bank[index];
        if (!bank.bringup) continue;
        if (bank.bank < s2banks || bank.bank >= banks) {
            printf("PICASSO_GPU smmu: bank %u outside [%u,%u)\n", bank.bank,
                s2banks, banks);
            return -1;
        }
    }
    if (!native_adreno620_smmu_attach) {
        printf("PICASSO_GPU smmu: attach hook absent; plan published, nothing programmed\n");
        return -1;
    }
    const int attached = native_adreno620_smmu_attach(&plan);
    if (attached == adreno620::ProbeHookAbsent) {
        printf("PICASSO_GPU smmu: attach hook absent; plan published, nothing programmed\n");
        return -1;
    }
    printf("PICASSO_GPU smmu: attach hook returned %d\n", attached);
    return attached;
}

int opGmuFirmware(void *, uint32_t *crc32, uint32_t *bytes)
{
    if (!gIo || !crc32 || !bytes) return -1;
    if (!gFirmwareGmu || !gFirmwareGmuBytes || !gFirmwareSqe ||
        !gFirmwareSqeBytes) {
        printf("PICASSO_GPU firmware: GMU/SQE blobs were not staged\n");
        return -1;
    }
    const FirmwareFile &expectedGmu = adreno620::gmu();
    if (!Adreno620FirmwareCheck(gFirmwareGmu, gFirmwareGmuBytes,
        expectedGmu.bytes, expectedGmu.crc32)) {
        printf("PICASSO_GPU firmware: %s size/CRC mismatch\n", expectedGmu.name);
        return -1;
    }
    const adreno620::GmuLayout &layout = adreno620::gmuLayout();
    const Adreno620GmuRegions regions = {
        layout.itcm_base, layout.itcm_bytes, layout.dtcm_base, layout.dtcm_bytes,
        layout.icache_iova, layout.icache_bytes, layout.dummy_iova,
        layout.dummy_bytes,
    };
    Adreno620GmuImage gmu;
    const Adreno620FwStatus gmuStatus = Adreno620GmuParse(gFirmwareGmu,
        gFirmwareGmuBytes, &regions, &gmu);
    if (gmuStatus != Adreno620FwOk) {
        printf("PICASSO_GPU firmware: GMU container refused status=%d\n", gmuStatus);
        return -1;
    }
    gIo->gmu_blocks = static_cast<uint32_t>(gmu.blocks);
    gIo->gmu_bytes = static_cast<uint32_t>(gmu.data_bytes);

    const FirmwareFile *expectedSqe = nullptr;
    uint32_t minimumVersion = 0;
    if (Adreno620FirmwareCheck(gFirmwareSqe, gFirmwareSqeBytes,
        adreno620::sqe().bytes, adreno620::sqe().crc32)) {
        expectedSqe = &adreno620::sqe();
        minimumVersion = 0x190u;   /* a6xx_gpu.c for a630_sqe.fw */
    } else if (adreno620::sqeAlternate() &&
        Adreno620FirmwareCheck(gFirmwareSqe, gFirmwareSqeBytes,
            adreno620::sqeAlternate()->bytes,
            adreno620::sqeAlternate()->crc32)) {
        expectedSqe = adreno620::sqeAlternate();
        /* wf101: the device's own a650_sqe build is 0x087; mainline's 0x095 floor
         * is a security-fix floor for a650 phones, not a CP-compatibility one.
         * The image is pinned by size/CRC/SHA-256 above, and the boot args already
         * carry sheng-gpu-allow-old-sqe=1 for this bring-up measurement. */
        minimumVersion = 0x080u;
    }
    if (!expectedSqe) {
        printf("PICASSO_GPU firmware: SQE image is neither pinned a630 nor a650\n");
        return -1;
    }
    Adreno620SqeImage sqe;
    const Adreno620FwStatus sqeStatus = Adreno620SqeParse(gFirmwareSqe,
        gFirmwareSqeBytes, minimumVersion, &sqe);
    if (sqeStatus != Adreno620FwOk) {
        printf("PICASSO_GPU firmware: SQE container refused status=%d\n", sqeStatus);
        return -1;
    }
    gIo->sqe_version = sqe.version;
    gIo->sqe_security = sqe.security_ok ? 1u : 0u;
    if (!sqe.security_ok) {
        uint32_t allow = 0;
        const bool overridden = PE_parse_boot_argn("sheng-gpu-allow-old-sqe",
            &allow, sizeof allow) && allow;
        printf("PICASSO_GPU firmware: %s version 0x%x is below the mainline floor "
               "0x%x\n", expectedSqe->name, sqe.version, minimumVersion);
        if (!overridden) {
            printf("PICASSO_GPU firmware: refusing old SQE; sheng-gpu-allow-old-sqe=1 "
                   "overrides\n");
            return -1;
        }
    }

    /* The ZAP is authenticated by PIL/TrustZone, not by this kernel. Validate
     * the container when the integrator staged it; its absence blocks secure
     * contexts, not the GMU non-secure bring-up. */
    if (gFirmwareZapMdt && gFirmwareZapMdtBytes && gFirmwareZapSegment[0] &&
        gFirmwareZapSegment[1] && gFirmwareZapSegment[2]) {
        if (!Adreno620FirmwareCheck(gFirmwareZapMdt, gFirmwareZapMdtBytes,
            adreno620::zapMdt().bytes, adreno620::zapMdt().crc32)) {
            printf("PICASSO_GPU firmware: a620_zap.mdt size/CRC mismatch\n");
            return -1;
        }
        for (unsigned index = 0; index < 3; ++index) {
            const FirmwareFile &file = adreno620::zapSegment(index);
            if (!Adreno620FirmwareCheck(gFirmwareZapSegment[index],
                gFirmwareZapSegmentBytes[index], file.bytes, file.crc32)) {
                printf("PICASSO_GPU firmware: %s size/CRC mismatch\n", file.name);
                return -1;
            }
        }
        Adreno620MdtImage zap;
        const Adreno620FwStatus zapStatus = Adreno620MdtParse(gFirmwareZapMdt,
            gFirmwareZapMdtBytes, gFirmwareZapSegment,
            gFirmwareZapSegmentBytes, 3, &zap);
        if (zapStatus != Adreno620FwOk) {
            printf("PICASSO_GPU firmware: ZAP container refused status=%d\n",
                zapStatus);
            return -1;
        }
        gIo->zap_segments = static_cast<uint32_t>(zap.segments);
        printf("PICASSO_GPU firmware: ZAP entry=%08x segments=%lu loaded=%llu\n",
            zap.entry, static_cast<unsigned long>(zap.segments),
            static_cast<unsigned long long>(zap.loaded_bytes));
    } else {
        printf("PICASSO_GPU firmware: ZAP not staged; secure context unavailable\n");
    }

    *crc32 = expectedGmu.crc32;
    *bytes = static_cast<uint32_t>(expectedGmu.bytes);
    printf("PICASSO_GPU firmware: gmu blocks=%lu data=%llu sqe=%s version=0x%x "
           "security=%u\n", static_cast<unsigned long>(gmu.blocks),
        static_cast<unsigned long long>(gmu.data_bytes), expectedSqe->name,
        sqe.version, sqe.security_ok);
    return 0;
}

int opGmuBoot(void *)
{
    if (!native_adreno620_gmu_boot) {
        printf("PICASSO_GPU gmu: boot hook absent; the linked stub fabricates "
               "success and is never called\n");
        return -1;
    }
    const int booted = native_adreno620_gmu_boot(gFirmwareGmu, gFirmwareGmuBytes,
        gFirmwareSqe, gFirmwareSqeBytes);
    if (booted == adreno620::ProbeHookAbsent) {
        printf("PICASSO_GPU gmu: boot hook absent; no GMU cold start\n");
        return -1;
    }
    return booted;
}

int opHfi(void *)
{
    if (!native_adreno620_hfi_handshake) {
        printf("PICASSO_GPU hfi: handshake hook absent\n");
        return -1;
    }
    const int started = native_adreno620_hfi_handshake();
    if (started == adreno620::ProbeHookAbsent) {
        printf("PICASSO_GPU hfi: handshake hook absent\n");
        return -1;
    }
    return started;
}

int opCpStart(void *)
{
    if (!native_adreno620_cp_start) {
        printf("PICASSO_GPU cp: start hook absent (GPU context not built)\n");
        return -1;
    }
    const int started = native_adreno620_cp_start();
    if (started == adreno620::ProbeHookAbsent) {
        printf("PICASSO_GPU cp: start hook absent (GPU context not built)\n");
        return -1;
    }
    return started;
}

int opCpSubmit(void *, uint32_t *fence)
{
    if (!fence || !native_adreno620_cp_submit) {
        printf("PICASSO_GPU cp: submit hook absent\n");
        return -1;
    }
    const int submitted = native_adreno620_cp_submit(fence);
    if (submitted == adreno620::ProbeHookAbsent) {
        printf("PICASSO_GPU cp: submit hook absent\n");
        return -1;
    }
    return submitted;
}

int opCpWait(void *, uint32_t fence)
{
    if (!native_adreno620_cp_wait) {
        printf("PICASSO_GPU cp: wait hook absent\n");
        return -1;
    }
    const int result = native_adreno620_cp_wait(fence);
    if (result == adreno620::ProbeHookAbsent) {
        printf("PICASSO_GPU cp: wait hook absent\n");
        return -1;
    }
    printf("PICASSO_GPU cp: fence=%u wait returned %d\n", fence, result);
    return result;
}

int opDraw(void *)
{
    if (!native_adreno620_draw) {
        printf("PICASSO_GPU draw: NativeMetal/metal2vulkan/Turnip/IR3 are not in "
               "this repository\n");
        return -1;
    }
    const int drawn = native_adreno620_draw();
    if (drawn == adreno620::ProbeHookAbsent) {
        printf("PICASSO_GPU draw: NativeMetal/metal2vulkan/Turnip/IR3 are not in "
               "this repository\n");
        return -1;
    }
    return drawn;
}

void opAbort(void *, uint32_t stage)
{
    /* Resources are retained on every outcome, as the rest of the driver does:
     * a boot monitor may still read the published diagnostics and the mapped
     * windows. Powering down here would hide the state that failed. */
    printf("PICASSO_GPU abort: stage=%u retained for inspection\n", stage);
}

void publish()
{
    if (!gProbe) return;
    const adreno620::ProbeDiagnostics &diagnostics = gProbe->diagnostics();
    native_adreno_stage = diagnostics.stage;
    native_adreno_step = diagnostics.step;
    native_adreno_error = diagnostics.error;
    native_adreno_ticks = diagnostics.ticks;
    native_adreno_smmu_banks = diagnostics.smmu_banks;
    native_adreno_smmu_sids = diagnostics.smmu_sids;
    native_adreno_fw_crc = diagnostics.firmware_crc;
    native_adreno_fw_bytes = diagnostics.firmware_bytes;
    native_adreno_fence = diagnostics.fence;
    native_adreno_identity_count =
        static_cast<uint32_t>(diagnostics.identity_count);
    for (unsigned index = 0; index < 8; ++index)
        native_adreno_identity[index] = diagnostics.identity[index];
    native_adreno_done = diagnostics.finished ? 1u : (diagnostics.failed ? 2u : 0u);
    if (gIo) {
        native_adreno_gx_powered = gIo->gx_powered;
        native_adreno_gmu_blocks = gIo->gmu_blocks;
        native_adreno_gmu_bytes = gIo->gmu_bytes;
        native_adreno_sqe_version = gIo->sqe_version;
        native_adreno_sqe_security = gIo->sqe_security;
        native_adreno_zap_segments = gIo->zap_segments;
        for (unsigned index = 0; index < 3; ++index)
            native_adreno_smmu_id[index] = gIo->smmu_id[index];
    }
}

void probeThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "picasso-gpu");
    for (unsigned tick = 0; tick < 8; ++tick) {
        const int result = gProbe->tick();
        publish();
        const adreno620::ProbeDiagnostics &diagnostics = gProbe->diagnostics();
        if (result || diagnostics.finished || diagnostics.failed) break;
        IOSleep(20);
    }
    const adreno620::ProbeDiagnostics &diagnostics = gProbe->diagnostics();
    if (!diagnostics.finished && !diagnostics.failed) {
        /* The state machine did not fail; it simply ran out of ticks. Report a
         * bounded stop, never a success. */
        native_adreno_error = adreno620::ProbeAborted;
        native_adreno_done = 2;
    }
    printf("PICASSO_GPU end stage=%u step=%u error=%d requested=%u ticks=%u "
           "done=%u\n", native_adreno_stage, native_adreno_step,
        native_adreno_error, diagnostics.requested, native_adreno_ticks,
        native_adreno_done);
}

} // namespace

extern "C" void native_adreno_set_firmware(const native_adreno620_firmware *firmware)
{
    if (!firmware) return;
    gFirmwareGmu = firmware->gmu;
    gFirmwareGmuBytes = firmware->gmu_bytes;
    gFirmwareSqe = firmware->sqe;
    gFirmwareSqeBytes = firmware->sqe_bytes;
    gFirmwareZapMdt = firmware->zap_mdt;
    gFirmwareZapMdtBytes = firmware->zap_mdt_bytes;
    for (unsigned index = 0; index < 3; ++index) {
        gFirmwareZapSegment[index] = firmware->zap_segment[index];
        gFirmwareZapSegmentBytes[index] = firmware->zap_segment_bytes[index];
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

extern "C" int native_adreno620_sqe_firmware(const uint8_t **bytes, size_t *length)
{
    if (!bytes || !length || !gFirmwareSqe || !gFirmwareSqeBytes) return -1;
    *bytes = gFirmwareSqe;
    *length = gFirmwareSqeBytes;
    return 0;
}

extern "C" void native_adreno_probe(void)
{
    uint32_t stage = 0;
    if (!PE_parse_boot_argn("sheng-gpu", &stage, sizeof stage) || stage < 1)
        return;
    if (native_adreno_stage_req) return;
    if (stage > 5) stage = 5;
    native_adreno_stage_req = stage;
    if (!gIo) gIo = new ProbeIo();
    if (!gProbe) gProbe = new Probe;
    if (!gIo || !gProbe) {
        native_adreno_error = adreno620::ProbeNoOps;
        native_adreno_done = 2;
        printf("PICASSO_GPU: probe allocation failed\n");
        return;
    }
    ProbeOps ops{};
    ops.clocks = opClocks;
    ops.identity = opIdentity;
    ops.smmu = opSmmu;
    ops.gmuFirmware = opGmuFirmware;
    ops.gmuBoot = opGmuBoot;
    ops.hfi = opHfi;
    ops.cpStart = opCpStart;
    ops.cpSubmit = opCpSubmit;
    ops.cpWait = opCpWait;
    ops.draw = opDraw;
    ops.abort = opAbort;
    const int configured = gProbe->configure(stage, &ops);
    if (configured) {
        native_adreno_error = configured;
        native_adreno_done = 2;
        printf("PICASSO_GPU: configure refused %d\n", configured);
        return;
    }
    publish();
    printf("PICASSO_GPU start requested=%u chip_id=%08x\n", stage,
        adreno620::ChipId);
    thread_t thread = nullptr;
    if (kernel_thread_start(probeThread, nullptr, &thread) == KERN_SUCCESS)
        thread_deallocate(thread);
    else {
        native_adreno_error = adreno620::ProbeAborted;
        native_adreno_done = 2;
        printf("PICASSO_GPU: kernel thread did not start\n");
    }
}
