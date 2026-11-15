// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef PICASSO_CAPTURE_H
#define PICASSO_CAPTURE_H
#include <stdint.h>
// Mono S16 capture owns a separate SID1401 pool. The caller serializes every
// operation on the ADSP worker and retains its pages until a real UNMAP ACK.
struct PicassoCaptureDma {
    void *context;
    int (*check)(PicassoCaptureDma &);
    int (*prepare)(PicassoCaptureDma &,unsigned); // invalidate/prepare a free slot
    int (*complete)(PicassoCaptureDma &,unsigned,unsigned,unsigned); // slot, offset, bytes
    // Worker-only PCM sink; the DMA allocation and client buffers never alias.
    int (*sink)(const uint8_t *,unsigned,uint64_t,uint32_t);
    uint64_t timestamp;
    uint32_t flags;
    uint64_t address;
    uint32_t bytes,slotBytes,busyMask;
    bool ready,retained;
    int error;
};
enum PicassoCaptureResource {
    PicassoCaptureCoreClock=1, PicassoCaptureNplClock=2,
    PicassoCaptureHardware=4, PicassoCapturePort=8, PicassoCaptureStream=16,
    PicassoCaptureMap=32, PicassoCaptureCopp=64, PicassoCaptureBuffer=128,
    PicassoCaptureMacroVote=256, PicassoCaptureCodecVote=512
};
enum PicassoCapturePhase {
    PicassoCaptureVoteMacro, PicassoCaptureVoteCodec,
    PicassoCaptureCoreOn, PicassoCaptureNplOn, PicassoCapturePrepare,
    PicassoCaptureConfigure, PicassoCaptureStartPort, PicassoCaptureOpen,
    PicassoCaptureFormat, PicassoCaptureMapBuffer, PicassoCaptureOpenCopp,
    PicassoCaptureRoute, PicassoCaptureUnmute, PicassoCapturePrime,
    PicassoCaptureRun, PicassoCaptureRead, PicassoCaptureClose,
    PicassoCaptureCloseCopp, PicassoCaptureUnmap, PicassoCaptureStopPort,
    PicassoCaptureHardwareStop, PicassoCaptureNplOff, PicassoCaptureCoreOff,
    PicassoCaptureUnvoteCodec, PicassoCaptureUnvoteMacro, PicassoCaptureDone
};
struct PicassoCapture {
    PicassoCaptureDma dma;
    void *context;
    // 0: prepare after both TX clock ACKs; 1: unmute; 2: restore after DSP stop;
    // 3: periodically recheck active bus and observed supply health.
    // -11 means asynchronous work has not finished; every wait remains bounded.
    int (*hardware)(void *,unsigned);
    uint32_t phase,events,tokenBase,token,wait,owned,accepted,status,opcode;
    uint32_t handle,copp,hwHandle[2],targetFrames,readSent,readDone,inflight;
    uint32_t readSequence[2],readWait[2],preparedSlot,streamTicks,healthDone,healthWait;
    uint64_t bytesDone;
    uint32_t readTokenBase;
    bool streaming;
    uint32_t lastOffset,lastBytes,lastFlags,lastFrames,lastSequence;
    uint64_t lastTimestamp;
    uint32_t rejected,replyReject,replyPhase,replyOpcode,replyType,replyBytes;
    uint32_t replySource,replyDestination,replyToken,replyRaw[11];
    bool enabled,attempted,sent,pending,responded,prepared,done,complete;
    bool coppValid,cancelled,transportLost,healthPending;
    int error,firstError,cleanupError,dmaError;
};
enum {
    PicassoCapturePortId=0xb037, // kona: tx_macro_tx1 / TX CDC DMA3
    PicassoCaptureRate=48000, PicassoCaptureBits=16,
    PicassoCaptureDefaultFrames=96000, PicassoCaptureMaxReads=0xe000
};
inline uint32_t picasso_capture_base(const PicassoCapture &v) {
    return v.tokenBase?v.tokenBase:0x30570000U;
}
inline uint32_t picasso_capture_read_token(const PicassoCapture &v,uint32_t sequence) {
    return v.streaming?(v.readTokenBase?v.readTokenBase:0x80000000U)+sequence:
        picasso_capture_base(v)+0x1000+sequence;
}
inline uint32_t picasso_capture_le32(const uint8_t *b) {
    return uint32_t(b[0])|(uint32_t(b[1])<<8)|(uint32_t(b[2])<<16)|(uint32_t(b[3])<<24);
}
inline bool picasso_capture_data_phase(unsigned phase) {
    return phase==PicassoCapturePrime || phase==PicassoCaptureRead;
}
inline uint32_t picasso_capture_dependencies() {
    return PicassoCaptureHardware|PicassoCapturePort|PicassoCaptureStream|
        PicassoCaptureCopp|PicassoCaptureBuffer;
}
inline void picasso_capture_fail(PicassoCapture &v,int error) {
    if(!error)return;
    if(v.phase<PicassoCaptureClose) {
        if(!v.firstError)v.firstError=error;
        v.phase=PicassoCaptureClose;
    } else {
        if(!v.cleanupError)v.cleanupError=error;
        ++v.phase;
    }
    v.error=error;v.sent=v.pending=v.responded=v.prepared=false;v.wait=0;++v.events;
}
// Transport loss does not prove DMA retirement or release a remote handle.
inline void picasso_capture_stop(PicassoCapture &v,int error) {
    if(!v.attempted || v.done)return;
    v.transportLost=true;v.done=true;v.complete=false;v.error=error?error:-104;
    if(!v.firstError)v.firstError=v.error;
    v.pending=v.prepared=false;++v.events;
}
inline void picasso_capture_cancel(PicassoCapture &v) {
    if(!v.attempted || v.done)return;
    v.cancelled=true;if(v.phase<PicassoCaptureClose)picasso_capture_fail(v,-125);
}
#endif
