// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_ADSP_BOOT_H
#define PICASSO_ADSP_BOOT_H
#include <stdint.h>
#include <stddef.h>
#include "PicassoSmp2p.h"
struct PicassoAdspEvents {
    void *context;
    int (*poll)(void *, PicassoSmp2pEvent *);
    int (*snapshot)(void *); // Bounded crash/GLINK observations after startup/failure.
    void (*service)(void *); // Only after PAS AUTH + fresh ready, never preflight/fatal.
};
struct PicassoAdspImage;
extern "C" PicassoAdspImage *picasso_adsp_image_open(int *error);
extern "C" const void *picasso_adsp_image_metadata(PicassoAdspImage *);
extern "C" int picasso_adsp_image_load(PicassoAdspImage *, volatile uint8_t *, size_t);
extern "C" void picasso_adsp_image_close(PicassoAdspImage *);
extern "C" void picasso_adsp_boot(const PicassoAdspEvents *);
extern "C" int picasso_aop_load_state(bool on);
extern "C" int picasso_aop_finish(void);
#endif
