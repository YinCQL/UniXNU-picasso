// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_PLAYBACK_H
#define PICASSO_PLAYBACK_H
#include <stdint.h>
#include "PicassoAudioPins.h"
enum { PicassoPlayClock=1,PicassoPlayPort=2,PicassoPlayStream=4,
    PicassoPlayMap=8,PicassoPlayCopp=16,PicassoPlayBuffer=32 };
enum PicassoPlayPhase {
    PicassoPlayConfigure, PicassoPlayClockOn, PicassoPlayStart,
    PicassoPlayOpen, PicassoPlayFormat, PicassoPlayMapBuffer,
    PicassoPlayOpenCopp, PicassoPlayRoute, PicassoPlayRun, PicassoPlayWrite,
    PicassoPlayClose, PicassoPlayCloseCopp, PicassoPlayUnmap,
    PicassoPlayStop, PicassoPlayClockOff, PicassoPlayDone
};
// One bounded 4096-byte silent buffer. This is a DSP path test, not an
// IOAudio device or amplifier enable. Ownership survives missing replies.
struct PicassoPlayback {
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
    picasso_audio_pins_restore(v.pins);
    v.pending=false;v.done=true;v.complete=false;v.error=error;
    if(!v.firstError)v.firstError=error;++v.events;
}
#endif
