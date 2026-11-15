// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "PicassoGlink.h"
#include "PicassoCaptureDma.h"
static std::vector<std::string> logs;
static PicassoCaptureSamples samples;
static unsigned hardwareCalls;
static int hardwareResult;
struct IOLock {bool held;};
static IOLock lock;
static IOLock *IOLockAlloc(){return &lock;}
static void IOLockLock(IOLock *p){assert(!p->held);p->held=true;}
static void IOLockUnlock(IOLock *p){assert(p->held);p->held=false;}
using thread_call_param_t=void *;
struct Call {void (*function)(void *,void *);bool queued;};
using thread_call_t=Call *;
static Call call;
static thread_call_t thread_call_allocate(void (*function)(void *,void *),void *){call.function=function;return &call;}
static bool thread_call_enter(thread_call_t c){assert(!c->queued);c->queued=true;return false;}
static void runHardware(){assert(call.queued);call.queued=false;call.function(nullptr,nullptr);}
static void IOLog(const char *fmt,...) {
    char text[1024];va_list args;va_start(args,fmt);int n=vsnprintf(text,sizeof text,fmt,args);va_end(args);
    assert(n>0 && n<256 && text[n-1]=='\n');logs.emplace_back(text);
}
extern "C" int picasso_mic_hardware(unsigned){assert(!lock.held);++hardwareCalls;return hardwareResult;}
extern "C" const PicassoCaptureSamples *picasso_capture_samples_snapshot(){return &samples;}
#include "PicassoMicService-body.inc"
int main(int argc,char **argv) {
    assert(argc==2);unsigned mode=unsigned(argv[1][0]-'0');PicassoAprProbe probe={};
    picasso_mic_service_configure(probe,mode);
    if(mode!=1 && mode!=2) {
        assert(!probe.capture.enabled && !probe.pcm.capture && !hardwareCalls && logs.empty());
        picasso_mic_service_tick(probe,false);assert(logs.empty());
    } else {
        assert(probe.capture.enabled && probe.pcm.capture==&probe.capture.dma && !hardwareCalls);
        probe.capture.attempted=true;probe.capture.done=true;probe.capture.complete=true;probe.capture.events=1;
        uint8_t pcm[130];for(unsigned i=0;i<sizeof pcm;++i)pcm[i]=uint8_t(i);
        assert(!picasso_capture_samples_append(samples,pcm,sizeof pcm));
        for(unsigned i=0;i<7;++i)picasso_mic_service_tick(probe,false);
        unsigned rows=0,ends=0,summaries=0;
        for(const auto &line:logs) {rows+=line.find("PICASSO_MIC pcm offset=")!=std::string::npos;ends+=line.find("pcm-end")!=std::string::npos;summaries+=line.find("PICASSO_MIC capture ")!=std::string::npos;}
        assert(summaries==1 && rows==(mode==2?3U:0U) && ends==(mode==2?1U:0U));
        assert(!hardwareCalls);
        assert(hardware(nullptr,0)==-11 && call.queued && !hardwareCalls);
        assert(hardware(nullptr,0)==-11 && !hardwareCalls);runHardware();
        assert(hardware(nullptr,0)==0 && hardwareCalls==1);
        hardwareResult=-5;assert(hardware(nullptr,3)==-11);runHardware();
        assert(hardware(nullptr,3)==-5 && hardwareCalls==2);hardwareResult=0;
        assert(hardware(nullptr,0)==-11 && hardware(nullptr,2)==-11);runHardware();
        assert(hardware(nullptr,2)==-11);runHardware();assert(hardware(nullptr,2)==0 && hardwareCalls==4);
        for(const auto &line:logs)fputs(line.c_str(),stdout);
    }
    puts("PASS optional capture reporting and bounded exact PCM export");
}
