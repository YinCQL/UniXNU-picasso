// SPDX-License-Identifier: BSD-2-Clause-Patent
// Execute the production platform adapter with owned mappings and a clock.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "PicassoQmp.h"
static uint32_t ram[1024], ipcc[1024];
static unsigned allocations,mappings,releases,unmaps,lines,sleeps;
static unsigned failAlloc,failMap;
static uint64_t ticks;
static bool stalled,closeStalled,opened;
enum {kIOMapAnywhere=1,kIOMapInhibitCache=2,OSNoRetain=0,kSecondScale=1};
class IOMemoryMap {
public:
    uintptr_t address;
    uintptr_t getVirtualAddress() {return address;}
    void release() {++unmaps;}
};
class IODeviceMemory {
public:
    unsigned slot;
    static IODeviceMemory *withRange(uint64_t base,uint64_t size) {
        assert((base==0x0c300000 || base==0x00408000) && size==4096);
        static IODeviceMemory memory[2];unsigned at=base==0x00408000;
        memory[at].slot=at;++allocations;return allocations==failAlloc?nullptr:&memory[at];
    }
    IOMemoryMap *map(unsigned flags) {
        assert(flags==3);++mappings;static IOMemoryMap maps[2];
        maps[slot].address=reinterpret_cast<uintptr_t>(slot?ipcc:ram);
        return mappings==failMap?nullptr:&maps[slot];
    }
    void release() {++releases;}
};
template<class T> class OSSharedPtr {
    T *p;
public:
    OSSharedPtr(T *value,int):p(value){}
    ~OSSharedPtr(){if(p)p->release();}
    explicit operator bool()const{return p!=nullptr;}
    T *operator->()const{return p;}
};
static void IOLog(const char *fmt,...) {
    va_list args;va_start(args,fmt);vprintf(fmt,args);va_end(args);++lines;
}
static uint64_t mach_absolute_time() {return ticks;}
static void clock_interval_to_deadline(unsigned n,unsigned scale,uint64_t *out) {assert(scale==kSecondScale);*out=ticks+n*1000;}
static void IOSleep(unsigned n) {
    ticks+=n;++sleeps;
    if(stalled)return;
    ram[10]=0xffff;
    if(ram[11]==0xffff)opened=true;
    if(!(closeStalled && opened && ram[11]==0xffff0000))ram[12]=ram[11];
}
#include "PicassoAop-body.inc"
int main(int argc,char **argv) {
    assert(argc==2);const char *mode=argv[1];
    ram[0]=0x4d41494c;ram[1]=1;ram[3]=ram[5]=0xffff;
    ram[7]=ram[13]=256;ram[8]=0x100;ram[14]=0x200;
    for(auto &v:ipcc)v=0x5aa55aa5;
    if(!strcmp(mode,"alloc-ram"))failAlloc=1;
    if(!strcmp(mode,"alloc-ipcc"))failAlloc=2;
    if(!strcmp(mode,"map-ram"))failMap=1;
    if(!strcmp(mode,"map-ipcc"))failMap=2;
    stalled=!strcmp(mode,"timeout");closeStalled=!strcmp(mode,"close-timeout");
    bool busy=!strcmp(mode,"busy"),bad=!strcmp(mode,"bad"),rx=!strcmp(mode,"rx");
    if(busy)ram[0x200/4]=96;
    if(rx)ram[0x100/4]=4;
    if(bad)ram[14]=60;
    uint32_t original[1024];memcpy(original,ram,sizeof ram);
    int result=picasso_aop_probe();assert(picasso_aop_probe()==-16);
    assert((result==0)==!(failAlloc || failMap || bad || busy || rx || stalled || closeStalled));
    assert(allocations==2 && releases==2-!!failAlloc);
    assert(mappings==releases && unmaps==mappings-!!failMap);
    assert(lines==((failAlloc || failMap)?1:2));
    for(unsigned n=0;n<1024;++n)if(n!=3)assert(ipcc[n]==0x5aa55aa5);
    for(unsigned n=16;n<1024;++n)assert(ram[n]==original[n]); // No message payload/length writes.
    if(failAlloc || failMap || bad || busy || rx) {
        assert(!sleeps && ipcc[3]==0x5aa55aa5 && !memcmp(original,ram,sizeof ram));
    } else {
        assert(ipcc[3]==0 && ram[11]==0xffff0000);
        if(stalled)assert(ticks==5000 && !opened); // Close detects the missing link immediately.
        else if(closeStalled)assert(ticks>=5000 && opened && ram[12]==0xffff);
        else assert(opened && ticks<10 && ram[12]==0xffff0000);
    }
    puts("PASS AOP adapter: one attempt, isolated IPCC send, bounded waits, no payload, mapping cleanup");
}
