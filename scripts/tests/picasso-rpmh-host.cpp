// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "PicassoRpmh.h"
static uint32_t regs[0x10000/4];
static unsigned writes,delays,barriers,completeAt,dropOffset;
static uint32_t active;
static bool stalled,dropControl,dropCleanup,partial,irqChanged;
static uint32_t rd(void *,uint32_t at){assert(!(at&3)&&at<sizeof regs);return regs[at/4];}
static void wr(void *,uint32_t at,uint32_t v) {
    assert(!(at&3)&&at<sizeof regs);++writes;
    if(at==dropOffset || (dropCleanup && at==active+0x1c && !v))return;
    if(at==0xd08) {regs[0xd04/4]&=~v;return;}
    if(at==active+0x14 && dropControl)return;
    regs[at/4]=v;
    if(at==active+0x14 && (v&(1U<<24))) {
        assert(barriers && regs[(active+0x1c)/4]==1);
        regs[(active+0x18)/4]=0;regs[(active+0x3c)/4]=0; // HW starts fresh command.
    }
}
static void fence(void *){++barriers;}
static void delay(void *,unsigned us) {
    delays+=us;
    if(!stalled && delays>=completeAt && (regs[(active+0x14)/4]&(1U<<24))) {
        regs[(active+0x18)/4]=1;regs[(active+0x3c)/4]=partial?0x10001:0x10101;
        if(irqChanged)regs[0xd00/4]|=0x8000;
    }
}
static PicassoRscIo io={nullptr,rd,wr,fence,delay};
static void setup(unsigned tcs=0) {
    memset(regs,0,sizeof regs);writes=delays=barriers=dropOffset=0;completeAt=30;
    stalled=dropControl=dropCleanup=partial=irqChanged=false;active=0xd00+672*tcs;
    regs[0]=0x20005;regs[0xd00/4]=0xa5;regs[(0xd00+0x18)/4]=1;regs[(0xd00+672+0x18)/4]=1;regs[(active+0x3c)/4]=0x10101;
}
static uint8_t db[2048];
static void p16(unsigned at,unsigned v){db[at]=uint8_t(v);db[at+1]=uint8_t(v>>8);}
static void p32(unsigned at,unsigned v){p16(at,v);p16(at+2,v>>16);}
static void database() {
    memset(db,0,sizeof db);p32(0,1);p32(4,0x0c0330db);p16(8,3);p16(10,0);p16(12,96);p16(14,3);
    const char *names[]={"lcx.lvl","lmx.lvl","xo.lvl"};
    for(unsigned i=0;i<3;++i) {
        unsigned at=144+i*24;memcpy(db+at,names[i],7);p32(at+16,0x30000+4*i);p16(at+20,i==2?0:12);p16(at+22,i*12);
        if(i<2){p16(240+i*12+2,48);p16(240+i*12+4,192);p16(240+i*12+6,384);p16(240+i*12+8,416);}
    }
}
int main() {
    PicassoRscResult r;
    for(unsigned t=0;t<2;++t) {
        setup(t);assert(!picasso_rsc_send(&io,t,0x5006c,0x6fffffff,&r));
        assert(!r.poisoned && r.spins==3 && regs[0xd00/4]==0xa5 && !regs[(active+0x1c)/4] && !regs[(active+0x14)/4]);
    }
    const unsigned busy[]={0x18,0x1c};
    for(unsigned at:busy){setup();regs[(active+at)/4]=at==0x18?0:1;assert(picasso_rsc_send(&io,0,0x30000,3,&r)==-16 && !writes);}
    setup();regs[(active+0x14)/4]=0x1010000;assert(!picasso_rsc_send(&io,0,0x30000,3,&r));
    setup();regs[(0xd00+672+0x1c)/4]=1;assert(picasso_rsc_send(&io,0,0x30000,3,&r)==-16 && !writes);
    setup();regs[0xd04/4]=1;assert(picasso_rsc_send(&io,0,0x30000,3,&r)==-16 && !writes);
    setup();regs[0]=0x30000;assert(picasso_rsc_send(&io,0,0x30000,3,&r)==-22 && !writes);
    setup();assert(picasso_rsc_send(&io,2,0x30000,3,&r)==-22 && !writes);
    setup();assert(picasso_rsc_send(&io,0,0x30001,3,&r)==-22 && !writes);
    setup();stalled=true;assert(picasso_rsc_send(&io,0,0x30000,3,&r)==-60 && r.poisoned);
    assert(regs[(active+0x1c)/4]==1 && (regs[(active+0x14)/4]&(1U<<24)) && regs[0xd00/4]==0xa4);
    unsigned old=writes;assert(picasso_rsc_send(&io,0,0x30004,4,&r)==-16 && writes==old);
    setup();dropControl=true;assert(picasso_rsc_send(&io,0,0x30000,3,&r)==-60 && r.poisoned && regs[0xd00/4]==0xa4);
    const unsigned payloadOffsets[]={0x30,0x34,0x38,0x1c};
    for(unsigned off:payloadOffsets) {
        setup();dropOffset=active+off;assert(picasso_rsc_send(&io,0,0x30000,3,&r)==-60 && r.poisoned);
        assert(!(regs[(active+0x14)/4]&(1U<<24)));
    }
    setup();dropCleanup=true;assert(picasso_rsc_send(&io,0,0x30000,3,&r)==-60 && r.poisoned && regs[(active+0x1c)/4]==1);
    setup();partial=true;assert(picasso_rsc_send(&io,0,0x30000,3,&r)==-60 && r.poisoned);
    setup();irqChanged=true;assert(!picasso_rsc_send(&io,0,0x30000,3,&r) && regs[0xd00/4]==0x80a5);
    database();PicassoArcResource a;
    assert(!picasso_arc_lookup(db,sizeof db,"lcx.lvl",384,&a) && a.address==0x30000 && a.index==3 && a.level==384);
    assert(!picasso_arc_lookup(db,sizeof db,"xo.lvl",UINT32_MAX,&a) && a.address==0x30008);
    assert(picasso_arc_lookup(db,sizeof db,"none",384,&a)==-2);
    assert(picasso_arc_lookup(db,sizeof db,"lcx.lvl",500,&a)==-34);
    database();p16(240+8,192);assert(picasso_arc_lookup(db,sizeof db,"lcx.lvl",384,&a)==-22);
    database();p16(240+6,0);assert(picasso_arc_lookup(db,sizeof db,"lcx.lvl",384,&a)==-22);
    database();p16(144+20,33);assert(picasso_arc_lookup(db,sizeof db,"lcx.lvl",384,&a)==-22);
    database();p16(144+22,2000);assert(picasso_arc_lookup(db,sizeof db,"lcx.lvl",384,&a)==-22);
    database();memcpy(db+168,db+144,24);assert(picasso_arc_lookup(db,sizeof db,"lcx.lvl",384,&a)==-22);
    database();p16(8,5);assert(picasso_arc_lookup(db,sizeof db,"lcx.lvl",384,&a)==-22);
    database();p32(160,0x50000);assert(picasso_arc_lookup(db,sizeof db,"lcx.lvl",384,&a)==-22);
    puts("PASS RPMh: fresh completion, shared bit preservation, busy refusal, timeout quarantine, ARC identity/levels/bounds/duplicates");
}
