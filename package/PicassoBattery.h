// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PicassoBatteryCore.h"
extern "C" void *picasso_battery_open();
extern "C" void picasso_battery_close(void *);
extern "C" int picasso_battery_read(void *,picasso_battery::Snapshot *);
