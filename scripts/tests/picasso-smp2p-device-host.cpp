// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "PicassoSmp2p.h"
#include "PicassoAdspBoot.h"
extern "C" void picasso_adsp_boot(const PicassoAdspEvents *e) { assert(e && e->poll && e->snapshot); }
static uint32_t ram[WLAN_SMEM_SIZE/4], mutexRam[1024], ipccRam[1024];
static unsigned allocations,mappings,releases,unmaps,lines,sleeps,preempt,plans,kicks;
static unsigned failAlloc,failMap,zeroMap;
static bool peer;
static constexpr unsigned part=0x1cf000, toc=WLAN_SMEM_SIZE-4096;
enum {kIOMapAnywhere=1,kIOMapInhibitCache=2,OSNoRetain=0};
class IOMemoryMap {
public:
    uintptr_t address;
    uintptr_t getVirtualAddress(){return address;}
    void release(){++unmaps;}
};
class IODeviceMemory {
public:
    unsigned slot;
    static IODeviceMemory *withRange(uint64_t base,uint64_t bytes) {
        unsigned at=base==WLAN_SMEM_BASE?0:base==0x01f43000?1:2;
        assert(at<2 || base==0x00408000);assert(bytes==(at?4096:WLAN_SMEM_SIZE));
        static IODeviceMemory memory[3];memory[at].slot=at;
        return ++allocations==failAlloc?nullptr:&memory[at];
    }
    IOMemoryMap *map(unsigned flags) {
        assert(flags==3);static IOMemoryMap maps[3];++mappings;
        maps[slot].address=mappings==zeroMap?0:reinterpret_cast<uintptr_t>(slot==0?ram:slot==1?mutexRam:ipccRam);
        return mappings==failMap?nullptr:&maps[slot];
    }
    void release(){++releases;}
};
template<class T> class OSSharedPtr {
    T *p;
public:
    OSSharedPtr(T *v,int):p(v){}
    ~OSSharedPtr(){if(p)p->release();}
    explicit operator bool()const{return p!=nullptr;}
    T *operator->()const{return p;}
};
static void IOLog(const char *fmt,...) {
    assert(!preempt);
    va_list a;va_start(a,fmt);vprintf(fmt,a);va_end(a);++lines;
}
static void disable_preemption(){assert(!preempt);++preempt;}
static void enable_preemption(){assert(preempt==1);--preempt;}
static unsigned inbound;
static void IOSleep(unsigned ms) {
    assert(!preempt);++sleeps;
    if(ms==1)return;
    assert(ms==20 && !mutexRam[0]);
    if(ipccRam[3]==0x30002){++kicks;ipccRam[3]=0;}
    if(peer && sleeps==1) {
        unsigned at=part+ram[(part+12)/4];
        ram[at/4]=0xa5a5|(443U<<16);ram[at/4+1]=344;ram[at/4+2]=4;
        ram[(part+12)/4]+=360;inbound=at+16;
        ram[inbound/4]=0x504d5324;ram[inbound/4+1]=0x101;
        ram[inbound/4+2]=2;ram[inbound/4+3]=16;ram[inbound/4+4]=1;
    }
    if(peer && sleeps==2) {
        memcpy(reinterpret_cast<uint8_t *>(ram)+inbound+20,"slave-kernel",13);
        ram[inbound/4+9]=2;ram[inbound/4+3]=16|(1U<<16);
    }
    if(peer && sleeps>2)ram[inbound/4+9]^=1; // Exercise the bounded event log.
    if(sleeps==1602)ram[(part+48+36)/4]=1; // External mutation stops service.
}
#include "PicassoSmp2pDevice-body.inc"
extern "C" int picasso_rpmh_adsp_plan(){assert(!preempt && !allocations);++plans;return 0;}
int main(int argc,char **argv) {
    assert(argc==2);const char *mode=argv[1];
    ram[0x5c/4]=12U<<16;ram[toc/4]=0x434f5424;ram[toc/4+1]=1;ram[toc/4+2]=1;
    ram[toc/4+8]=part;ram[toc/4+9]=0x30000;ram[toc/4+11]=2U<<16;ram[toc/4+12]=64;
    ram[part/4]=0x54525024;ram[part/4+1]=2U<<16;ram[part/4+2]=0x30000;
    ram[part/4+3]=32;ram[part/4+4]=0x30000;
    // Include the two real XBL reservations in the device-adapter fixture.
    ram[toc/4+2]=3;
    ram[(toc+80)/4]=toc;ram[(toc+84)/4]=4096;ram[(toc+88)/4]=1;ram[(toc+92)/4]=0xffffffff;
    ram[(toc+132)/4]=4096;ram[(toc+136)/4]=3;ram[(toc+140)/4]=0xffffffff;
    if(!strncmp(mode,"alloc",5))failAlloc=unsigned(mode[5]-'0');
    if(!strncmp(mode,"map",3))failMap=unsigned(mode[3]-'0');
    if(!strncmp(mode,"zero",4))zeroMap=unsigned(mode[4]-'0');
    if(!strcmp(mode,"owner"))mutexRam[0]=1;
    if(!strcmp(mode,"remote"))mutexRam[0]=2;
    if(!strcmp(mode,"gate"))allocatorGate=1;
    if(!strcmp(mode,"bad"))ram[part/4]=0;
    peer=!strcmp(mode,"peer");
    // Exercise aligned word reads, arbitrary output alignment and boundaries.
    SmemDevice d={reinterpret_cast<uintptr_t>(ram),reinterpret_cast<uintptr_t>(mutexRam),0,false};
    uint8_t data[11]={};assert(!smemRead(&d,0x5d,data+1,7));
    assert(!memcmp(data+1,reinterpret_cast<uint8_t *>(ram)+0x5d,7));
    assert(smemRead(&d,WLAN_SMEM_SIZE-1,data,2)==-22);
    picasso_smp2p_start();picasso_smp2p_start();
    assert(allocations==3 && releases==3-!!failAlloc && mappings==releases && unmaps==mappings-!!failMap && !preempt);
    if(!strcmp(mode,"normal") || peer) {
        assert(plans==1 && sleeps==1602 && kicks==(peer?2U:1U));
        assert(lines==(peer?69U:7U));assert(ram[(part+12)/4]==(peer?752U:392U));
        assert(!allocatorGate && !mutexRam[0]);
    } else {
        assert(plans==1 && !kicks && ram[(part+12)/4]==32);
        assert(sleeps==(!strcmp(mode,"remote")?100U:0U));
        if(strcmp(mode,"gate"))assert(!allocatorGate);
    }
    puts("PASS SMP2P device mappings, local/remote ownership, one-shot gate, persistent poll/negotiation and bounded logging");
}
