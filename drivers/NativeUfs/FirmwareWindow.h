// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

// Read-only handle owned by one firmware transaction. Never a write window.
struct UfsFirmwareWindow {
  uint64_t first, last;
  uint32_t lun;
};
extern "C" int native_ufs_modem_open(UfsFirmwareWindow *, void *, uint32_t);
extern "C" int native_ufs_modem_read(const UfsFirmwareWindow *, uint64_t, void *, uint32_t, void *);
