// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_AUDIO_INPUT_H
#define PICASSO_AUDIO_INPUT_H
#include "PicassoAudioAbi.h"
// Callers serialize access. Packet positions derive from DSP microsecond
// timestamps; missing time is reported, never removed by concatenating audio.
struct PicassoAudioInput {
    struct Packet {uint64_t frame;uint32_t bytes,offset;uint8_t data[PicassoAudioInputPacketBytes];};
    Packet packet[PicassoAudioInputPackets];
    PicassoAudioInputState status;
    uint64_t owner,firstTimestamp,endFrame;
    uint32_t head,count;
    bool ready,cancel,positioned;
};
inline void picasso_input_discard(PicassoAudioInput &q) {
    for(auto &p:q.packet) {for(auto &b:p.data)b=0;p.bytes=p.offset=0;p.frame=0;}
    q.head=q.count=0;q.status.queuedBytes=0;
}
inline int picasso_input_begin(PicassoAudioInput &q,uint64_t owner,const PicassoAudioFormat &f) {
    if(!owner || f.version!=1 || f.rate!=48000 || f.channels!=1 || f.bits!=16)return -22;
    if(!q.ready || q.status.state==PicassoAudioFailed)return -6;
    if(q.owner || q.status.state!=PicassoAudioIdle)return -16;
    uint32_t epoch=q.status.epoch+1;if(!epoch)return -75;
    picasso_input_discard(q);q.status={};q.status.version=1;q.status.epoch=epoch;q.status.state=PicassoAudioStarting;
    q.owner=owner;q.cancel=q.positioned=false;q.firstTimestamp=q.endFrame=0;return 0;
}
inline int picasso_input_stop(PicassoAudioInput &q,uint64_t owner) {
    if(!owner || q.owner!=owner)return -1;
    q.cancel=true;q.status.state=PicassoAudioDraining;picasso_input_discard(q);return 0;
}
inline int picasso_input_append(PicassoAudioInput &q,const uint8_t *data,unsigned bytes,uint64_t timestamp,uint32_t flags,uint64_t host) {
    if(q.cancel || !q.owner)return 0;
    if(!data || !bytes || bytes>PicassoAudioInputPacketBytes || (bytes&1) || flags!=0x80000000U)return -22;
    if(q.count==PicassoAudioInputPackets) {++q.status.overruns;return -28;}
    if(!q.positioned) {q.firstTimestamp=timestamp;q.status.firstHostTime=host;q.positioned=true;}
    if(timestamp<q.firstTimestamp || timestamp<q.status.lastTimestamp)return -71;
    uint64_t delta=timestamp-q.firstTimestamp;
    if(delta>(UINT64_MAX-500)/48)return -75;
    uint64_t frame=(delta*48+500)/1000;
    // Microsecond rounding can move a boundary by one sample. Larger overlap
    // is a protocol error; a gap retains its true position and increments loss.
    if(frame<q.endFrame) {if(q.endFrame-frame>1)return -71;frame=q.endFrame;}
    if(frame>q.endFrame)q.status.gapFrames+=frame-q.endFrame;
    auto &p=q.packet[(q.head+q.count)%PicassoAudioInputPackets];
    p.frame=frame;p.offset=0;p.bytes=bytes;for(unsigned i=0;i<bytes;++i)p.data[i]=data[i];
    for(unsigned i=0;i<bytes;i+=2) {
        int sample=int16_t(uint16_t(data[i])|(uint16_t(data[i+1])<<8));
        unsigned magnitude=sample<0?unsigned(-sample):unsigned(sample);
        if(magnitude>q.status.peak)q.status.peak=magnitude;
    }
    ++q.count;q.status.queuedBytes+=bytes;q.status.capturedFrames+=bytes/2;
    q.status.lastTimestamp=timestamp;q.endFrame=frame+bytes/2;q.status.state=PicassoAudioRunning;return 0;
}
inline int picasso_input_read(PicassoAudioInput &q,uint64_t owner,PicassoAudioInputData &out) {
    if(!owner || q.owner!=owner)return -1;
    if(q.cancel || q.status.state==PicassoAudioFailed)return -6;
    if(!q.count)return -11;
    auto &p=q.packet[q.head];unsigned bytes=p.bytes-p.offset;
    if(bytes>PicassoAudioInputReadBytes)bytes=PicassoAudioInputReadBytes;
    out={};out.version=1;out.epoch=q.status.epoch;out.bytes=bytes;out.flags=0x80000000U;out.firstFrame=p.frame+p.offset/2;
    for(unsigned i=0;i<bytes;++i) {out.data[i]=p.data[p.offset+i];p.data[p.offset+i]=0;}
    p.offset+=bytes;q.status.queuedBytes-=bytes;q.status.deliveredFrames+=bytes/2;
    if(p.offset==p.bytes) {q.head=(q.head+1)%PicassoAudioInputPackets;--q.count;}
    return 0;
}
inline void picasso_input_retire(PicassoAudioInput &q,bool clean,int error) {
    picasso_input_discard(q);q.owner=0;q.cancel=false;
    q.status.state=clean?PicassoAudioIdle:PicassoAudioFailed;q.status.error=clean?0:error;
    if(!clean)q.ready=false;
}
#endif
