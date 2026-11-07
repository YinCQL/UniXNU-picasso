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
static void channelTests();
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
    assert(!picasso_glink_service(&io,&s,&kick) && s.pendingCommand==2 && s.pendingChannel==7);
    assert(s.rxTail==tail && !memcmp(before,ram,sizeof ram)); // incomplete OPEN remains unacknowledged
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
    unsigned oldWrites=writes;assert(picasso_glink_service(&io,&s,&kick)==-11 && !s.rxTail && writes==oldWrites+3 && kick);
    assert(r32(s.tx.offset+8)==13 && s.txHead==16);oldWrites=writes;
    for(unsigned n=0;n<20;++n)assert(picasso_glink_service(&io,&s,&kick)==-11 && !s.retries && !s.failed);
    assert(writes==oldWrites);
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
    channelTests();
    printf("PASS GLINK fresh allocation, %u write/%u read failures, version/ACK, late peer, wrap, variable RX, pressure, incomplete OPEN, malformed cursor and deadline\n",maxWrites,maxReads);
}
// Synthetic remote encodes wire bytes independently of the implementation.
static void frame(PicassoGlink &s,unsigned rx,const uint32_t *words,unsigned bytes){
    unsigned head=r32(s.desc.offset+12);assert(!(bytes&7));
    for(unsigned n=0;n<bytes;n+=4)p32(rx+(head+n)%s.rx.size,words[n/4]);
    p32(s.desc.offset+12,(head+bytes)%s.rx.size);
}
static void openPeer(PicassoGlink &s,unsigned rx,unsigned id,const char *name,unsigned prio=0){
    uint32_t words[10]={2|(id<<16),unsigned(strlen(name)+1)|(prio<<16)};
    memcpy(words+2,name,strlen(name)+1);frame(s,rx,words,8+((unsigned(strlen(name))+8)&~7U));
}
static void drain(PicassoGlink &s){p32(s.desc.offset,s.txHead);}
static void step(PicassoGlink &s){bool kick;assert(!picasso_glink_service(&io,&s,&kick));}
static unsigned handshake(PicassoGlink &s){
    setup();PicassoSmemAudit audit={};assert(!picasso_glink_prepare(&io,&s,&audit));unsigned rx=peer();held=false;
    message(s,rx,0,1,0);message(s,rx,1,1,0);step(s);assert(s.ready);drain(s);return rx;
}
static unsigned connect(PicassoGlink &s){
    unsigned rx=handshake(s);step(s);assert(s.channel[0].openSent);drain(s);
    openPeer(s,rx,7,"apr_audio_svc",4);message(s,rx,4,1,0);step(s);
    assert(!s.channel[0].up);drain(s);step(s);
    assert(s.channel[0].up && s.channel[0].remote==7 && s.channel[0].priority==4);
    assert(s.localIntent[0].id==1 && s.apr.intentAsked);drain(s);return rx;
}
static void offer(PicassoGlink &s,unsigned rx,unsigned id=91){
    const uint32_t words[4]={0x70005,1,512,id};frame(s,rx,words,16);
}
static unsigned query(PicassoGlink &s){
    unsigned rx=connect(s);offer(s,rx);message(s,rx,8,7,1);step(s);drain(s);
    unsigned at=s.txHead;step(s);
    assert(s.apr.pending && s.apr.attempts==1 && s.apr.token==0x18300001);
    assert(r32(s.tx.offset+at)==0x10005); // second advertisement
    const uint32_t expected[]={0x10009,91,20,0,0x00140250,0x503,0x403,0x18300001,0x1290c,0};
    assert(!memcmp(ram+s.tx.offset+at+16,expected,sizeof expected));
    drain(s);return rx;
}
static void packet(PicassoGlink &s,unsigned rx,unsigned iid,const uint8_t *data,unsigned count,
                   unsigned left=0,bool cont=false){
    uint32_t words[132]={unsigned(cont?12:9)|(7U<<16),iid,count,left};
    assert(count<=512);memcpy(words+4,data,count);frame(s,rx,words,16+((count+7)&~7U));
}
static void channelTests(){
    PicassoGlink s={};unsigned rx=handshake(s);
    // One absent service does not block APR or a subsequent READ_NOTIF.
    openPeer(s,rx,2,"sensor-service",9);openPeer(s,rx,7,"apr_audio_svc");message(s,rx,4,1,0);
    step(s);assert(s.deferredOpens==1 && s.channel[1].remote==2 && !s.channel[1].ackSent);
    drain(s);step(s);assert(s.channel[0].up && !s.channel[1].up);drain(s);
    message(s,rx,3,2,0);step(s);assert(!s.channel[1].used && s.channel[0].up);
    // Truncated OPEN is retried, then both its name and the TX OPEN wrap.
    rx=handshake(s);s.rxTail=16376;p32(s.desc.offset+8,16376);p32(s.desc.offset+12,16376);
    s.txHead=16376;p32(s.desc.offset+4,16376);p32(s.desc.offset,16376);
    message(s,rx,2,7,14);step(s);assert(s.rxTail==16376 && s.txHead==16);
    assert(r32(s.tx.offset+16376)==0x10002 && r32(s.tx.offset+16380)==14);
    assert(!memcmp(ram+s.tx.offset,"apr_audio_svc",14));
    uint32_t name[4]={};memcpy(name,"apr_audio_svc",14);frame(s,rx,name,16);step(s);assert(s.rxTail==16);
    // Unknown duplicate IDs/names, bad names, overflow counts and unnegotiated reuse fail closed.
    rx=handshake(s);openPeer(s,rx,2,"one");openPeer(s,rx,2,"two");bool kick;
    assert(picasso_glink_service(&io,&s,&kick)==-17 && s.failed);
    rx=handshake(s);openPeer(s,rx,2,"same");openPeer(s,rx,3,"same");assert(picasso_glink_service(&io,&s,&kick)==-17);
    rx=handshake(s);message(s,rx,2,7,0xffff);assert(picasso_glink_service(&io,&s,&kick)==-22);
    rx=handshake(s);uint32_t badName[4]={0x70002,4,0x64636261,0};frame(s,rx,badName,16);
    assert(picasso_glink_service(&io,&s,&kick)==-22);
    rx=connect(s);message(s,rx,5,7,0x20000000);assert(picasso_glink_service(&io,&s,&kick)==-22);
    rx=connect(s);message(s,rx,14,7,1);assert(picasso_glink_service(&io,&s,&kick)==-95);
    rx=connect(s);uint32_t dup[6]={0x70005,2,512,91,256,91};frame(s,rx,dup,24);
    assert(picasso_glink_service(&io,&s,&kick)==-17 && !s.remoteIntent[0].used);
    // A late intent-request ACK remains valid after the granted intent was used.
    rx=connect(s);offer(s,rx);step(s);drain(s);step(s);assert(s.apr.pending && s.apr.intentAsked);
    message(s,rx,8,7,1);step(s);assert(!s.failed && !s.apr.intentAsked);
    // Full APR request/response, reassembly and non-reuse buffer ownership.
    rx=query(s);uint32_t response[6]={0x00180150,0x403,0x503,s.apr.token,0x1290d,5};
    unsigned iid=s.localIntent[0].id;
    packet(s,rx,iid,reinterpret_cast<uint8_t *>(response),11,13);step(s);
    assert(s.localIntent[0].active && s.localIntent[0].fill==11 && !s.apr.ready);drain(s);
    packet(s,rx,iid,reinterpret_cast<uint8_t *>(response)+11,13,0,true);step(s);
    assert(s.apr.ready && s.apr.responded && s.apr.state==5 && s.localIntent[0].done);drain(s);
    unsigned at=s.txHead;step(s);assert(r32(s.tx.offset+at)==0x10006 && r32(s.tx.offset+at+4)==iid);
    assert(s.localIntent[0].id!=iid && s.remoteIntent[0].busy);
    message(s,rx,6,7,91);step(s);assert(!s.remoteIntent[0].used);
    drain(s);message(s,rx,3,7,0);step(s);
    assert(!s.apr.ready && !s.channel[0].up && s.channel[0].closing && !s.localIntent[0].id);
    message(s,rx,11,1,0);step(s);assert(s.channel[0].closeAck);
    memcpy(before,ram,sizeof ram);picasso_glink_stop(&s);assert(s.failed && !memcmp(before,ram,sizeof ram));
    // Wrong token/domain, malformed APR, basic success and not-ready cannot claim ready.
    rx=query(s);response[3]=s.apr.token+1;packet(s,rx,1,reinterpret_cast<uint8_t *>(response),24);step(s);
    assert(s.apr.pending && !s.apr.ready && s.apr.ignored==1);drain(s);step(s);drain(s);
    response[3]=s.apr.token;response[1]=0x503;packet(s,rx,2,reinterpret_cast<uint8_t *>(response),24);step(s);
    assert(s.apr.ignored==2 && !s.apr.ready);drain(s);step(s);drain(s);
    response[1]=0x403;response[0]=0x00190150;packet(s,rx,3,reinterpret_cast<uint8_t *>(response),24);step(s);
    assert(s.apr.malformed==1 && !s.apr.ready);
    rx=query(s);uint32_t basic[7]={0x001c0150,0x403,0x503,s.apr.token,0x110e8,0x1290c,0};
    packet(s,rx,1,reinterpret_cast<uint8_t *>(basic),28);step(s);assert(s.apr.pending && !s.apr.ready);
    rx=query(s);basic[6]=10;packet(s,rx,1,reinterpret_cast<uint8_t *>(basic),28);step(s);
    assert(s.apr.error==-5 && s.apr.status==10 && !s.apr.ready);
    rx=query(s);response[0]=0x00180150;response[1]=0x403;response[3]=s.apr.token;response[5]=0;
    packet(s,rx,1,reinterpret_cast<uint8_t *>(response),24);step(s);assert(s.apr.responded && !s.apr.ready);
    // Corrupt fragment continuity, unknown intent and oversized packets fail without ACK/data publication.
    rx=query(s);packet(s,rx,1,reinterpret_cast<uint8_t *>(response),8,16);step(s);drain(s);
    packet(s,rx,1,reinterpret_cast<uint8_t *>(response)+8,16,1,true);
    assert(picasso_glink_service(&io,&s,&kick)==-90 && !s.apr.ready);
    rx=query(s);packet(s,rx,999,reinterpret_cast<uint8_t *>(response),24);
    assert(picasso_glink_service(&io,&s,&kick)==-71);
    rx=query(s);const uint32_t huge[4]={0x70009,1,0xffffffff,0};frame(s,rx,huge,16);
    assert(picasso_glink_service(&io,&s,&kick)==-90);
    // Oversized intent requests are denied, capacity exhaustion cannot falsely grant.
    rx=connect(s);unsigned head=s.txHead;message(s,rx,7,7,513);step(s);
    assert(r32(s.tx.offset+head+16)==0x10008 && !r32(s.tx.offset+head+20));
    rx=connect(s);for(unsigned n=0;n<20;++n){drain(s);step(s);}drain(s);
    head=s.txHead;message(s,rx,7,7,512);step(s);assert(r32(s.tx.offset+head)==0x10008 && !r32(s.tx.offset+head+4));
    // Supported request advertises real storage and ACK atomically.
    rx=connect(s);head=s.txHead;message(s,rx,7,7,512);step(s);
    assert(r32(s.tx.offset+head+16)==0x10005 && r32(s.tx.offset+head+32)==0x10008 && r32(s.tx.offset+head+36)==1);
    // Close with only reserve space sends one READ_NOTIF, preserves RX, then commits once.
    rx=connect(s);s.apr.error=-60;for(auto &i:s.localIntent)if(!i.id)i.id=99;
    p32(s.desc.offset,(s.txHead+24)%s.tx.size);message(s,rx,3,7,0);unsigned tail=s.rxTail;head=s.txHead;
    step(s);assert(s.rxTail==tail && s.txHead==(head+8)%s.tx.size && s.channel[0].up);
    unsigned count=writes;step(s);assert(writes==count && s.rxTail==tail);drain(s);step(s);
    assert(s.channel[0].closing && s.rxTail!=tail);
    // Timeout never recycles the in-flight TX intent or claims q6core readiness.
    rx=query(s);for(unsigned n=0;n<260;++n){drain(s);step(s);}
    assert(s.apr.error==-60 && !s.apr.ready && s.remoteIntent[0].busy && s.apr.attempts==1);
    // Every OPEN publication write failure is terminal, with no duplicate retry.
    for(unsigned n=1;n<=7;++n){rx=handshake(s);writes=0;failWrite=n;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed);count=writes;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && writes==count);}
    // A published ACK+intent+query or RX_DONE must never be replayed following
    // an uncertain write. Inject every write in these compound service passes.
    rx=connect(s);offer(s,rx);message(s,rx,8,7,1);step(s);drain(s);writes=0;step(s);
    unsigned queryWrites=writes;
    for(unsigned n=1;n<=queryWrites;++n){
        rx=connect(s);offer(s,rx);message(s,rx,8,7,1);step(s);drain(s);writes=0;failWrite=n;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed && !s.apr.ready);count=writes;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && writes==count);
    }
    // RX data can wrap with only a header initially present; do not consume it
    // or notify completion until the entire padded fragment has arrived.
    rx=query(s);response[5]=5;s.rxTail=16368;p32(s.desc.offset+8,16368);p32(s.desc.offset+12,16368);
    const uint32_t dataHeader[4]={0x70009,1,24,0};frame(s,rx,dataHeader,16);step(s);
    assert(s.rxTail==16368 && !s.apr.ready);drain(s);frame(s,rx,response,24);step(s);
    assert(s.rxTail==24 && s.apr.ready);drain(s);writes=0;step(s);unsigned doneWrites=writes;
    for(unsigned n=1;n<=doneWrites;++n){
        rx=query(s);packet(s,rx,1,reinterpret_cast<uint8_t *>(response),24);step(s);drain(s);
        writes=0;failWrite=n;assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed && !s.apr.ready);
        count=writes;assert(picasso_glink_service(&io,&s,&kick)==-5 && writes==count);
    }
    // Retry with a new token, retaining both remote ownerships until RX_DONE.
    rx=query(s);offer(s,rx,92);step(s);
    for(unsigned n=0;n<57;++n){drain(s);step(s);}
    assert(s.apr.pending && s.apr.attempts==2 && s.apr.token==0x18300002);
    response[3]=0x18300001;packet(s,rx,1,reinterpret_cast<uint8_t *>(response),24);step(s);
    assert(s.apr.ignored==1 && !s.apr.ready);drain(s);step(s);drain(s);
    response[3]=s.apr.token;packet(s,rx,2,reinterpret_cast<uint8_t *>(response),24);step(s);assert(s.apr.ready);
    assert(s.remoteIntent[0].busy && s.remoteIntent[1].busy);
    // Timeout cancels an incomplete OPEN with our local ID, and late traffic
    // cannot resurrect the closed channel or emit a readiness result.
    rx=handshake(s);for(unsigned n=0;n<501;++n){drain(s);step(s);}
    assert(s.channel[0].closing && s.apr.error==-60 && !s.channel[0].up);
    message(s,rx,11,1,0);step(s);assert(s.channel[0].closeAck);
    // 15 distinct unsupported names are retained without ACK. A sixteenth
    // must fail explicitly rather than overwrite another channel's identity.
    rx=handshake(s);for(unsigned n=0;n<15;++n){char name[32];snprintf(name,sizeof name,"unsupported%u",n);
        openPeer(s,rx,n+2,name);step(s);drain(s);}
    assert(s.deferredOpens==15);openPeer(s,rx,30,"overflow");assert(picasso_glink_service(&io,&s,&kick)==-28);
    // Permanent TX pressure still expires the request deadline.
    rx=query(s);p32(s.desc.offset,(s.txHead+8)%s.tx.size);
    for(unsigned n=0;n<260;++n)step(s);
    assert(s.apr.error==-60 && !s.apr.pending && !s.apr.ready && s.remoteIntent[0].busy);
    // Fault every read in a real channel/data receive pass. Only the allocator
    // scanner's consistency check may retry; no retry duplicates the APR send.
    rx=query(s);response[3]=s.apr.token;packet(s,rx,1,reinterpret_cast<uint8_t *>(response),24);
    reads=0;step(s);unsigned packetReads=reads;
    for(unsigned n=1;n<=packetReads;++n){
        rx=query(s);packet(s,rx,1,reinterpret_cast<uint8_t *>(response),24);reads=0;failRead=n;
        int result=picasso_glink_service(&io,&s,&kick);count=writes;
        if(s.failed){assert(result && !s.apr.ready);assert(picasso_glink_service(&io,&s,&kick) && writes==count);}
        else {failRead=0;step(s);assert(s.apr.ready && s.apr.attempts==1);}
    }
    // A full remote intent table never overwrites an in-flight buffer.
    rx=connect(s);uint32_t intents[130]={0x70005,64};
    for(unsigned n=0;n<64;++n){intents[2+n*2]=512;intents[3+n*2]=n+1;}
    frame(s,rx,intents,sizeof intents);step(s);drain(s);step(s);assert(s.remoteIntent[0].busy);
    offer(s,rx,100);assert(picasso_glink_service(&io,&s,&kick)==-28 && s.remoteIntent[0].id==1);
    puts("PASS GLINK channel isolation, OPEN/CLOSE/intent/data, bounded APR query, fragmentation, ownership, pressure, malformed input and response matching");
}
