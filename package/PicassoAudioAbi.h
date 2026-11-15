// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_AUDIO_ABI_H
#define PICASSO_AUDIO_ABI_H
#include <stdint.h>
// Inline PCM transport; playback and capture have independent owners and queues.
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
// kind 7: selector/object/scope/element/operation in selector/value/calls/
// sentFrames/missingFrames (Get=0, Size=1, Has=2, Set=3).
// kinds 9..12: VirtualAudio Get/Size/Has/Set with the same fields; 13: throw
// offset and exception category (CA=1, logic=2, runtime=3, invalid_argument=4).
// kind 14: guarded trace installation result; selector 3/op 5 reports the
// original MGGetProductType low/high halves in value/calls. Observations only.
typedef struct PicassoAudioClientNote {
    uint32_t version,kind;
    int32_t result;
    uint32_t selector,value,reserved;
    uint64_t calls,sentFrames,missingFrames;
} PicassoAudioClientNote;
// Input selectors preserve every existing playback selector and layout.
enum { PicassoAudioInputStatus=16, PicassoAudioInputBegin, PicassoAudioInputRead, PicassoAudioInputStop };
enum { PicassoAudioInputReadBytes=3840, PicassoAudioInputPacketBytes=4096, PicassoAudioInputPackets=32 };
typedef struct PicassoAudioInputState {
    uint32_t version,state,epoch,queuedBytes;
    uint64_t capturedFrames,deliveredFrames,gapFrames,overruns;
    uint64_t firstHostTime,lastTimestamp;
    int32_t error;
    uint32_t peak;
} PicassoAudioInputState;
typedef struct PicassoAudioInputData {
    uint32_t version,epoch,bytes,flags;
    uint64_t firstFrame;
    uint8_t data[PicassoAudioInputReadBytes];
} PicassoAudioInputData;
#endif
