// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "PicassoGlink.h"
#include "PicassoAudioInput.h"
static std::vector<std::string> logs;
static unsigned hardwareCalls;
static int hardwareResult;
struct IOLock {bool held;};
static IOLock locks[4];static unsigned lockCount;
static IOLock *IOLockAlloc(){assert(lockCount<4);return &locks[lockCount++];}
static void IOLockLock(IOLock *p){assert(!p->held);p->held=true;}
static void IOLockUnlock(IOLock *p){assert(p->held);p->held=false;}
using thread_call_param_t=void *;
struct Call {void (*function)(void *,void *);bool queued;};
using thread_call_t=Call *;
static Call call;
static thread_call_t thread_call_allocate(void (*function)(void *,void *),void *){call.function=function;return &call;}
static bool thread_call_enter(thread_call_t c){assert(!c->queued);c->queued=true;return false;}
static void runHardware(){assert(call.queued);call.queued=false;call.function(nullptr,nullptr);}
static uint64_t mach_absolute_time(){return 1000000000;}
static void nanoseconds_to_absolutetime(uint64_t n,uint64_t *t){*t=n;}
static void IOLog(const char *fmt,...) {
    char text[1024];va_list args;va_start(args,fmt);int n=vsnprintf(text,sizeof text,fmt,args);va_end(args);
    assert(n>0 && n<256 && text[n-1]=='\n');logs.emplace_back(text);
}
extern "C" int picasso_mic_hardware(unsigned){for(const auto &l:locks)assert(!l.held);++hardwareCalls;return hardwareResult;}
#include "PicassoMicService-body.inc"
static void queueTests() {
    static PicassoAudioInput q;const PicassoAudioFormat f={1,48000,1,16};
    static uint8_t data[4096];for(unsigned i=0;i<sizeof data;++i)data[i]=uint8_t(i);
    PicassoAudioInputData out;
    assert(picasso_input_begin(q,7,f)==-6);q.ready=true;assert(!picasso_input_begin(q,7,f));
    assert(picasso_input_begin(q,8,f)==-16&&picasso_input_read(q,8,out)==-1);
    assert(picasso_input_append(q,data,384,100000,0,1)==-22);
    assert(!picasso_input_append(q,data,384,100000,0x80000000,1));
    assert(!picasso_input_read(q,7,out)&&out.firstFrame==0&&out.bytes==384&&!memcmp(out.data,data,384));
    // 21ms between 4ms packets retains 17ms of missing time.
    assert(!picasso_input_append(q,data,384,121000,0x80000000,2));
    assert(q.status.gapFrames==816);
    assert(!picasso_input_read(q,7,out)&&out.firstFrame==1008);
    assert(picasso_input_append(q,data,384,120000,0x80000000,3)==-71);
    assert(!picasso_input_append(q,data,4096,125000,0x80000000,4));
    assert(!picasso_input_read(q,7,out)&&out.bytes==3840&&out.firstFrame==1200);
    assert(!picasso_input_read(q,7,out)&&out.bytes==256&&out.firstFrame==3120&&!memcmp(out.data,data+3840,256));
    assert(picasso_input_read(q,7,out)==-11);
    for(unsigned i=0;i<PicassoAudioInputPackets;++i)assert(!picasso_input_append(q,data,384,200000+i*4000,0x80000000,5));
    assert(picasso_input_append(q,data,384,400000,0x80000000,5)==-28&&q.status.overruns==1);
    assert(picasso_input_stop(q,8)==-1);assert(!picasso_input_stop(q,7)&&!q.count);
    for(const auto &p:q.packet)for(auto b:p.data)assert(!b);
    picasso_input_retire(q,true,0);assert(!picasso_input_begin(q,8,f)&&q.status.epoch==2&&!q.status.gapFrames);
    picasso_input_retire(q,false,-5);assert(picasso_input_begin(q,7,f)==-6);
}
int main() {
    queueTests();PicassoAprProbe probe={};picasso_mic_service_init(probe);
    assert(!probe.capture.enabled&&probe.pcm.capture==&probe.capture.dma&&!hardwareCalls);
    for(unsigned i=0;i<1000;++i)picasso_mic_service_tick(probe,false);
    assert(!probe.capture.attempted&&!probe.capture.enabled&&!hardwareCalls);
    assert(hardware(nullptr,0)==-11&&call.queued);assert(hardware(nullptr,0)==-11);runHardware();assert(hardware(nullptr,0)==0);
    hardwareResult=-5;assert(hardware(nullptr,3)==-11);runHardware();assert(hardware(nullptr,3)==-5);hardwareResult=0;
    assert(hardware(nullptr,0)==-11&&hardware(nullptr,2)==-11);runHardware();assert(hardware(nullptr,2)==-11);runHardware();assert(!hardware(nullptr,2));
    probe.ready=probe.fw.valid=probe.pcm.done=probe.pcm.complete=probe.capture.dma.ready=true;
    picasso_mic_service_tick(probe,false);const PicassoAudioFormat f={1,48000,1,16};
    assert(!picasso_mic_service_call(5,PicassoAudioInputBegin,&f,sizeof f,nullptr,0));
    picasso_mic_service_tick(probe,false);assert(probe.capture.enabled&&active);
    uint8_t bytes[384]={};assert(!sink(bytes,sizeof bytes,100,0x80000000));
    PicassoAudioInputData packet;assert(!picasso_mic_service_call(5,PicassoAudioInputRead,nullptr,0,&packet,sizeof packet));
    picasso_mic_service_close(6);assert(!input.cancel);picasso_mic_service_close(5);assert(input.cancel);
    picasso_mic_service_tick(probe,false);assert(!active&&input.status.state==PicassoAudioIdle&&!input.owner);
    assert(!picasso_mic_service_call(6,PicassoAudioInputBegin,&f,sizeof f,nullptr,0));picasso_mic_service_tick(probe,false);assert(active);
    picasso_mic_service_tick(probe,true);assert(!active&&input.status.state==PicassoAudioFailed);
    for(const auto &l:logs)assert(l.find("pcm offset=")==std::string::npos);
    puts("PASS on-demand capture, timestamp gaps, ownership, bounded PCM, stop and failure");
}
