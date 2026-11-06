// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real shared lease/probe and GPU resume lambda. Only SMC and register asm are
// substituted on host; the original register assembly is compiled for ARM64.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "PicassoPas.h"
using ScmReply=PicassoPasReply;
static unsigned sleeps,calls,mode,dumps;
static bool waitingRelease;
static void IOSleep(unsigned ms);
#include "PicassoPas-lease.inc"
static void IOSleep(unsigned ms) {
    assert(ms==1);++sleeps;
    if(waitingRelease){waitingRelease=false;picasso_pas_release(false);}
}
static ScmReply scmSipCall(uint32_t fn,uint32_t,uint64_t,uint64_t=0,
                          uint64_t=0,uint64_t x5=0,uint64_t x6=0) {
    ++calls;assert(!x5);
    if(fn==1)assert(x6==0xaabbccdd00000000ULL+calls-1);
    else assert(!x6);
    if(mode==2 || (mode==1 && calls<4))return {1,0,0,0,0xaabbccdd00000000ULL+calls};
    return {};
}
#include "PicassoPas-probe.inc"
int main() {
    assert(!picasso_pas_acquire());
    assert(picasso_pas_acquire()==-16 && sleeps==30000 && pasGate);
    waitingRelease=true;assert(!picasso_pas_acquire() && sleeps==30001);
    picasso_pas_release(true);assert(picasso_pas_acquire()==-115 && !pasGate);
    pasPoisoned=0;
    for(mode=0;mode<3;++mode) {
        calls=sleeps=0;pasPoisoned=0;scmProbe();
        assert(!pasGate && pasPoisoned==(mode==2));
        assert(calls==(mode==2?65U:mode==1?9U:6U));
    }
    struct {bool uncertain=false;} lease;
    auto dumpGic=[](){++dumps;};
#include "PicassoPas-gpu.inc"
    for(mode=0;mode<3;++mode) {
        calls=sleeps=dumps=0;lease.uncertain=false;
        auto r=pasCall(0x42000201,0x82,13,0x8bc18000,0,true);
        assert(lease.uncertain==(mode==2) && (r.a0==1)==(mode==2));
        assert(calls==(mode==2?64U:mode==1?4U:1U));
        assert(sleeps==calls-1 && dumps==unsigned(mode!=0));
    }
    puts("PASS actual PIL lease contention/quarantine, SCM probe and GPU 64-bit x6 resume");
}
