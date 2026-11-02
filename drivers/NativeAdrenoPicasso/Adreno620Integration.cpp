/* SPDX-License-Identifier: MIT */
/* Strong implementations of the Adreno620Probe integration hooks. Each hook is declared weak-imported by
 * Adreno620Probe.cpp and weak-defined in Adreno620Hooks.cpp; a definition here replaces the weak one, so the probe
 * stops calling it "absent" and runs it. Only the hooks implemented so far are defined here.
 *
 * smmu_attach uses the qualified A740 context machinery unchanged: NativeAdrenoGmuContextBringup builds the GMU's own
 * context (bank, ASID, SMR slot and stream are the board plan's: bank 3 / ASID 4 / slot 0 / SID 5) and
 * NativeAdrenoGpuContextBringupStreams builds the GPU context (bank 0 / ASID 1) with all four exact SIDs of gfx3d_user
 * (0, 1 and the secure aliases 0x400/0x401 of the same stream mask). The CP runs in secure mode until the zap shader
 * switches it out, so those aliases must be routed to the same bank, as the device tree's <&adreno_smmu 0 0x401> does.
 * The SMMU global state is XBL-owned (SCR0 0x00210406, which the stream kit verifies and never rewrites).
 *
 * Both bring-ups allocate and map before any traffic exists (GX is still off in stage 2), so enabling the banks cannot
 * fault a running master. Each context object lives forever and can be brought up once per boot. */
#include "Adreno620Probe.h"
#include "Adreno620ProbeCore.h"
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGpuContext.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <string.h>

extern "C" int native_adreno620_smmu_attach(const adreno620::ProbeSmmuPlan *plan)
{
    if (!plan) return -1;
    const adreno620::ProbeSmmuBank *gmu = nullptr;
    const adreno620::ProbeSmmuBank *gpu = nullptr;
    for (size_t index = 0; index < plan->banks; ++index) {
        const adreno620::ProbeSmmuBank &bank = plan->bank[index];
        if (!bank.bringup) continue;
        if (bank.name && strcmp(bank.name, "gmu_kernel") == 0) gmu = &bank;
        else if (bank.name && strcmp(bank.name, "gfx3d_user") == 0) gpu = &bank;
    }
    if (!gmu || !gpu || !gmu->sid_count || gpu->sid_count < 4) {
        printf("PICASSO_GPU smmu: plan lacks the GMU/GPU bring-up banks (gmu=%u gpu=%u)\n",
               gmu ? gmu->sid_count : 0u, gpu ? gpu->sid_count : 0u);
        return -1;
    }

    /* 0x3da0000/0x10000 is kgsl_smmu reg[0] in picasso.dtb. The mapping and both contexts live for the boot. */
    IODeviceMemory *memory = IODeviceMemory::withRange(0x3da0000, 0x10000);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    if (!map) {
        printf("PICASSO_GPU smmu: cannot map the qsmmu-v500 window\n");
        return -1;
    }
    volatile uint8_t *smmu = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());

    /* The kit requires every target stream slot to be faulting before it publishes its SMR, so that no stale
     * XBL/adreno route can be inherited. XBL leaves S2CR0 as a valid translation (0x3a6a09ff observed), which
     * made the first attempt stop with route_status=10 (Busy). Stop translation first and verify it: S2CR is
     * GR0 + 0xc00 + 4*slot, FAULT = type 2 << 16 (arm-smmu.h ARM_SMMU_S2CR_TYPE), SMR is GR0 + 0x800 + 4*slot. */
    auto readReg = [&](size_t offset) {
        return *reinterpret_cast<volatile uint32_t *>(smmu + offset);
    };
    auto quiesce = [&](unsigned slot) {
        const size_t s2crAt = 0xc00 + 4 * slot;
        const uint32_t smrBefore = readReg(0x800 + 4 * slot);
        const uint32_t s2crBefore = readReg(s2crAt);
        *reinterpret_cast<volatile uint32_t *>(smmu + s2crAt) = 0x00020000u;
        __asm__ volatile("dsb sy" ::: "memory");
        const uint32_t s2crAfter = readReg(s2crAt);
        printf("PICASSO_GPU smmu: quiesce slot=%u smr=%08x s2cr=%08x -> %08x\n",
               slot, smrBefore, s2crBefore, s2crAfter);
        return s2crAfter == 0x00020000u;
    };
    if (!quiesce(0)) {
        printf("PICASSO_GPU smmu: GMU stream slot did not reach FAULT\n");
        return -1;
    }

    static NativeAdrenoGmuContextReport gmuReport;
    static NativeAdrenoGmuContextReport gpuReport;
    gmuReport = {};
    gpuReport = {};

    const uint32_t gmuSid = gmu->sids[0];
    IOReturn status = NativeAdrenoGmuContextBringup(smmu, 0x10000, gmu->bank, gmu->asid,
        0, gmuSid, nullptr, &gmuReport);
    printf("PICASSO_GPU smmu: gmu bank=%u asid=%u slot=0 sid=%u status=%08x step=%u program=%u/%u route=%u/%u "
           "sctlr=%08x cbar=%08x smr=%08x s2cr=%08x\n",
           gmu->bank, gmu->asid, gmuSid, (unsigned)status, gmuReport.step,
           gmuReport.program_status, gmuReport.program_enabled,
           gmuReport.route_status, gmuReport.routes_verified,
           gmuReport.sctlr, gmuReport.cbar, gmuReport.smr, gmuReport.s2cr);
    if (status) return -1;

    for (unsigned slot = 1; slot <= 4; ++slot) {
        if (!quiesce(slot)) {
            printf("PICASSO_GPU smmu: GPU stream slot %u did not reach FAULT\n", slot);
            return -1;
        }
    }
    /* gfx3d_user's iommus mask is 0x401, so its exact SIDs are 0, 1 and the
     * secure aliases 0x400/0x401 that the CP uses before the zap shader makes
     * it non-secure. Attach all four to the one GPU context. */
    const unsigned gpuSlots[4] = {1, 2, 3, 4};
    const uint16_t gpuSids[4] = {gpu->sids[0], gpu->sids[1], gpu->sids[2], gpu->sids[3]};
    status = NativeAdrenoGpuContextBringupStreams(smmu, 0x10000, gpu->bank, gpu->asid,
        gpuSlots, gpuSids, 4, nullptr, &gpuReport);
    printf("PICASSO_GPU smmu: gpu bank=%u asid=%u slots=1,2,3,4 sids=%u,%u,%u,%u status=%08x step=%u "
           "program=%u/%u route=%u/%u reads=%u id1=%08x id2=%08x oas=%u aero=%u sctlr=%08x cbar=%08x "
           "smr=%08x s2cr=%08x\n",
           gpu->bank, gpu->asid, gpuSids[0], gpuSids[1], gpuSids[2], gpuSids[3], (unsigned)status,
           gpuReport.step, gpuReport.program_status, gpuReport.program_enabled,
           gpuReport.route_status, gpuReport.routes_verified, gpuReport.spans,
           gpuReport.id1, gpuReport.id2, gpuReport.output_bits, gpuReport.program_readback_valid,
           gpuReport.sctlr, gpuReport.cbar, gpuReport.smr, gpuReport.s2cr);
    if (status) return -1;

    printf("PICASSO_GPU smmu: both contexts ready (gfx3d_user secure aliases routed)\n");
    return 0;
}
