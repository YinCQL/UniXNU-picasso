// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t mdp[0x90000/4], vbif[0x1040/4], smmu[0xd0000/4];
static unsigned creates, maps, releases, unmaps, failCreate, failMap, zeroMap;
static unsigned bufferCreates, bufferReleases, bufferQueries, failBuffer;
static bool enabled = true;
static bool PE_parse_boot_argn(const char *, uint32_t *v, unsigned) { *v=enabled;return true; }
using vm_size_t = uintptr_t;
using IOOptionBits = uint32_t;
using IOByteCount = uintptr_t;
using addr64_t = uint64_t;
enum { kIODirectionIn=1,kIODirectionInOut=3,kIOMemoryPhysicallyContiguous=0x10,
       kIOMapAnywhere=1,kIOMapInhibitCache=2 };
struct IOMemoryMap {
    uintptr_t va;uint64_t bytes;
    uintptr_t getVirtualAddress() {return va;}
    uint64_t getPhysicalAddress() {return 0xa0000000;}
    uint64_t getLength() {return bytes;}
    void release() {++unmaps;}
};
struct IOMemoryDescriptor {
    uintptr_t va;uint64_t bytes;
    static IOMemoryDescriptor *withPhysicalAddress(uint64_t pa,uint64_t bytes,unsigned direction) {
        unsigned slot=pa==0xae00000?0:pa==0xaeb0000?1:2;
        assert(slot!=2 || pa==0x15000000);
        assert(direction==(slot==2?kIODirectionIn:kIODirectionInOut));
        assert(bytes==(slot==0?sizeof mdp:slot==1?sizeof vbif:sizeof smmu));
        static IOMemoryDescriptor d[3];
        d[slot]={reinterpret_cast<uintptr_t>(slot==0?mdp:slot==1?vbif:smmu),bytes};
        return ++creates==failCreate?nullptr:&d[slot];
    }
    IOMemoryMap *map(unsigned flags) {
        assert(flags==3);static IOMemoryMap m[3];unsigned n=++maps;
        m[n-1]={n==zeroMap?0:va,bytes};return n==failMap?nullptr:&m[n-1];
    }
    void release() {++releases;}
};
struct IOBufferMemoryDescriptor {
    vm_size_t bytes;
    static IOBufferMemoryDescriptor *withOptions(IOOptionBits options,vm_size_t bytes,vm_size_t alignment) {
        assert(options==(kIOMemoryPhysicallyContiguous|kIODirectionInOut) && alignment==4096);
        const vm_size_t sizes[]={1u<<20,12u<<20,64u<<20};
        unsigned slot=bufferCreates++;
        assert(slot<3 && bytes==sizes[slot]);
        static IOBufferMemoryDescriptor buffers[3];
        buffers[slot].bytes=bytes;
        return bufferCreates==failBuffer?nullptr:&buffers[slot];
    }
    addr64_t getPhysicalSegment(IOByteCount offset,IOByteCount *length,IOOptionBits options) {
        assert(offset==0 && length && options==0);++bufferQueries;
        *length=bytes;return 0x180000000ull;
    }
    IOByteCount getLength() {return bytes;}
    void release() {++bufferReleases;}
};
#define OSSafeReleaseNULL(p) do {if(p){p->release();p=nullptr;}}while(0)
struct Probe {
    IOMemoryMap boot={0x1000,4320*2400};IOMemoryMap *bootMapping=&boot;
    uint32_t panelWidth=1080,panelHeight=2400;
    // PROBE_METHODS
};
int main(int argc,char **argv) {
    assert(argc==2);const char *mode=argv[1];
    smmu[0x20/4]=0x4c017e51;smmu[0x24/4]=0x6000003e;
    smmu[(0x800+3*4)/4]=0x84020b40;smmu[(0xc00+3*4)/4]=2;
    smmu[(0x1000+2*4)/4]=0x1f000;smmu[(128+2)*1024]=0xe0;
    if(!strncmp(mode,"alloc",5))failCreate=unsigned(mode[5]-'0');
    if(!strncmp(mode,"map",3))failMap=unsigned(mode[3]-'0');
    if(!strncmp(mode,"zero",4))zeroMap=unsigned(mode[4]-'0');
    if(!strncmp(mode,"buffer",6))failBuffer=unsigned(mode[6]-'0');
    if(!strcmp(mode,"disabled"))enabled=false;
    if(!strcmp(mode,"bad-pages"))smmu[0x24/4]=0x7000003e;
    if(!strcmp(mode,"64k-pages"))smmu[0x24/4]=0xe000003e;
    if(!strcmp(mode,"zero-banks"))smmu[0x24/4]=0x60000000;
    if(!strcmp(mode,"too-many-banks"))smmu[0x24/4]=0x600000ff;
    if(!strcmp(mode,"outside-map"))smmu[0x24/4]=0x60000080;
    if(!strcmp(mode,"bad-bank"))smmu[(0xc00+3*4)/4]=255;
    // Use complete byte snapshots to prove the production probe writes no MMIO.
    static uint32_t m[sizeof mdp/4],v[sizeof vbif/4],s[sizeof smmu/4];
    memcpy(m,mdp,sizeof mdp);memcpy(v,vbif,sizeof vbif);memcpy(s,smmu,sizeof smmu);
    Probe p;if(!strcmp(mode,"no-boot"))p.bootMapping=nullptr;
    p.sdeProbe();p.sdeDiag();
    assert(!memcmp(m,mdp,sizeof mdp) && !memcmp(v,vbif,sizeof vbif) && !memcmp(s,smmu,sizeof smmu));
    assert(releases==creates-!!failCreate && unmaps==maps-!!failMap);
    assert(bufferCreates==(enabled?3u:0u));
    assert(bufferReleases==bufferCreates-!!failBuffer && bufferQueries==bufferReleases);
    if(!enabled)assert(!creates);
    if(!strcmp(mode,"normal"))assert(creates==3 && maps==3);
    if(!strcmp(mode,"no-boot"))assert(creates==1 && maps==1);
    puts("PASS read-only SDE/SMMU probe, malformed geometry and mapping cleanup");
}
