// SPDX-License-Identifier: BSD-2-Clause-Patent
// Service pattern: UniXNU-sheng 460179565a018c741b93e0b1efa07c55d4de497a
// ShengAudioService.cpp. Independent output ABI; Picasso uses APR/TFA9874.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include "PicassoAudioQueue.h"
#include "PicassoApr.h"
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void picasso_audio_static_init(void) __asm__("__GLOBAL__sub_I_PicassoAudioService.cpp");
static PicassoAudioQueue queue;
static IOLock *queueLock;
static uint8_t scratch[16384]; // worker-only staging, never stack or user DMA
static bool attempted,published,registered,active,registrationFailed;
static unsigned testPhase,testFrames,testPause,lastEvents,startWait;

static const PicassoAudioFormat format={1,48000,2,16};

static int audioCall(uint64_t owner,unsigned selector,const void *input,unsigned bytes,void *output,unsigned *outBytes,const bool *closed=nullptr) {
    if(!queueLock)return -6;
    IOLockLock(queueLock);int e=-22;
    if(closed && *closed) {IOLockUnlock(queueLock);return -1;}
    if(selector==PicassoAudioGetStatus && !bytes && output && outBytes && *outBytes==sizeof(PicassoAudioStatus)) {
        *static_cast<PicassoAudioStatus *>(output)=picasso_audio_status(queue);e=0;
    } else if(selector==PicassoAudioBegin && input && bytes==sizeof format && !output)
        e=picasso_audio_begin(queue,owner,*static_cast<const PicassoAudioFormat *>(input));
    else if(selector==PicassoAudioWrite && !output)e=picasso_audio_write(queue,owner,static_cast<const uint8_t *>(input),bytes);
    else if((selector==PicassoAudioEnd || selector==PicassoAudioCancel) && !bytes && !output)e=picasso_audio_end(queue,owner,selector==PicassoAudioCancel);
    IOLockUnlock(queueLock);return e;
}
class PicassoAudio : public IOService {
    OSDeclareDefaultStructors(PicassoAudio)
public:
    bool start(IOService *provider) override {
        if(!IOService::start(provider))return false;
        if(!setProperty("IOUserClientClass","PicassoAudioUserClient") ||
            !setProperty("PicassoAudioABIVersion",uint64_t(1),32) ||
            !setProperty("PicassoAudioRate",uint64_t(48000),32))return false;
        registerService();return true;
    }
};
OSDefineMetaClassAndStructors(PicassoAudio,IOService)
class PicassoAudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PicassoAudioUserClient)
    bool closed=false;
