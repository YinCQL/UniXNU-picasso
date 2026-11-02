/* SPDX-License-Identifier: MIT */
/* Strong implementations of the Adreno620Probe integration hooks. Each hook is declared weak-imported by
 * Adreno620Probe.cpp and weak-defined in Adreno620Hooks.cpp; a definition here replaces the weak one, so the probe
 * stops calling it "absent" and runs it. Only the hooks implemented so far are defined here.
 *
 * smmu_attach uses the qualified A740 context machinery unchanged: NativeAdrenoGmuContextBringup builds the GMU's own
 * context (bank, ASID, SMR slot and stream are the board plan's: bank 3 / ASID 4 / slot 0 / SID 5) and
 * NativeAdrenoGpuContextBringup builds the GPU context (bank 0 / ASID 1) with the first two exact SIDs of gfx3d_user.
 * The masked SIDs 0x400/0x401 are the secure aliases of the same streams; the non-secure GPU traffic uses 0 and 1, and
 * the kit attaches exact SIDs only, so the first bring-up leaves the secure aliases unrouted. The SMMU global state is
 * XBL-owned (SCR0 0x00210406, which the stream kit verifies and never rewrites).
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
    if (!gmu || !gpu || !gmu->sid_count || gpu->sid_count < 2) {
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

    if (!quiesce(1) || !quiesce(2)) {
        printf("PICASSO_GPU smmu: GPU stream slots did not reach FAULT\n");
        return -1;
    }
    const uint32_t gpuSid0 = gpu->sids[0], gpuSid1 = gpu->sids[1];
    status = NativeAdrenoGpuContextBringup(smmu, 0x10000, gpu->bank, gpu->asid,
        1, gpuSid0, 2, gpuSid1, nullptr, &gpuReport);
    printf("PICASSO_GPU smmu: gpu bank=%u asid=%u slots=1,2 sids=%u,%u status=%08x step=%u program=%u/%u "
           "route=%u/%u reads=%u id1=%08x id2=%08x oas=%u aero=%u sctlr=%08x cbar=%08x smr=%08x s2cr=%08x\n",
           gpu->bank, gpu->asid, gpuSid0, gpuSid1, (unsigned)status, gpuReport.step,
           gpuReport.program_status, gpuReport.program_enabled,
           gpuReport.route_status, gpuReport.routes_verified, gpuReport.spans,
           gpuReport.id1, gpuReport.id2, gpuReport.output_bits, gpuReport.program_readback_valid,
           gpuReport.sctlr, gpuReport.cbar, gpuReport.smr, gpuReport.s2cr);
    if (status) return -1;

    printf("PICASSO_GPU smmu: both contexts ready (secure SIDs 0x400/0x401 left unrouted)\n");
    return 0;
}
