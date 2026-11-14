// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_PLAYBACK_H
#define PICASSO_PLAYBACK_H
#include <stdint.h>
#include "PicassoAudioPins.h"
#include "PicassoPcm.h"
#include "PicassoTfa.h"
enum { PicassoPlayClock=1,PicassoPlayPort=2,PicassoPlayStream=4,
    PicassoPlayMap=8,PicassoPlayCopp=16,PicassoPlayBuffer=32 };
enum PicassoPlayPhase {
    PicassoPlayConfigure, PicassoPlayClockOn, PicassoPlayStart,
    PicassoPlayOpen, PicassoPlayFormat, PicassoPlayMapBuffer,
    PicassoPlayOpenCopp, PicassoPlayRoute, PicassoPlayRun, PicassoPlayWrite, PicassoPlayDrain,
    PicassoPlayClose, PicassoPlayCloseCopp, PicassoPlayUnmap,
    PicassoPlayStop, PicassoPlayClockOff, PicassoPlayDone
};
// Bounded two-slot PCM stream and real rendered-EOS drain with a TFA lease.
// This is not an IOAudio device.
struct PicassoPlayback {
    int (*amplifier)(PicassoPlayback &,unsigned); // prepare, enable, finish, health
    // 0: a real CPU PCM chunk copied to slot; -11: wait; 1: end of source.
    int (*source)(PicassoPlayback &,PicassoPcm &,unsigned,unsigned *);
    PicassoTfaPlayback amp;
    bool ampPrepared,ampFinished,tone,drained;
    uint32_t phase,events,token,wait,status,opcode,api,copp,handle,owned;
    uint32_t accepted,statuses[PicassoPlayDone],writeAddressLow,writeAddressHigh,writeHandle,writeStatus;
    // Latest matching-token reply, including bounded raw rejected fields.
    uint32_t replyPhase,replyOpcode,replyType,replyBytes,replySource,replyDestination;
    uint32_t replyRaw[4],replyReject,rejected;
    PicassoAudioPins pins;
    bool bufferReady,ampEnabled;
    uint32_t writeSent,writeDone,inflight,writeSequence[2],writeWait[2],streamTicks,healthDone;
    uint32_t bytesSent,bytesDone,tokenBase,nextBytes,writeBytes[2],starveTicks;
    bool sourceEnded;
    bool enabled,attempted,sent,pending,responded,done,complete,consumed;
    int error,firstError,cleanupError,dmaError;
};
inline bool picasso_play_queued(const PicassoPlayback &v) {return v.tone || v.source;}
inline uint32_t picasso_play_base(const PicassoPlayback &v) {return v.tokenBase?v.tokenBase:0x20570000;}
inline uint32_t picasso_play_bytes(const PicassoPlayback &v) {
    return v.source?v.nextBytes:v.tone?picasso_pcm_chunk_bytes(v.writeSent):4096;
}
inline void picasso_play_stop(PicassoPlayback &v,int error) {
    if(!v.attempted)return;
    if(v.amplifier && !v.ampFinished) {
        int e=v.amplifier(v,2);v.ampFinished=true;if(e && !v.cleanupError)v.cleanupError=e;
    }
    picasso_audio_pins_restore(v.pins);
    v.pending=false;v.done=true;v.complete=false;v.error=error;
    if(!v.firstError)v.firstError=error;++v.events;
}
#endif
