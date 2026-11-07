// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_PCM_H
#define PICASSO_PCM_H
#include <stdint.h>
// Dedicated SID1401 pool. MAP ownership is separate from GLINK and ASM stream.
// Retain memory even after transport loss. Filling is allowed only with no DSP buffer owner.
struct PicassoPcm {
    void (*prepare)(PicassoPcm &);
    int (*check)(PicassoPcm &);
    int (*silence)(PicassoPcm &);
    int (*tone)(PicassoPcm &);
    uint32_t events,phase,token,wait,status,opcode,handle,statuses[2];
    uint32_t sid,bytes,slot,bank,asid,fsr,farLow,farHigh,syndrome;
    uint64_t address,physical;
    bool attempted,prepared,sent,pending,responded,mapped,unmapped,uncertain,done,complete,retained;
    int error,mapError,dmaError;
};
// 8192 stereo frames, 1 kHz triangle, peak 64/32768 (-54 dBFS),
// 5 ms fade at each edge. Integer arithmetic only; table pages are untouched.
inline int picasso_pcm_tone(uint8_t *data,unsigned bytes) {
    if(!data || bytes!=32768)return -22;
    for(unsigned frame=0;frame<8192;++frame) {
        unsigned phase=frame%48;
        int sample=phase<12?int(phase)*64/12:phase<36?(24-int(phase))*64/12:(int(phase)-48)*64/12;
        unsigned edge=frame<240?frame:frame>7951?8191-frame:240;
        sample=sample*int(edge)/240;
        uint16_t value=uint16_t(int16_t(sample));
        for(unsigned channel=0;channel<2;++channel) {
            data[frame*4+channel*2]=uint8_t(value);data[frame*4+channel*2+1]=uint8_t(value>>8);
        }
    }
    return 0;
}
extern "C" void picasso_pcm_device_prepare(PicassoPcm &);
#endif
