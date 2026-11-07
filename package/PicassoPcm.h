// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_PCM_H
#define PICASSO_PCM_H
#include <stdint.h>
// Dedicated SID1401 pool. MAP ownership is separate from GLINK and ASM stream.
// No RUN/WRITE or audio-output claim. Retain memory even after transport loss.
struct PicassoPcm {
    void (*prepare)(PicassoPcm &);
    int (*check)(PicassoPcm &);
    uint32_t events,phase,token,wait,status,opcode,handle,statuses[2];
    uint32_t sid,bytes,slot,bank,asid,fsr,farLow,farHigh,syndrome;
    uint64_t address,physical;
    bool attempted,prepared,sent,pending,responded,mapped,unmapped,uncertain,done,complete,retained;
    int error,mapError,dmaError;
};
extern "C" void picasso_pcm_device_prepare(PicassoPcm &);
#endif
