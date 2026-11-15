// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_MIC_HARDWARE_H
#define PICASSO_MIC_HARDWARE_H
#include <stdint.h>

// Board route: WCD938x AMIC1 / ADC1, SWR TX port 2 lane 1, DEC0.
// The caller serializes actions on a worker, after genuine AFE clock/resource
// acknowledgements. Stop requires DSP CLOSE and AFE STOP acknowledgements.
enum PicassoMicHardwareAction { PicassoMicPrepare, PicassoMicUnmute, PicassoMicStop, PicassoMicCheck };
enum PicassoMicHardwareStage {
    PicassoMicIdle, PicassoMicRails, PicassoMicPins, PicassoMicClocks,
    PicassoMicBus, PicassoMicEnumerate, PicassoMicCodec, PicassoMicRoute,
    PicassoMicPrepared, PicassoMicRunning, PicassoMicRestoring, PicassoMicStopped
};
enum PicassoMicHardwareRegion {
    PicassoMicTx, PicassoMicSwr, PicassoMicVa, PicassoMicLpi,
    PicassoMicSlew, PicassoMicReset, PicassoMicRegionCount, PicassoMicSlave
};
struct PicassoMicHardwareStatus {
    uint32_t version, stage, failedStage;
    int32_t error, restoreError;
    uint32_t ready, unmuted, rails, dirty, slave, swrVersion, chipId;
    uint32_t lastRegion, lastRegister, lastValue, events, activeBank;
    uint32_t lastSlaveRegister, lastSlaveCommand;
    uint64_t slaveId;
};
struct PicassoMicHardwareIO {
    void *context;
    int (*read)(void *, unsigned region, uint32_t reg, uint32_t *);
    int (*write)(void *, unsigned region, uint32_t reg, uint32_t);
    void (*delay)(void *, unsigned microseconds);
    int (*rails)(void *, unsigned action); // 0 acquire, 1 check, 2 release
    void (*note)(void *, const PicassoMicHardwareStatus *);
};
struct PicassoMicHardwareSaved {
    uint32_t region, reg, mask, value;
};
struct PicassoMicHardwareCore {
    PicassoMicHardwareIO io;
    PicassoMicHardwareStatus status;
    PicassoMicHardwareSaved saved[128];
    unsigned savedCount, writeDepth, commandId;
    bool portsTouched;
};

// The injected core is also used by host fault-injection tests. A nonzero stop
// result leaves the remaining journal intact: clocks must stay voted on.
int picasso_mic_hardware_action(PicassoMicHardwareCore &, unsigned action);
extern "C" int picasso_mic_hardware(unsigned action);
extern "C" void picasso_mic_hardware_snapshot(PicassoMicHardwareStatus *);
#endif
