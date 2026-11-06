// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "picasso-rpmh-device-api.h"
static uint32_t regs[0x10000/4], db[0x20000/4];
static unsigned maps,unmaps,allocs,frees,sleeps,delays,writes,commands,failMap,zeroMap,failCommand;
static unsigned ticks,triggerAt,active,dbReads;
static bool noMemory,unstable,reenter,entered;
static uint32_t addresses[10],values[10];
void IOMemoryMap::release(){++unmaps;}
bool mapPhysical(uint64_t base,uint64_t bytes,Mapping &m) {
    assert(base==0x18220000 || base==0x80860000);++maps;
    if(maps==failMap)return false;
    assert(bytes==(base==0x18220000?sizeof regs:sizeof db));
    static IOMemoryMap handles[2];
    m={&handles[base==0x80860000],maps==zeroMap?nullptr:reinterpret_cast<uint8_t *>(base==0x18220000?regs:db),bytes,base};
    return m.base!=nullptr;
}
uint32_t read32(const Mapping &m,uint64_t at) {
    assert(m.base && !(at&3) && at<m.bytes);
    auto value=*reinterpret_cast<volatile uint32_t *>(m.base+at);
    if(m.physical==0x80860000 && unstable && ++dbReads>sizeof(db)/4)return value^1;
    return value;
}
void write32(const Mapping &m,uint64_t at,uint32_t value) {
    assert(m.physical==0x18220000 && !(at&3) && at<m.bytes);++writes;
    if(at==0xd08){regs[0xd04/4]&=~value;return;}
    regs[at/4]=value;
    if((at==0xd14 || at==0xfb4) && (value&(1U<<24))) {
        active=unsigned(at-0x14);++commands;triggerAt=ticks;
        addresses[commands-1]=regs[(active+0x34)/4];values[commands-1]=regs[(active+0x38)/4];
        regs[(active+0x18)/4]=0;regs[(active+0x3c)/4]=0;
    }
}
void barrier(){}
void IOSleep(unsigned ms){assert(ms==1);++sleeps;}
void IODelay(unsigned us) {
    ++delays;ticks+=us;
    if(reenter && !entered) {entered=true;assert(picasso_rpmh_active_write(0x30000,3)==-16);}
    if(commands!=failCommand && ticks-triggerAt>=30) {regs[(active+0x18)/4]=1;regs[(active+0x3c)/4]=0x10101;}
}
void *IOMalloc(size_t n){assert(n==sizeof db);++allocs;return noMemory?nullptr:malloc(n);}
void IOFree(void *p,size_t n){assert(n==sizeof db);++frees;free(p);}
#include "PicassoRpmh-owner-body.inc"
static void p16(unsigned at,unsigned v){auto *p=reinterpret_cast<uint8_t *>(db)+at;p[0]=uint8_t(v);p[1]=uint8_t(v>>8);}
static void p32(unsigned at,unsigned v){p16(at,v);p16(at+2,v>>16);}
static void setup() {
    memset(regs,0,sizeof regs);memset(db,0,sizeof db);
    maps=unmaps=allocs=frees=sleeps=delays=writes=commands=failMap=zeroMap=failCommand=ticks=dbReads=0;
    noMemory=unstable=reenter=entered=false;rpmhGate=rpmhPoisoned=0;
    regs[0]=0x20005;regs[0xd00/4]=0xa5;regs[(0xd00+0x18)/4]=regs[(0xd00+672+0x18)/4]=1;
    p32(0,1);p32(4,0x0c0330db);p16(8,5);p16(10,0);p16(12,240);p16(14,7);
    const char *bcm[]={"CE0","SN6","SN0","SH0","MC0","ACV","CN0"};
    const unsigned addr[]={0x5006c,0x50048,0x50028,0x50004,0x50000,0x5007c,0x50074};
    for(unsigned i=0;i<7;++i) {
        unsigned at=144+24*i;memcpy(reinterpret_cast<uint8_t *>(db)+at,bcm[i],strlen(bcm[i]));
        p32(at+16,addr[i]);p16(at+20,8);p16(at+22,8*i);p32(384+8*i,6802);p16(388+8*i,8);
    }
    p16(24,3);p16(26,320);p16(28,400);p16(30,3);
    const char *arc[]={"lcx.lvl","lmx.lvl","xo.lvl"};
    for(unsigned i=0;i<3;++i) {
        unsigned at=464+24*i;memcpy(reinterpret_cast<uint8_t *>(db)+at,arc[i],7);
        p32(at+16,0x30000+4*i);p16(at+20,i==2?0:12);p16(at+22,12*i);
        if(i<2){p16(544+12*i+2,48);p16(544+12*i+4,192);p16(544+12*i+6,384);p16(544+12*i+8,416);}
    }
}
int main() {
    setup();reenter=true;assert(rpmhVoteCryptoPath()==7 && entered && sleeps==100 && maps==unmaps && !rpmhGate);
    const uint32_t expected[]={0x5006c,0x50048,0x50028,0x50004,0x50000,0x5007c,0x50074};
    for(unsigned i=0;i<7;++i)assert(addresses[i]==expected[i] && values[i]==(i==5?0x60000008U:0x6fffffffU));
    assert(!picasso_rpmh_active_write(0x30000,3) && commands==8 && maps==unmaps && regs[0xd00/4]==0xa5);
    for(unsigned n=1;n<=7;++n) {
        setup();failCommand=n;assert(rpmhVoteCryptoPath()==n-1 && commands==n && rpmhPoisoned && !rpmhGate && maps==unmaps);
        unsigned old=writes;assert(picasso_rpmh_active_write(0x30000,3)==-16 && writes==old && !rpmhGate);
    }
    for(unsigned n=1;n<=2;++n) {
        setup();failMap=n;assert(!rpmhVoteCryptoPath() && maps==unmaps+1 && !rpmhGate && !commands);
        setup();zeroMap=n;assert(!rpmhVoteCryptoPath() && maps==unmaps && !rpmhGate && !commands);
    }
    setup();rpmhGate=1;assert(picasso_rpmh_active_write(0x30000,3)==-16 && sleeps==100 && !maps);rpmhGate=0;
    setup();failMap=1;assert(picasso_rpmh_active_write(0x30000,3)==-12 && !rpmhGate);
    setup();assert(!picasso_rpmh_adsp_plan() && allocs==frees && maps==unmaps && !writes && !rpmhGate);
    setup();unstable=true;assert(picasso_rpmh_adsp_plan()==-11 && allocs==frees && maps==unmaps && !writes && !rpmhGate);
    setup();noMemory=true;assert(picasso_rpmh_adsp_plan()==-12 && !frees && !maps && !rpmhGate);
    setup();failMap=1;assert(picasso_rpmh_adsp_plan()==-12 && frees==1 && !unmaps && !rpmhGate);
    setup();zeroMap=1;assert(picasso_rpmh_adsp_plan()==-12 && frees==1 && unmaps==1 && !rpmhGate);
    setup();p32(488+16,0x30000);assert(picasso_rpmh_adsp_plan()==-22 && !writes && maps==unmaps && allocs==frees);
    puts("PASS production RPMh GPU votes/values, serialized re-entry, quarantine at all seven commands, mapping/allocator cleanup, stable ARC plan without writes");
}
