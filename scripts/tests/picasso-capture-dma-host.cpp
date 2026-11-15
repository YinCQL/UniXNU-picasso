// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
static uint32_t registers[0x100000/4];
alignas(4096) static uint8_t playback[49152],captured[36864];
static unsigned mode,allocations,barriers;
using IOOptionBits=unsigned long long;using IOByteCount=unsigned long long;
static void *kernel_task=nullptr;
enum {kIOMapAnywhere=1,kIOMapInhibitCache=2,kIODirectionInOut=3,kIOMemoryPhysicallyContiguous=16,kIOMemoryMapperNone=32};
class IOMemoryMap {public:uintptr_t getVirtualAddress(){return uintptr_t(registers);}};
class IODeviceMemory {
public:
    static IODeviceMemory *withRange(uint64_t address,uint64_t bytes) {
        assert(address==0x15000000 && bytes==0x100000);static IODeviceMemory d;return &d;
    }
    IOMemoryMap *map(unsigned flags){assert(flags==3);static IOMemoryMap m;return &m;}
    void release(){}
};
class IOBufferMemoryDescriptor {
public:
    unsigned index;
    static IOBufferMemoryDescriptor *inTaskWithOptions(void *task,IOOptionBits flags,unsigned bytes,unsigned alignment) {
        assert(!task && flags==51 && alignment==4096 && allocations<2);
        unsigned i=allocations++;assert(bytes==(i?32768:45056));
        static IOBufferMemoryDescriptor d[2];d[i].index=i;return i && mode==1?nullptr:&d[i];
    }
    void *getBytesNoCopy(){return index?(mode==7?nullptr:captured+(mode==6?1:0)):playback;}
    uint64_t getPhysicalSegment(unsigned offset,IOByteCount *bytes,unsigned flags) {
        assert(!offset && flags==32);*bytes=index?(mode==2?28672:32768):45056;
        if(!index)return 0x123450000ULL;
        return mode==3?0x123480001ULL:mode==4?0x123458000ULL:mode==5?0xfffffc000ULL:0x123480000ULL;
    }
};
static void IOLog(const char *fmt,...) {
    char text[1024];va_list a;va_start(a,fmt);int n=vsnprintf(text,sizeof text,fmt,a);va_end(a);
    assert(n>0 && n<256 && text[n-1]=='\n');
}
static void dmaBarrier() {
    ++barriers;
    for(unsigned b=0;b<62;++b) {auto &f=registers[((128+b)*4096+0x58)/4];if(f==0xffffffff)f=0;}
}
#include "PicassoRpcDma-body.inc"
int main(int argc,char **argv) {
    assert(argc==2);mode=unsigned(atoi(argv[1]));assert(mode<8);
    registers[8]=0x4c017e51;registers[9]=0x6000003e;registers[10]=0x5111;
    for(unsigned b=0;b<62;++b) {registers[(128+b)*1024]=0x166;registers[(128+b)*1024+1]=0x32b;}
    registers[0x800/4]=0x80001403;registers[0xc00/4]=61;registers[(189*4096+0x24)/4]=0x1880000;
    registers[0x804/4]=0x80001404;registers[0xc04/4]=60;registers[(188*4096+0x24)/4]=0x1890000;
    memset(playback,0xa5,sizeof playback);memset(captured,0xa5,sizeof captured);
    PicassoPcm p={};PicassoCaptureDma c={};p.capture=&c;picasso_pcm_device_prepare(p);
    assert(p.prepared && !p.dmaError && p.address==0x180000000ULL && p.bytes==32768);
    assert(p.bank==59 && p.sid==0x1401 && allocations==2 && !p.check(p));
    auto *tables=reinterpret_cast<uint64_t *>(playback+32768);
    for(unsigned i=0;i<8;++i)assert(tables[1024+i]==picasso_capture_dma_leaf(0x123450000ULL+i*4096));
    if(mode) {
        assert(!c.ready && c.error && c.retained==(mode!=1));
        for(unsigned i=0;i<8;++i)assert(!tables[1032+i]);
        for(unsigned i=0;i<sizeof captured;++i)assert(captured[i]==0xa5);
    } else {
        assert(c.ready && !c.error && c.retained && c.address==0x180008000ULL && c.bytes==32768 && c.slotBytes==4096);
        assert(c.check && c.prepare && c.complete && !c.check(c));
        for(unsigned i=0;i<8;++i)assert(tables[1032+i]==picasso_capture_dma_leaf(0x123480000ULL+i*4096));
        for(unsigned i=0;i<sizeof captured;++i)assert(captured[i]==(i<32768?0:0xa5));
        uint64_t before=tables[1032];tables[1032]^=4096;assert(c.prepare(c,0)==-5);tables[1032]=before;
        before=tables[1024];tables[1024]^=4096;assert(c.check(c)==-5);tables[1024]=before;
        registers[(187*4096+0x58)/4]=2;assert(c.check(c)==-5);registers[(187*4096+0x58)/4]=0;
        memset(captured,0x5a,8192);c.busyMask=1;
        assert(c.prepare(c,0)==-16 && captured[0]==0x5a);
        assert(!c.prepare(c,1));
        for(unsigned i=0;i<8192;++i)assert(captured[i]==(i<4096?0x5a:0));
        // The CPU must never expose a buffer without its matching DSP lease.
        assert(c.complete(c,1,0,4)==-22 && c.complete(c,0,4094,4)==-22 && c.complete(c,0,1,2)==-22);
        captured[128]=0;captured[129]=0x80;captured[130]=0;captured[131]=0;captured[132]=0xff;captured[133]=0x7f;
        assert(!c.complete(c,0,128,6));
        const auto &samples=*picasso_capture_samples_snapshot();
        assert(samples.saved==6 && samples.totalBytes==6 && samples.nonzero==2 && samples.peak==32768 && samples.clipped==2 && samples.sum==-1 && samples.absoluteSum==65535);
        assert(!memcmp(samples.data,captured+128,6));
        static uint8_t chunk[4096];memset(chunk,0x7f,sizeof chunk);
        for(unsigned i=0;i<50;++i)assert(!picasso_capture_samples_append(captureMemory.samples,chunk,sizeof chunk));
        assert(samples.saved==192000 && samples.totalBytes==204806);
        unsigned prior=samples.saved;assert(picasso_capture_samples_append(captureMemory.samples,chunk,4097)==-22 && samples.saved==prior);
        assert(c.complete(c,2,0,4)==-22);
        c.busyMask=0;assert(c.complete(c,0,128,6)==-22);
        // Verify failed map extension is transactional and keeps all old leaves.
        alignas(8) static uint64_t copy[1536];memcpy(copy,tables,sizeof copy);
        assert(picasso_capture_dma_add(copy,0x123480000ULL,0x123450000ULL)==-16 && !memcmp(copy,tables,sizeof copy));
        assert(picasso_capture_dma_geometry(0x123458000ULL,0x123450000ULL)==-16);
        assert(picasso_capture_dma_geometry(0,0x123450000ULL)==-22);
    }
    assert(registers[0x800/4]==0x80001403 && registers[0x804/4]==0x80001404);
    assert(registers[(189*4096+0x24)/4]==0x1880000 && registers[(188*4096+0x24)/4]==0x1890000);
    for(unsigned i=45056;i<sizeof playback;++i)assert(playback[i]==0xa5);
    puts("PASS capture allocator, SID1401 isolation, ownership, bounds, exact payload and retained failures");
}
