// SPDX-License-Identifier: BSD-2-Clause-Patent
// Direct capture qualification precedes publication of any OS input stream.
#include <IOKit/IOLib.h>
#include <kern/thread_call.h>
#include "PicassoGlink.h"
#include "PicassoCaptureDma.h"
#include "PicassoMicService.h"
extern "C" int picasso_mic_hardware(unsigned);
namespace {
struct MicReport {
    uint32_t mode,events,readDone,phase,dumpOffset,dumpHash;
    bool configured,summary,dumpDone;
};
static MicReport report;
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
void dumpPcm(const PicassoCaptureSamples &s) {
    if(report.dumpDone)return;
    if(report.dumpOffset==s.saved) {
        IOLog("PICASSO_MIC pcm-end bytes=%u fnv1a=%08x rate=48000 channels=1 bits=16 little_endian=1\n",s.saved,report.dumpHash);
        report.dumpDone=true;return;
    }
    unsigned bytes=s.saved-report.dumpOffset;if(bytes>64)bytes=64;
    char hex[129];const char alphabet[]="0123456789abcdef";
    for(unsigned i=0;i<bytes;++i) {
        unsigned b=s.data[report.dumpOffset+i];hex[2*i]=alphabet[b>>4];hex[2*i+1]=alphabet[b&15];
        report.dumpHash=(report.dumpHash^b)*16777619U;
    }
    hex[bytes*2]=0;
    // One small row per existing 20ms worker tick, after the DSP stream closes.
    IOLog("PICASSO_MIC pcm offset=%u bytes=%u hex=%s\n",report.dumpOffset,bytes,hex);
    report.dumpOffset+=bytes;
}
}
extern "C" void picasso_mic_service_configure(PicassoAprProbe &a,uint32_t mode) {
    if(report.configured)return;
    report.configured=true;
    // Mode 1 records two seconds and statistics; mode 2 also emits finite PCM.
    // No capture, register accesses or new OS input on a normal silent boot.
    if(mode!=1 && mode!=2)return;
    report.mode=mode;report.dumpHash=2166136261U;
    actionLock=IOLockAlloc();
    if(actionLock)actionCall=thread_call_allocate(performHardware,nullptr);
    if(!actionCall) {
        a.capture.enabled=a.capture.done=true;a.capture.firstError=a.capture.error=-12;++a.capture.events;
        return;
    }
    a.capture.enabled=true;a.capture.targetFrames=PicassoCaptureDefaultFrames;
    a.capture.hardware=hardware;a.pcm.capture=&a.capture.dma;
    IOLog("PICASSO_MIC configured=1 mode=%u route=amic1 rate=48000 channels=1 bits=16 frames=96000 os_input=0\n",mode);
}
extern "C" void picasso_mic_service_tick(PicassoAprProbe &a,bool failed) {
    if(!report.mode)return;
    auto &v=a.capture;
    if(failed && !v.done) {
        if(v.attempted)picasso_capture_stop(v,-104);
        else {v.done=true;v.error=v.firstError=-104;++v.events;}
    }
    if(v.events!=report.events) {
        bool progress=v.phase!=report.phase || v.readDone!=report.readDone || v.error || v.done;
        report.events=v.events;report.phase=v.phase;report.readDone=v.readDone;
        if(progress) {
            IOLog("PICASSO_MIC state phase=%u token=%x owned=%x inflight=%x pending=%u reads=%u/%u bytes=%llu error=%d first=%d cleanup=%d\n",
                v.phase,v.token,v.owned,v.inflight,v.pending,v.readDone,v.readSent,v.bytesDone,v.error,v.firstError,v.cleanupError);
            if(v.readDone || v.rejected)
                IOLog("PICASSO_MIC reply opcode=%x status=%u offset=%u bytes=%u flags=%x timestamp=%llu rejected=%u reason=%u\n",
                    v.replyOpcode,v.status,v.lastOffset,v.lastBytes,v.lastFlags,v.lastTimestamp,v.rejected,v.replyReject);
        }
    }
    if(!v.done)return;
    const auto &samples=*picasso_capture_samples_snapshot();
    if(!report.summary) {
        report.summary=true;
        IOLog("PICASSO_MIC capture complete=%u periods=%u bytes=%llu saved=%u nonzero=%u peak=%u clipped=%u sum=%lld abs_sum=%llu os_input=0\n",
            v.complete,samples.periods,samples.totalBytes,samples.saved,samples.nonzero,samples.peak,samples.clipped,samples.sum,samples.absoluteSum);
    }
    if(report.mode==2 && !v.owned && !v.inflight && !v.dma.busyMask)dumpPcm(samples);
}
