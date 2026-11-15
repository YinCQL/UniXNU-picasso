// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_MIC_SERVICE_H
#define PICASSO_MIC_SERVICE_H
#include <stdint.h>
struct PicassoAprProbe;
extern "C" void picasso_mic_service_configure(PicassoAprProbe &,uint32_t);
extern "C" void picasso_mic_service_tick(PicassoAprProbe &,bool);
#endif