public:
    bool initWithTask(task_t task,void *security,UInt32 type,OSDictionary *props) override {
        if(type || clientHasPrivilege(task,kIOClientPrivilegeAdministrator)!=kIOReturnSuccess)return false;
        return IOUserClient::initWithTask(task,security,type,props);
    }
    bool start(IOService *provider) override {
        return OSDynamicCast(PicassoAudio,provider) && IOUserClient::start(provider);
    }
    IOReturn clientClose() override {
        if(queueLock) {IOLockLock(queueLock);closed=true;
            if(queue.owner==reinterpret_cast<uintptr_t>(this))queue.cancel=true;
            IOLockUnlock(queueLock);}
        terminate();return kIOReturnSuccess;
    }
    IOReturn clientDied() override {return clientClose();}
    IOReturn externalMethod(uint32_t selector,IOExternalMethodArguments *a,IOExternalMethodDispatch *,OSObject *,void *) override {
        if(!a || a->scalarInputCount || a->scalarOutputCount || a->structureInputDescriptor || a->structureOutputDescriptor ||
            a->asyncWakePort || a->structureInputSize>PicassoAudioWriteMax)return kIOReturnBadArgument;
        // The IOUserClient copied inline data before entry; never retain caller
        // pointers, map the DSP pool, or permit arbitrary memory/register access.
        if(!queueLock)return kIOReturnNotReady;
        IOLockLock(queueLock);bool wasClosed=closed;IOLockUnlock(queueLock);
        if(wasClosed)return kIOReturnNotOpen;
        if(selector==PicassoAudioGetStatus) {
            if(a->structureInputSize || !a->structureOutput || a->structureOutputSize!=sizeof(PicassoAudioStatus))return kIOReturnBadArgument;
        } else if(a->structureOutputSize)return kIOReturnBadArgument;
        unsigned size=a->structureOutputSize;
        int e=audioCall(reinterpret_cast<uintptr_t>(this),selector,a->structureInput,a->structureInputSize,
            selector==PicassoAudioGetStatus?a->structureOutput:nullptr,&size,&closed);
        return !e?kIOReturnSuccess:e==-22?kIOReturnBadArgument:e==-1?kIOReturnNotPermitted:e==-11?kIOReturnNoSpace:e==-16?kIOReturnBusy:kIOReturnNotReady;
    }
};
OSDefineMetaClassAndStructors(PicassoAudioUserClient,IOUserClient)
static void publishAudio() {
    if(published || registrationFailed)return;
    if(!registered) {
        void *load=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if(!load) {registrationFailed=true;IOLog("PICASSO_AUDIO publish error=preload\n");return;}
        picasso_audio_static_init();OSReturn e=OSMetaClass::postModLoad(load);
        if(e) {registrationFailed=true;IOLog("PICASSO_AUDIO publish error=metaclass status=%x\n",e);return;}registered=true;
    }
    auto *provider=IOService::getResourceService();auto *service=OSTypeAlloc(PicassoAudio);
    bool initialized=service && service->init();bool attached=initialized && provider && service->attach(provider);
    published=attached && service->start(provider);
    if(!published) {registrationFailed=true;if(attached)service->detach(provider);}
    if(service)service->release();
    IOLog("PICASSO_AUDIO published=%u output=1 input=0 rate=48000 bits=16 channels=2 hal=0\n",published);
}
static int audioSource(PicassoPlayback &,PicassoPcm &pcm,unsigned slot,unsigned *bytes) {
    *bytes=0;IOLockLock(queueLock);
    if(queue.cancel) {IOLockUnlock(queueLock);return -125;}
    unsigned n=picasso_audio_available(queue);
    if(!n) {int e=queue.end?1:-11;IOLockUnlock(queueLock);return e;}
    for(unsigned i=0;i<n;++i)scratch[i]=queue.data[(queue.read+i)%PicassoAudioRingBytes];
    IOLockUnlock(queueLock);
    int e=pcm.copy?pcm.copy(pcm,slot,scratch,n):-22;
    IOLockLock(queueLock);
    if(!e && queue.cancel)e=-125;
    if(!e) {queue.read=(queue.read+n)%PicassoAudioRingBytes;queue.used-=n;queue.dma+=n;*bytes=n;}
    IOLockUnlock(queueLock);return e;
}
// Initial 2s tone remains the hardware qualification gate. Then run 1s of
// externally supplied PCM through this exact producer/copy/rearm path. A second
// short session validates start/stop and token generations before publication.
static void feedSelftest() {
    if(testPhase!=1 && testPhase!=2)return;
    unsigned total=testPhase==1?48000:24000;
    IOLockLock(queueLock);
    if(!queue.owner && queue.state==PicassoAudioIdle) {
        if(++testPause<10) {IOLockUnlock(queueLock);return;}testPause=0;
        (void)picasso_audio_begin(queue,1,format);testFrames=0;
    }
    while(queue.owner==1 && !queue.end && !queue.cancel && testFrames<total && queue.used<=PicassoAudioRingBytes-4096) {
        unsigned n=total-testFrames;if(n>1024)n=1024;
        for(unsigned i=0;i<n;++i) {
            unsigned f=testFrames+i,period=testPhase==1?96:64,phase=f%period;
            int sample=phase<period/4?int(phase)*512/int(period/4):phase<period*3/4?(int(period/2)-int(phase))*512/int(period/4):(int(phase)-int(period))*512/int(period/4);
            unsigned edge=f<480?f:f>=total-480?total-1-f:480;sample=sample*int(edge)/480;
            uint16_t v=uint16_t(int16_t(sample));scratch[i*4]=scratch[i*4+2]=uint8_t(v);scratch[i*4+1]=scratch[i*4+3]=uint8_t(v>>8);
        }
        int e=picasso_audio_write(queue,1,scratch,n*4);if(e)break;testFrames+=n;
    }
    if(queue.owner==1 && testFrames==total && !queue.end)(void)picasso_audio_end(queue,1,false);
    IOLockUnlock(queueLock);
}
extern "C" void picasso_audio_service_tick(PicassoAprProbe &p,bool transportFailed) {
    auto &v=p.play;
    if(!attempted && v.done) {
        attempted=true;
        if(!v.complete || transportFailed)return;
        queueLock=IOLockAlloc();if(!queueLock) {IOLog("PICASSO_AUDIO error=lock\n");return;}
        queue.ready=true;testPhase=1;
    }
    if(!queueLock)return;
    if(active) {
        IOLockLock(queueLock);
        if(queue.cancel && v.phase<PicassoPlayClose && !v.error) {v.error=-125;v.pending=false;++v.events;}
        queue.returned=v.bytesDone;queue.replies=v.writeDone;
        if(v.phase>=PicassoPlayDrain && v.phase<PicassoPlayDone)queue.state=PicassoAudioDraining;
        if(transportFailed && !v.done) {v.error=-104;v.pending=false;}
        if(v.done) {
            bool clean=!v.owned && !p.pcm.busyMask && !v.inflight && !v.pins.dirty && v.amp.stopped && !v.cleanupError && !v.dmaError;
            bool cancelled=queue.cancel;int error=v.firstError?v.firstError:v.cleanupError;
            picasso_audio_retire(queue,clean && (v.complete || (cancelled && v.firstError==-125)),error,v.drained);active=false;
            IOLog("PICASSO_AUDIO session=%u returned=%llu submitted=%llu replies=%u complete=%u drained=%u clean=%u error=%d test=%u\n",
                queue.epoch,queue.returned,queue.submitted,queue.replies,v.complete,v.drained,clean,error,testPhase);
            if(testPhase<3) {if(v.complete)++testPhase;else testPhase=4;}
        }
        IOLockUnlock(queueLock);
    }
    if(transportFailed) {IOLockLock(queueLock);queue.ready=false;queue.state=PicassoAudioFailed;queue.error=-104;IOLockUnlock(queueLock);return;}
    if(!active && testPhase==3 && queue.ready && !published)publishAudio();
    feedSelftest();
    IOLockLock(queueLock);
    if(!active && queue.state==PicassoAudioStarting && !queue.cancel && !picasso_audio_available(queue)) {
        if(++startWait>250) {picasso_audio_retire(queue,true,-60,false);startWait=0;}
    } else startWait=0;
    if(!active && queue.state==PicassoAudioStarting && !queue.cancel && picasso_audio_available(queue)) {
        if(picasso_play_rearm(p)) {v.source=audioSource;active=true;queue.state=PicassoAudioRunning;}
        else picasso_audio_retire(queue,false,-16,false);
    } else if(!active && queue.cancel)picasso_audio_retire(queue,true,-125,false);
    if(queue.epoch!=lastEvents) {lastEvents=queue.epoch;IOLog("PICASSO_AUDIO begin epoch=%u state=%u buffered=%u test=%u\n",queue.epoch,queue.state,queue.used,testPhase);}
    IOLockUnlock(queueLock);
}
