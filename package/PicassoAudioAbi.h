// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_AUDIO_ABI_H
#define PICASSO_AUDIO_ABI_H
#include <stdint.h>
// Output-only transport for a future iOS HAL client. No capture or mapped DMA.
#define PICASSO_AUDIO_CLASS "PicassoAudio"
enum { PicassoAudioVersion=1, PicassoAudioRate=48000, PicassoAudioChannels=2,
    PicassoAudioBits=16, PicassoAudioWriteMax=4096, PicassoAudioRingBytes=131072 };
enum { PicassoAudioGetStatus, PicassoAudioBegin, PicassoAudioWrite, PicassoAudioEnd, PicassoAudioCancel, PicassoAudioNote };
enum { PicassoAudioIdle, PicassoAudioStarting, PicassoAudioRunning, PicassoAudioDraining, PicassoAudioFailed };
typedef struct PicassoAudioFormat {uint32_t version,rate,channels,bits;} PicassoAudioFormat;
typedef struct PicassoAudioStatus {
    uint32_t version,state,epoch,queuedBytes;
    uint64_t submittedBytes,dmaBytes,returnedBytes;
    uint32_t writeReplies,drained;
    int32_t error;
    uint32_t reserved;
} PicassoAudioStatus;
// Bounded diagnostics reported by the HAL client; never interpreted as DSP
// completion, timestamps, pointer values or register operations.
typedef struct PicassoAudioClientNote {
    uint32_t version,kind;
    int32_t result;
    uint32_t selector,value,reserved;
    uint64_t calls,sentFrames,missingFrames;
} PicassoAudioClientNote;
#endif
