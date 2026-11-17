// SPDX-License-Identifier: BSD-2-Clause-Patent
// Production pool/UPIU/PRDT with a non-coherent DMA and MMIO host fixture.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <array>
#include <vector>
using IOReturn=uint32_t;
constexpr IOReturn kIOReturnSuccess=0,kIOReturnBadArgument=1,kIOReturnBusy=2,kIOReturnIOError=3,kIOReturnTimeout=4,kIOReturnNotReady=5;
constexpr unsigned kMicrosecondScale=1;
constexpr int THREAD_UNINT=0;
#include "../RootDiagnostics.h"
#include "../TransferPoll.h"
#include "../RootTransportLayout.h"
#include "../PollPause.h"
struct IOLock{std::mutex mutex;std::condition_variable changed;};
static void IOLockLock(IOLock*l){l->mutex.lock();}static void IOLockUnlock(IOLock*l){l->mutex.unlock();}
static void IOLockSleep(IOLock*l,void*,int){std::unique_lock<std::mutex>g(l->mutex,std::adopt_lock);l->changed.wait(g);g.release();}
static void IOLockWakeup(IOLock*l,void*,bool){l->changed.notify_all();}
struct Host{uint8_t*virt;uint64_t phys;bool ready=true,poisoned=false;uintptr_t hc=1;IOLock*lock;};
alignas(4096)static uint8_t memory[kDmaBytes];
static std::array<uint8_t,kDmaBytes> device;
static IOLock diagnostic,pool,submit,write,bootstrap;
static IOLock*gDiagnosticLock=&diagnostic;
static Host gHost{memory,0x800000000ULL,true,false,1,&bootstrap};
static UfsRootDiagnostics gRootDiagnostic;
static uint32_t registers[64];
static std::atomic<uint64_t> tick{1};
static unsigned rings=0, dataAcquires=0,busyLeft=0;
static bool completeImmediately=true, failResidual=false, fatal=false, boundary=false;
static unsigned boundarySkip=0;
static uint32_t read32(uintptr_t,unsigned at){return registers[at/4];}
static uint64_t mach_absolute_time(){
  if (boundary && registers[0x58/4] && boundarySkip-- == 0) {
    tick=2000000;registers[0x58/4]=0;
    *reinterpret_cast<uint32_t*>(device.data()+8)=0;boundary=false;
  }
  return tick.fetch_add(1);
}
static void clock_interval_to_deadline(uint32_t n,unsigned,uint64_t*out){*out=mach_absolute_time()+n;}
static void absolutetime_to_nanoseconds(uint64_t n,uint64_t*out){*out=n*1000;}
static void hostCpuRelax(){tick.fetch_add(1);std::this_thread::yield();}
static const uint8_t *directFixture = nullptr;
static uint64_t directFixtureBytes=8192;
static bool directContiguous=false;
static std::array<uint8_t,kDataMax> directDevice;
static uint64_t directAcquireBytes=0;
static void cacheCivac(const void*s,size_t n){
  if(directFixture && uintptr_t(s)>=uintptr_t(directFixture) && uintptr_t(s)-uintptr_t(directFixture)<directFixtureBytes){
    size_t at=uintptr_t(s)-uintptr_t(directFixture);assert(at+n<=directFixtureBytes);memcpy(directDevice.data()+at,s,n);return;
  }
  auto at=static_cast<const uint8_t*>(s)-memory;assert(at>=0&&size_t(at)+n<=device.size());memcpy(device.data()+at,s,n);}
static void cacheInvalidate(const void*s,size_t n){
  if(directFixture && uintptr_t(s)>=uintptr_t(directFixture) && uintptr_t(s)-uintptr_t(directFixture)<directFixtureBytes){
    size_t at=uintptr_t(s)-uintptr_t(directFixture);assert(at+n<=directFixtureBytes);memcpy(const_cast<void*>(s),directDevice.data()+at,n);directAcquireBytes+=n;return;
  }
  auto at=static_cast<const uint8_t*>(s)-memory;assert(at>=0&&size_t(at)+n<=device.size());memcpy(const_cast<void*>(s),device.data()+at,n);if(n>=4096)++dataAcquires;}
