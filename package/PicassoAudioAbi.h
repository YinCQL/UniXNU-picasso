// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_AUDIO_ABI_H
#define PICASSO_AUDIO_ABI_H
#include <stdint.h>
// Output-only transport for a future iOS HAL client. No capture or mapped DMA.
#define PICASSO_AUDIO_CLASS "PicassoAudio"
enum { PicassoAudioVersion=1, PicassoAudioRate=48000, PicassoAudioChannels=2,
    PicassoAudioBits=16, PicassoAudioWriteMax=4096, PicassoAudioRingBytes=131072 };
enum { PicassoAudioGetStatus, PicassoAudioBegin, PicassoAudioWrite, PicassoAudioEnd, PicassoAudioCancel };
enum { PicassoAudioIdle, PicassoAudioStarting, PicassoAudioRunning, PicassoAudioDraining, PicassoAudioFailed };
struct PicassoAudioFormat {uint32_t version,rate,channels,bits;};
struct PicassoAudioStatus {
    uint32_t version,state,epoch,queuedBytes;
    uint64_t submittedBytes,dmaBytes,returnedBytes;
    uint32_t writeReplies,drained;
    int32_t error;
    uint32_t reserved;
};
#endif
