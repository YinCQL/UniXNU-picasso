#include "../../drivers/NativeAdreno/NativeAdrenoProfile.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>
extern "C" {
uint32_t native_adreno_profile_enabled;
uint64_t (*native_adreno_vm_now_us)(void);
volatile uint64_t native_adreno_profile_phases[NativeProfilePhaseCount][3];
volatile uint64_t native_adreno_profile_counters[NativeProfileCounterCount];
}
static thread_local uint64_t now=100;
static unsigned clock_reads;
static uint64_t clock_now() {++clock_reads;return now;}
static uint64_t thread_clock() {return now;}
int main()
{
    native_adreno_vm_now_us=clock_now;
    {NativeAdrenoProfileTimer off(NativeProfileCall);now=120;}
    nativeAdrenoProfileAdd(NativeProfilePublishVisited,7);
    assert(clock_reads==0 && native_adreno_profile_counters[NativeProfilePublishVisited]==0);
    native_adreno_profile_enabled=1;
    now=100;
    {NativeAdrenoProfileTimer t(NativeProfilePublish);now=125;t.finish();now=130;}
    assert(clock_reads==2);
    assert(native_adreno_profile_phases[NativeProfilePublish][0]==1);
    assert(native_adreno_profile_phases[NativeProfilePublish][1]==25);
    assert(native_adreno_profile_phases[NativeProfilePublish][2]==25);
    {NativeAdrenoProfileTimer parent(NativeProfileBody);now=135;
     {NativeAdrenoProfileTimer child(NativeProfileRing);now=138;}now=141;}
    assert(native_adreno_profile_phases[NativeProfileBody][1]==11);
    assert(native_adreno_profile_phases[NativeProfileRing][1]==3);
    {NativeAdrenoProfileTimer inactive(NativeProfileCall,false);now=150;}
    assert(native_adreno_profile_phases[NativeProfileCall][0]==0);
    native_adreno_vm_now_us=thread_clock;
    std::vector<std::thread> workers;
    for(unsigned i=0;i<4;++i)workers.emplace_back([] {
        for(unsigned j=0;j<5000;++j) {now=200;NativeAdrenoProfileTimer timer(NativeProfileAudit);now=207;
            nativeAdrenoProfileAdd(NativeProfileAcquireVisited,2);}
    });
    for(auto &worker:workers)worker.join();
    assert(native_adreno_profile_phases[NativeProfileAudit][0]==20000);
    assert(native_adreno_profile_phases[NativeProfileAudit][1]==140000);
    assert(native_adreno_profile_phases[NativeProfileAudit][2]==7);
    assert(native_adreno_profile_counters[NativeProfileAcquireVisited]==40000);
    native_adreno_profile_enabled=0;
    std::puts("PASS: disabled clock, scoped phases, explicit finish, nesting, and concurrent accumulation");
}
