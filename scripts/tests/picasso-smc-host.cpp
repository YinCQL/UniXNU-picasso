// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <initializer_list>
static unsigned calls,interruptions;
static int32_t status;
static uint32_t response,fn,info;
static uint64_t arg,arg2;
static void fake_smc(uint64_t &x0,uint64_t &x1,uint64_t &x2,uint64_t &x3,
                     uint64_t &x4,uint64_t &x5,uint64_t &x6,uint64_t &x7){
    if(!calls){assert(x0==fn&&x1==info&&x2==arg&&x3==arg2&&!x4&&!x5&&!x6&&!x7);}
    else {assert(x0==1&&x1==101&&x2==102&&x3==103&&x4==104&&x5==105&&x6==calls+106&&x7==107);}
    ++calls;
    if(calls<=interruptions){x0=1;x1=101;x2=102;x3=103;x4=104;x5=105;x6=calls+106;x7=107;}
    else {x0=uint32_t(status);x1=response;}
}
#include "power-smc.inc"
static void setup(unsigned count,int32_t error,uint32_t value,uint32_t function,uint32_t argc,uint64_t a,uint64_t b){
    calls=0;interruptions=count;status=error;response=value;fn=function;info=argc;arg=a;arg2=b;
}
int main(){
    for(unsigned n: {0U,1U,63U}){
        setup(n,0,17,0xc2000110,2,0,0);Reply r=secure(fn,arg,info,arg2);
        assert(r.status==0&&r.value==17&&calls==n+1);
    }
    setup(64,0,0,0xc2000901,1,0,0);assert(secure(fn,arg).status==-23&&calls==64);
    setup(0,-12,0,0xc2000109,2,1,0);assert(secure(fn,arg,info,arg2).status==-12&&calls==1);
    setup(0,0,1,0xc2000601,1,0x02000901,0);assert(available(uint32_t(arg))&&calls==1);
    setup(0,0,0,0xc2000601,1,0x02000901,0);assert(!available(uint32_t(arg))&&calls==1);
    setup(0,-1,1,0xc2000601,1,0x02000901,0);assert(!available(uint32_t(arg))&&calls==1);
    puts("PASS production SMC loop argument encoding, returned register preservation, bounded interruption and support replies; instruction emulated");
}
