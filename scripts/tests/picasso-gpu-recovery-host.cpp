// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "NativeAdrenoA620HwInit.h"
using IOReturn=int;
enum {kIOReturnSuccess=0,kIOReturnNotReady=-1,kIOReturnIOError=-2,kIOReturnTimeout=-3};
static constexpr uint32_t kGmuGxPwrStatus=0x1f88d, kGxOffBits=0xc0;
static std::vector<std::string> events;
static std::string failure;
static bool held,off,halted,asserted,resetDone,tableDone,initialized,probeDone,disabled;
static unsigned resetDelay,releases,resets;
static void event(const char *s){events.emplace_back(s);}
struct CpState {unsigned next=999,sequence=26;};
static CpState state,*gState=&state;
static uint32_t gmuRead(uint32_t r){assert(r==kGmuGxPwrStatus);return off?0xc0:0;}
static uint32_t gpuRead(uint32_t){assert(held&&!off);return 0;}
static void gpuWrite(uint32_t reg,uint32_t value) {
    assert(held&&!off);
    if(reg==A620_CP_SQE_CNTL){halted=value==3;event(halted?"halt":"start");}
    if(reg==0x43){
        if(value){assert(halted);asserted=true;resetDelay=0;event("reset-on");}
        else {assert(asserted&&resetDelay>=100);asserted=false;resetDone=true;++resets;event("reset-off");}
    }
}
static void barrier(){}
static void IODelay(unsigned us){if(asserted)resetDelay+=us;}
static int native_adreno620_gmu_acquire_gpu_set(){event("hold");if(failure=="hold")return -1;held=true;return 0;}
static int native_adreno620_gmu_restart(){event("cold");if(failure=="cold")return -1;held=true;off=false;return 0;}
static void native_adreno620_gmu_release_gpu_set(){assert(held);held=false;++releases;event("release");}
struct NativeAdrenoVm {uint64_t ttbr0(){return 0x100001000ULL;}};
struct NativeAdrenoGpuContext {
    NativeAdrenoVm memory;
    NativeAdrenoVm *vm(){return &memory;}
    IOReturn writeTtbr0(uint64_t t){assert(resetDone&&t==memory.ttbr0());event("table");tableDone=true;return failure=="table"?-2:0;}
};
static NativeAdrenoGpuContext context;
static NativeAdrenoGpuContext *NativeAdrenoGpuContextObject(){return failure=="context"?nullptr:&context;}
struct NativeAdrenoRing {
    IOReturn busHalt(bool gx,unsigned){assert(held&&!gx&&halted&&!resetDone);event("bus");return failure=="bus"?-3:0;}
    IOReturn restart(uint32_t n){assert(resetDone&&tableDone&&n==27);event("ring");return failure=="ring"?-2:0;}
    IOReturn resync(uint32_t n){assert(probeDone&&n==27);event("resync");return failure=="resync"?-2:0;}
    unsigned writePointer(){return 20;}
};
static bool hardwareInit(){assert(resetDone&&tableDone);event("hardware");return failure!="hardware";}
static bool uploadSqe(){event("upload");return failure!="upload";}
static bool programCp(){event("program");return failure!="program";}
static bool cpInit(){assert(!halted&&state.next==0);event("init");initialized=true;return failure!="init";}
static bool loadZap(){event("zap");return failure!="zap";}
static bool leaveSecureMode(){assert(initialized);event("secure");return failure!="secure";}
static bool restartProbe(){event("probe");probeDone=true;return failure!="probe";}
static bool PE_parse_boot_argn(const char *,void *v,int){*static_cast<uint32_t *>(v)=disabled?0:1;return true;}
#include "recovery.inc"
static void setup(const char *fail,bool cold=false) {
    failure=fail;events.clear();held=halted=asserted=resetDone=tableDone=initialized=probeDone=disabled=false;
    resetDelay=releases=resets=0;off=cold;state={};gState=&state;
}
int main() {
    NativeAdrenoRing ring;
    for(bool cold:{false,true}) {
        setup("",cold);assert(native_adreno620_cp_restart(nullptr,&ring,27)==0);
        assert(resets==1&&releases==1&&!held&&probeDone&&state.next==20&&state.sequence==27);
        std::vector<std::string> expected={cold?"cold":"hold","halt","bus","reset-on","reset-off","table",
            "hardware","upload","program","ring","start","init","zap","secure","probe","resync","release"};
        assert(events==expected);
    }
    for(const char *f:{"hold","bus","context","table","hardware","upload","program","ring","init","zap","secure","probe","resync"}) {
        setup(f);assert(native_adreno620_cp_restart(nullptr,&ring,27)!=0);
        assert(!held&&releases==(failure=="hold"?0u:1u));
        if(failure!="hold")assert(halted);
        if(failure=="bus")assert(!resets&&!tableDone);
        if(failure=="zap"||failure=="secure")assert(!probeDone);
    }
    setup("cold",true);assert(native_adreno620_cp_restart(nullptr,&ring,27)!=0&&off&&!releases&&!resets);
    setup("");gState=nullptr;assert(native_adreno620_cp_restart(nullptr,&ring,27)!=0&&events.empty());
    setup("zap");assert(!initializeSecureMode()&&events==std::vector<std::string>{"zap"});
    setup("");disabled=true;assert(initializeSecureMode()&&events.empty());
    puts("PASS warm/cold reset order, 14 injected recovery failures, no premature ring reuse and secure-start refusal");
}
