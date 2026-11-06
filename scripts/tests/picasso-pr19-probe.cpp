// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t mdp[0x90000/4], vbif[0x1040/4], smmu[0xd0000/4];
static unsigned creates, maps, releases, unmaps, failCreate, failMap, zeroMap;
static bool enabled = true;
static bool PE_parse_boot_argn(const char *, uint32_t *v, unsigned) { *v=enabled;return true; }
enum { kIODirectionIn=1,kIOMapAnywhere=1,kIOMapInhibitCache=2 };
struct PE_Video {uint64_t v_width=1080,v_height=2400;};
static int PE_current_console(PE_Video *) {return 0;}
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
        assert(direction==kIODirectionIn);
        unsigned slot=pa==0xae00000?0:pa==0xaeb0000?1:2;
        assert(slot!=2 || pa==0x15000000);
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
#define OSSafeReleaseNULL(p) do {if(p){p->release();p=nullptr;}}while(0)
struct Probe {
    IOMemoryMap boot={0x1000,4320*2400};IOMemoryMap *bootMapping=&boot;
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
    if(!strcmp(mode,"disabled"))enabled=false;
    if(!strcmp(mode,"bad-pages"))smmu[0x24/4]=0x7000003e;
    if(!strcmp(mode,"64k-pages"))smmu[0x24/4]=0xe000003e;
    if(!strcmp(mode,"bad-bank"))smmu[(0xc00+3*4)/4]=255;
    // Use complete byte snapshots to prove the production probe writes no MMIO.
    static uint32_t m[sizeof mdp/4],v[sizeof vbif/4],s[sizeof smmu/4];
    memcpy(m,mdp,sizeof mdp);memcpy(v,vbif,sizeof vbif);memcpy(s,smmu,sizeof smmu);
    Probe p;p.sdeProbe();p.sdeDiag();
    assert(!memcmp(m,mdp,sizeof mdp) && !memcmp(v,vbif,sizeof vbif) && !memcmp(s,smmu,sizeof smmu));
    assert(releases==creates-!!failCreate && unmaps==maps-!!failMap);
    if(!enabled)assert(!creates);
    puts("PASS read-only SDE/SMMU probe, malformed geometry and mapping cleanup");
}
