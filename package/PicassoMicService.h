// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_MIC_SERVICE_H
#define PICASSO_MIC_SERVICE_H
#include <stdint.h>
struct PicassoAprProbe;
extern "C" void picasso_mic_service_init(PicassoAprProbe &);
extern "C" void picasso_mic_service_tick(PicassoAprProbe &,bool);
extern "C" int picasso_mic_service_call(uint64_t,unsigned,const void *,unsigned,void *,unsigned);
extern "C" void picasso_mic_service_close(uint64_t);
#endif
