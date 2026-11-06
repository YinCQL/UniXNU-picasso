// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real SMEM allocator and GLINK state machine, synthetic peer and faulting I/O.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "PicassoGlink.h"
#include "PicassoAdsp.h"
static uint8_t ram[WLAN_SMEM_SIZE], before[WLAN_SMEM_SIZE];
static constexpr uint32_t part=0x1cf000, bytes=0x30000, toc=WLAN_SMEM_SIZE-4096;
static bool held;static unsigned writes,reads,failWrite,failRead,barriers;
static void p32(unsigned at,unsigned v){for(unsigned i=0;i<4;++i)ram[at+i]=uint8_t(v>>(i*8));}
static unsigned r32(unsigned at){return wlan_sm32(ram+at);}
static int rd(void *,uint32_t at,void *out,uint32_t n){assert(at<=sizeof ram && n<=sizeof ram-at);if(++reads==failRead)return -5;memcpy(out,ram+at,n);return 0;}
static int wr(void *,uint32_t at,uint32_t v){assert(at>=part && at<part+bytes && !(at&3));if(++writes==failWrite)return -5;p32(at,v);return 0;}
static void barrier(void *){++barriers;}
static bool lock(void *){return held;}
static PicassoSmemIo io={{nullptr,rd},wr,barrier,lock};
static void setup(){
    memset(ram,0,sizeof ram);writes=reads=failWrite=failRead=barriers=0;held=true;
    p32(0x5c,12U<<16);p32(toc,0x434f5424);p32(toc+4,1);p32(toc+8,1);
    p32(toc+32,part);p32(toc+36,bytes);p32(toc+44,2U<<16);p32(toc+48,64);
    p32(part,0x54525024);p32(part+4,2U<<16);p32(part+8,bytes);p32(part+12,32);p32(part+16,bytes);
}
static unsigned peer(unsigned size=16384){
    unsigned at=part+r32(part+12);p32(at,0xa5a5|(480U<<16));p32(at+4,size);p32(at+8,0);
    p32(part+12,r32(part+12)+16+size);return at+16;
}
static void message(PicassoGlink &s,unsigned rx,unsigned cmd,unsigned a,unsigned b){
    unsigned head=r32(s.desc.offset+12),size=s.rx.size?s.rx.size:16384;
    p32(rx+head,cmd|(a<<16));p32(rx+head+4,b);p32(s.desc.offset+12,(head+8)%size);
}
int main(){
    PicassoGlink s={};PicassoSmemAudit audit={};bool kick;
    setup();held=false;assert(picasso_glink_prepare(&io,&s,&audit)==-16 && !writes);
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));unsigned maxWrites=writes,maxReads=reads;
    assert(s.prepared && s.desc.offset==part+48 && s.tx.offset==part+96 && s.tx.size==16384);
    assert(r32(part+12)==16480 && barriers>=2);
    for(unsigned i=0;i<32;++i)assert(!ram[s.desc.offset+i]);
    for(unsigned i=0;i<16384;++i)assert(!ram[s.tx.offset+i]);
    memcpy(before,ram,sizeof ram);writes=0;PicassoGlink other={};
    assert(picasso_glink_prepare(&io,&other,&audit)==-17 && !writes && !memcmp(before,ram,sizeof ram));
    for(unsigned n=1;n<=maxWrites;++n){setup();failWrite=n;assert(picasso_glink_prepare(&io,&s,&audit));assert(r32(part+12)==32);}
    for(unsigned n=1;n<=maxReads;++n){setup();failRead=n;assert(picasso_glink_prepare(&io,&s,&audit));assert(r32(part+12)==32 || r32(part+12)==16480);}
    setup();p32(part+16,64);p32(part+bytes-64,0);assert(picasso_glink_prepare(&io,&s,&audit) && !writes);
    setup();p32(toc+80,0x49494953);p32(toc+84,WLAN_SMEM_SIZE);p32(toc+88,WLAN_SMEM_BASE);p32(toc+96,450);
    assert(picasso_glink_prepare(&io,&s,&audit)==-22 && !writes);
    setup();peer();assert(picasso_glink_prepare(&io,&s,&audit)==-17 && !writes);
    setup();unsigned inherited=peer();p32(inherited-16,0xa5a5|(443U<<16));
    assert(picasso_glink_prepare(&io,&s,&audit)==-16 && !writes);
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));held=false;
    assert(!picasso_glink_service(&io,&s,&kick) && kick && s.sent && !s.ready && s.txHead==8);
    assert(r32(s.tx.offset)==0x10000 && !r32(s.tx.offset+4) && !r32(s.desc.offset+8));
    assert(!picasso_glink_service(&io,&s,&kick) && !kick && s.txHead==8); // late RX FIFO
    unsigned rx=peer();message(s,rx,0,1,1);message(s,rx,1,1,0);
    assert(!picasso_glink_service(&io,&s,&kick) && kick && s.ready && s.rxTail==16 && s.commands==2);
    assert(r32(s.tx.offset+8)==0x10001 && !r32(s.tx.offset+12)); // features intersection zero
    message(s,rx,13,0,0);assert(!picasso_glink_service(&io,&s,&kick) && kick && s.rxTail==24);
    message(s,rx,2,7,14);unsigned tail=s.rxTail;memcpy(before,ram,sizeof ram);
    assert(!picasso_glink_service(&io,&s,&kick) && s.blocked && s.pendingCommand==2 && s.pendingChannel==7);
    assert(s.rxTail==tail && !memcmp(before,ram,sizeof ram)); // OPEN remains unacknowledged
    assert(!picasso_glink_service(&io,&s,&kick) && !kick && !memcmp(before,ram,sizeof ram));
    // Wrap both rings at their last complete eight-byte control record.
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));rx=peer();held=false;
    s.txHead=16376;p32(s.desc.offset+4,16376);p32(s.desc.offset,32);
    s.rxTail=16376;p32(s.desc.offset+8,16376);p32(s.desc.offset+12,16376);
    message(s,rx,0,1,0);message(s,rx,1,1,0);
    assert(!picasso_glink_service(&io,&s,&kick) && s.ready && s.txHead==8 && s.rxTail==8);
    // The peer's RX FIFO can have a different validated size.
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));rx=peer(8192);held=false;
    message(s,rx,0,1,0);message(s,rx,1,1,0);assert(!picasso_glink_service(&io,&s,&kick) && s.ready && s.rx.size==8192);
    // Backpressure does not consume an unanswered VERSION or spin.
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));rx=peer();held=false;
    assert(!picasso_glink_service(&io,&s,&kick));message(s,rx,0,1,0);p32(s.desc.offset,24);
    unsigned oldWrites=writes;assert(picasso_glink_service(&io,&s,&kick)==-11 && !s.rxTail && writes==oldWrites);
    for(unsigned n=0;n<20;++n)assert(picasso_glink_service(&io,&s,&kick)==-11 && !s.retries && !s.failed);
    p32(s.desc.offset,8);assert(!picasso_glink_service(&io,&s,&kick) && s.rxTail==8);
    // Protocol version rejection, invalid cursors, immutable local ownership.
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));rx=peer();held=false;message(s,rx,0,0,0);
    assert(picasso_glink_service(&io,&s,&kick)==-93 && s.failed && !s.rxTail);
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));rx=peer();held=false;p32(s.desc.offset+12,3);
    assert(picasso_glink_service(&io,&s,&kick)==-22 && s.failed);
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));held=false;p32(s.desc.offset+4,8);
    assert(picasso_glink_service(&io,&s,&kick)==-22 && s.failed);
    // Failed publication is terminal: never duplicate or reset an uncertain TX.
    for(unsigned n=1;n<=3;++n){setup();assert(!picasso_glink_prepare(&io,&s,&audit));held=false;writes=0;failWrite=n;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed);oldWrites=writes;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && writes==oldWrites);}
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));held=false;
    for(unsigned n=0;n<500;++n)assert(!picasso_glink_service(&io,&s,&kick));
    assert(picasso_glink_service(&io,&s,&kick)==-60 && s.failed);
    // Read failures are terminal except the SMEM scanner's retryable final
    // consistency read. A retry must not duplicate an already-published VERSION.
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));rx=peer();held=false;
    message(s,rx,0,1,0);message(s,rx,1,1,0);reads=0;assert(!picasso_glink_service(&io,&s,&kick));
    unsigned serviceReads=reads;
    for(unsigned n=1;n<=serviceReads;++n) {
        setup();assert(!picasso_glink_prepare(&io,&s,&audit));rx=peer();held=false;
        message(s,rx,0,1,0);message(s,rx,1,1,0);reads=0;failRead=n;
        int result=picasso_glink_service(&io,&s,&kick);oldWrites=writes;
        if(s.failed){assert(result);assert(picasso_glink_service(&io,&s,&kick) && writes==oldWrites);}
        else {assert(!result && !s.commands && s.txHead==8);failRead=0;
            assert(!picasso_glink_service(&io,&s,&kick) && s.ready && s.txHead==16);}
    }
    // Remote offers and acknowledgements converge to v1/features0; an endless
    // unsupported negotiation cannot monopolize the ADSP service thread.
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));rx=peer();held=false;
    message(s,rx,0,2,3);message(s,rx,1,2,3);assert(!picasso_glink_service(&io,&s,&kick) && !s.ready);
    message(s,rx,0,1,3);message(s,rx,1,1,1);assert(!picasso_glink_service(&io,&s,&kick) && !s.ready);
    message(s,rx,1,1,0);assert(!picasso_glink_service(&io,&s,&kick) && s.ready);
    setup();assert(!picasso_glink_prepare(&io,&s,&audit));rx=peer();held=false;
    for(unsigned n=0;n<24;++n)message(s,rx,0,2,0);
    assert(!picasso_glink_service(&io,&s,&kick) && s.commands==8);
    assert(!picasso_glink_service(&io,&s,&kick) && s.commands==16);
    assert(picasso_glink_service(&io,&s,&kick)==-71 && s.commands==16);
    printf("PASS GLINK fresh allocation, %u write/%u read failures, version/ACK, late peer, wrap, variable RX, pressure, OPEN preservation, malformed cursor and deadline\n",maxWrites,maxReads);
}
