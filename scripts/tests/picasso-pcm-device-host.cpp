// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
static uint32_t registers[0x100000/4];
alignas(4096) static uint8_t storage[49152];
static unsigned mode,allocations,descriptors,mappings,barriers;
using IOOptionBits=unsigned long long;using IOByteCount=unsigned long long;
static void *kernel_task=nullptr;
enum { kIOMapAnywhere=1,kIOMapInhibitCache=2,kIODirectionInOut=3,kIOMemoryPhysicallyContiguous=16,kIOMemoryMapperNone=32 };
class IOMemoryMap {public:uintptr_t getVirtualAddress(){return mode==3?0:uintptr_t(registers);} };
class IODeviceMemory {
public:
    static IODeviceMemory *withRange(uint64_t address,uint64_t bytes) {
        assert(address==0x15000000 && bytes==0x100000);++descriptors;
        static IODeviceMemory d;return mode==1?nullptr:&d;
    }
    IOMemoryMap *map(unsigned flags) { assert(flags==3);++mappings;static IOMemoryMap m;return mode==2?nullptr:&m; }
    void release() {}
};
class IOBufferMemoryDescriptor {
public:
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *task,IOOptionBits flags,unsigned bytes,unsigned alignment) {
        assert(!task && flags==51 && bytes==45056 && alignment==4096);++allocations;
        static IOBufferMemoryDescriptor d;return mode==4?nullptr:&d;
    }
    void *getBytesNoCopy(){return mode==5?nullptr:storage+(mode==6?1:0);}
    uint64_t getPhysicalSegment(unsigned offset,IOByteCount *bytes,unsigned flags) {
        assert(!offset && flags==32);*bytes=mode==7?40960:45056;
        return mode==8?0:mode==9?0x123450001ULL:mode==10?(1ULL<<36):mode==11?0xfffff8000ULL:0x123450000ULL;
    }
};
static void IOLog(const char *fmt,...) {
    char text[1024];va_list a;va_start(a,fmt);int n=vsnprintf(text,sizeof text,fmt,a);va_end(a);
    assert(n>0 && n<256 && text[n-1]=='\n');
}
static void dmaBarrier() {
    ++barriers;
    // Model FSR W1C. Other register accesses are the production volatile MMIO.
    for(unsigned bank=0;bank<62;++bank) {
        auto &fsr=registers[((128+bank)*4096+0x58)/4];if(fsr==0xffffffff)fsr=0;
    }
}
#include "PicassoRpcDma-body.inc"
int main(int argc,char **argv) {
    assert(argc==2);mode=unsigned(atoi(argv[1]));assert(mode<16);
    registers[8]=0x4c017e51;registers[9]=0x6000003e;registers[10]=0x5111;
    for(unsigned b=0;b<62;++b) {registers[(128+b)*1024]=0x166;registers[(128+b)*1024+1]=0x32b;}
    // Two already routed FastRPC streams and ASIDs must survive unchanged.
    registers[0x800/4]=0x80001403;registers[0xc00/4]=61;registers[(189*4096+0x24)/4]=0x1880000;
    registers[0x804/4]=0x80001404;registers[0xc04/4]=60;registers[(188*4096+0x24)/4]=0x1890000;
    if(mode==12)registers[0x808/4]=0x80001401;
    if(mode==13)registers[0]=1;
    memset(storage,0xa5,sizeof storage);PicassoPcm p={};picasso_pcm_device_prepare(p);
    if(mode && mode<14) {
        assert(!p.prepared && p.dmaError && !p.check && !p.address);
        assert(p.retained==(mode>=5 && mode<=11));
    } else {
        assert(p.prepared && p.retained && !p.dmaError && p.address==0x180000000ULL && p.bytes==32768);
        assert(p.bank==59 && p.slot==2 && p.asid==0x18a && p.sid==0x1401 && !p.check(p));
        for(unsigned i=0;i<32768;++i)assert(!storage[i]);
        auto *table=reinterpret_cast<uint64_t *>(storage+32768);
        assert(table[2]==0x123459003ULL && table[512]==0x12345a003ULL);
        for(unsigned i=0;i<1536;++i)if(i!=2 && i!=512 && (i<1024 || i>=1032))assert(!table[i]);
        for(unsigned i=0;i<8;++i)assert(table[1024+i]==((0x123450000ULL+4096*i)|3|(1U<<6)|(2U<<8)|(1U<<10)|(1U<<11)|(3ULL<<53)));
        for(unsigned i=45056;i<sizeof storage;++i)assert(storage[i]==0xa5);
        registers[(187*4096+0x58)/4]=0x400;assert(!p.check(p) && p.fsr==0x400);
        for(unsigned bit=1;bit<32;++bit)if((1U<<bit)&0xc00001feU) {
            registers[(187*4096+0x58)/4]=0x400|(1U<<bit);assert(p.check(p)==-5);
        }
        registers[(187*4096+0x58)/4]=0x400;
        memset(storage,0xa5,8192);assert(p.silence && !p.silence(p));
        for(unsigned i=0;i<8192;++i)assert(storage[i]==(i<4096?0:0xa5));
        static uint8_t saved[49152];int peak=0;long sum=0;unsigned frames=0;
        for(unsigned seq=0;seq<24;++seq) {
            unsigned slot=seq&1,bytes=seq==23?7168:16384,offset=slot*16384;
            memcpy(saved,storage,sizeof storage);p.busyMask=1U<<slot;
            assert(p.tone && p.tone(p,slot,seq)==-16 && !memcmp(saved,storage,sizeof storage));
            assert(p.silence(p)==-16);p.busyMask=1U<<(slot^1);
            assert(!p.tone(p,slot,seq));p.busyMask=0;
            for(unsigned i=0;i<sizeof storage;++i)if(i<offset || i>=offset+bytes)assert(saved[i]==storage[i]);
            for(unsigned i=0;i<bytes/4;++i,++frames) {
                unsigned at=offset+i*4;int sample=int16_t(uint16_t(storage[at])|(uint16_t(storage[at+1])<<8));
                assert(storage[at]==storage[at+2] && storage[at+1]==storage[at+3]);
                assert(sample>=-512 && sample<=512);if(sample>peak)peak=sample;sum+=sample;
                if(!frames || frames==95999)assert(!sample);
                // 48-frame continuity across the alternating slot boundary.
                if(frames>=480 && frames<95520 && frames%48==12)assert(sample==512);
                if(frames>=480 && frames<95520 && frames%48==36)assert(sample==-512);
            }
        }
        assert(frames==96000 && peak==512 && sum>-1000 && sum<1000);
        memcpy(saved,storage,sizeof storage);
        assert(p.tone(p,2,0)==-22 && p.tone(p,0,24)==-22);
        registers[(187*4096+0x58)/4]=2;assert(p.tone(p,0,0)==-5);registers[(187*4096+0x58)/4]=0x400;
        assert(!memcmp(saved,storage,sizeof storage));
        assert(picasso_pcm_tone(nullptr,16384,0)==-22 && picasso_pcm_tone(storage,32768,0)==-22);
        assert(picasso_pcm_tone(storage,16384,95000)==-22 && picasso_pcm_tone(storage,3,0)==-22);
        if(mode==14){registers[(187*4096+0x58)/4]=2;assert(p.check(p)==-5 && p.fsr==2);}
        if(mode==15){registers[(187*4096+4)/4]^=1;assert(p.check(p)==-5);}
    }
    assert(registers[0x800/4]==0x80001403 && registers[0x804/4]==0x80001404);
    assert(registers[(189*4096+0x24)/4]==0x1880000 && registers[(188*4096+0x24)/4]==0x1890000);
    unsigned a=allocations,d=descriptors,m=mappings;PicassoPcm second={};picasso_pcm_device_prepare(second);
    assert(second.dmaError==-16 && !second.prepared && a==allocations && d==descriptors && m==mappings);
    printf("PASS production PCM DMA allocator case %u, ownership, bounds and repeat refusal\n",mode);
}
