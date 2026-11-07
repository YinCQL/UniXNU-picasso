// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real SMEM allocator and GLINK state machine, synthetic peer and faulting I/O.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "PicassoGlink.h"
#include "PicassoAdsp.h"
#include "PicassoApr.h"
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
static void build185Tests();
static void build186Tests();
static void local187Tests();
static void fastrpcTests();
static void tftpTests();
static void afeTests();
static void registryQmiTests();
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
    registryQmiTests();afeTests();tftpTests();fastrpcTests();local187Tests();build186Tests();build185Tests();build184Tests();channelTests();qrtrTests();
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
    // Unknown duplicate IDs/names, bad names, overflow counts and invalid completion fail closed.
    rx=handshake(s);openPeer(s,rx,2,"one");openPeer(s,rx,2,"two");bool kick;
    assert(picasso_glink_service(&io,&s,&kick)==-17 && s.failed);
    rx=handshake(s);openPeer(s,rx,2,"same");openPeer(s,rx,3,"same");assert(picasso_glink_service(&io,&s,&kick)==-17);
    rx=handshake(s);message(s,rx,2,7,0xffff);assert(picasso_glink_service(&io,&s,&kick)==-22);
    rx=handshake(s);uint32_t badName[4]={0x70002,4,0x64636261,0};frame(s,rx,badName,16);
    assert(picasso_glink_service(&io,&s,&kick)==-22);
    rx=connect(s);message(s,rx,5,7,0x20000000);assert(picasso_glink_service(&io,&s,&kick)==-90);
    rx=connect(s);message(s,rx,14,7,1);assert(picasso_glink_service(&io,&s,&kick)==-71);
    rx=connect(s);uint32_t dup[6]={0x70005,2,512,91,256,91};frame(s,rx,dup,24);
    assert(picasso_glink_service(&io,&s,&kick)==-17 && !s.apr.pending);
    // A late intent-request ACK remains valid after the granted intent was used.
    rx=connect(s);offer(s,rx);step(s);drain(s);step(s);assert(s.apr.pending && s.apr.intentAsked);
    message(s,rx,8,7,1);step(s);assert(!s.failed && !s.apr.intentAsked);
    // Full APR request/response, reassembly and reusable local buffer ownership.
    rx=query(s);uint32_t response[6]={0x00180150,0x403,0x503,s.apr.token,0x1290d,5};
    unsigned iid=s.localIntent[0].id;
    packet(s,rx,iid,reinterpret_cast<uint8_t *>(response),11,13);step(s);
    assert(s.localIntent[0].active && s.localIntent[0].fill==11 && !s.apr.ready);drain(s);
    packet(s,rx,iid,reinterpret_cast<uint8_t *>(response)+11,13,0,true);step(s);
    assert(s.apr.ready && s.apr.responded && s.apr.state==5 && s.localIntent[0].done);drain(s);
    unsigned at=s.txHead;step(s);assert(r32(s.tx.offset+at)==0x1000e && r32(s.tx.offset+at+4)==iid);
    assert(s.localIntent[0].id==iid && s.localIntent[0].advertised && s.remoteIntent[0].busy);
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
    head=s.txHead;message(s,rx,7,7,512);step(s);assert(r32(s.tx.offset+head)==0x10008 && r32(s.tx.offset+head+4)==1);
    // A full idle pool grants its existing storage; a fully busy pool cannot.
    for(auto &i:s.localIntent)i.active=true;drain(s);head=s.txHead;message(s,rx,7,7,512);step(s);
    assert(r32(s.tx.offset+head)==0x10008 && !r32(s.tx.offset+head+4));
    // Existing reusable storage grants without a duplicate advertisement.
    rx=connect(s);head=s.txHead;message(s,rx,7,7,512);step(s);
    assert(r32(s.tx.offset+head+16)==0x10008 && r32(s.tx.offset+head+20)==1);
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
    unsigned iid=s.qrtrLocal[0].id;step(s);drain(s);assert(s.qrtrLocal[0].id==iid && s.qrtrLocal[0].advertised);
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
__attribute__((noinline)) static void build185Tests() {
    PicassoGlink s={};unsigned rx=connectQrtr(s);bool kick;
    // 185's stopping command is RX_DONE_W_REUSE/rcid4/iid0. Here APR is
    // already connected as rcid7: exercise the same completion on both peers.
    s.channel[0].remote=4;
    const uint32_t aprOffer[]={0x40005,1,128,0},qrtrOffer[]={0x20005,1,8320,0};
    frame(s,rx,aprOffer,16);frame(s,rx,qrtrOffer,16);step(s);drain(s);step(s);drain(s);
    assert(s.apr.pending && s.qrtr.helloSent && s.remoteIntent[0].busy && s.qrtrRemote[0].busy);
    unsigned tail=s.rxTail;message(s,rx,14,4,0);message(s,rx,14,2,0);step(s);drain(s);
    assert(s.rxTail==tail+16 && s.ready && !s.failed && s.remoteReuse==2 && !s.remoteDone);
    assert(s.remoteIntent[0].used && s.qrtrRemote[0].used && !s.remoteIntent[0].busy && !s.qrtrRemote[0].busy);
    assert(!s.apr.ready && s.apr.pending && !s.qrtr.helloReceived); // completion is not application success
    uint32_t reply[]={0x40009,1,24,0,0x00180150,0x403,0x503,s.apr.token,0x1290d,5};
    frame(s,rx,reply,sizeof reply);
    uint32_t hello[]={1,2,2,0xfffffffe,0,20,1,0xfffffffe,2,0,0,0,0};
    qrtrPacket(s,rx,s.qrtrLocal[0].id,hello,52);step(s);drain(s);
    assert(s.apr.ready && s.qrtr.helloReceived && s.apr.header==0x00180150 && s.qrtr.sourceNode==2);
    unsigned local=s.qrtrLocal[0].id;step(s);drain(s);
    assert(s.localReuse==2 && s.localIntent[0].id==1 && s.qrtrLocal[0].id==local);
    assert(s.qrtr.locator.announced);message(s,rx,14,2,0);step(s);drain(s);
    step(s);drain(s);assert(s.qrtr.tftp.announced);message(s,rx,14,2,0);step(s);drain(s);
    uint32_t server[]={1,4,2,0xfffffffe,0,20,1,0xfffffffe,4,0x40,0x101,2,73};
    qrtrPacket(s,rx,local,server,52);step(s);drain(s);assert(s.qrtr.services[0].used);
    step(s);drain(s);uint32_t data[]={1,1,2,73,1,20,1,99,0,0,0,0,0};
    qrtrPacket(s,rx,local,data,52);step(s);drain(s);step(s);drain(s);
    assert(s.qrtr.resumes==1 && s.qrtrRemote[0].busy);
    message(s,rx,14,2,0);step(s);drain(s);assert(!s.qrtrRemote[0].busy);
    qrtrPacket(s,rx,local,data,52);step(s);drain(s);step(s);drain(s);
    assert(s.qrtr.resumes==2 && s.qrtrRemote[0].busy && s.qrtrLocal[0].id==local);
    message(s,rx,6,2,0);step(s);drain(s);assert(!s.qrtrRemote[0].used && s.remoteDone==1);
    // Enough repeated complete packets to exhaust non-reuse buffers if they
    // were not safely returned; no new ID or advertisement is needed.
    for(unsigned n=0;n<20;++n){step(s);drain(s);}unsigned next=s.nextIntent;
    for(unsigned n=0;n<100;++n) {
        qrtrPacket(s,rx,local,hello,52);step(s);drain(s);step(s);drain(s);
        assert(s.qrtrLocal[0].id==local && s.qrtrLocal[0].advertised && !s.qrtrLocal[0].fill);
    }
    assert(s.nextIntent==next && s.apr.ready && s.qrtr.helloReceived);
    // Duplicate/stale/not-busy completions must never create capacity.
    const unsigned completions[]={6,14};
    for(unsigned command: completions) {
        rx=connect(s);offer(s,rx,0);step(s);drain(s);step(s);drain(s);
        message(s,rx,command,7,0);step(s);drain(s);s.apr.error=-60;
        message(s,rx,14,7,0);assert(picasso_glink_service(&io,&s,&kick)==-71);
        assert(s.faultBytes==8 && s.faultWords[0]==0x7000e && !s.faultWords[1]);
    }
    rx=connect(s);offer(s,rx,0xffffffff);step(s);drain(s);step(s);drain(s);
    s.rxTail=16376;p32(s.desc.offset+8,16376);p32(s.desc.offset+12,16376);
    message(s,rx,14,7,0xffffffff);step(s);assert(s.rxTail==0 && s.remoteIntent[0].used && !s.remoteIntent[0].busy);
    // APR can retry using the same returned intent, but a late response from
    // its previous token still cannot complete the new query.
    for(unsigned n=0;n<57;++n){drain(s);step(s);}
    assert(s.apr.attempts==2 && s.remoteIntent[0].busy && s.remoteIntent[0].id==0xffffffff);
    uint32_t late[]={0x00180150,0x403,0x503,s.apr.token-1,0x1290d,5};
    packet(s,rx,1,reinterpret_cast<uint8_t *>(late),24);step(s);drain(s);step(s);drain(s);
    assert(s.apr.ignored==1 && s.apr.pending && !s.apr.ready);
    message(s,rx,6,7,0xffffffff);step(s);drain(s);assert(!s.remoteIntent[0].used);
    late[3]=s.apr.token;packet(s,rx,1,reinterpret_cast<uint8_t *>(late),24);step(s);
    assert(s.apr.ready && s.localIntent[0].done);
    // Failure to publish the RX tail after a completion terminates the
    // session; it cannot transmit using that just-returned slot on retry.
    rx=query(s);s.apr.error=-60;for(unsigned n=0;n<20;++n){step(s);drain(s);}
    message(s,rx,14,7,91);unsigned oldTail=s.rxTail;writes=0;failWrite=1;
    assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed && s.rxTail==oldTail);
    unsigned countWrites=writes;assert(picasso_glink_service(&io,&s,&kick)==-5 && writes==countWrites);
    // A pending local completion stays unavailable while TX is full. A new
    // packet using the same ID is legal only after completion is published.
    rx=query(s);uint32_t response[]={0x00180150,0x403,0x503,s.apr.token,0x1290d,5};
    packet(s,rx,1,reinterpret_cast<uint8_t *>(response),24);step(s);drain(s);
    p32(s.desc.offset,(s.txHead+8)%s.tx.size);unsigned localReuse=s.localReuse;
    for(unsigned n=0;n<12;++n)step(s);
    assert(s.localIntent[0].done && !s.localIntent[0].advertised && s.localReuse==localReuse);
    drain(s);step(s);drain(s);assert(s.localIntent[0].advertised && s.localIntent[0].id==1);
    packet(s,rx,1,reinterpret_cast<uint8_t *>(response),24);step(s);assert(s.apr.received==2 && s.apr.ignored==1);
    // Fault evidence wraps and stops at peer head, leaving unknown data intact.
    rx=connect(s);s.rxTail=16376;p32(s.desc.offset+8,16376);p32(s.desc.offset+12,16376);
    const uint32_t unknown[]={0x70063,0xabc,0x11223344,0x55667788};frame(s,rx,unknown,sizeof unknown);
    assert(picasso_glink_service(&io,&s,&kick)==-95 && s.rxTail==16376 && s.faultBytes==16);
    assert(!memcmp(s.faultWords,unknown,sizeof unknown));
    puts("PASS build185 reuse completion with features0, APR/QRTR responses, stable local pool, repeated send, stale completion, wrap and pressure");
}
static unsigned discoveredQmi(PicassoGlink &s) {
    unsigned rx=connectQrtr(s);s.apr.error=-60;
    const uint32_t grant[]={0x20005,1,128,0};frame(s,rx,grant,16);step(s);drain(s);step(s);drain(s);
    uint32_t hello[]={1,2,5,0xfffffffe,0,20,1,0xfffffffe,2,0,0,0,0};
    qrtrPacket(s,rx,s.qrtrLocal[0].id,hello,52);message(s,rx,14,2,0);step(s);drain(s);step(s);drain(s);
    assert(s.qrtr.locator.announced);message(s,rx,14,2,0);step(s);drain(s);
    step(s);drain(s);assert(s.qrtr.tftp.announced);message(s,rx,14,2,0);step(s);drain(s);
    uint32_t server[]={1,4,5,0xfffffffe,0,20,1,0xfffffffe,4,0x42,0x4a01,5,3};
    qrtrPacket(s,rx,s.qrtrLocal[0].id,server,52);step(s);drain(s);return rx;
}
static unsigned frameworkReady(PicassoGlink &s) {
    unsigned rx=query(s);const uint32_t state[]={0x00240081,0x403,0x503,s.apr.token,0x1290d,0,0,0,1};
    packet(s,rx,1,reinterpret_cast<const uint8_t *>(state),36);message(s,rx,14,7,91);
    step(s);drain(s);assert(s.apr.ready && !s.apr.fw.sent);return rx;
}
__attribute__((noinline)) static void registryQmiTests() {
    PicassoGlink s={};unsigned rx=discoveredQmi(s);
    for(auto &d:s.qrtr.qmi.domains) { d.sent=true;d.completed=true;d.stateValid=true;d.state=0x1fffffff; }
    picasso_qmi_registry_idle(&s.qrtr.qmi,1283);
    p32(s.desc.offset,(s.txHead+8)%s.tx.size);
    for(unsigned n=0;n<10;++n)step(s);
    assert(s.qrtr.qmi.round==1 && !s.qrtr.qmi.domains[0].sent && !s.qrtrRemote[0].busy);
    drain(s);unsigned at=s.txHead;step(s);drain(s);
    assert(s.qrtr.qmi.domains[0].pending && s.qrtrRemote[0].busy);
    bool found=false;for(unsigned n=at;n+68<=s.txHead;n+=8)if(r32(s.tx.offset+n)==0x20009) {
        assert(picasso_qmi_u16(ram+s.tx.offset+n+49)==4);found=true;
    }assert(found);
    const uint8_t stale[]={2,1,0,0x21,0,14,0,2,4,0,0,0,0,0,0x10,4,0,0xff,0xff,0xff,0x1f};
    uint32_t data[14]={1,1,5,3,0,21,1,0x186};memcpy(data+8,stale,sizeof stale);
    qrtrPacket(s,rx,s.qrtrLocal[0].id,data,56);step(s);drain(s);
    assert(s.qrtr.qmi.domains[0].pending && !s.qrtr.qmi.domains[0].stateValid);
    reinterpret_cast<uint8_t *>(data+8)[1]=4;
    qrtrPacket(s,rx,s.qrtrLocal[0].id,data,56);step(s);drain(s);
    assert(s.qrtr.qmi.domains[0].stateValid && s.qrtr.qmi.domains[0].completed && !s.failed);
    picasso_glink_stop(&s);assert(!s.qrtr.qmi.domains[0].stateValid && !s.qrtr.qmi.bound);
    puts("PASS registry QMI refresh through GLINK under TX pressure, stale response rejection and shutdown");
}
static unsigned locatorAnnounceReady(PicassoGlink &s) {
    unsigned rx=connectQrtr(s);s.apr.error=-60;
    const uint32_t grant[]={0x20005,1,512,0};frame(s,rx,grant,16);step(s);drain(s);step(s);drain(s);
    const uint32_t hello[]={1,2,5,0xfffffffe,0,20,1,0xfffffffe,2,0,0,0,0};
    qrtrPacket(s,rx,s.qrtrLocal[0].id,hello,52);message(s,rx,14,2,0);step(s);drain(s);
    assert(!s.qrtr.locator.announced);return rx;
}
static unsigned afeReady(PicassoGlink &s) {
    unsigned rx=frameworkReady(s);step(s);drain(s);
    uint32_t fw[]={0x00340150,0x403,0x503,0x18710001,0x1292d,2,9,0,0,1,4,7,0};
    packet(s,rx,1,reinterpret_cast<uint8_t *>(fw),sizeof fw);message(s,rx,14,7,91);
    step(s);drain(s);assert(s.apr.fw.valid && !s.apr.afe.sent);return rx;
}
__attribute__((noinline)) static void afeTests() {
    // Stock q6afe v2 in-band wire, no mapped payload or SET/START command.
    const uint32_t expected[]={0x00380250,0x504,0x404,0x20520001,0x100f0,0x00201002,0,0,0,0x1020c,0x1020d,0x1020c,0x1020d,20};
    uint32_t command[14];picasso_afe_query(command);assert(!memcmp(command,expected,sizeof command));
    PicassoAprProbe a={};assert(!picasso_afe_due(&a) && !a.afe.error);
    a.ready=true;assert(!picasso_afe_due(&a) && !a.afe.error);
    a.fw.valid=true;assert(!picasso_afe_due(&a) && a.afe.error==-2);
    a.afe={};a.fw.count=1;a.fw.services[0][0]=4;
    assert(!picasso_afe_due(&a) && a.afe.error==-95);
    a.afe={};a.fw.services[0][1]=7;assert(picasso_afe_due(&a) && a.afe.api==7);
    // Both v0 and the observed v1 extended APR header locate the payload.
    uint32_t response[]={0x00440081,0x404,0x504,0x20520001,0x10106,0xaaa,0xbbb,0xccc,0,0x1020c,0x1020d,20,1,0x00010010,0x00010001,48000,0};
    auto receive=[&](const void *p,unsigned bytes){picasso_apr_receive(&a,static_cast<const uint8_t *>(p),bytes);};
    auto pending=[&](){a={};a.ready=true;a.afe.sent=a.afe.pending=true;a.afe.token=0x20520001;};
    pending();for(unsigned bytes=0;bytes<sizeof response;++bytes)receive(response,bytes);
    assert(a.afe.pending && !a.afe.responded);
    const unsigned indices[]={1,2,3,4,9,10,11,12,16};
    for(auto i:indices) {auto saved=response[i];response[i]^=i==16?0x10000:1;receive(response,sizeof response);response[i]=saved;assert(a.afe.pending && !a.afe.responded);}
    const uint32_t headers[]={0x00440082,0x00440041,0x00450081,0x00440281,0x00440381};
    for(auto h:headers) {response[0]=h;receive(response,sizeof response);assert(a.afe.pending && !a.afe.responded);}
    response[0]=0x00440081;
    uint32_t basic[]={0x001c0150,0x404,0x504,0x20520001,0x110e8,0x100f0,0};
    receive(basic,sizeof basic);assert(a.afe.pending && !a.afe.valid && !a.afe.responded);
    receive(response,sizeof response);assert(a.afe.valid && a.afe.responded && !a.afe.pending && a.afe.config[3]==48000 && a.ready);
    unsigned events=a.afe.events;receive(response,sizeof response);assert(a.afe.events==events);
    pending();basic[6]=3;receive(basic,sizeof basic);assert(a.afe.error==-5 && a.afe.status==3 && a.afe.responded && !a.afe.valid && a.ready);
    uint32_t v0[14]={0x00380150,0x404,0x504,0x20520001,0x10106};memcpy(v0+5,response+8,36);
    pending();receive(v0,sizeof v0);assert(a.afe.valid && a.afe.config[3]==48000);
    // 21:10 hardware captured header/size/opcode, not its payload bytes. Model
    // both status outcomes of the firmware's 48-byte allocated payload, with
    // nonzero spare capacity to prove it is not interpreted as config data.
    uint32_t allocated[20];memcpy(allocated,response,sizeof response);
    allocated[0]=0x00500181;allocated[17]=0xaaaaaaaa;allocated[18]=0xbbbbbbbb;allocated[19]=0xcccccccc;
    pending();receive(allocated,sizeof allocated);
    assert(a.afe.valid && a.afe.responded && a.afe.payloadBytes==48 && a.afe.headerBytes==32 && a.afe.config[3]==48000 && a.afe.raw[11]==0xcccccccc);
    pending();allocated[8]=2;receive(allocated,sizeof allocated);
    assert(a.afe.responded && !a.afe.valid && !a.afe.pending && a.afe.status==2 && a.afe.error==-5 && a.ready);
    allocated[8]=0;
    pending();allocated[11]=19;receive(allocated,sizeof allocated);
    assert(a.afe.pending && !a.afe.valid && a.afe.rejected==1 && a.afe.raw[3]==19);
    for(unsigned i=0;i<10;++i)receive(allocated,sizeof allocated);
    assert(a.afe.rejected==4 && a.afe.events==4);allocated[11]=20;
    for(unsigned bytes=0;bytes<sizeof allocated;++bytes) {
        if(bytes==68)continue; // legitimate compact form
        pending();uint32_t h=allocated[0];allocated[0]=(bytes<<16)|0x181;
        receive(allocated,bytes);assert(!a.afe.valid);allocated[0]=h;
    }
    pending();allocated[12]=2;receive(allocated,sizeof allocated);assert(!a.afe.valid && a.afe.pending);allocated[12]=1;
    const unsigned payloads[]={4,16,36};for(unsigned payload:payloads) {
        pending();v0[0]=((20+payload)<<16)|0x150;v0[5]=7;receive(v0,20+payload);
        assert(a.afe.responded && !a.afe.pending && !a.afe.valid && a.afe.status==7 && a.afe.error==-5);
    }
    PicassoGlink s={};unsigned rx=afeReady(s),at=s.txHead;step(s);drain(s);
    assert(s.apr.afe.pending && s.apr.afe.sent && s.apr.afe.api==7 && s.remoteIntent[0].busy);
    bool found=false;for(unsigned n=at;n+72<=s.txHead;n+=8)if(r32(s.tx.offset+n)==0x10009) {
        assert(r32(s.tx.offset+n+8)==56 && !memcmp(ram+s.tx.offset+n+16,expected,sizeof expected));found=true;
    }assert(found);
    packet(s,rx,1,reinterpret_cast<uint8_t *>(response),29,39);step(s);drain(s);assert(!s.apr.afe.responded);
    packet(s,rx,1,reinterpret_cast<uint8_t *>(response)+29,39,0,true);step(s);drain(s);
    assert(s.apr.afe.valid && s.apr.ready && !s.failed);
    picasso_glink_stop(&s);assert(!s.apr.afe.valid && !s.apr.afe.pending && s.apr.afe.error==-104);
    rx=afeReady(s);step(s);drain(s);
    packet(s,rx,1,reinterpret_cast<uint8_t *>(allocated),sizeof allocated);step(s);drain(s);
    assert(s.apr.afe.valid && s.apr.afe.payloadBytes==48 && !s.apr.malformed && s.apr.ready);
    // Too-small intents request the actual packet size. Denial stays local to
    // this optional probe; the CORE/framework and file transport stay usable.
    rx=afeReady(s);s.remoteIntent[0].size=20;at=s.txHead;step(s);drain(s);
    assert(!s.apr.afe.sent && s.apr.intentAsked);found=false;
    for(unsigned n=at;n+8<=s.txHead;n+=8)if(r32(s.tx.offset+n)==0x10007) {assert(r32(s.tx.offset+n+4)==56);found=true;}
    assert(found);message(s,rx,8,7,0);step(s);drain(s);
    assert(s.apr.afe.error==-28 && !s.apr.error && s.apr.ready && s.apr.fw.valid && !s.failed);
    rx=afeReady(s);s.remoteIntent[0].size=20;step(s);drain(s);message(s,rx,8,7,1);step(s);drain(s);
    for(unsigned n=0;n<251;++n){step(s);drain(s);}assert(s.apr.afe.error==-60 && !s.apr.afe.sent && !s.failed);
    // Advertisements can arrive before ACK; a late denial cannot invalidate
    // a probe already transmitted in a real buffer.
    rx=afeReady(s);s.remoteIntent[0].size=20;step(s);drain(s);s.remoteIntent[0].size=512;
    step(s);drain(s);message(s,rx,8,7,0);step(s);drain(s);assert(s.apr.afe.pending && !s.apr.afe.error && !s.apr.error);
    rx=afeReady(s);p32(s.desc.offset,(s.txHead+8)%s.tx.size);
    for(unsigned n=0;n<10;++n)step(s);assert(!s.apr.afe.sent && !s.remoteIntent[0].busy);
    drain(s);step(s);drain(s);assert(s.apr.afe.pending);
    p32(s.desc.offset,(s.txHead+8)%s.tx.size);for(unsigned n=0;n<102;++n)step(s);
    assert(s.apr.afe.error==-60 && !s.apr.afe.pending && s.remoteIntent[0].busy && s.apr.ready && !s.failed);
    picasso_apr_receive(&s.apr,reinterpret_cast<uint8_t *>(response),sizeof response);assert(!s.apr.afe.valid);
    rx=afeReady(s);writes=0;step(s);unsigned total=writes;bool kick;
    for(unsigned n=1;n<=total;++n) {
        rx=afeReady(s);writes=0;failWrite=n;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed && !s.apr.afe.pending && !s.apr.afe.valid);
    }
    rx=afeReady(s);step(s);drain(s);message(s,rx,3,7,0);step(s);
    assert(!s.apr.afe.pending && !s.apr.afe.valid && s.apr.afe.error==-104);
    puts("PASS AFE in-band GET wire, service gate, response validation, fragmentation, timeout, pressure, intent denial and invalidation");
}
__attribute__((noinline)) static void local187Tests() {
    PicassoGlink s={};unsigned rx=frameworkReady(s);unsigned at=s.txHead;step(s);drain(s);
    assert(s.apr.fw.sent && s.apr.fw.pending && s.remoteIntent[0].busy);
    // Completion, local intent advertisement, then the real FWK command.
    bool found=false;
    for(unsigned n=at;n+36<=s.txHead;n+=8)if(r32(s.tx.offset+n)==0x10009) {
        assert(r32(s.tx.offset+n+28)==0x18710001 && r32(s.tx.offset+n+32)==0x1292c);found=true;
    }
    assert(found);
    uint32_t reply[]={0x00580081,0x403,0x503,0x18710001,0x1292d,0,0,0,5,6,1,9,3,4,3,0,7,2,1,8,4,2};
    packet(s,rx,1,reinterpret_cast<uint8_t *>(reply),31,57);step(s);drain(s);
    assert(!s.apr.fw.valid);
    packet(s,rx,1,reinterpret_cast<uint8_t *>(reply)+31,57,0,true);step(s);drain(s);
    assert(s.apr.fw.valid && !s.apr.fw.pending && s.apr.fw.count==3 && s.apr.fw.services[2][0]==8);
    assert(s.apr.fw.build[3]==9 && s.apr.ready);
    picasso_glink_stop(&s);assert(!s.apr.fw.valid && !s.apr.fw.pending);
    // Pure decoder rejects all truncations, excess/duplicate service records,
    // wrong token/address/type and basic success without changing the version.
    PicassoAprProbe a={};a.ready=true;a.fw.pending=true;a.fw.token=0x18710001;
    for(unsigned n=0;n<sizeof reply;++n)picasso_apr_receive(&a,reinterpret_cast<uint8_t *>(reply),n);
    assert(a.fw.pending && !a.fw.valid);
    reply[12]=26;picasso_apr_receive(&a,reinterpret_cast<uint8_t *>(reply),sizeof reply);assert(!a.fw.valid);reply[12]=3;
    reply[19]=4;picasso_apr_receive(&a,reinterpret_cast<uint8_t *>(reply),sizeof reply);assert(!a.fw.valid);reply[19]=8;
    reply[3]++;picasso_apr_receive(&a,reinterpret_cast<uint8_t *>(reply),sizeof reply);assert(!a.fw.valid);reply[3]--;
    uint32_t basic[]={0x001c0150,0x403,0x503,0x18710001,0x110e8,0x1292c,0};
    picasso_apr_receive(&a,reinterpret_cast<uint8_t *>(basic),sizeof basic);assert(a.fw.pending && !a.fw.valid);
    basic[6]=3;picasso_apr_receive(&a,reinterpret_cast<uint8_t *>(basic),sizeof basic);
    assert(a.fw.error==-5 && a.fw.status==3 && !a.fw.pending && a.ready);
    rx=frameworkReady(s);step(s);drain(s);p32(s.desc.offset,(s.txHead+8)%s.tx.size);
    for(unsigned n=0;n<102;++n)step(s);
    assert(s.apr.fw.error==-60 && !s.apr.fw.pending && s.remoteIntent[0].busy && s.apr.ready);
    rx=frameworkReady(s);writes=0;step(s);unsigned total=writes;bool kick;
    for(unsigned n=1;n<=total;++n) {
        rx=frameworkReady(s);writes=0;failWrite=n;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed && !s.apr.fw.pending && !s.apr.fw.valid);
    }
    // Real GLINK HELLO -> APSS announcement -> confirmed domain-list query.
    rx=locatorAnnounceReady(s);writes=0;step(s);unsigned announceWrites=writes;
    for(unsigned n=1;n<=announceWrites;++n) {
        rx=locatorAnnounceReady(s);writes=0;failWrite=n;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed && !s.qrtr.locator.announced);
    }
    rx=locatorAnnounceReady(s);p32(s.desc.offset,(s.txHead+8)%s.tx.size);
    for(unsigned n=0;n<10;++n)step(s);
    assert(!s.qrtr.locator.announced && !s.qrtrRemote[0].busy);drain(s);
    at=s.txHead;step(s);drain(s);assert(s.qrtr.locator.announced);
    found=false;for(unsigned n=at;n+68<=s.txHead;n+=8)if(r32(s.tx.offset+n)==0x20009) {
        const uint32_t expected[]={1,4,1,0xfffffffe,0,20,5,0xfffffffe,4,0x40,0x101,1,0x187};
        assert(!memcmp(ram+s.tx.offset+n+16,expected,sizeof expected));found=true;
    }
    assert(found);message(s,rx,14,2,0);step(s);drain(s);
    step(s);drain(s);assert(s.qrtr.tftp.announced);message(s,rx,14,2,0);step(s);drain(s);
    uint32_t request[14]={1,1,5,88,1,19,1,0x187};
    const uint8_t qmi[]={0,9,0,0x21,0,12,0,1,9,0,'a','v','s','/','a','u','d','i','o'};
    memcpy(request+8,qmi,sizeof qmi);
    qrtrPacket(s,rx,s.qrtrLocal[0].id,request,52);step(s);drain(s);
    assert(s.qrtr.locator.requests==1 && s.qrtr.count==2);
    step(s);drain(s);assert(s.qrtr.resumes==1 && !s.qrtr.locator.replies);
    message(s,rx,14,2,0);step(s);drain(s);s.qrtrRemote[0].size=64;
    step(s);drain(s);assert(!s.qrtr.locator.replies && s.qrtr.count==1 && !s.qrtrRemote[0].busy);
    s.qrtrRemote[0].size=512;step(s);drain(s);
    assert(s.qrtr.locator.replies==1 && !s.qrtr.count && s.qrtrRemote[0].busy);
    picasso_glink_stop(&s);assert(!s.qrtr.locator.announced && !s.qrtr.count);
    puts("PASS local187 real locator announcement/replies and AVCS version wire, fragmentation, validation, timeout, TX failures and invalidation");
}
__attribute__((noinline)) static void build186Tests() {
    PicassoGlink s={};unsigned rx=query(s);bool kick;
    // 186 logs prove the first five words and size/header, but do not include
    // extension/payload bytes. Distinct synthetic extension/state values test
    // offsets without pretending the actual DSP state was captured.
    uint32_t response[]={0x00240081,0x403,0x503,s.apr.token,0x1290d,0xaaaa,0xbbbb,0xcccc,5};
    packet(s,rx,1,reinterpret_cast<uint8_t *>(response),36);step(s);drain(s);
    assert(s.apr.ready && s.apr.responded && s.apr.state==5 && !s.apr.malformed);
    rx=query(s);response[8]=0;packet(s,rx,1,reinterpret_cast<uint8_t *>(response),36);step(s);drain(s);
    assert(s.apr.responded && !s.apr.ready && !s.apr.pending);
    rx=query(s);response[8]=5;response[3]=s.apr.token+1;packet(s,rx,1,reinterpret_cast<uint8_t *>(response),36);step(s);
    assert(s.apr.ignored==1 && !s.apr.ready);
    response[3]=s.apr.token;
    const uint32_t badHeaders[]={0x00240082,0x00240041,0x002400a1,0x00280081};
    for(auto h:badHeaders) {rx=query(s);response[0]=h;packet(s,rx,1,reinterpret_cast<uint8_t *>(response),36);step(s);assert(s.apr.malformed==1 && !s.apr.ready);}
    rx=query(s);response[0]=0x00240281;packet(s,rx,1,reinterpret_cast<uint8_t *>(response),36);step(s);assert(s.apr.ignored==1);
    rx=query(s);response[0]=0x00240081;packet(s,rx,1,reinterpret_cast<uint8_t *>(response),13,23);step(s);drain(s);
    packet(s,rx,1,reinterpret_cast<uint8_t *>(response)+13,23,0,true);step(s);assert(s.apr.ready);
    // Actual186 discovery tuple. Sending starts only after HELLO and service
    // publication, without hardcoding the observed node5/port3 into transport.
    rx=connectQrtr(s);s.apr.error=-60;const uint32_t grant[]={0x20005,1,128,0};frame(s,rx,grant,16);step(s);drain(s);step(s);drain(s);
    uint32_t hello[]={1,2,5,0xfffffffe,0,20,0xffffffff,0xfffffffe,2,0,0,0,0};
    qrtrPacket(s,rx,s.qrtrLocal[0].id,hello,52);message(s,rx,14,2,0);step(s);drain(s);step(s);drain(s);
    assert(s.qrtr.locator.announced);message(s,rx,14,2,0);step(s);drain(s);
    step(s);drain(s);assert(s.qrtr.tftp.announced);message(s,rx,14,2,0);step(s);drain(s);
    uint32_t server[]={1,4,5,0xfffffffe,0,20,1,0xfffffffe,4,0x42,0x4a01,5,3};
    qrtrPacket(s,rx,s.qrtrLocal[0].id,server,52);step(s);drain(s);assert(s.qrtr.qmi.bound);
    unsigned tx=s.txHead;p32(s.desc.offset,(tx+8)%s.tx.size);for(unsigned n=0;n<5;++n)step(s);
    assert(!s.qrtr.qmi.domains[0].sent);drain(s);step(s);drain(s);
    assert(s.qrtr.qmi.domains[0].pending && s.qrtrRemote[0].busy);
    const uint8_t answer[]={2,1,0,0x21,0,14,0,2,4,0,0,0,0,0,0x10,4,0,0xff,0xff,0xff,0x1f};
    uint32_t data[14]={1,1,5,3,1,21,1,0x186};memcpy(data+8,answer,sizeof answer);
    qrtrPacket(s,rx,s.qrtrLocal[0].id,data,56);message(s,rx,14,2,0);step(s);drain(s);
    assert(s.qrtr.qmi.domains[0].stateValid && s.qrtr.qmi.domains[0].state==0x1fffffff && s.qrtr.count==1);
    step(s);drain(s);assert(s.qrtr.resumes==1 && !s.qrtr.qmi.domains[1].sent);
    message(s,rx,14,2,0);step(s);drain(s);step(s);drain(s);assert(s.qrtr.qmi.domains[1].pending);
    // A disappearing remote service invalidates pending transactions/states.
    server[1]=5;server[8]=5;qrtrPacket(s,rx,s.qrtrLocal[0].id,server,52);step(s);
    assert(!s.qrtr.qmi.bound && s.qrtr.qmi.lost && !s.qrtr.qmi.domains[0].stateValid && !s.qrtr.qmi.domains[1].pending);
    assert(!s.failed);picasso_glink_stop(&s);assert(picasso_glink_service(&io,&s,&kick)==-104);
    rx=discoveredQmi(s);writes=0;step(s);unsigned queryWrites=writes;
    for(unsigned n=1;n<=queryWrites;++n) {
        rx=discoveredQmi(s);writes=0;failWrite=n;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed && !s.qrtr.qmi.bound && !s.qrtr.qmi.domains[0].pending);
        unsigned count=writes;assert(picasso_glink_service(&io,&s,&kick)==-5 && writes==count);
    }
    // A missing QMI response expires even with permanent FIFO pressure, and
    // cannot free the remote intent or block APR/transport service forever.
    rx=discoveredQmi(s);step(s);drain(s);p32(s.desc.offset,(s.txHead+8)%s.tx.size);
    for(unsigned n=0;n<102;++n)step(s);
    assert(s.qrtr.qmi.domains[0].error==-60 && !s.qrtr.qmi.domains[0].pending && s.qrtrRemote[0].busy && !s.failed);
    puts("PASS build186 APR v1 extended EVENT replies and discovered QMI query, pressure, confirmed response and service loss");
}
// Keep this independent scenario out of main: Clang 18 Windows machine-sink
// crashes after inlining both large fixture bodies. Production flags unchanged.
__attribute__((noinline)) static void build184Tests(){
    PicassoGlink s={};unsigned rx=handshake(s);bool kick;
    // Build184's observed OPEN order and channel IDs. The log omits intent
    // payloads: zero-ID/512 bytes below is a protocol regression, not a dump.
    openPeer(s,rx,1,"fastrpcglink-apps-dsp");openPeer(s,rx,2,"IPCRTR");
    openPeer(s,rx,3,"LOOPBACK_CTL_LPASS");openPeer(s,rx,4,"apr_audio_svc");
    step(s);assert(s.rxTail==120 && s.deferredOpens==1);drain(s);step(s);
    assert(s.txHead==112 && s.channel[0].ackSent && s.channel[1].local==3 && s.channel[1].ackSent);drain(s);
    message(s,rx,4,1,0);uint32_t zero[4]={0x40005,1,512,0};frame(s,rx,zero,16);step(s);
    assert(!s.failed && s.channel[0].up && s.rxTail==144);
    assert(s.remoteIntent[0].used && s.remoteIntent[0].id==0 && s.intentOffers==1);
    step(s);assert(s.apr.pending && s.remoteIntent[0].busy);
    const uint32_t expected[]={0x10009,0,20,0,0x00140250,0x503,0x403,0x18300001,0x1290c,0};
    assert(!memcmp(ram+s.tx.offset+128,expected,sizeof expected));drain(s);
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

// Stock FastRPC binds a transport without sending a startup invocation. These
// fixtures prove real backing storage/ownership and isolation, not RPC success.
static unsigned connectFastrpc(PicassoGlink &s) {
    unsigned rx=connect(s);s.apr.ready=true;s.apr.fw.sent=true;
    openPeer(s,rx,9,"fastrpcglink-apps-dsp");step(s);drain(s);
    unsigned at=s.txHead;step(s);
    assert(!s.deferredOpens && s.channel[1].local==3 && !s.channel[1].up);
    assert(r32(s.tx.offset+at+16)==0x30002 && r32(s.tx.offset+at+20)==22);
    assert(!memcmp(ram+s.tx.offset+at+24,"fastrpcglink-apps-dsp",22));
    assert(r32(s.tx.offset+at+48)==0x90004);drain(s);
    message(s,rx,4,3,0);step(s);assert(s.channel[1].up);drain(s);
    for(unsigned n=0;n<64;++n) { step(s);drain(s); }
    for(unsigned n=0;n<64;++n) {
        auto &i=s.fastrpcLocal[n];assert(i.advertised && i.id && i.capacity==100);
        assert(i.data==s.fastrpcStorage[n] && !i.done && !i.active);
        for(unsigned k=0;k<n;++k)assert(i.id!=s.fastrpcLocal[k].id);
        for(auto &a:s.localIntent)assert(i.id!=a.id);
    }
    at=s.txHead;step(s);assert(at==s.txHead); // passive, no fabricated RPC
    return rx;
}
static void fastrpcPacket(PicassoGlink &s,unsigned rx,unsigned iid,const uint8_t *p,unsigned size,unsigned left=0,bool cont=false) {
    uint32_t words[30]={unsigned(cont?12:9)|(9U<<16),iid,size,left};
    assert(size<=104);memcpy(words+4,p,size);frame(s,rx,words,16+((size+7)&~7U));
}
__attribute__((noinline)) static void fastrpcTests() {
    PicassoGlink s={};unsigned rx=connectFastrpc(s);bool kick;
    // Reassembly across arbitrary byte boundaries records raw data once. The
    // RPC context remains opaque even if it resembles a successful response.
    uint8_t payload[104];for(unsigned n=0;n<sizeof payload;++n)payload[n]=uint8_t(n+1);
    unsigned iid=s.fastrpcLocal[0].id;
    fastrpcPacket(s,rx,iid,payload,7,17);step(s);drain(s);
    assert(s.fastrpcLocal[0].active && !s.fastrpc.received);
    fastrpcPacket(s,rx,iid,payload+7,17,0,true);step(s);drain(s);
    assert(s.fastrpc.received==1 && s.fastrpc.bytes==24 && s.fastrpc.words[0]==0x04030201);
    assert(s.fastrpc.words[5]==0x18171615 && s.fastrpcLocal[0].done && !s.qrtr.received && !s.apr.received);
    unsigned at=s.txHead;step(s);assert(r32(s.tx.offset+at)==0x3000e && r32(s.tx.offset+at+4)==iid);
    assert(s.fastrpcLocal[0].advertised && s.fastrpc.received==1);drain(s);
    // Backpressure retains ownership; neither dispatch nor RX_DONE duplicates.
    fastrpcPacket(s,rx,iid,payload,100);step(s);drain(s);at=s.txHead;
    p32(s.desc.offset,(at+16)%s.tx.size);step(s);
    assert(s.fastrpcLocal[0].done && !s.fastrpcLocal[0].advertised && s.fastrpc.received==2);
    unsigned count=writes;step(s);assert(writes==count && s.fastrpc.received==2);
    drain(s);step(s);assert(s.fastrpcLocal[0].advertised);drain(s);
    fastrpcPacket(s,rx,iid,payload,1);step(s);drain(s);step(s);drain(s);
    assert(s.fastrpc.bytes==1 && s.fastrpc.words[0]==1 && !s.fastrpc.words[1]);
    // Stock capacity request succeeds; larger buffers are explicitly denied.
    at=s.txHead;message(s,rx,7,9,100);step(s);
    assert(r32(s.tx.offset+at)==0x30008 && r32(s.tx.offset+at+4)==1);drain(s);
    at=s.txHead;message(s,rx,7,9,101);step(s);
    assert(r32(s.tx.offset+at)==0x30008 && !r32(s.tx.offset+at+4));drain(s);
    // Equal remote IDs on independent channels never alias another pool.
    s.remoteIntent[0]={0,512,true,true};s.qrtrRemote[0]={0,512,true,true};
    const uint32_t offerFast[4]={0x90005,1,100,0};frame(s,rx,offerFast,16);step(s);drain(s);
    assert(s.fastrpcRemote[0].used && !s.fastrpcRemote[0].busy && s.remoteIntent[0].busy && s.qrtrRemote[0].busy);
    s.qrtr.helloSent=true;s.qrtr.helloReceived=true;s.qrtr.locator.announced=true;
    message(s,rx,3,9,0);step(s);
    assert(s.fastrpc.error==-104 && s.channel[1].closing && !s.channel[1].up);
    assert(s.apr.ready && s.qrtr.locator.announced && s.remoteIntent[0].busy && s.qrtrRemote[0].busy);
    for(auto &i:s.fastrpcLocal)assert(!i.id && !i.advertised && !i.active);
    for(auto &i:s.fastrpcRemote)assert(!i.used);
    message(s,rx,11,3,0);step(s);assert(s.channel[1].closeAck);
    // Oversize/foreign intent and impossible unsolicited ACK fail explicitly.
    rx=connectFastrpc(s);fastrpcPacket(s,rx,s.fastrpcLocal[0].id,payload,101);
    assert(picasso_glink_service(&io,&s,&kick)==-90 && !s.fastrpc.received && !s.channel[1].up);
    rx=connectFastrpc(s);fastrpcPacket(s,rx,s.localIntent[0].id,payload,12);
    assert(picasso_glink_service(&io,&s,&kick)==-71 && !s.apr.received);
    rx=connectFastrpc(s);s.qrtr.intentAsked=true;message(s,rx,8,9,1);
    assert(picasso_glink_service(&io,&s,&kick)==-71 && !s.qrtr.intentGranted);
    rx=connectFastrpc(s);message(s,rx,14,9,0);
    assert(picasso_glink_service(&io,&s,&kick)==-71); // no invoke owns this ID
    // Every payload/head write failure during reuse is terminal, never retried.
    for(unsigned n=1;n<=3;++n) {
        rx=connectFastrpc(s);fastrpcPacket(s,rx,s.fastrpcLocal[0].id,payload,24);step(s);drain(s);
        writes=0;failWrite=n;assert(picasso_glink_service(&io,&s,&kick)==-5);
        count=writes;assert(picasso_glink_service(&io,&s,&kick)==-5 && writes==count && !s.channel[1].up);
    }
    rx=connectFastrpc(s);memcpy(before,ram,sizeof ram);picasso_glink_stop(&s);
    assert(s.failed && s.fastrpc.error==-104 && !memcmp(before,ram,sizeof ram));
    for(auto &i:s.fastrpcLocal)assert(!i.id && !i.advertised);
    // Missing OPEN_ACK times out only this channel, leaving APR/QRTR intact.
    rx=connect(s);s.apr.ready=true;s.apr.fw.sent=true;openPeer(s,rx,9,"fastrpcglink-apps-dsp");step(s);
    for(unsigned n=0;n<502;++n){drain(s);step(s);}
    assert(s.channel[1].closing && s.fastrpc.error==-60 && !s.qrtr.error && s.apr.ready && !s.failed);
    // Real SMQ queue: RX_DONE releases transport capacity, not the RPC call.
    alignas(4096) static uint8_t rpcMemory[2][PicassoRpcDomainBytes];
    auto bind=[&](){
        PicassoRpcIo rpcIo={nullptr,[](void *,unsigned,unsigned,bool){return 0;},[](void *){return 0;}};
        assert(!picasso_rpc_bind(&s.fastrpc,rpcIo,rpcMemory[0],rpcMemory[1]));
    };
    rx=connectFastrpc(s);bind();at=s.txHead;step(s);drain(s);
    assert(s.fastrpc.intentAsked && s.fastrpc.domain[0].call[0].queued && !s.fastrpc.sent);
    assert(r32(s.tx.offset+at)==0x30007 && r32(s.tx.offset+at+4)==40);
    message(s,rx,8,9,1);frame(s,rx,offerFast,16);step(s);drain(s);
    at=s.txHead;step(s);drain(s);
    assert(s.fastrpc.sent==1 && s.fastrpc.domain[0].call[0].pending && s.fastrpcRemote[0].busy);
    assert(r32(s.tx.offset+at)==0x30009 && !r32(s.tx.offset+at+4) && r32(s.tx.offset+at+8)==40);
    assert(r32(s.tx.offset+at+16)==0 && r32(s.tx.offset+at+32)==1 && r32(s.tx.offset+at+36)==0x10000);
    message(s,rx,14,9,0);step(s);drain(s);
    assert(s.fastrpc.domain[0].call[0].pending && !s.fastrpcRemote[0].busy);
    uint8_t response[16]={};rpcPut64(response,s.fastrpc.domain[0].call[0].ctx);
    fastrpcPacket(s,rx,s.fastrpcLocal[0].id,response,16);step(s);drain(s);step(s);drain(s);
    assert(s.fastrpc.completed==1 && s.fastrpc.domain[0].stage==2);
    // Full FIFO cannot publish a context or reuse its DMA slot.
    rx=connectFastrpc(s);frame(s,rx,offerFast,16);step(s);drain(s);bind();at=s.txHead;
    p32(s.desc.offset,(at+8)%s.tx.size);step(s);
    assert(!s.fastrpc.sent && s.fastrpc.domain[0].call[0].queued && !s.fastrpcRemote[0].busy);
    drain(s);step(s);assert(s.fastrpc.sent==1 && s.fastrpc.domain[0].call[0].pending);
    // Failure at each actual TX word/head write never fabricates a send.
    for(unsigned n=1;n<=15;++n) {
        rx=connectFastrpc(s);frame(s,rx,offerFast,16);step(s);drain(s);bind();
        writes=0;failWrite=n;assert(picasso_glink_service(&io,&s,&kick)==-5);
        assert(s.fastrpc.stopped && !s.fastrpc.sent && !s.fastrpc.domain[0].call[0].pending);
    }
    puts("PASS FastRPC stock OPEN and 64x100 RX buffers, passive transport, fragments, reuse, pressure, isolation, invalid frames, I/O failures and shutdown");
}

static unsigned tftpReady(PicassoGlink &s) {
    unsigned rx=locatorAnnounceReady(s);step(s);drain(s);assert(s.qrtr.locator.announced);
    message(s,rx,14,2,0);step(s);drain(s);assert(!s.qrtr.tftp.announced);return rx;
}
__attribute__((noinline)) static void tftpTests() {
    PicassoGlink s={};unsigned rx=tftpReady(s);bool kick;
    writes=0;step(s);unsigned count=writes;assert(s.qrtr.tftp.announced);
    for(unsigned i=1;i<=count;++i) {
        rx=tftpReady(s);writes=0;failWrite=i;
        assert(picasso_glink_service(&io,&s,&kick)==-5 && s.failed && !s.qrtr.tftp.announced);
    }
    rx=tftpReady(s);p32(s.desc.offset,(s.txHead+8)%s.tx.size);
    for(unsigned i=0;i<10;++i)step(s);
    assert(!s.qrtr.tftp.announced && !s.qrtrRemote[0].busy);drain(s);
    unsigned at=s.txHead;step(s);drain(s);assert(s.qrtr.tftp.announced);
    bool found=false;
    for(unsigned i=at;i+68<=s.txHead;i+=8)if(r32(s.tx.offset+i)==0x20009) {
        const uint32_t expected[]={1,4,1,0xfffffffe,0,20,5,0xfffffffe,4,0x1000,2,1,0x188};
        assert(!memcmp(ram+s.tx.offset+i+16,expected,sizeof expected));found=true;
    }
    assert(found);message(s,rx,14,2,0);step(s);drain(s);
    const uint8_t wrq[]={0,2,'/','r','e','a','d','w','r','i','t','e','/','c','h','e','c','k',0,'o','c','t','e','t',0};
    uint32_t request[137]={1,1,5,73,1,sizeof wrq,1,0x188};memcpy(request+8,wrq,sizeof wrq);
    unsigned iid=s.qrtrLocal[0].id;s.qrtr.count=7;
    for(unsigned i=0;i<7;++i) { s.qrtr.queueBytes[i]=52;s.qrtr.queue[i][1]=7; }
    // Full queue + unavailable remote intent must retain the RX buffer once.
    s.qrtrRemote[0].size=0;
    qrtrPacket(s,rx,iid,request,32+((sizeof wrq+3)&~3U));step(s);drain(s);
    assert(!s.qrtr.tftp.requests && s.qrtrLocal[0].done && !s.qrtrLocal[0].delivered);
    s.qrtr.count=0;s.qrtrRemote[0].size=548;step(s);drain(s);
    assert(s.qrtr.tftp.requests==1 && s.qrtr.resumes==1 && s.qrtr.count==1);
    message(s,rx,14,2,0);step(s);drain(s);step(s);drain(s);
    assert(s.qrtr.tftp.replies==1 && !s.qrtr.count && !s.qrtr.locator.replies);
    unsigned port=s.qrtr.tftp.transfers[0].port;
    message(s,rx,14,2,0);step(s);drain(s);
    // Timeout resends exact ACK0 from the same temporary port.
    for(unsigned i=0;i<55 && s.qrtr.tftp.replies<2;++i) { step(s);drain(s); }
    assert(s.qrtr.tftp.retries==1 && s.qrtr.tftp.replies==2);
    message(s,rx,14,2,0);step(s);drain(s);
    request[4]=0;request[5]=516;request[7]=port;
    auto *body=reinterpret_cast<uint8_t *>(request+8);body[0]=0;body[1]=3;body[2]=0;body[3]=1;
    for(unsigned i=0;i<512;++i)body[4+i]=uint8_t(i);
    qrtrPacket(s,rx,iid,request,548);step(s);drain(s);step(s);drain(s);
    assert(!s.qrtr.tftp.writes && s.qrtr.tftp.replies==3);
    message(s,rx,14,2,0);step(s);drain(s);
    request[5]=4;body[3]=2;qrtrPacket(s,rx,iid,request,36);step(s);drain(s);step(s);drain(s);
    assert(s.qrtr.tftp.writes==1 && s.qrtr.tftp.files[0].size==512);
    message(s,rx,14,2,0);step(s);drain(s);
    request[5]=sizeof wrq;request[7]=0x188;memcpy(body,wrq,sizeof wrq);body[1]=1;
    qrtrPacket(s,rx,iid,request,32+((sizeof wrq+3)&~3U));step(s);drain(s);
    // 516-byte TFTP DATA + 32-byte QRTR header + GLINK alignment = 568.
    unsigned replies=s.qrtr.tftp.replies;s.qrtrRemote[0].size=544;
    step(s);drain(s);assert(s.qrtr.count==1 && s.qrtr.tftp.replies==replies && !s.qrtrRemote[0].busy);
    s.qrtrRemote[0].size=548;at=s.txHead;step(s);drain(s);
    assert(s.qrtr.tftp.replies==replies+1 && s.qrtrRemote[0].busy);
    found=false;for(unsigned i=at;i+568<=s.txHead;i+=8)if(r32(s.tx.offset+i)==0x20009) {
        assert(r32(s.tx.offset+i+8)==548 && r32(s.tx.offset+i+36)==516);
        for(unsigned j=0;j<512;++j)assert(ram[s.tx.offset+i+52+j]==uint8_t(j));
        assert(!r32(s.tx.offset+i+564));found=true;
    }
    assert(found);picasso_glink_stop(&s);assert(!s.qrtr.tftp.announced && !s.qrtr.tftp.files[0].used);
    puts("PASS stock instance2 TFTP announcement, fault injection, RX hold, timeout retry and max DATA through production GLINK");
}
