// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "PicassoPas.h"
static unsigned calls,delays,mode;
static PicassoPasReply call(void *,uint32_t fn,uint32_t info,uint64_t pas,
                           uint64_t addr,uint64_t bytes,uint64_t session,bool first) {
    ++calls;assert(pas==1 && info==0x82 && addr==0x8bc18000 && !bytes);
    if(mode==1 && calls<4) {
        assert(fn==(calls==1?0x42000201U:1U) && first==(calls==1));
        assert(session==(calls==1?0:0xaabbccdd00000000ULL+calls-1));
        return {1,0,0,0,0xaabbccdd00000000ULL+calls};
    }
    if(mode==1){assert(fn==1&&!first&&session==0xaabbccdd00000003ULL);return {};}
    if(mode==2){assert(first&&fn==0x42000201&&!session);return {uint32_t(-12),0,0,0,0};}
    if(mode==3)return {1,0,0,0,0x1234567800000000ULL+calls};
    if(mode==4)return {0,7,0,0,0};
    if(mode==5)return {uint32_t(-1),0,0,0,0};
    assert(fn==0x42000201 && first && !session);return {};
}
static void delay(void *,unsigned ms){assert(ms==1||ms==30);++delays;}
int main(){
    PicassoPasIo io={nullptr,call,delay};PicassoPasReply r={};
    for(mode=0;mode<6;++mode) {
        calls=delays=0;int error=picasso_pas_execute(&io,1,0x8bc18000,0,&r);
        const int errors[]={0,0,-16,-115,-13,-5};assert(error==errors[mode]);
        if(mode==1)assert(calls==4&&delays==3);
        if(mode==2)assert(calls==68&&delays==68);
        if(mode==3)assert(calls==65&&delays==64);
    }
    calls=0;
    assert(picasso_pas_execute(&io,1,0,0,&r)==-22);
    assert(picasso_pas_execute(&io,2,0x89200000,0x2800001,&r)==-22);
    assert(picasso_pas_execute(&io,5,1,0,&r)==-22);
    assert(picasso_pas_execute(&io,7,0,0,&r)==-22);
    assert(!calls);
    puts("PASS PAS x6 resume token, bounded interrupted/busy responses, TZ errors and exact arguments");
}
