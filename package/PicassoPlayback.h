// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_PLAYBACK_H
#define PICASSO_PLAYBACK_H
#include <stdint.h>
#include "PicassoAudioPins.h"
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
// One bounded buffer and real rendered-EOS drain. Production uses a brief
// low-level tone with a TFA lease; this is not an IOAudio device.
struct PicassoPlayback {
    int (*amplifier)(PicassoPlayback &,unsigned); // prepare, enable, finish, health
    PicassoTfaPlayback amp;
    bool ampPrepared,ampFinished,tone,drained;
    uint32_t phase,events,token,wait,status,opcode,api,copp,handle,owned;
    uint32_t accepted,statuses[PicassoPlayDone],writeAddressLow,writeAddressHigh,writeHandle,writeStatus;
    // Latest matching-token reply, including bounded raw rejected fields.
    uint32_t replyPhase,replyOpcode,replyType,replyBytes,replySource,replyDestination;
    uint32_t replyRaw[4],replyReject,rejected;
    PicassoAudioPins pins;
    bool bufferReady;
    bool enabled,attempted,sent,pending,responded,done,complete,consumed;
    int error,firstError,cleanupError,dmaError;
};
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
