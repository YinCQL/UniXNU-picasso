#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "ArmCpuTimer.h"
#include "ArmCpuSpi.h"
using IOReturn=int;
enum { kIOReturnSuccess=0, kIOReturnNotReady=1, kIOReturnBadArgument=2,
       kIOReturnExclusiveAccess=3, kIOReturnNoResources=4, kIOReturnTimeout=5 };
struct IOLock { std::mutex mutex; };
static std::atomic<unsigned> allocs{0}, frees{0}, arrivals{0};
static unsigned allocationRace=0;
static bool allocationFail=false, timeout=false, secureView=false;
static IOLock *IOLockAlloc() {
    if(allocationFail) return nullptr;
    IOLock *p=new IOLock; ++allocs;
    if(allocationRace) { ++arrivals; while(arrivals.load()<allocationRace) std::this_thread::yield(); }
    return p;
}
static void IOLockFree(IOLock *p) { ++frees; delete p; }
static void IOLockLock(IOLock *p) { p->mutex.lock(); }
static void IOLockUnlock(IOLock *p) { p->mutex.unlock(); }
alignas(8) static uint8_t mmio[0x10000];
static uintptr_t gDist=reinterpret_cast<uintptr_t>(mmio);
static volatile uint32_t armcpu_gic_ready=1;
static unsigned read32(uintptr_t base,unsigned offset) {
    if(secureView && offset>=0x80 && offset<0x100) return 0;
    uint32_t value; std::memcpy(&value,reinterpret_cast<void *>(base+offset),4); return value;
}
static void write32(uintptr_t base,unsigned offset,uint32_t value) {
    if(offset>=0x100 && offset<0x180) value|=read32(base,offset);
    if(offset>=0x180 && offset<0x200) { offset-=0x80; value=read32(base,offset)&~value; }
    std::memcpy(reinterpret_cast<void *>(base+offset),&value,4);
    // Increase interleaving opportunities inside register read-modify-write.
    std::this_thread::yield();
}
static bool waitClear(uintptr_t,unsigned,uint32_t) { return !timeout; }
#include "registration.inc"
static void handler(void *) {}
static void reset() {
    if(gSpiRegistrationLock) IOLockFree(gSpiRegistrationLock);
    gSpiRegistrationLock=nullptr; gSpiUsed=0;
    std::memset(gSpi,0,sizeof(gSpi)); std::memset(mmio,0,sizeof(mmio));
    const uint32_t typer=29; std::memcpy(mmio+4,&typer,4); // INTIDs 0..959.
    gDist=reinterpret_cast<uintptr_t>(mmio); armcpu_gic_ready=1;
    gBootAffinity=0x0300020104ULL;
    allocationFail=false; allocationRace=0; arrivals=0; timeout=false; secureView=false;
}
static void concurrency(bool duplicate) {
    reset(); allocationRace=16;
    std::vector<std::thread> threads;
    IOReturn result[16]; uint32_t readback[16]{}; uint64_t router[16]{};
    const unsigned a=allocs.load(), f=frees.load();
    for(unsigned i=0;i<16;++i) threads.emplace_back([&,i]{
        const uint32_t id=duplicate?297:288+i;
        result[i]=armcpu_spi_attach(id,false,handler,reinterpret_cast<void *>(uintptr_t(i+1)),&readback[i],&router[i]);
    });
    for(auto &t:threads) t.join(); allocationRace=0;
    assert(allocs-a==16 && frees-f==15); // CAS losers discarded, one lifetime lock.
    unsigned successful=0;
    for(unsigned i=0;i<16;++i) {
        if(result[i]==kIOReturnSuccess) {
            ++successful; assert(readback[i]==0xa003 && router[i]==0x0302010400000000ULL);
        } else assert(duplicate && result[i]==kIOReturnExclusiveAccess);
    }
    assert(successful==(duplicate?1:16) && gSpiUsed==successful);
    uint32_t seen=0;
    for(unsigned i=0;i<gSpiUsed;++i) {
        assert(gSpi[i].handler==handler && gSpi[i].context);
        uint32_t bit=1U<<(gSpi[i].id-288); assert(!(seen&bit)); seen|=bit;
    }
    assert(seen==(duplicate?(1U<<9):0xffff));
    assert((read32(gDist,0x80+9*4)&0xffff)==seen);
    assert((read32(gDist,0x100+9*4)&0xffff)==seen);
    for(unsigned i=0;i<gSpiUsed;++i) {
        const uint32_t id=gSpi[i].id, bit=1U<<(id%32);
        assert(armcpu_spi_mask(id,true)==0 && !(read32(gDist,0x100+(id/32)*4)&bit));
        assert(armcpu_spi_mask(id,false)==0 && (read32(gDist,0x100+(id/32)*4)&bit));
    }
}
int main() {
    for(unsigned i=0;i<64;++i) { concurrency(false); concurrency(true); }
    reset(); allocationFail=true;
    assert(armcpu_spi_attach(297,false,handler,nullptr,nullptr,nullptr)==kIOReturnNoResources && !gSpiUsed);
    allocationFail=false; armcpu_gic_ready=0;
    assert(armcpu_spi_attach(297,false,handler,nullptr,nullptr,nullptr)==kIOReturnNotReady && !gSpiUsed);
    armcpu_gic_ready=1;
    assert(armcpu_spi_attach(297,false,nullptr,nullptr,nullptr,nullptr)==kIOReturnNotReady);
    for(uint32_t id: {0U,31U,960U,1019U,1020U,UINT32_MAX})
        assert(armcpu_spi_attach(id,false,handler,nullptr,nullptr,nullptr)==kIOReturnBadArgument);
    timeout=true;
    assert(armcpu_spi_attach(297,false,handler,nullptr,nullptr,nullptr)==kIOReturnTimeout && !gSpiUsed);
    assert(!(read32(gDist,0x100+9*4)&(1U<<9))); timeout=false;
    for(unsigned i=0;i<16;++i) assert(armcpu_spi_attach(320+i,true,handler,nullptr,nullptr,nullptr)==0);
    assert(armcpu_spi_attach(400,false,handler,nullptr,nullptr,nullptr)==kIOReturnNoResources);
    assert(armcpu_spi_attach(320,false,handler,nullptr,nullptr,nullptr)==kIOReturnExclusiveAccess);
    assert(armcpu_spi_mask(400,true)==kIOReturnBadArgument);
    gSpi[0].count=123; assert(armcpu_spi_count(320)==123 && armcpu_spi_count(400)==0);
    // Independent byte-level ADT oracle and SGI ownership: SGI 0 only.
    for(uint32_t ppi=16;ppi<=31;++ppi) {
        uint8_t property[4]={uint8_t(ppi),0,0,0}; uint32_t decoded=0;
        assert(ArmCpuTimerIntid(property,4,&decoded) && decoded==ppi);
        assert(ArmCpuSgiPpiEnable(ppi)==((1U<<ppi)|1U));
        assert((ArmCpuSgiPpiEnable(ppi)&0xffff)==1);
    }
    uint32_t v=0; uint8_t bad[4]={32,0,0,0};
    assert(!ArmCpuTimerIntid(bad,4,&v)); assert(!ArmCpuTimerIntid(bad,3,&v));
    assert(ArmCpuTimerIntid(nullptr,0,&v) && v==27);
    reset(); secureView=true;
    const uint32_t securityTyper=0x7b041d; std::memcpy(mmio+4,&securityTyper,4);
    uint32_t rb=0;
    assert(armcpu_spi_attach(297,false,handler,nullptr,&rb,nullptr)==0);
    assert(rb==0x1a002 && !(rb&1U) && ArmCpuSpiLevelConfigured(rb));
    // No security flag on a single-state GIC; a missing group bit must fail.
    assert(ArmCpuSpiReadbackView(0xa002,29)==0xa002);
    assert(!ArmCpuSpiLevelConfigured(0xa002));
    assert(ArmCpuSpiLevelConfigured(0xa003));
    for(uint32_t bits: {0U,1U,4U,5U,6U,7U}) {
        assert(!ArmCpuSpiLevelConfigured(0x1a000|bits));
        assert(!ArmCpuSpiLevelConfigured(0xa000|bits));
    }
    for(uint32_t priority: {0U,0x80U,0x9fU,0xa1U,0xffU})
        assert(!ArmCpuSpiLevelConfigured((1U<<16)|(priority<<8)|2U));
    reset();
    assert(allocs==frees);
    puts("PASS ArmCpu registration concurrency, publication, timeout, boundaries, ownership");
}
