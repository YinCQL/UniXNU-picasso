/* SPDX-License-Identifier: MIT */
#ifndef ADRENO620_PROBE_CORE_H
#define ADRENO620_PROBE_CORE_H
#include <stddef.h>
#include <stdint.h>
#include "Adreno620Board.h"

/* The picasso GPU probe as a tick-driven state machine. One tick performs at
 * most one stage; the object never loops, sleeps or waits. Every hardware
 * operation is an injected callback that must itself be bounded (the board
 * carries the timeouts). This split is what makes the stage transitions and
 * failure behaviour testable on the host with a fake transport, while the
 * kernel adapter in Adreno620Probe.cpp supplies the real I/O.
 *
 * Stages, selected by sheng-gpu=<n>:
 *   1  CX power/clocks and GPU identity readback
 *   2  SMMU ID read, context-bank plan, stream attachment
 *   3  GMU firmware size/CRC check, upload, boot and HFI handshake
 *   4  CP ring start, no-op IB submit and fence wait
 *   5  first draw/composite, only with the userland stack present
 * A failure stops the sequence, records a negative diagnostic code and calls
 * abort exactly once. It is never retried. The requested stage is the last
 * stage that may run; reaching it marks the probe finished. */
namespace adreno620 {

enum ProbeStage : uint32_t {
    ProbeIdle = 0,
    ProbeClocks = 1,
    ProbeSmmu = 2,
    ProbeGmu = 3,
    ProbeCp = 4,
    ProbeDraw = 5
};

enum ProbeError : int {
    ProbeOk = 0,
    ProbeBadRequest = -1,
    ProbeNoOps = -2,
    ProbeClockFailure = -10,
    ProbeIdentityFailure = -11,
    ProbeSmmuPlanFailure = -20,
    ProbeSmmuFailure = -21,
    ProbeGmuFirmwareFailure = -30,
    ProbeGmuBootFailure = -31,
    ProbeHfiFailure = -32,
    ProbeCpFailure = -40,
    ProbeCpFenceFailure = -41,
    ProbeDrawUnavailable = -50,
    /* A weak "not implemented" hook definition (Adreno620Hooks.cpp). A static kernel link cannot leave the optional
     * hooks undefined, so they are defined weakly and return this; a real implementation overrides them. */
    ProbeHookAbsent = -78,
    ProbeAborted = -90
};

struct ProbeSmmuBank {
    const char *name;
    uint32_t bank;
    uint32_t asid;
    uint16_t mask;
    uint16_t sids[16];
    size_t sid_count;
    bool bringup;
};

struct ProbeSmmuPlan {
    size_t banks;
    ProbeSmmuBank bank[4];
};

struct ProbeOps {
    void *cookie;
    int (*clocks)(void *);
    int (*identity)(void *, uint32_t *words, size_t capacity, size_t *count);
    int (*smmu)(void *, const ProbeSmmuPlan &plan);
    int (*gmuFirmware)(void *, uint32_t *crc32, uint32_t *bytes);
    int (*gmuBoot)(void *);
    int (*hfi)(void *);
    int (*cpStart)(void *);
    int (*cpSubmit)(void *, uint32_t *fence);
    int (*cpWait)(void *, uint32_t fence);
    int (*draw)(void *);
    void (*abort)(void *, uint32_t stage);
};

struct ProbeDiagnostics {
    uint32_t requested;
    uint32_t stage;
    uint32_t step;
    int error;
    uint32_t identity[8];
    size_t identity_count;
    uint32_t firmware_crc;
    uint32_t firmware_bytes;
    uint32_t smmu_banks;
    uint32_t smmu_sids;
    uint32_t fence;
    uint32_t ticks;
    bool finished;
    bool failed;
};

class Probe final {
public:
    Probe() = default;
    Probe(const Probe &) = delete;
    Probe &operator=(const Probe &) = delete;

    /* One configuration per object. requested is 1..5. Missing callbacks are
     * reported by the stage that needs them, so a caller can run stage 1
     * without owning the CP path. */
    int configure(uint32_t requested, const ProbeOps *ops);
    /* Executes the current stage. Returns 0 while the sequence can continue
     * (including after the final stage), or the negative failure code. */
    int tick();

    const ProbeDiagnostics &diagnostics() const { return diagnostics_; }
    const ProbeSmmuPlan &smmuPlan() const { return plan_; }

private:
    int fail(int error);
    int runStage();
    bool buildPlan();

    ProbeOps ops_{};
    ProbeSmmuPlan plan_{};
    ProbeDiagnostics diagnostics_{};
    bool configured_ = false;
};

} // namespace adreno620

#endif /* ADRENO620_PROBE_CORE_H */
