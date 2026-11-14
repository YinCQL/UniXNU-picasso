// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
// All board consumers use this one controller-wide transaction lock.
// Call init in thread context before publishing either consumer.
extern "C" bool picasso_spmi_init();
extern "C" int picasso_spmi_read8(volatile uint32_t *, uint8_t, uint8_t &);
extern "C" int picasso_spmi_write8(volatile uint32_t *, uint8_t, uint8_t);
extern "C" int picasso_spmi_quiesce();
