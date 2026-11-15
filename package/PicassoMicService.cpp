// SPDX-License-Identifier: BSD-2-Clause-Patent
// Capture starts exclusively on an authenticated input client's Begin request.
#include <IOKit/IOLib.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include "PicassoGlink.h"
#include "PicassoCaptureApr.h"
#include "PicassoAudioInput.h"
#include "PicassoMicService.h"
extern "C" int picasso_mic_hardware(unsigned);
namespace {
static PicassoAudioInput input;
static IOLock *inputLock;
static bool configured,active;
static uint32_t lastPhase,reportTicks;
struct HardwareWork {
    unsigned action;
    int result;
    bool busy,complete;
};
static HardwareWork work;
static IOLock *actionLock;
static thread_call_t actionCall;
void performHardware(thread_call_param_t,thread_call_param_t) {
    IOLockLock(actionLock);unsigned action=work.action;IOLockUnlock(actionLock);
    int result=picasso_mic_hardware(action);
    IOLockLock(actionLock);work.result=result;work.complete=true;IOLockUnlock(actionLock);
}
int hardware(void *,unsigned action) {
    if(!actionLock || !actionCall)return -12;
    IOLockLock(actionLock);
    if(work.busy) {
        if(!work.complete) {IOLockUnlock(actionLock);return -11;}
        work.busy=false;work.complete=false;
        if(work.action==action) {int result=work.result;IOLockUnlock(actionLock);return result;}
        // A cancelled request may finish before its requested rollback. Consume
        // that result, then enqueue rollback; never run the two concurrently.
    }
    work.action=action;work.busy=true;work.complete=false;
    IOLockUnlock(actionLock);thread_call_enter(actionCall);return -11;
}
int sink(const uint8_t *data,unsigned bytes,uint64_t timestamp,uint32_t flags) {
    uint64_t duration;nanoseconds_to_absolutetime(uint64_t(bytes/2)*1000000000ULL/48000,&duration);
    uint64_t now=mach_absolute_time(),host=now>duration?now-duration:0;
    IOLockLock(inputLock);int e=picasso_input_append(input,data,bytes,timestamp,flags,host);
    IOLockUnlock(inputLock);return e;
}
}
extern "C" void picasso_mic_service_init(PicassoAprProbe &a) {
    if(configured)return;configured=true;
    inputLock=IOLockAlloc();actionLock=IOLockAlloc();
    if(actionLock)actionCall=thread_call_allocate(performHardware,nullptr);
    input.status.version=1;
    if(!inputLock || !actionCall) {input.status.state=PicassoAudioFailed;input.status.error=-12;return;}
    // Reserve the independent DMA mapping before SID1401 is enabled. This
    // does not vote clocks, enable bias, start a port, or issue a capture READ.
    a.capture.hardware=hardware;a.capture.dma.sink=sink;a.pcm.capture=&a.capture.dma;
    a.capture.streaming=true;
    IOLog("PICASSO_MIC initialized=1 automatic_capture=0 route=amic1 rate=48000 channels=1 bits=16\n");
}
extern "C" int picasso_mic_service_call(uint64_t owner,unsigned selector,const void *in,unsigned bytes,void *out,unsigned size) {
    if(!inputLock)return -6;
    IOLockLock(inputLock);int e=-22;
    if(selector==PicassoAudioInputStatus && !bytes && out && size==sizeof(PicassoAudioInputState)) {
        *static_cast<PicassoAudioInputState *>(out)=input.status;e=0;
    } else if(selector==PicassoAudioInputBegin && in && bytes==sizeof(PicassoAudioFormat) && !out)
        e=picasso_input_begin(input,owner,*static_cast<const PicassoAudioFormat *>(in));
    else if(selector==PicassoAudioInputRead && !bytes && out && size==sizeof(PicassoAudioInputData))
        e=picasso_input_read(input,owner,*static_cast<PicassoAudioInputData *>(out));
    else if(selector==PicassoAudioInputStop && !bytes && !out)e=picasso_input_stop(input,owner);
    IOLockUnlock(inputLock);return e;
}
extern "C" void picasso_mic_service_close(uint64_t owner) {
    if(!inputLock)return;IOLockLock(inputLock);
    if(owner && input.owner==owner)(void)picasso_input_stop(input,owner);
    IOLockUnlock(inputLock);
}
extern "C" void picasso_mic_service_tick(PicassoAprProbe &a,bool failed) {
    if(!inputLock || !actionCall)return;
    auto &v=a.capture;IOLockLock(inputLock);
    if(failed) {
        input.ready=false;
        if(active && !v.done) {
            if(v.attempted)picasso_capture_stop(v,-104);
            else {v.done=true;v.error=v.firstError=-104;}
        }
        input.status.state=PicassoAudioFailed;input.status.error=-104;
    } else if(!active && input.status.state!=PicassoAudioFailed)
        input.ready=a.ready && a.fw.valid && a.pcm.done && a.pcm.complete && v.dma.ready;
    if(active) {
        if(input.cancel && !v.done) {
            if(v.attempted)picasso_capture_cancel(v);
            else {v.cancelled=v.done=true;v.firstError=v.error=-125;}
        }
        if(v.phase!=lastPhase || ++reportTicks>=50 || v.done) {
            lastPhase=v.phase;reportTicks=0;
            IOLog("PICASSO_MIC session=%u phase=%u reads=%u bytes=%llu gaps=%llu overruns=%llu peak=%u error=%d\n",
                input.status.epoch,v.phase,v.readDone,v.bytesDone,input.status.gapFrames,input.status.overruns,input.status.peak,v.error);
            IOLog("PICASSO_MIC flow epoch=%u sent=%u inflight=%x waits=%u/%u pending=%u token=%x rejected=%u opcode=%x bytes=%u offset=%u sequence=%u timestamp=%llu first=%d cleanup=%d dma=%d owned=%x\n",
                input.status.epoch,v.readSent,v.inflight,v.readWait[0],v.readWait[1],v.pending,v.token,
                v.rejected,v.replyOpcode,v.lastBytes,v.lastOffset,v.lastSequence,v.lastTimestamp,
                v.firstError,v.cleanupError,v.dmaError,v.owned);
        }
        if(v.done) {
            bool clean=!v.owned && !v.inflight && !v.dma.busyMask && !v.cleanupError && !v.dmaError &&
                (v.complete || (v.cancelled && v.firstError==-125));
            int error=v.firstError?v.firstError:v.cleanupError;
            IOLog("PICASSO_MIC stopped epoch=%u frames=%llu delivered=%llu gaps=%llu clean=%u error=%d\n",
                input.status.epoch,input.status.capturedFrames,input.status.deliveredFrames,input.status.gapFrames,clean,error);
            picasso_input_retire(input,clean,error);active=false;
        }
    }
    if(!active && input.cancel)picasso_input_retire(input,true,0);
    if(!failed && !active && input.status.state==PicassoAudioStarting) {
        bool ready=!v.enabled && !v.attempted;
        if(v.done)ready=picasso_capture_rearm(v);
        if(ready) {v.enabled=v.streaming=true;active=true;lastPhase=UINT32_MAX;reportTicks=0;}
        else picasso_input_retire(input,false,-16);
    }
    IOLockUnlock(inputLock);
}
