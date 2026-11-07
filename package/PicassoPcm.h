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
    int (*tone)(PicassoPcm &,unsigned,unsigned); // slot, sequence
    uint32_t busyMask; // DMA slots remain owned until WRITE_DONE or CLOSE ACK
    uint32_t events,phase,token,wait,status,opcode,handle,statuses[2];
    uint32_t sid,bytes,slot,bank,asid,fsr,farLow,farHigh,syndrome;
    uint64_t address,physical;
    bool attempted,prepared,sent,pending,responded,mapped,unmapped,uncertain,done,complete,retained;
    int error,mapError,dmaError;
};
// Two 16 KiB slots in the existing 32 KiB pool; 2 seconds at 48 kHz.
// Peak 512 (-36.1 dBFS), 1 kHz triangle, 10 ms global fades. No FP or heap.
enum { PicassoPcmSlotBytes=16384, PicassoPcmChunkFrames=4096,
    PicassoPcmToneFrames=96000, PicassoPcmToneChunks=24, PicassoPcmTonePeak=512 };
inline unsigned picasso_pcm_chunk_bytes(unsigned sequence) {
    if(sequence>=PicassoPcmToneChunks)return 0;
    unsigned left=PicassoPcmToneFrames-sequence*PicassoPcmChunkFrames;
    return (left<PicassoPcmChunkFrames?left:PicassoPcmChunkFrames)*4;
}
inline int picasso_pcm_tone(uint8_t *data,unsigned bytes,unsigned firstFrame) {
    if(!data || !bytes || bytes>PicassoPcmSlotBytes || (bytes&3) ||
        firstFrame>=PicassoPcmToneFrames || bytes/4>PicassoPcmToneFrames-firstFrame)return -22;
    for(unsigned i=0;i<bytes/4;++i) {
        unsigned frame=firstFrame+i,phase=frame%48;
        int sample=phase<12?int(phase)*PicassoPcmTonePeak/12:phase<36?(24-int(phase))*PicassoPcmTonePeak/12:(int(phase)-48)*PicassoPcmTonePeak/12;
        unsigned edge=frame<480?frame:frame>=PicassoPcmToneFrames-480?PicassoPcmToneFrames-1-frame:480;
        sample=sample*int(edge)/480;
        uint16_t value=uint16_t(int16_t(sample));
        for(unsigned channel=0;channel<2;++channel) {
            data[i*4+channel*2]=uint8_t(value);data[i*4+channel*2+1]=uint8_t(value>>8);
        }
    }
    return 0;
}
extern "C" void picasso_pcm_device_prepare(PicassoPcm &);
#endif
