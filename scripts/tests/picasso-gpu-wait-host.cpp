// SPDX-License-Identifier: MIT
#include <cassert>
#include <cstdint>
#include <cstdio>
static unsigned sleeps,elapsed,reads;
static uint32_t fence;
static bool readFailed;
static void IOSleep(unsigned ms){++sleeps;elapsed+=ms;}
struct Ring {int fenceValue(uint32_t *v){++reads;*v=fence;return readFailed?1:0;}};
static Ring ring;
static uint64_t now(void *){return uint64_t(elapsed)*1000;}
static struct {bool published;Ring *ring;struct {uint64_t (*now_us)(void *);void *cookie;} clock;} g{true,&ring,{now,nullptr}};
static volatile uint32_t native_adreno_tl_cap;
static volatile uint64_t native_adreno_fence_done[64][2];
#include "wait.inc"
int main(){
    // One minute of normal service: 120 watchdog waits, no diagnostic fence reads.
    for(unsigned i=0;i<120;++i)nativeAdrenoHangcheckWait();
    assert(elapsed==60000 && sleeps==120 && reads==0);
    native_adreno_tl_cap=16384;fence=3;nativeAdrenoHangcheckWait();
    assert(elapsed==60500 && sleeps==620 && reads==500);
    for(unsigned i=1;i<=3;++i)assert(native_adreno_fence_done[i][0]==i && native_adreno_fence_done[i][1]==60001000);
    const uint64_t stamp=native_adreno_fence_done[3][1];nativeAdrenoHangcheckWait();
    assert(native_adreno_fence_done[3][1]==stamp);
    readFailed=true;fence=4;nativeAdrenoHangcheckWait();assert(!native_adreno_fence_done[4][0]);
    readFailed=false;nativeAdrenoHangcheckWait();assert(native_adreno_fence_done[4][0]==4);
    native_adreno_tl_cap=0;unsigned previous=reads;nativeAdrenoHangcheckWait();assert(reads==previous);
    native_adreno_tl_cap=16384;g.published=false;nativeAdrenoHangcheckWait();assert(reads==previous);
    g.published=true;g.ring=nullptr;nativeAdrenoHangcheckWait();assert(reads==previous);
    puts("PASS normal 120 wakes/minute versus old 60000; diagnostic completion, duplicate, read failure and absent ring");
}
