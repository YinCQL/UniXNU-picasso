// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_AUDIO_QUEUE_H
#define PICASSO_AUDIO_QUEUE_H
#include "PicassoAudioAbi.h"
// Protected by the service's IOLock; only the DSP worker consumes and retires.
struct PicassoAudioQueue {
    uint8_t data[PicassoAudioRingBytes];
    uint64_t owner,submitted,dma,returned;
    uint32_t read,write,used,state,epoch,replies;
    bool ready,end,cancel,drained;
    int error;
};
inline int picasso_audio_begin(PicassoAudioQueue &q,uint64_t owner,const PicassoAudioFormat &f) {
    if(!owner || f.version!=1 || f.rate!=48000 || f.channels!=2 || f.bits!=16)return -22;
    if(!q.ready)return -6;
    if(q.state!=PicassoAudioIdle || q.owner)return -16;
    if(q.epoch==0xffffffff)return -75;
    ++q.epoch;q.owner=owner;q.read=q.write=q.used=0;q.submitted=q.dma=q.returned=0;q.replies=0;
    q.end=q.cancel=q.drained=false;q.error=0;q.state=PicassoAudioStarting;return 0;
}
inline int picasso_audio_write(PicassoAudioQueue &q,uint64_t owner,const uint8_t *data,unsigned bytes) {
    if(!owner || owner!=q.owner)return -1;
    if(!data || !bytes || bytes>PicassoAudioWriteMax || (bytes&3))return -22;
    if((q.state!=PicassoAudioStarting && q.state!=PicassoAudioRunning) || q.end || q.cancel)return -16;
    if(bytes>PicassoAudioRingBytes-q.used)return -11;
    // Explicit per-session limit (about 83 minutes), before counters/token wrap.
    if(q.submitted>960000000ULL-bytes)return -75;
    for(unsigned i=0;i<bytes;++i)q.data[(q.write+i)%PicassoAudioRingBytes]=data[i];
    q.write=(q.write+bytes)%PicassoAudioRingBytes;q.used+=bytes;q.submitted+=bytes;return 0;
}
inline int picasso_audio_end(PicassoAudioQueue &q,uint64_t owner,bool cancel) {
    if(!owner || owner!=q.owner)return -1;
    if(q.state==PicassoAudioIdle || q.state==PicassoAudioFailed)return -16;
    if(!cancel && !q.submitted)return -22;
    if(cancel)q.cancel=true;else q.end=true;return 0;
}
inline unsigned picasso_audio_available(const PicassoAudioQueue &q) {
    if(q.cancel)return 0;
    if(q.used>=16384)return 16384;
    return q.end?q.used:0;
}
inline void picasso_audio_retire(PicassoAudioQueue &q,bool clean,int error,bool drained) {
    q.drained=drained;q.error=error;
    q.owner=0;q.used=0;q.read=q.write=0;q.end=q.cancel=false;
    q.state=clean?PicassoAudioIdle:PicassoAudioFailed;
    if(!clean)q.ready=false;
}
inline PicassoAudioStatus picasso_audio_status(const PicassoAudioQueue &q) {
    return {1,q.state,q.epoch,q.used,q.submitted,q.dma,q.returned,q.replies,q.drained,q.error,0};
}
#endif
