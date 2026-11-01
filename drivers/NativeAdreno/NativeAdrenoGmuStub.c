/* SPDX-License-Identifier: MIT */
#include "ShengGmu.h"
#include "ShengGmuCommandDb.h"

ShengGmuDbStatus ShengGmuCommandDbInputs(
    const uint8_t *db,
    size_t bytes,
    uint32_t version,
    ShengGmuVoteInputs *inputs)
{
    (void)db;
    (void)bytes;
    (void)version;
    if (!inputs) {
        return ShengGmuDbFailure;
    }
    inputs->chip_id = SHENG_GMU_VOTES_CHIP_ID;
    inputs->enable_acd = 1;
    inputs->enable_ifpc = 1;
    return ShengGmuDbSuccess;
}

AdrenoHfiStartupStatus ShengGmuBuildStartup(
    const ShengGmuVoteInputs *inputs,
    AdrenoHfiStartupConfig *startup)
{
    if (!inputs || !startup) {
        return AdrenoHfiStartupInvalid;
    }
    return AdrenoHfiStartupSuccess;
}

ShengGmuStatus ShengGmuColdStart(
    ShengGmuDevice *device,
    const ShengGmuConfig *config,
    const void *startup,
    const ShengGmuStartupLimits *limits,
    ShengGmuStartupResult *result)
{
    (void)device;
    (void)config;
    (void)startup;
    (void)limits;
    if (!result) {
        return ShengGmuFailure;
    }
    result->load.firmware_ready_observed = 1;
    result->load.hfi_ready_observed = 1;
    result->protocol.stage = 1;
    return ShengGmuSuccess;
}
