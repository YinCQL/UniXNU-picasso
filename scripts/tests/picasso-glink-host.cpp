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
static void build184Tests();
static void qrtrTests();
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
    build184Tests();channelTests();qrtrTests();
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
    rx=connect(s);message(s,rx,5,7,0x20000000);assert(picasso_glink_service(&io,&s,&kick)==-90);
    rx=connect(s);message(s,rx,14,7,1);assert(picasso_glink_service(&io,&s,&kick)==-95);
    rx=connect(s);uint32_t dup[6]={0x70005,2,512,91,256,91};frame(s,rx,dup,24);
    assert(picasso_glink_service(&io,&s,&kick)==-17 && !s.apr.pending);
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
    frame(s,rx,intents,sizeof intents);for(unsigned n=0;n<4;++n)step(s);drain(s);step(s);assert(s.remoteIntent[0].busy);
    offer(s,rx,100);step(s);assert(s.intentSkipped==1 && s.remoteIntent[0].id==1 && s.remoteIntent[0].busy);
    puts("PASS GLINK channel isolation, OPEN/CLOSE/intent/data, bounded APR query, fragmentation, ownership, pressure, malformed input and response matching");
}
static void qrtrPacket(PicassoGlink &s,unsigned rx,unsigned id,const uint32_t *p,unsigned bytes,unsigned left=0,bool cont=false) {
    static uint32_t w[0x1104];assert(bytes<=0x4400);
    w[0]=unsigned(cont?12:9)|(2U<<16);w[1]=id;w[2]=bytes;w[3]=left;
    memset(w+4,0,((bytes+7)&~7U));memcpy(w+4,p,bytes);frame(s,rx,w,16+((bytes+7)&~7U));
}
static unsigned connectQrtr(PicassoGlink &s) {
    unsigned rx=connect(s);openPeer(s,rx,2,"IPCRTR");step(s);drain(s);step(s);drain(s);
    message(s,rx,4,2,0);step(s);drain(s);step(s);drain(s);return rx;
}
__attribute__((noinline)) static void qrtrTests() {
    PicassoGlink s={};unsigned rx=connect(s);openPeer(s,rx,2,"IPCRTR");step(s);drain(s);step(s);drain(s);
    assert(s.channel[1].local==2 && s.channel[1].openSent && !s.channel[1].up);
    message(s,rx,4,2,0);step(s);drain(s);step(s);drain(s);assert(s.channel[1].up);
    const uint32_t grant[]={0x20005,1,2048,0};frame(s,rx,grant,16);offer(s,rx,0);step(s);drain(s);
    assert(s.qrtrRemote[0].used && s.remoteIntent[0].used);step(s);drain(s);
    assert(s.qrtr.helloSent && s.qrtrRemote[0].busy && s.apr.pending && s.remoteIntent[0].busy);
    message(s,rx,6,2,0);step(s);drain(s);assert(!s.qrtrRemote[0].used && s.remoteIntent[0].busy);
    uint32_t hello[]={1,2,2,0xfffffffe,0,20,1,0xfffffffe,2,0,0,0,0};
    qrtrPacket(s,rx,s.qrtrLocal[0].id,hello,52);step(s);drain(s);
    assert(s.qrtr.helloReceived && s.qrtr.peer==2 && s.qrtrLocal[0].done);
    unsigned iid=s.qrtrLocal[0].id;step(s);drain(s);assert(s.qrtrLocal[0].id!=iid);
    // The sizes are advertised from the matching physical storage, and
    // fragmented payloads >APR's 512 bytes stay within their channel pool.
    for(unsigned n=0;n<12;++n){step(s);drain(s);}
    for(unsigned n=0;n<10;++n)assert(s.qrtrLocal[n].advertised && s.qrtrLocal[n].capacity==(n<5?0x800U:n<8?0x2000U:0x4400U));
    static uint32_t large[0x1100];memset(large,0,sizeof large);
    large[0]=1;large[1]=1;large[2]=2;large[3]=73;large[4]=1;large[5]=sizeof large-32;large[6]=1;large[7]=99;
    iid=s.qrtrLocal[8].id;qrtrPacket(s,rx,iid,large,8192,sizeof large-8192);step(s);drain(s);
    assert(s.qrtrLocal[8].active && s.qrtrLocal[8].fill==8192);
    qrtrPacket(s,rx,iid,large+2048,sizeof large-8192,0,true);step(s);drain(s);
    assert(s.qrtr.data==1 && s.qrtr.count==1 && s.qrtrLocal[8].done);
    // Queue pressure retains completed input without re-appending fragments.
    for(unsigned n=0;n<8;++n){step(s);drain(s);uint32_t data[]={1,1,2,73,1,20,1,99,0,0,0,0,0};
        qrtrPacket(s,rx,s.qrtrLocal[0].id,data,52);step(s);drain(s);}
    assert(s.qrtr.count==8 && s.qrtrLocal[0].done && !s.qrtrLocal[0].delivered);
    unsigned packets=s.qrtr.received;step(s);drain(s);assert(s.qrtr.received==packets);
    frame(s,rx,grant,16);step(s);drain(s);unsigned tx=s.txHead;step(s);drain(s);
    const uint32_t resume[]={0x20009,0,52,0,1,7,1,99,0,20,2,73,7,1,99,0,0,0};
    assert(!memcmp(ram+s.tx.offset+tx,resume,sizeof resume) && s.qrtr.resumes==1);
    step(s);drain(s);assert(s.qrtr.received==packets+1 && s.qrtr.count==8);
    // Closing IPCRTR invalidates only its ownership; APR's outstanding ID0
    // remains busy until that channel receives its own RX_DONE.
    message(s,rx,3,2,0);step(s);drain(s);
    assert(s.channel[0].up && s.apr.pending && s.remoteIntent[0].busy && !s.qrtrRemote[0].used && !s.qrtr.count);
    message(s,rx,6,7,0);step(s);assert(!s.remoteIntent[0].used);
    picasso_glink_stop(&s);assert(!s.channel[0].up && !s.qrtr.helloReceived);
    // HELLO is never reported as sent when any TX word/publication failed.
    // A pressure retry preserves intent ownership and emits exactly one HELLO.
    rx=connectQrtr(s);s.apr.error=-60;frame(s,rx,grant,16);step(s);drain(s);writes=0;step(s);
    unsigned helloWrites=writes;bool kick;
    for(unsigned n=1;n<=helloWrites;++n) {
        rx=connectQrtr(s);s.apr.error=-60;frame(s,rx,grant,16);step(s);drain(s);writes=0;failWrite=n;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed && !s.qrtr.helloSent);
        unsigned count=writes;assert(picasso_glink_service(&io,&s,&kick)==-5 && writes==count);
    }
    rx=connectQrtr(s);s.apr.error=-60;frame(s,rx,grant,16);step(s);drain(s);
    unsigned head=s.txHead;p32(s.desc.offset,(head+32)%s.tx.size);step(s);
    assert(!s.qrtr.helloSent && !s.qrtrRemote[0].busy);unsigned count=writes;
    for(unsigned n=0;n<10;++n)step(s);assert(writes==count && !s.qrtr.helloSent);
    drain(s);step(s);assert(s.qrtr.helloSent && s.qrtrRemote[0].busy);drain(s);
    // A large grant request selects its actual backing tier. A request beyond
    // the stock maximum is denied without advertising undersized storage.
    rx=connectQrtr(s);message(s,rx,7,2,0x4400);step(s);drain(s);
    assert(s.qrtrLocal[8].advertised && s.qrtrLocal[8].capacity==0x4400);
    message(s,rx,7,2,0x4401);step(s);drain(s);assert(!s.failed && !s.qrtrLocal[9].advertised);
    // RX ids are checked within their endpoint, even when the numeric value
    // is a valid live intent in the other endpoint.
    rx=connectQrtr(s);qrtrPacket(s,rx,s.localIntent[0].id,hello,52);
    assert(picasso_glink_service(&io,&s,&kick)==-71 && !s.apr.ready);
    // APR close leaves QRTR live, complementing the reverse close above.
    rx=connectQrtr(s);frame(s,rx,grant,16);step(s);drain(s);step(s);drain(s);
    message(s,rx,3,7,0);step(s);assert(s.channel[1].up && s.qrtr.helloSent && s.qrtrRemote[0].busy);
    puts("PASS GLINK APR/IPCRTR isolation, shared ID0, stock buffer tiers, large fragments, HELLO/RESUME backpressure and selective close");
}
// Keep this independent scenario out of main: Clang 18 Windows machine-sink
// crashes after inlining both large fixture bodies. Production flags unchanged.
__attribute__((noinline)) static void build184Tests(){
    PicassoGlink s={};unsigned rx=handshake(s);bool kick;
    // Build184's observed OPEN order and channel IDs. The log omits intent
    // payloads: zero-ID/512 bytes below is a protocol regression, not a dump.
    openPeer(s,rx,1,"fastrpcglink-apps-dsp");openPeer(s,rx,2,"IPCRTR");
    openPeer(s,rx,3,"LOOPBACK_CTL_LPASS");openPeer(s,rx,4,"apr_audio_svc");
    step(s);assert(s.rxTail==120 && s.deferredOpens==2);drain(s);step(s);
    assert(s.txHead==72 && s.channel[0].ackSent);drain(s);
    message(s,rx,4,1,0);uint32_t zero[4]={0x40005,1,512,0};frame(s,rx,zero,16);step(s);
    assert(!s.failed && s.channel[0].up && s.rxTail==144);
    assert(s.remoteIntent[0].used && s.remoteIntent[0].id==0 && s.intentOffers==1);
    step(s);assert(s.apr.pending && s.remoteIntent[0].busy);
    const uint32_t expected[]={0x10009,0,20,0,0x00140250,0x503,0x403,0x18300001,0x1290c,0};
    assert(!memcmp(ram+s.tx.offset+88,expected,sizeof expected));drain(s);
    uint32_t answer[10]={0x40009,1,24,0,0x00180150,0x403,0x503,s.apr.token,0x1290d,5};
    frame(s,rx,answer,sizeof answer);step(s);assert(s.apr.ready && s.apr.state==5);drain(s);
    message(s,rx,6,4,0);step(s);assert(!s.remoteIntent[0].used);
    frame(s,rx,zero,16);step(s);assert(s.remoteIntent[0].used && !s.remoteIntent[0].busy);
    frame(s,rx,zero,16);assert(picasso_glink_service(&io,&s,&kick)==-17 && s.intentFault==5);

    // Remote IDs are opaque: zero and high unsigned IDs retain exact TX bits.
    rx=connect(s);offer(s,rx,0xffffffff);step(s);drain(s);unsigned at=s.txHead;step(s);
    assert(r32(s.tx.offset+at+20)==0xffffffff && s.remoteIntent[0].busy);
    message(s,rx,6,7,0xffffffff);step(s);assert(!s.remoteIntent[0].used);

    // Queue 116 independent advertisements (same byte span as 0x80..0x7c0).
    // The inferred count is only a stress case, not an assertion about firmware.
    rx=connect(s);unsigned tail=s.rxTail;
    for(unsigned n=0;n<116;++n)offer(s,rx,n);
    message(s,rx,13,0,0);
    for(unsigned n=0;n<117;++n){drain(s);step(s);}
    assert(s.intentOffers==116 && s.intentFrames==116 && s.intentSkipped==52);
    assert(s.rxTail==(tail+116*16+8)%s.rx.size && s.remoteIntent[0].busy && s.remoteIntent[0].id==0);

    // Batched offers larger than cache capacity are processed in chunks of 16.
    // No query may use a staged descriptor before the whole frame is committed.
    rx=connect(s);uint32_t batch[2+116*2]={0x70005,116};
    for(unsigned n=0;n<116;++n){batch[2+n*2]=16;batch[3+n*2]=n;}
    tail=s.rxTail;frame(s,rx,batch,sizeof batch);
    for(unsigned n=1;n<=7;++n){step(s);assert(s.intentProgress==n*16 && s.rxTail==tail && !s.apr.pending);}
    step(s);assert(s.intentProgress==0 && s.intentFrames==1 && s.intentSkipped==52 && s.rxTail!=tail);
    // Larger remote grant must displace an idle small descriptor, never a busy one.
    uint32_t grant[4]={0x70005,1,512,1000};frame(s,rx,grant,16);step(s);
    assert(s.intentReplaced==1 && s.remoteIntent[0].id==1000);drain(s);step(s);assert(s.remoteIntent[0].busy);
    grant[2]=1024;grant[3]=1001;frame(s,rx,grant,16);step(s);
    assert(s.remoteIntent[0].busy && s.remoteIntent[0].id==1000 && s.remoteIntent[1].id==1001);

    // Entire table busy: extra offers are left unused, never falsely completed.
    rx=connect(s);for(unsigned n=0;n<PicassoGlinkRemoteIntents;++n)
        s.remoteIntent[n]={n,16,true,true};
    frame(s,rx,grant,16);step(s);assert(s.intentSkipped==1 && s.remoteIntent[0].busy);

    // Empty lists are a valid no-op. Empty buffers are not.
    rx=connect(s);message(s,rx,5,7,0);step(s);assert(s.intentFrames==1 && !s.intentOffers);
    zero[0]=0x70005;zero[2]=0;frame(s,rx,zero,16);
    assert(picasso_glink_service(&io,&s,&kick)==-22 && s.intentFault==4 && !s.lastIntentId && !s.lastIntentSize);

    // Truncated and wrapped batches preserve RX tail until all bytes arrive.
    rx=connect(s);s.rxTail=16376;p32(s.desc.offset+8,16376);p32(s.desc.offset+12,16376);
    message(s,rx,5,7,116);step(s);assert(!s.intentProgress && s.rxTail==16376);
    frame(s,rx,batch+2,sizeof batch-8);
    for(unsigned n=0;n<8;++n)step(s);
    assert(s.rxTail==(16376+sizeof batch)%s.rx.size && s.intentOffers==116);

    // A changed in-progress command is a protocol fault, never a new frame.
    rx=connect(s);frame(s,rx,batch,sizeof batch);tail=s.rxTail;step(s);
    p32(rx+tail+4,115);assert(picasso_glink_service(&io,&s,&kick)==-71 && s.intentFault==3);
    rx=connect(s);frame(s,rx,batch,sizeof batch);tail=s.rxTail;step(s);
    p32(rx+tail,13);assert(picasso_glink_service(&io,&s,&kick)==-71 && s.intentFault==3);

    // Duplicate zero within a batch, while busy, and across parsing boundaries.
    rx=connect(s);batch[3+16*2]=0;frame(s,rx,batch,sizeof batch);step(s);
    assert(picasso_glink_service(&io,&s,&kick)==-17 && s.intentFault==5 && !s.apr.pending);batch[3+16*2]=16;
    rx=connect(s);offer(s,rx,0);step(s);drain(s);step(s);assert(s.remoteIntent[0].busy);
    offer(s,rx,0);assert(picasso_glink_service(&io,&s,&kick)==-17 && s.remoteIntent[0].busy);

    // A read failure partway through a staged batch cannot publish a query;
    // retryable SMEM consistency scans must resume without duplicating offers.
    rx=connect(s);frame(s,rx,batch,sizeof batch);step(s);reads=0;step(s);unsigned totalReads=reads;
    for(unsigned n=1;n<=totalReads;++n){
        rx=connect(s);frame(s,rx,batch,sizeof batch);step(s);reads=0;failRead=n;
        int result=picasso_glink_service(&io,&s,&kick);unsigned count=writes;
        if(s.failed){assert(result && !s.apr.pending);assert(picasso_glink_service(&io,&s,&kick) && writes==count);}
        else {failRead=0;for(unsigned j=0;j<8;++j)step(s);assert(s.intentOffers==116 && s.intentFrames==1);}
    }
    puts("PASS build184 remote intent zero, 116 offers, bounded batches, large grants, busy ownership and diagnostics");
}