static IOReturn recordTransport(IOReturn r,uint32_t o){gRootDiagnostic.result=r;gRootDiagnostic.ocs=o;return r;}
static void hostWrite(unsigned,uint32_t);
static bool gStoreOpen=true,gLogOpen=false,gAuxOpen=false;
static bool hostInWindow(uint64_t lba,uint32_t blocks){return lba>=100&&lba+blocks<=1000;}
static void IOLog(const char*,...){}
#define NATIVE_UFS_HOST_RUNTIME_PLATFORM 1
static IOReturn runtimeRead4k(uint8_t,uint64_t,uint32_t,void*,bool=false);
#include "../RuntimeSlots.h"
static uint32_t rtCompletionCount(unsigned s){return gSlotGen[s];}
static bool rtInterruptMode(){return false;}
static void rtWaitCompletion(unsigned slot,uint32_t,uint64_t,uint64_t){
  if(boundary){tick=2000000;registers[0x58/4]&=~gSlotGeom[slot].bit;*reinterpret_cast<uint32_t*>(device.data()+ufsSlotUtrdOffset(slot)+8)=0;boundary=false;}
  else ufsPollPause(hostCpuRelax);
}
static void rtWaitDone(uint64_t){}
static uint64_t rtDmaAddress(const void *p){
  auto at=static_cast<const uint8_t*>(p);
  if (!directFixture || at < directFixture || at >= directFixture+directFixtureBytes) return 0;
  return 0x900000000ULL + uint64_t(at-directFixture)*(directContiguous?1:2);
}
static void complete(unsigned entry){
  unsigned slot=unsigned(gEntrySlot[entry]);const UfsSlot&s=gSlotGeom[slot];
  auto offset=s.ucd-memory;uint8_t*c=device.data()+offset,*r=c+kRspOffset;
  memcpy(r,c,32);r[0]=c[0]+0x20;r[1]=0;
  if(c[0]==1){rtPutBe32(r+12,failResidual?4096:0);r[7]=busyLeft?8:0;if(busyLeft)--busyLeft;}
  else{r[11]=r[19]=0x23;uint8_t*d=r+32;memset(d,0,0x23);d[0]=0x23;d[1]=2;d[2]=4;d[3]=1;d[10]=12;rtPutBe32(d+15,1001);}
  auto data=s.data-memory;memset(device.data()+data,0x70+slot,kDataMax);
  if(c[0]==1 && c[16]==0x28 && directFixture) {
    const unsigned entries=*reinterpret_cast<uint32_t*>(device.data()+(s.utrd-memory)+28)&0xffff;
    uint32_t *prdt=reinterpret_cast<uint32_t*>(c+kPrdtOffset);
    for(unsigned i=0;i<entries;++i) {
      const uint64_t pa=uint64_t(prdt[4*i])|(uint64_t(prdt[4*i+1])<<32);
      const uint32_t bytes=prdt[4*i+3]+1;
      if(pa<0x900000000ULL) continue; // staged path still uses its slot area.
      const uint64_t at=(pa-0x900000000ULL)/(directContiguous?1:2);
      assert(at+bytes<=directFixtureBytes);
      for(unsigned j=0;j<bytes;++j)directDevice[at+j]=uint8_t((at+j)*17+slot);
    }
  }

  if(c[0]==1&&c[16]==0x25){rtPutBe32(device.data()+data,1000);rtPutBe32(device.data()+data+4,4096);}
  // A write-combined list is the same memory at both endpoints; cached
  // descriptors require the production invalidate before the CPU sees OCS.
  uint8_t *list=gListUncached?memory:device.data();
  *reinterpret_cast<uint32_t*>(list+(s.utrd-memory)+8)=0;
  registers[0x58/4]&=~s.bit;
}
static void hostWrite(unsigned at,uint32_t value){
  if(at==0x20)registers[at/4]&=~value;
  else if(at==0x58){registers[at/4]|=value;++rings;assert(value&&!(value&(value-1)));if(fatal)registers[0x20/4]=0x20000;else if(completeImmediately)complete(__builtin_ctz(value));}
  else registers[at/4]=value;
}
static void reset(unsigned count=4){
  memset(memory,0,sizeof(memory));device.fill(0);memset(registers,0,sizeof(registers));
  gHost.ready=true;gHost.poisoned=false;gSlotLock=&pool;gSubmitLock=&submit;gWriteLock=&write;
  rtSlotGeometryRegionA(count);gSlotCount=count;gSlotFree=count>1?(1U<<(count-1))-1:1;gSlotsBusy=gSlotsPeak=0;gWaitHead=gWaitTail=nullptr;
  rings=dataAcquires=busyLeft=0;completeImmediately=true;failResidual=fatal=boundary=false;gOutstanding=0;tick=1;
}
int main(){

  // Non-coherent DMA lands in the owned record pages and becomes visible only
  // after completion's invalidate. Exercise scattered and coalesced PRDTs.
  for(bool contiguous : {false,true}) {
    reset(4); alignas(4096) uint8_t record[kDataMax]; memset(record,0xa5,sizeof(record));
    directFixture=record; directFixtureBytes=sizeof(record);directContiguous=contiguous;
    directDevice.fill(0);directAcquireBytes=0;gZeroCopy=true;
    const uint64_t directBefore=gDirectCommands,stagedBefore=gStagedCommands;
    assert(!runtimeRead4k(0,1,32,record,true));
    assert(gDirectCommands==directBefore+1 && gStagedCommands==stagedBefore);
    assert(directAcquireBytes==sizeof(record));
    for(unsigned i=0;i<sizeof(record);++i)assert(record[i]==uint8_t(i*17));
    assert(gSlotPrds[0][0].bytes==(contiguous?kDataMax:4096));
    // The same DMA-capable address without ownership must use staged transport.
    memset(record,0xa5,sizeof(record)); directAcquireBytes=0;
    assert(!runtimeRead4k(0,1,32,record,false));
    assert(gStagedCommands==stagedBefore+1 && !directAcquireBytes && record[0]==0x70);
    directFixture=nullptr;directFixtureBytes=8192;directContiguous=false;gZeroCopy=false;
  }
  {reset(4);alignas(4096) uint8_t record[kDataMax];memset(record,0xa5,sizeof(record));
    directFixture=record;directFixtureBytes=sizeof(record);directAcquireBytes=0;gZeroCopy=true;
    completeImmediately=false;
    assert(runtimeRead4k(0,1,32,record,true)==kIOReturnTimeout && gHost.poisoned && rings==1);
    assert(!directAcquireBytes && record[0]==0xa5);
    assert(runtimeRead4k(0,2,32,record,true)==kIOReturnNotReady && rings==1 && record[0]==0xa5);
    directFixture=nullptr;directFixtureBytes=8192;gZeroCopy=false;
  }


  // Four total / three leased descriptors, in both cached-line-isolated and
  // write-combined list geometry. Complete out of order without touching peers.
  for (bool wc : {false,true}) {
    reset(4); gListUncached=wc;
    if(wc) {
      for(unsigned i=0;i<32;++i) gEntrySlot[i]=-1;
      for(unsigned i=0;i<4;++i) rtSlotGeometry(i,memory+32*i,i,
        memory+ufsSlotUcdOffset(i),gHost.phys+ufsSlotUcdOffset(i),
        memory+ufsSlotDataOffset(i),gHost.phys+ufsSlotDataOffset(i));
    }
    completeImmediately=false;
    UfsSlot leased[3]; bool pooled[3]; uint64_t deadlines[3]; uint32_t scsi=0;
    uint8_t cdb[10]={0x28}; uint32_t bits=0;
    for(unsigned i=0;i<3;++i) {
      leased[i]=rtSlotTake(&pooled[i]); assert(pooled[i]);
      assert(leased[i].entry==(wc?i:2*i));
      assert(!rtScsiIssue(leased[i],0,cdb,10,2,4096,1000,&deadlines[i],&scsi));
      bits|=leased[i].bit;
    }
    assert(gSlotsPeak==3 && !gSlotFree && registers[0x58/4]==bits);
    for(unsigned i : {1U,2U,0U}) {
      complete(leased[i].entry); bits&=~leased[i].bit;
      assert(registers[0x58/4]==bits);
      assert(!rtScsiAwait(leased[i],0,4096,deadlines[i],&scsi));
      rtSlotGive(leased[i],pooled[i]);
    }
    assert(gSlotFree==7 && !gSlotsBusy && !gOutstanding);
    gListUncached=false;
  }

  // Two leased slots build discontiguous PRDTs without sharing CPU scratch.
  {reset(); alignas(4096) uint8_t pages[8192]{}; directFixture=pages; gZeroCopy=true;
    bool a,b; auto x=rtSlotTake(&a),y=rtSlotTake(&b); assert(a&&b);
    assert(rtBuildPrds(pages,8192,gSlotPrds[x.index])==2);
    const UfsPrd first=gSlotPrds[x.index][0],second=gSlotPrds[x.index][1];
    assert(rtBuildPrds(pages+4096,4096,gSlotPrds[y.index])==1);
    assert(gSlotPrds[x.index]!=gSlotPrds[y.index]);
    assert(gSlotPrds[x.index][0].addr==first.addr&&gSlotPrds[x.index][0].bytes==first.bytes);
    assert(gSlotPrds[x.index][1].addr==second.addr&&gSlotPrds[x.index][1].bytes==second.bytes);
    directFixture=nullptr; assert(!rtBuildPrds(pages,8192,gSlotPrds[x.index]));
    gZeroCopy=false; rtSlotGive(x,a); rtSlotGive(y,b);
  }
  {reset();std::vector<uint8_t>out(4096);assert(!runtimeRead4k(0,1,1,out.data()));assert(out[0]==0x70&&rings==1);}
  {reset();completeImmediately=false;bool a,b;auto x=rtSlotTake(&a),y=rtSlotTake(&b);uint8_t cdb[10]={0x28};uint64_t dx,dy;uint32_t scsi;
    assert(!rtScsiIssue(x,0,cdb,10,2,4096,1000,&dx,&scsi));assert(!rtScsiIssue(y,0,cdb,10,2,4096,1000,&dy,&scsi));assert(registers[0x58/4]==(x.bit|y.bit));
    complete(x.entry);assert(!rtScsiAwait(x,0,4096,dx,&scsi));assert(registers[0x58/4]==y.bit);complete(y.entry);assert(!rtScsiAwait(y,0,4096,dy,&scsi));rtSlotGive(x,a);rtSlotGive(y,b);}
  {reset();busyLeft=2;std::vector<uint8_t>out(4096);assert(!runtimeRead4k(0,1,1,out.data())&&rings==3);}
  {reset();std::vector<uint8_t>src(4096,0x5a);assert(!runtimeWrite4k(0,100,1,src.data(),true));assert(!dataAcquires);assert(device[ufsSlotUcdOffset(0)+17]==8);assert(!runtimeSyncCache());assert(runtimeWrite4k(0,99,1,src.data(),true)==1);}
  {reset();uint64_t blocks=0;assert(!runtimeFirmwareGeometry(4,&blocks)&&blocks==1001&&rings==2);}
  {reset();completeImmediately=false;bool pooled;auto slot=rtSlotTake(&pooled);uint8_t cdb[10]={0x28};uint64_t end;uint32_t scsi;
    assert(!rtScsiIssue(slot,0,cdb,10,2,4096,1000,&end,&scsi));complete(slot.entry);registers[0x58/4]|=slot.bit;boundary=true;boundarySkip=1;assert(!rtScsiAwait(slot,0,4096,end,&scsi));assert(rtDiagnostic(slot).deadlineSample==1);rtSlotGive(slot,pooled);}
  {reset();completeImmediately=false;std::vector<uint8_t>out(4096,0xa5);assert(runtimeRead4k(0,1,1,out.data())==4&&gHost.poisoned&&rings==1);assert(runtimeRead4k(0,2,1,out.data())==5&&rings==1&&out[0]==0xa5);}
  {reset();failResidual=true;std::vector<uint8_t>out(4096,0xa5);assert(runtimeRead4k(0,1,1,out.data())==3&&gHost.poisoned&&out[0]==0xa5);}
  {reset(3);bool a,b;auto x=rtSlotTake(&a),y=rtSlotTake(&b);std::atomic<bool>taken{false};std::thread waiter([&]{bool p;auto z=rtSlotTake(&p);assert(!p);taken=true;(void)z;});
    for (;;) { IOLockLock (&pool); bool waiting = gWaitHead != nullptr; IOLockUnlock (&pool); if (waiting) break; std::this_thread::yield (); }rtQuarantine();waiter.join();assert(taken);rtSlotGive(x,a);rtSlotGive(y,b);}
  puts("PASS runtime multi-slot DMA isolation, BUSY retry, FUA/flush, firmware slot, final deadline read, quarantine and waiter wakeup");
}
