/* SPDX-License-Identifier: MIT */
#ifndef SHENG_GMU_H
#define SHENG_GMU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ShengGmuWindowIndex {
    ShengGmuCore   = 0,
    ShengGmuRscc   = 1,
    ShengGmuPdc    = 2,
    ShengGmuGpu    = 3,
    ShengGmuCxMisc = 4,
    ShengGmuWindows = 5
};

typedef struct ShengGmuIo {
    void *cookie;
    size_t bytes[ShengGmuWindows];
    int (*read32)(void *cookie, unsigned window, size_t at, uint32_t *out);
    int (*write32)(void *cookie, unsigned window, size_t at, uint32_t val);
    int (*barrier)(void *cookie);
} ShengGmuIo;

typedef struct ShengGmuServices {
    void *cookie;
    int (*sha256)(void *cookie, const void *bytes, size_t length, uint8_t digest[32]);
    int (*now_us)(void *cookie, uint64_t *out);
    int (*wait_us)(void *cookie, unsigned us);
} ShengGmuServices;

typedef struct ShengGmuMemory {
    volatile uint32_t *cpu;
    size_t bytes;
    uint64_t iova;
} ShengGmuMemory;

typedef struct ShengGmuConfig {
    ShengGmuMemory icache;
    ShengGmuMemory dummy;
    ShengGmuMemory debug;
    ShengGmuMemory log;
    ShengGmuMemory hfi;
    uint32_t chip_id;
    const uint8_t *firmware;
    size_t firmware_bytes;
    unsigned poll_budget;
} ShengGmuConfig;

typedef struct ShengGmuDevice {
    void *cookie;
} ShengGmuDevice;

typedef struct ShengGmuStartupLimits {
    unsigned poll_attempts;
    unsigned wait_interval_us;
    unsigned poll_budget;
    unsigned total_timeout_us;
} ShengGmuStartupLimits;

typedef struct ShengGmuStartupResult {
    struct {
        uint32_t firmware_ready_observed;
        uint32_t hfi_ready_observed;
    } load;
    struct {
        uint32_t stage;
    } protocol;
} ShengGmuStartupResult;

typedef enum ShengGmuStatus {
    ShengGmuSuccess = 0,
    ShengGmuFailure = 1,
    ShengGmuTimeout = 2,
    ShengGmuCorrupt = 3
} ShengGmuStatus;

ShengGmuStatus ShengGmuColdStart(
    ShengGmuDevice *device,
    const ShengGmuConfig *config,
    const void *startup,
    const ShengGmuStartupLimits *limits,
    ShengGmuStartupResult *result);

#ifdef __cplusplus
}
#endif

#endif /* SHENG_GMU_H */
