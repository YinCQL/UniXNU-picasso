// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute the real boot coordinator; replace only hardware/transport boundaries.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "PicassoAdspBoot.h"
#include "PicassoAdspLoadPlan.h"
#include "PicassoRpmh.h"
#include "PicassoPas.h"
static const char *mode;
static unsigned maps,unmaps,mdrefs,leases,releases,opens,closes,loads,polls,auths,shutdowns,on,off,finishes,writes,snapshots;
static bool owned,poison;
static uint64_t ticks;
static uint8_t meta[0x3000], image[64], expected[picassoAdspMetadataBytes];
static unsigned commands[8],commandCount,addresses[32],values[32];
static bool is(const char *s){return !strcmp(mode,s);}
struct boot_args { uint64_t physBase,memSize; } args={0xc007c000,0x13ca00000};
struct {void *bootArgs;} PE_state={&args};
enum {kIOMapAnywhere=1,kIOMapInhibitCache=2,kSecondScale=1};
class IOMemoryMap {
public:
    uintptr_t address;
    uintptr_t getVirtualAddress(){return address;}
    void release(){++unmaps;}
};
class IODeviceMemory {
public:
    unsigned slot;
    static IODeviceMemory *withRange(uint64_t base,uint64_t size) {
        assert((base==0x8bc18000 && size==sizeof meta) || (base==picassoAdspBase && size==picassoAdspBytes));
        ++mdrefs;static IODeviceMemory m[2];unsigned slot=base==picassoAdspBase;m[slot].slot=slot;
        if((is("alloc1")&&!slot)||(is("alloc2")&&slot))return nullptr;
        return &m[slot];
    }
    IOMemoryMap *map(unsigned flags) {
        assert(flags==3);++maps;static IOMemoryMap m[2];m[slot].address=reinterpret_cast<uintptr_t>(slot?image:meta);
        if((is("map1")&&!slot)||(is("map2")&&slot))return nullptr;
        if((is("zero1")&&!slot)||(is("zero2")&&slot))m[slot].address=0;
        return &m[slot];
    }
    void release(){--mdrefs;}
};
static void IOLog(const char *fmt,...) {va_list a;va_start(a,fmt);vprintf(fmt,a);va_end(a);}
static void IOSleep(unsigned ms){ticks+=ms;assert(ticks<45000);}
static uint64_t mach_absolute_time(){return ticks;}
static void clock_interval_to_deadline(unsigned n,unsigned scale,uint64_t *out){assert(scale==1);*out=ticks+n*1000;}
#include "PicassoAdspBoot-body.inc"
extern "C" int picasso_pas_acquire(){++leases;assert(!owned);if(is("lease") || (is("reacquire")&&leases>1))return -16;owned=true;return 0;}
extern "C" void picasso_pas_release(bool uncertain){assert(owned);owned=false;poison=uncertain;++releases;}
extern "C" int picasso_rpmh_adsp_resources(PicassoArcResource *rails){
    for(unsigned i=0;i<3;++i)rails[i]={0x30030+16*i,i==2?3U:7-i,384};
    return is("plan")?-22:0;
}
extern "C" int picasso_rpmh_crypto_ready(){assert(!owned);return is("crypto")?-5:0;}
extern "C" int picasso_rpmh_active_write(uint32_t a,uint32_t v){
    assert(writes<32);addresses[writes]=a;values[writes]=v;++writes;
    if((is("vote1")&&writes==1)||(is("vote2")&&writes==2)||(is("vote3")&&writes==3)||
       (is("shutdown-vote")&&writes==4))return -60;
    return 0;
}
extern "C" PicassoAdspImage *picasso_adsp_image_open(int *error){++opens;*error=is("firmware")?-8:0;return *error?nullptr:reinterpret_cast<PicassoAdspImage *>(image);}
extern "C" const void *picasso_adsp_image_metadata(PicassoAdspImage *){return expected;}
extern "C" void picasso_adsp_image_close(PicassoAdspImage *){++closes;}
extern "C" int picasso_adsp_image_load(PicassoAdspImage *,volatile uint8_t *to,size_t n){
    assert(owned && to==image && n==picassoAdspBytes && commandCount==2);++loads;return is("load")?-8:0;
}
extern "C" int picasso_aop_load_state(bool enabled){
    if(enabled){++on;return is("aop")?-22:is("aop-uncertain")?-115:0;}
    ++off;assert(!auths || shutdowns);return is("off")?-115:0;
}
extern "C" int picasso_aop_finish(){++finishes;return is("close")?-60:0;}
extern "C" int picasso_pas_adsp(unsigned cmd,uint64_t a,uint64_t n){
    assert(owned && commandCount<8);commands[commandCount++]=cmd;
    if(cmd==1){assert(a==0x8bc18000&&!n&&!memcmp(meta,expected,sizeof expected));for(unsigned i=sizeof expected;i<sizeof meta;++i)assert(!meta[i]);}
    if(cmd==2)assert(a==picassoAdspBase && n==picassoAdspBytes);
    if(cmd==5){assert(loads==1);++auths;}
    if(cmd==6)++shutdowns;
    if((is("init")&&cmd==1)||(is("setup")&&cmd==2)||(is("auth")&&cmd==5)||(is("shutdown")&&cmd==6))return -13;
    if(is("interrupted")&&cmd==1)return -115;
    return 0;
}
static int poll(void *,PicassoSmp2pEvent *e){
    ++polls;*e={};
    if(polls==1){if(is("stale")){e->hasValue=true;e->value=2;return 0;}return -2;}
    if(!auths)return is("pre-auth-peer")?0:-2;
    if(is("timeout"))return -2;
    if(is("fatal")){e->hasValue=true;e->value=1;return 0;}
    if(is("initial-restart")){e->restart=true;e->flags=1;return 0;}
    if(polls==3){e->hasValue=true;e->value=2;e->changed=2;return 0;}
    if(is("restart-no-entry")){e->restart=true;return 0;}
    if(is("lost-ready")){e->hasValue=true;return 0;}
    if(is("missing"))return -2;
    if(is("missing-entry"))return 0;
    e->hasValue=true;e->value=is("no-handover")?2:6;e->changed=polls==4?4:0;
    if(is("long-ready")){if(ticks>=31000)e->value|=1;}
    else if(polls>=6){if(is("restart"))e->restart=true;else e->value|=1;}
    return 0;
}
static int snapshot(void *){++snapshots;return -2;}
int main(int argc,char **argv){
    assert(argc==2);mode=argv[1];memset(expected,0x5a,sizeof expected);
    if(is("memory-overlap"))args.physBase=0x89200000;
    if(is("memory-missing"))PE_state.bootArgs=nullptr;
    PicassoAdspEvents events={nullptr,poll,snapshot};picasso_adsp_boot(&events);picasso_adsp_boot(&events);
    assert(!owned && opens<=1 && closes==(opens && !is("firmware")));
    assert(releases==(is("lease")?0U:is("reacquire")?1U:leases));
    bool quarantine=is("interrupted")||is("shutdown")||is("aop-uncertain")||is("off")||is("close")||is("shutdown-vote")||is("reacquire")||is("vote1")||is("vote2")||is("vote3");
    assert(poison==(quarantine&&!is("reacquire")));
    if(quarantine){assert(!unmaps);if(is("interrupted")||is("aop-uncertain"))assert(!shutdowns&&!off);}
    else assert(unmaps==maps-unsigned(is("map1")||is("map2")));
    if(is("stale"))assert(!opens&&!on&&!commandCount);
    if(is("pre-auth-peer")||is("load"))assert(loads==1&&!auths&&shutdowns==1);
    if(is("timeout"))assert(ticks==10000 && shutdowns==1);
    if(is("normal")||is("restart")||is("missing")||is("missing-entry")||is("no-handover"))assert(auths==1&&shutdowns==1&&on==1&&off==1&&!poison);
    // Never lower inherited APPS requests on handover or shutdown.
    for(unsigned i=0;i<writes;++i)assert(values[i]==(addresses[i]==0x30050?3U:addresses[i]==0x30030?7U:6U));
    if(is("memory-overlap")||is("memory-missing"))assert(!maps&&!on&&!writes&&!commandCount);
    if(is("initial-restart")||is("restart-no-entry")||is("lost-ready"))assert(polls<=4 && shutdowns==1);
    if(is("long-ready"))assert(snapshots==4 && ticks==31000 && shutdowns==1);
    puts("PASS production ADSP transaction ordering, failure cleanup, ownership and stale/ready/fatal handling");
}
