/* SPDX-License-Identifier: MIT */
#include "Adreno620ProbeCore.h"

namespace adreno620 {

int Probe::configure(uint32_t requested, const ProbeOps *ops)
{
    if (configured_ || !ops) return ProbeBadRequest;
    if (requested < ProbeClocks || requested > ProbeDraw) return ProbeBadRequest;
    ops_ = *ops;
    plan_ = {};
    diagnostics_ = {};
    diagnostics_.requested = requested;
    diagnostics_.stage = ProbeClocks;
    configured_ = true;
    return ProbeOk;
}

int Probe::fail(int error)
{
    diagnostics_.error = error;
    diagnostics_.failed = true;
    if (ops_.abort) ops_.abort(ops_.cookie, diagnostics_.stage);
    return error;
}

bool Probe::buildPlan()
{
    size_t count = 0;
    const SmmuBank *banks = smmuBanks(&count);
    plan_ = {};
    const size_t slots = sizeof(plan_.bank) / sizeof(plan_.bank[0]);
    if (!banks || !count || count > slots) return false;
    for (size_t index = 0; index < count; ++index) {
        ProbeSmmuBank &bank = plan_.bank[index];
        bank.name = banks[index].name;
        bank.bank = banks[index].bank;
        bank.asid = banks[index].asid;
        bank.mask = banks[index].mask;
        bank.bringup = banks[index].bringup;
        const size_t required = expandStreamMask(banks[index].sid,
            banks[index].mask, bank.sids,
            sizeof(bank.sids) / sizeof(bank.sids[0]));
        if (!required || required > sizeof(bank.sids) / sizeof(bank.sids[0])) {
            plan_ = {};
            return false;
        }
        bank.sid_count = required;
    }
    plan_.banks = count;
    return true;
}

int Probe::runStage()
{
    switch (diagnostics_.stage) {
    case ProbeClocks: {
        diagnostics_.step = 1;
        if (!ops_.clocks) return ProbeClockFailure;
        if (ops_.clocks(ops_.cookie)) return ProbeClockFailure;
        diagnostics_.step = 2;
        if (!ops_.identity) return ProbeIdentityFailure;
        uint32_t words[8] = {};
        size_t count = 0;
        if (ops_.identity(ops_.cookie, words, 8, &count) || !count ||
            count > sizeof(words) / sizeof(words[0]))
            return ProbeIdentityFailure;
        for (size_t index = 0; index < count; ++index)
            diagnostics_.identity[index] = words[index];
        diagnostics_.identity_count = count;
        diagnostics_.step = 3;
        return ProbeOk;
    }
    case ProbeSmmu: {
        diagnostics_.step = 1;
        if (!buildPlan()) return ProbeSmmuPlanFailure;
        uint32_t banks = 0, sids = 0;
        for (size_t index = 0; index < plan_.banks; ++index) {
            if (!plan_.bank[index].bringup) continue;
            ++banks;
            sids += static_cast<uint32_t>(plan_.bank[index].sid_count);
        }
        diagnostics_.smmu_banks = banks;
        diagnostics_.smmu_sids = sids;
        diagnostics_.step = 2;
        if (!ops_.smmu) return ProbeSmmuFailure;
        if (ops_.smmu(ops_.cookie, plan_)) return ProbeSmmuFailure;
        diagnostics_.step = 3;
        return ProbeOk;
    }
    case ProbeGmu: {
        diagnostics_.step = 1;
        if (!ops_.gmuFirmware) return ProbeGmuFirmwareFailure;
        uint32_t crc = 0, bytes = 0;
        if (ops_.gmuFirmware(ops_.cookie, &crc, &bytes))
            return ProbeGmuFirmwareFailure;
        const FirmwareFile &expected = gmu();
        if (static_cast<uint64_t>(bytes) != expected.bytes ||
            crc != expected.crc32)
            return ProbeGmuFirmwareFailure;
        diagnostics_.firmware_crc = crc;
        diagnostics_.firmware_bytes = bytes;
        diagnostics_.step = 2;
        if (!ops_.gmuBoot) return ProbeGmuBootFailure;
        if (ops_.gmuBoot(ops_.cookie)) return ProbeGmuBootFailure;
        diagnostics_.step = 3;
        if (!ops_.hfi) return ProbeHfiFailure;
        if (ops_.hfi(ops_.cookie)) return ProbeHfiFailure;
        diagnostics_.step = 4;
        return ProbeOk;
    }
    case ProbeCp: {
        diagnostics_.step = 1;
        if (!ops_.cpStart) return ProbeCpFailure;
        if (ops_.cpStart(ops_.cookie)) return ProbeCpFailure;
        diagnostics_.step = 2;
        if (!ops_.cpSubmit) return ProbeCpFailure;
        uint32_t fence = 0;
        if (ops_.cpSubmit(ops_.cookie, &fence)) return ProbeCpFailure;
        diagnostics_.fence = fence;
        diagnostics_.step = 3;
        if (!ops_.cpWait) return ProbeCpFenceFailure;
        if (ops_.cpWait(ops_.cookie, fence)) return ProbeCpFenceFailure;
        diagnostics_.step = 4;
        return ProbeOk;
    }
    case ProbeDraw: {
        diagnostics_.step = 1;
        if (!ops_.draw) return ProbeDrawUnavailable;
        if (ops_.draw(ops_.cookie)) return ProbeDrawUnavailable;
        diagnostics_.step = 2;
        return ProbeOk;
    }
    default:
        return ProbeBadRequest;
    }
}

int Probe::tick()
{
    if (!configured_) return ProbeBadRequest;
    if (diagnostics_.failed) return diagnostics_.error;
    if (diagnostics_.finished) return ProbeOk;
    if (diagnostics_.stage < ProbeClocks || diagnostics_.stage > ProbeDraw)
        return fail(ProbeBadRequest);
    ++diagnostics_.ticks;
    const int result = runStage();
    if (result) return fail(result);
    if (diagnostics_.stage >= diagnostics_.requested) {
        diagnostics_.finished = true;
    } else {
        ++diagnostics_.stage;
        diagnostics_.step = 0;
    }
    return ProbeOk;
}

} // namespace adreno620
