// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
static constexpr uint32_t kGmuGmu2HostIntrClr=1,kGmuHost2GmuIntrSet=2,kGmuGmu2HostIntrInfo=3,kGmuGxPwrStatus=4;
struct GmuState {bool booted=true,hfiStarted=true;};
static GmuState state,*gState=&state;
static unsigned ticks,requestTick,ackAfter,powerAfter,clears,requests,releases,mode;
static bool stale;
static void coreWrite(uint32_t reg,uint32_t v) {
    if(reg==kGmuGmu2HostIntrClr){assert(v==(1u<<31));stale=false;++clears;}
    else {assert(reg==kGmuHost2GmuIntrSet);if(v==(1u<<30)){assert(clears==1);requestTick=ticks;++requests;}else{assert(v==(1u<<31));++releases;}}
}
static uint32_t coreRead(uint32_t reg) {
    if(reg==kGmuGmu2HostIntrInfo)return stale||(!mode&&ticks-requestTick>=ackAfter)?1u<<31:0;
    assert(reg==kGmuGxPwrStatus);return powerAfter>ticks-requestTick?0xc0:0;
}
static void barrier(){}
static void IODelay(unsigned us){assert(us==100);ticks+=us;}
#include "gpu-set.inc"
static void setup(){state={};gState=&state;ticks=requestTick=clears=requests=releases=mode=0;ackAfter=300;powerAfter=600;stale=true;}
int main(){
    setup();assert(!native_adreno620_gmu_acquire_gpu_set()&&ticks==600&&requests==1&&clears==2&&!releases);
    native_adreno620_gmu_release_gpu_set();assert(releases==1);
    setup();mode=1;assert(native_adreno620_gmu_acquire_gpu_set()==-1&&ticks==100000&&releases==1);
    setup();powerAfter=1000000;assert(native_adreno620_gmu_acquire_gpu_set()==-1&&ticks==100300&&releases==1);
    setup();state.hfiStarted=false;assert(native_adreno620_gmu_acquire_gpu_set()==-1&&!requests&&!releases);
    setup();gState=nullptr;assert(native_adreno620_gmu_acquire_gpu_set()==-1&&!requests);native_adreno620_gmu_release_gpu_set();
    puts("PASS fresh GPU_SET ACK, power settle, stale ACK rejection, bounded timeouts and failed-request cleanup");
}
