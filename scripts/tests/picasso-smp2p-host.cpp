// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "PicassoSmp2p.h"
#include "PicassoSmemLock.h"
static uint8_t ram[WLAN_SMEM_SIZE], before[WLAN_SMEM_SIZE];
static uint32_t part=0x1cf000, partSize=0x30000, toc=WLAN_SMEM_SIZE-4096;
static unsigned reads,writes,failRead,failWrite,barriers;
static bool held=true;
static void p16(unsigned at,unsigned v){ram[at]=uint8_t(v);ram[at+1]=uint8_t(v>>8);}
static void p32(unsigned at,unsigned v){p16(at,v);p16(at+2,v>>16);}
static unsigned r32(unsigned at){return wlan_sm32(ram+at);}
static int readMem(void *,uint32_t at,void *to,uint32_t n) {
    assert(at<=sizeof ram && n<=sizeof ram-at);if(++reads==failRead)return -5;
    memcpy(to,ram+at,n);return 0;
}
static int writeMem(void *,uint32_t at,uint32_t v) {
    assert(!(at&3) && at>=part && at<part+partSize);
    ++writes;if(writes==failWrite)return -5;
    if(at==part+12) {
        assert(held && barriers>=2);
        assert(r32(part+32)==(0xa5a5|(429U<<16)));
        assert(r32(part+32+16+4)==0x101);
    }
    p32(at,v);return 0;
}
static void fence(void *){++barriers;}
static bool locked(void *){return held;}
static PicassoSmemIo io={{nullptr,readMem},writeMem,fence,locked};
static void setup() {
    memset(ram,0,sizeof ram);reads=writes=failRead=failWrite=barriers=0;held=true;
    p32(0x5c,12U<<16);p32(toc,0x434f5424);p32(toc+4,1);p32(toc+8,1);
    p32(toc+32,part);p32(toc+36,partSize);p16(toc+44,0);p16(toc+46,2);p32(toc+48,64);
    p32(part,0x54525024);p16(part+4,0);p16(part+6,2);p32(part+8,partSize);
    p32(part+12,32);p32(part+16,partSize);
}
static unsigned inbound(unsigned features=1,unsigned value=2,unsigned flags=0) {
    unsigned at=part+r32(part+12);p16(at,0xa5a5);p16(at+2,443);p32(at+4,344);p16(at+8,4);
    p32(part+12,r32(part+12)+360);at+=16;
    p32(at,0x504d5324);p32(at+4,1|(features<<8));p32(at+8,2);p32(at+12,16|(1U<<16));p32(at+16,flags);
    memcpy(ram+at+20,"slave-kernel",13);p32(at+36,value);return at;
}
static void tests() {
    PicassoSmp2p s={};PicassoSmp2pEvent e={};
    setup();held=false;assert(picasso_smp2p_prepare(&io,&s)==-16 && !writes);
    setup();assert(!picasso_smp2p_prepare(&io,&s));assert(s.created && s.out.offset==part+48 && s.out.size==340);
    assert(r32(part+12)==392);unsigned maxReads=reads,maxWrites=writes;
    assert(!memcmp(ram+s.out.offset+20,"master-kernel",14));
    for(unsigned i=s.out.offset+40;i<s.out.offset+344;++i)assert(!ram[i]);
    held=false;assert(picasso_smp2p_service(&io,&s,&e)==-2);
    unsigned in=inbound();assert(!picasso_smp2p_service(&io,&s,&e));assert(e.value==2 && e.changed==2 && !e.restart);
    assert(!picasso_smp2p_service(&io,&s,&e) && !e.changed && !e.kick);
    p32(in+36,7);assert(!picasso_smp2p_service(&io,&s,&e) && e.changed==5 && e.value==7);
    p32(in+16,1);assert(!picasso_smp2p_service(&io,&s,&e) && e.restart && e.kick && r32(s.out.offset+16)==2);
    p32(in+16,0);assert(!picasso_smp2p_service(&io,&s,&e) && e.restart && r32(s.out.offset+16)==0);
    p32(in+4,1);assert(picasso_smp2p_service(&io,&s,&e)==-57);
    setup();assert(!picasso_smp2p_prepare(&io,&s));in=inbound(0);held=false;
    assert(!picasso_smp2p_service(&io,&s,&e) && e.kick && s.features==0 && r32(s.out.offset+4)==1);
    // Mutating our single-writer state is not accepted as a new owner/session.
    p32(s.out.offset+36,1);assert(picasso_smp2p_service(&io,&s,&e)==-22);
    setup();assert(!picasso_smp2p_prepare(&io,&s));in=inbound(1,2,1);p32(in+12,16);held=false;
    assert(!picasso_smp2p_service(&io,&s,&e) && !e.hasValue && e.restart && e.kick && s.negotiated);
    p32(in+12,16|(1U<<16));assert(!picasso_smp2p_service(&io,&s,&e) && e.hasValue && e.value==2 && e.changed==2);
    // Existing, including malformed or cached items, must never be overwritten.
    setup();assert(!picasso_smp2p_prepare(&io,&s));memcpy(before,ram,sizeof ram);writes=0;
    assert(picasso_smp2p_prepare(&io,&s)==-17 && !writes && !memcmp(before,ram,sizeof ram));
    setup();unsigned ca=part+partSize-64;p16(ca,0xa5a5);p16(ca+2,429);p32(ca+4,384);p16(ca+8,44);p32(part+16,partSize-448);
    assert(picasso_smp2p_prepare(&io,&s)==-17 && !writes);
    setup();p16(toc+46,1);p16(part+6,1);assert(picasso_smp2p_prepare(&io,&s)==-2 && !writes);
    setup();ca=part+partSize-64;p16(ca,0xa5a5);p16(ca+2,99);p32(ca+4,partSize-448);p32(part+16,384);
    assert(picasso_smp2p_prepare(&io,&s)==-28 && !writes);
    setup();p32(toc+8,2);p32(toc+80,part+0x100);p32(toc+84,0x1000);p16(toc+92,0);p16(toc+94,1);
    assert(picasso_smp2p_prepare(&io,&s)==-22 && !writes);
    setup();p32(toc+80,0x49494953);p32(toc+84,WLAN_SMEM_SIZE);p32(toc+88,0x80900000);p16(toc+96,400);
    assert(picasso_smp2p_prepare(&io,&s)==-22 && !writes);
    setup();p32(part+12,56);p16(part+32,0);assert(picasso_smp2p_prepare(&io,&s)==-22 && !writes);
    // Inject every allocation read/write failure. Before commit the free mark
    // remains unchanged; after commit it is never decremented or freed.
    for(unsigned n=1;n<=maxReads;++n) {setup();failRead=n;assert(picasso_smp2p_prepare(&io,&s));assert(r32(part+12)==32 || r32(part+12)==392);}
    for(unsigned n=1;n<=maxWrites;++n) {setup();failWrite=n;assert(picasso_smp2p_prepare(&io,&s));assert(r32(part+12)==32);}
    setup();assert(!picasso_smp2p_prepare(&io,&s));in=inbound();held=false;reads=0;
    assert(!picasso_smp2p_service(&io,&s,&e));unsigned serviceReads=reads;
    for(unsigned n=1;n<=serviceReads;++n) {
        setup();assert(!picasso_smp2p_prepare(&io,&s));inbound();held=false;reads=0;failRead=n;
        assert(picasso_smp2p_service(&io,&s,&e));
    }
    printf("PASS SMP2P: locked allocation, publication, existing/cached refusal, identity/overlap, %u reads/%u writes and %u peer read faults, restart\n",maxReads,maxWrites,serviceReads);
}
static uint32_t owner;
static unsigned lockWrites;
static bool raced;
static uint32_t lr(void *){return owner;}
static void lw(void *,uint32_t value){++lockWrites;if(!value)owner=0;else if(!owner)owner=raced?2:value;}
int main() {
    tests();PicassoSmemLockIo l={nullptr,lr,lw,fence};
    owner=1;lockWrites=0;assert(picasso_smem_trylock(&l)==-16 && !lockWrites);
    owner=2;assert(picasso_smem_trylock(&l)==-11 && !lockWrites);assert(picasso_smem_unlock(&l)==-16 && !lockWrites);
    owner=0;raced=true;assert(picasso_smem_trylock(&l)==-11 && owner==2);
    owner=0;raced=false;assert(!picasso_smem_trylock(&l) && owner==1);assert(!picasso_smem_unlock(&l)&&!owner);
    puts("PASS TCSR mutex: local/stale owner refusal, remote contention/race and ownership-checked unlock");
}
