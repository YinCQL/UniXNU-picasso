// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real protocol body with an adversarial MMIO peer; no production logic mocked.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "PicassoQmp.h"
static constexpr uint32_t UP=0xffff, DOWN=0xffff0000;
struct Bus {
    uint32_t ram[1024];
    unsigned reads, writes, barriers, kicks, changed;
    bool payload, published;
};
static uint32_t get(void *ctx,uint32_t at) {
    auto *b=(Bus *)ctx; assert(!(at&3) && at<sizeof b->ram); ++b->reads;
    uint32_t v=b->ram[at/4];
    if(b->changed==b->reads)b->ram[14]+=4;
    return v;
}
static void put(void *ctx,uint32_t at,uint32_t v) {
    auto *b=(Bus *)ctx; assert(!(at&3) && at<sizeof b->ram); ++b->writes;
    // Only local state, remote acknowledgements and the negotiated TX window.
    assert(at==16 || at==24 || at==36 || at==44 || (at>=0x200 && at<0x264));
    if(at>0x200) {b->payload=true; b->barriers=0;}
    if(at==0x200) {assert(b->payload && b->barriers && v==96); b->published=true;b->barriers=0;}
    b->ram[at/4]=v;
}
static void fence(void *ctx) {++((Bus *)ctx)->barriers;}
static void bell(void *ctx) {auto *b=(Bus *)ctx;assert(b->barriers);++b->kicks;}
static Bus setup() {
    Bus b={}; b.ram[0]=0x4d41494c;b.ram[1]=1;
    b.ram[3]=UP;b.ram[5]=UP;b.ram[7]=256;b.ram[8]=0x100;b.ram[13]=256;b.ram[14]=0x200;
    return b;
}
static QmpIo io(Bus *b) {return {b,get,put,fence,bell};}
static void ready(PicassoQmp *q,Bus *b) {
    QmpIo i=io(b); assert(picasso_qmp_begin(q,&i,4096,100)==QmpLinkWait);
    b->ram[10]=UP;b->ram[12]=DOWN;
    assert(picasso_qmp_step(q,1)==QmpChannelWait);
    b->ram[12]=UP; assert(picasso_qmp_step(q,2)==QmpOpen);
}
int main() {
    Bus b=setup();QmpIo i=io(&b);PicassoQmp q={};
    assert(picasso_qmp_begin(&q,&i,60,100)==QmpDescriptor && !b.reads);
    const unsigned fields[]={0,1,2,7,8,13,14};
    for(unsigned f:fields) {
        const uint32_t bad[]={1,3,60,4092,4096,0xfffffffc};
        for(uint32_t v:bad) {
            b=setup();b.ram[f]=v;
            if(f==1 && v==1)continue;
            // Some sizes/offsets are legal; exact boundary scenarios below.
            if((f==7 && (v==60 || v==4092)) || (f==13 && v==4092))continue;
            assert(picasso_qmp_begin(&q,&i,4096,100)==QmpDescriptor && !b.writes);
        }
    }
    b=setup();b.ram[8]=0x180;assert(picasso_qmp_begin(&q,&i,4096,100)==QmpDescriptor && !b.writes);
    b=setup();b.ram[13]=96;assert(picasso_qmp_begin(&q,&i,4096,100)==QmpDescriptor && !b.writes);
    b=setup();b.changed=15;assert(picasso_qmp_begin(&q,&i,4096,100)==QmpDescriptor && !b.writes);
    b=setup();b.ram[0x200/4]=96;assert(picasso_qmp_begin(&q,&i,4096,100)==QmpBusy && !b.writes);
    b=setup();b.ram[0x100/4]=4;assert(picasso_qmp_begin(&q,&i,4096,100)==QmpReceivePending && !b.writes);
    b=setup();b.ram[3]=7;assert(picasso_qmp_begin(&q,&i,4096,100)==QmpDescriptor && !b.writes);
    // Inherited UP cannot bypass the fresh channel-down acknowledgement.
    b=setup();b.ram[4]=b.ram[6]=b.ram[9]=b.ram[10]=b.ram[11]=b.ram[12]=UP;
    assert(picasso_qmp_begin(&q,&i,4096,100)==QmpLinkWait);
    assert(b.ram[11]==DOWN && picasso_qmp_step(&q,1)==QmpLinkWait);
    assert(picasso_qmp_step(&q,100)==QmpTimeout);
    assert(picasso_qmp_close(&q,200)==QmpClosing);b.ram[12]=DOWN;
    assert(picasso_qmp_step(&q,101)==QmpClosed && !q.owned);
    // Peer can open either before or after acknowledging our channel.
    b=setup();b.ram[5]=DOWN;assert(picasso_qmp_begin(&q,&i,4096,100)==QmpLinkWait);
    b.ram[10]=UP;b.ram[12]=DOWN;assert(picasso_qmp_step(&q,1)==QmpChannelWait);
    b.ram[12]=UP;assert(picasso_qmp_step(&q,2)==QmpChannelWait);
    b.ram[5]=UP;assert(picasso_qmp_step(&q,3)==QmpOpen);
    // Wire size is stock MAX_LEN=96, including zero padding, and length is last.
    const char *msg="{class: image, res: load_state, name: adsp, val: on}";
    assert(picasso_qmp_send(&q,msg,100)==QmpSending && b.published);
    assert(!memcmp(&b.ram[0x204/4],msg,strlen(msg)));
    for(unsigned n=(unsigned)strlen(msg);n<96;++n)assert(((uint8_t *)&b.ram[0x204/4])[n]==0);
    assert(picasso_qmp_send(&q,msg,100)==QmpBusy);
    assert(picasso_qmp_step(&q,5)==QmpSending);b.ram[0x200/4]=0;
    assert(picasso_qmp_step(&q,6)==QmpOpen);
    char longText[97];memset(longText,'x',96);longText[96]=0;
    unsigned before=b.writes;assert(picasso_qmp_send(&q,longText,100)==QmpTooLong && b.writes==before);
    assert(picasso_qmp_send(&q,"",100)==QmpTooLong && b.writes==before);
    longText[95]=0;assert(picasso_qmp_send(&q,longText,100)==QmpSending);
    assert(((uint8_t *)&b.ram[0x204/4])[95]==0);b.ram[0x200/4]=0;
    assert(picasso_qmp_step(&q,7)==QmpOpen);
    assert(picasso_qmp_close(&q,100)==QmpClosing);b.ram[12]=DOWN;
    assert(picasso_qmp_step(&q,7)==QmpClosed && !q.owned);
    // Timeout preserves a pending packet and refuses to close/overwrite it.
    b=setup();ready(&q,&b);assert(picasso_qmp_send(&q,msg,10)==QmpSending);
    before=b.writes;assert(picasso_qmp_step(&q,10)==QmpTimeout && b.writes==before);
    assert(picasso_qmp_close(&q,20)==QmpBusy && b.ram[0x200/4]==96 && b.writes==before);
    // Link/channel loss wins even if the same peer update clears TX length.
    const unsigned loss[]={3,4,5,6,9,10,11,12};
    for(unsigned f:loss) {
        b=setup();ready(&q,&b);assert(picasso_qmp_send(&q,msg,100)==QmpSending);
        b.ram[f]=DOWN;b.ram[0x200/4]=0;
        assert(picasso_qmp_step(&q,3)==QmpDisconnected);
    }
    // Changed geometry never reaches an MMIO write; unsolicited RX stays intact.
    b=setup();ready(&q,&b);b.ram[14]=0x300;before=b.writes;
    assert(picasso_qmp_send(&q,msg,100)==QmpDescriptor && before==b.writes);
    assert(picasso_qmp_close(&q,100)==QmpDescriptor && before==b.writes);
    b=setup();ready(&q,&b);b.ram[0x100/4]=4;
    assert(picasso_qmp_step(&q,3)==QmpReceivePending && b.ram[0x100/4]==4);
    puts("PASS QMP: descriptors, overlap, stable layout, stale ownership, handshake orders, wire/barriers, timeout, disconnect, cleanup");
}
