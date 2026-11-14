// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "PicassoPowerCore.h"
struct Backend {
    uint8_t regs[256]={};
    unsigned started=0,operations=0,failAt=0,drops=0,drains=0,halts=0,delayUs=0;
    bool corrupt=false;
    int (*previous)(unsigned)=nullptr;
    std::vector<unsigned> writes;
    Backend(){regs[5]=4;regs[0x5a]=0xa7;regs[0x5b]=0xd5;}
    int read(uint8_t reg,uint8_t &value){if(++operations==failAt)return -40;value=regs[reg];if(corrupt && writes.size()==3 && reg==0x5a)value^=1;return 0;}
    int write(uint8_t reg,uint8_t value){if(++operations==failAt)return -40;assert(reg==0x5a||reg==0x5b);regs[reg]=value;writes.push_back(unsigned(reg)<<8|value);return 0;}
    void delay(unsigned us){delayUs+=us;}
    int prepare(){return ++operations==failAt?-40:0;}
    int quiesce(){++drains;assert((regs[0x5a]&15)==4&&(regs[0x5b]&128));return ++operations==failAt?-40:0;}
    int halt_arbiter(){++halts;assert(drains);return ++operations==failAt?-40:0;}
    void drop_hold(){assert(drains&&halts);++drops;}
};
static Backend *backend;
enum { kPEHaltCPU=0 };
static void IOLog(const char *,...){}
#include "power-dispatch.inc"
static unsigned delegated;
static int previous(unsigned type){delegated=type;return 71;}
int main(){
    for(unsigned i=1;i<=11;++i){Backend b;b.failAt=i;auto r=picasso_power::shutdown(b);assert(r.error==-40&&!b.drops);}
    {Backend b;b.regs[5]=5;auto r=picasso_power::shutdown(b);assert(r.error==-20&&b.writes.empty()&&!b.drains);}
    {Backend b;b.corrupt=true;auto r=picasso_power::shutdown(b);assert(r.error==-21&&!b.drains&&!b.drops);}
    {Backend b;auto r=picasso_power::shutdown(b);assert(r.stage==picasso_power::returned&&r.error==-22&&b.drops==1&&b.delayUs==1000500);
     assert((b.writes==std::vector<unsigned>{0x5b55,0x5aa4,0x5bd5}));}
    assert(halt(0)==-1);
    Backend b;backend=&b;
    for(unsigned t=1;t<12;++t){assert(halt(t)==-1);assert(!b.operations&&!b.started);}
    b.previous=previous;
    for(unsigned t=1;t<12;++t){assert(halt(t)==71&&delegated==t);assert(!b.operations&&!b.started);}
    assert(halt(0)==-22&&b.started&&b.drops==1);unsigned operations=b.operations;
    assert(halt(0)==-1&&b.operations==operations&&b.drops==1);
    puts("PASS primary PON identity, masked sequencing/delay/readback, eleven failure boundaries, no success on returned power-off, halt-only dispatch and repeated-call guard");
}
