/* SPDX-License-Identifier: MIT */
#ifndef SHENG_GMU_COMMAND_DB_H
#define SHENG_GMU_COMMAND_DB_H

#include <stddef.h>
#include <stdint.h>
#include "AdrenoHfiStartup.h"

#define SHENG_GMU_COMMAND_DB_BASE  0x000c0000ULL
#define SHENG_GMU_COMMAND_DB_BYTES 0x1000
#define SHENG_GMU_VOTES_CHIP_ID    0x00060200U /* Adreno 620 */

typedef enum ShengGmuDbStatus {
    ShengGmuDbSuccess = 0,
    ShengGmuDbFailure = 1,
    ShengGmuDbNotFound = 2
} ShengGmuDbStatus;

typedef struct ShengGmuVoteInputs {
    uint32_t chip_id;
    uint32_t enable_acd;
    uint32_t enable_ifpc;
} ShengGmuVoteInputs;

#ifdef __cplusplus
extern "C" {
#endif

ShengGmuDbStatus ShengGmuCommandDbInputs(
    const uint8_t *db,
    size_t bytes,
    uint32_t version,
    ShengGmuVoteInputs *inputs);

AdrenoHfiStartupStatus ShengGmuBuildStartup(
    const ShengGmuVoteInputs *inputs,
    AdrenoHfiStartupConfig *startup);

#ifdef __cplusplus
}
#endif

#endif /* SHENG_GMU_COMMAND_DB_H */
