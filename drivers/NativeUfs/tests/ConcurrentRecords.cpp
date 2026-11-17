// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnIOError=2;
constexpr uint64_t kDataMax=131072;
#include "../RootWindowRead.h"
#include "../ConcurrentRecords.h"
struct Shared {
  std::mutex mutex;
  std::condition_variable changed;
  UfsConcurrentCache cache;
  std::vector<uint8_t> disk, original, buffers;
  std::atomic<unsigned> reads{0}, verifying{0}, peak{0};
  bool fail=false, pause=false;
  std::atomic<bool> release{false}, inCopy{false};
  Shared(unsigned slots=8): disk(12*kDataMax), buffers(slots*kDataMax) {
    for(size_t i=0;i<disk.size();++i)disk[i]=uint8_t(i/512+i/131072);
    original=disk; cache.count=slots;
  }
};
struct Ops {
  Shared &s;
  std::unique_lock<std::mutex> guard;
  std::vector<uint8_t> out;
  bool parkCopy=false;
  Ops(Shared &v):s(v),guard(v.mutex,std::defer_lock),out(2*kDataMax,0xa5){}
  void lock(){guard.lock();} void unlock(){guard.unlock();}
  void sleep(void *){s.changed.wait(guard);} void wakeup(void *){s.changed.notify_all();}
  IOReturn read(uint64_t lba,uint32_t blocks,void *dst){
    assert(!guard.owns_lock()); ++s.reads;
    memcpy(dst,s.disk.data()+lba*4096,blocks*4096);return s.fail?2:0;
  }
  IOReturn verify(uint64_t record,const void *data,size_t bytes){
    assert(!guard.owns_lock());unsigned n=++s.verifying, peak=s.peak;
    while(n>peak&&!s.peak.compare_exchange_weak(peak,n)){}
    if(s.pause)while(!s.release.load())std::this_thread::yield();
    bool ok=!memcmp(data,s.original.data()+record*kDataMax,bytes);--s.verifying;return ok?0:2;
  }
  uint64_t copy(uint64_t at,const void *data,uint64_t bytes){
    assert(!guard.owns_lock());
    if(parkCopy){s.inCopy=true;while(!s.release.load())std::this_thread::yield();}
    memcpy(out.data()+at,data,bytes);return bytes;
  }
  UfsWindowResult run(uint64_t block,uint64_t count=8){
    return ufsReadConcurrentRecords(*this,0,s.disk.size(),block,count,s.buffers.data(),
      s.cache.count>1?s.buffers.data()+kDataMax:nullptr,kDataMax,s.cache,nullptr);
  }
};
int main(){
  {Shared s;Ops o(s);for(unsigned i=0;i<32;++i){auto r=o.run(i*8);assert(!r.status&&r.done==4096);}assert(s.reads==1);}
  {Shared s;s.pause=true;Ops a(s),b(s);std::thread x([&]{assert(!a.run(0).status);});
    while(!s.verifying.load())std::this_thread::yield();
    std::thread y([&]{assert(!b.run(256).status);});
    while(s.peak<2)std::this_thread::yield();s.release=true;x.join();y.join();assert(s.reads==2&&s.peak==2);}
  {Shared s;s.pause=true;Ops a(s),b(s);std::thread x([&]{assert(!a.run(0).status);});
    while(!s.verifying.load())std::this_thread::yield();std::thread y([&]{assert(!b.run(8).status);});
    s.release=true;x.join();y.join();assert(s.reads==1&&s.cache.stats.hits==1);}
  {Shared s(1);Ops a(s),b(s);a.parkCopy=true;std::thread x([&]{assert(!a.run(0).status);});
    while(!s.inCopy.load())std::this_thread::yield();std::thread y([&]{assert(!b.run(256).status);});
    s.release=true;x.join();y.join();assert(s.reads==2&&!memcmp(a.out.data(),s.original.data(),4096));}
  {Shared s;Ops o(s);s.fail=true;auto r=o.run(0);assert(r.status==2&&!r.done&&o.out[0]==0xa5&&s.cache.entries[0].state==0);
    s.fail=false;s.disk[0]^=1;r=o.run(0);assert(r.status==2&&!r.done&&s.cache.entries[0].state==0);s.disk=s.original;assert(!o.run(0).status);}
  {Shared s;Ops o(s);assert(!o.run(255,258).status);assert(s.reads==3);assert(o.run(s.disk.size()/512,1).status==1);}
  puts("PASS concurrent independent fills, same-record sharing, pinned copy/eviction, failure/tamper refusal and boundaries");
}
