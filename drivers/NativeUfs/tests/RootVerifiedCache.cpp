// SPDX-License-Identifier: BSD-2-Clause-Patent
// Exercises the production reader. The verifier compares the entire record
// with immutable fixture bytes; owner signature/SHA primitives are unchanged.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <thread>
#include <condition_variable>
using IOReturn = uint32_t;
constexpr IOReturn kIOReturnSuccess=0, kIOReturnBadArgument=1, kIOReturnIOError=2, kIOReturnNotPrivileged=3;
constexpr uint64_t kDataMax=131072;
#include "../RootWindowRead.h"
struct Shared {
  std::mutex window, control;
  std::condition_variable changed;
  unsigned attempts=0;
  bool concurrent=false;
  std::vector<uint8_t> bounce;
  std::vector<uint8_t> extra;
  UfsVerifiedRecordCache cache;
  std::vector<uint8_t> original, disk;
  Shared(unsigned records=4, unsigned slots=1) : bounce(kDataMax),
      extra(slots > 1 ? (slots-1)*kDataMax+32 : 0, 0xa7), original(records*kDataMax) {
    for(size_t i=0;i<original.size();++i) original[i]=uint8_t(i*17+i/512);
    disk=original;
    if(slots>1) { cache.extraBuffer=extra.data()+16; cache.extraBytes=extra.size()-32; }
  }
  void canaries() const {
    if(extra.empty())return;
    for(unsigned i=0;i<16;++i)assert(extra[i]==0xa7 && extra[extra.size()-1-i]==0xa7);
  }
};
struct Ops {
  Shared &s;
  uint64_t absolute=0, bytes=4*kDataMax;
  uint32_t recordBytes=kDataMax;
  std::vector<uint8_t> output;
  unsigned reads=0, checks=0, copies=0, badRead=0, badVerify=0;
  uint64_t shortCopy=UINT64_MAX;
  bool held=false, parked=false;
  explicit Ops(Shared &v) : s(v), output(v.original.size(),0xa5) { bytes=v.original.size(); }
  void lock() {
    { std::lock_guard<std::mutex> g(s.control); ++s.attempts; s.changed.notify_all(); }
    s.window.lock(); held=true;
  }
  void unlock() { assert(held); held=false; s.window.unlock(); }
  IOReturn read(uint64_t lba,uint32_t blocks,void *out) {
    assert(held && blocks && blocks<=32); ++reads;
    assert(lba*4096+blocks*4096<=s.disk.size());
    if(reads==badRead) { memset(out,0xbd,4096); return kIOReturnIOError; }
    memcpy(out,s.disk.data()+lba*4096,blocks*4096); return 0;
  }
  IOReturn verify(uint64_t index,const void *data,size_t length) {
    assert(held); ++checks;
    if(s.concurrent && !parked) {
      parked=true;
      std::unique_lock<std::mutex> guard(s.control);
      s.changed.wait(guard,[&]{return s.attempts>=2;});
    }
    uint64_t offset=index*recordBytes;
    assert(offset<bytes && length==(bytes-offset<recordBytes?bytes-offset:recordBytes));
    if(checks==badVerify || memcmp(data,s.original.data()+absolute+offset,length)) return kIOReturnNotPrivileged;
    return 0;
  }
  uint64_t copy(uint64_t offset,const void *data,uint64_t length) {
    assert(held); ++copies;
    uint64_t n=length<shortCopy?length:shortCopy;
    assert(offset+n<=output.size()); memcpy(output.data()+offset,data,n); return n;
  }
  UfsWindowResult run(uint64_t block,uint64_t count,UfsVerifiedReadStats *report=nullptr,uint8_t *buffer=nullptr) {
    return ufsReadVerifiedRecords(*this,absolute,bytes,block,count,buffer?buffer:s.bounce.data(),recordBytes,s.cache,report);
  }
  void good(uint64_t block,uint64_t count) {
    auto r=run(block,count);assert(!r.status && r.done==count*512 && !held);
    assert(!memcmp(output.data(),s.original.data()+absolute+block*512,r.done));
  }
};
int main() {
  {
    Shared s; Ops o(s);
    for(unsigned i=0;i<32;++i)o.good(i*8,8);
    assert(o.reads==1 && o.checks==1 && s.cache.stats.hits==31);
    assert(s.cache.stats.requestedBytes==kDataMax && s.cache.stats.readBytes==kDataMax);
    puts("PASS sequential 32 page reads: 1 authenticated transport read, 31 hits");
  }
  {
    Shared s; Ops o(s);
    for(unsigned i=0;i<256;++i)o.good(i,1);
    assert(o.reads==1 && o.checks==1 && s.cache.stats.hits==255);
    puts("PASS all 512-byte offsets retain correct authenticated bytes");
  }
  {
    Shared s; Ops o(s);o.good(255,258);
    assert(o.reads==3 && o.checks==3);o.good(520,8);assert(o.reads==3);
    puts("PASS cross-record request and final-record reuse");
  }
  {
    Shared s; Ops o(s);o.absolute=4096;o.bytes=kDataMax+512;
    o.good(256,1);o.good(256,1);
    assert(o.reads==1 && o.checks==1 && s.cache.stats.readBytes==4096);
    puts("PASS nonzero window start and short final authenticated record");
  }
  {
    Shared s; Ops o(s);o.good(0,8);
    s.disk[0]^=0xff; // Read-only authenticated snapshot remains trustworthy.
    o.good(0,8);assert(o.reads==1);
    o.good(256,8);auto r=o.run(0,8);
    assert(r.status==3 && !r.done && !s.cache.entries[0].valid && o.reads==3);
    s.disk=s.original;o.good(0,8);assert(o.reads==4);
    puts("PASS cached snapshot survives backing mutation; eviction reauthenticates and rejects tampering");
  }
  {
    Shared s; Ops o(s);o.good(0,8);o.badRead=2;
    auto r=o.run(256,8);assert(r.status==2 && !r.done && !s.cache.entries[0].valid);
    o.good(0,8);assert(o.reads==3 && o.checks==2);
    puts("PASS partial failed read invalidates previous cache before buffer overwrite");
  }
  {
    Shared s; Ops o(s);o.good(0,8);o.badVerify=2;
    auto r=o.run(256,8);assert(r.status==3 && !r.done && !s.cache.entries[0].valid);
    o.good(0,8);assert(o.reads==3 && o.checks==3);
    puts("PASS failed verification cannot publish or resurrect an old cache entry");
  }
  {
    Shared s; Ops o(s);o.badVerify=2;auto r=o.run(0,257);
    assert(r.status==3 && r.done==kDataMax && o.copies==1 && o.output[r.done]==0xa5 && !s.cache.entries[0].valid);
    puts("PASS partial completion stops exactly before unauthenticated record");
  }
  {
    Shared s; Ops o(s);o.shortCopy=17;auto r=o.run(0,8);
    assert(r.status==2 && r.done==17 && s.cache.entries[0].valid && o.output[17]==0xa5);
    o.shortCopy=UINT64_MAX;o.good(0,8);assert(o.reads==1);
    puts("PASS copy failure preserves only the previously authenticated snapshot");
  }
  {
    Shared s; Ops o(s);o.good(0,8);
    o.absolute=4096;o.bytes-=4096;o.good(0,8);assert(o.reads==2);
    o.bytes-=512;o.good(0,8);assert(o.reads==3);
    o.recordBytes=4096;o.good(0,8);assert(o.reads==4);
    uint8_t other[4096];auto r=o.run(0,8,nullptr,other);assert(!r.status && o.reads==5);
    s.cache={};o.good(0,8);assert(o.reads==6);
    puts("PASS geometry, record size, buffer changes and lifecycle reset force reauthentication");
  }
  {
    Shared s; Ops o(s);UfsVerifiedReadStats report;report.requests=99;
    assert(o.run(0,0,&report).status==1 && !report.requests && !o.reads);
    assert(o.run(0,UINT64_MAX).status==1 && o.run(o.bytes/512,1).status==1);
    for(auto size:{0u,4095u,131073u}){o.recordBytes=size;assert(o.run(0,1).status==1);}
    assert(!o.reads && !s.attempts);
    puts("PASS invalid requests do not read, verify, lock or emit statistics");
  }
  {
    Shared s; Ops o(s);unsigned reports=0;
    for(uint64_t i=1;i<=1048578;++i){
      UfsVerifiedReadStats report;auto r=o.run(0,1,&report);assert(!r.status);
      if(report.requests){++reports;assert(report.requests==i && i>=64 && i<=1048576 && !(i&(i-1)));}
    }
    assert(reports==15 && o.reads==1);
    puts("PASS 15-report limit and cumulative hit/miss statistics over 1048578 requests");
  }
  for(bool same:{false,true}) {
    Shared s;s.concurrent=true;Ops first(s),second(s);UfsWindowResult a{},b{};
    std::thread one([&]{a=first.run(0,8);});
    { std::unique_lock<std::mutex> g(s.control);s.changed.wait(g,[&]{return s.attempts==1;}); }
    std::thread two([&]{b=second.run(same?8:256,8);});
    one.join();two.join();assert(!a.status && !b.status);
    assert(!memcmp(first.output.data(),s.original.data(),4096));
    assert(!memcmp(second.output.data(),s.original.data()+(same?4096:kDataMax),4096));
    assert(first.reads+second.reads==(same?1u:2u));
    assert(s.window.try_lock());s.window.unlock();
    puts(same?"PASS concurrent same-record hit keeps authenticated snapshot":"PASS concurrent eviction keeps each reader's snapshot and releases completion lock");
  }
  {
    Shared s(12,8); Ops o(s);
    for(unsigned page=0;page<32;++page)
      for(unsigned record=0;record<8;++record)o.good(record*256+page*8,8);
    assert(o.reads==8 && o.checks==8 && s.cache.stats.hits==248);
    assert(s.cache.stats.requestedBytes==8*kDataMax && s.cache.stats.readBytes==8*kDataMax);
    s.canaries();
    puts("PASS 8 interleaved records: 256 page reads use 8 reads/verifications instead of 256");
  }
  {
    Shared s(12,8); Ops o(s);
    for(unsigned record=0;record<8;++record)o.good(record*256,8);
    o.good(0,8); // Refresh zero; one is now the LRU victim.
    s.disk[kDataMax]^=0xff;
    o.good(8*256,8);o.good(0,8);assert(o.reads==9);
    auto r=o.run(256,8);assert(r.status==3 && !r.done && o.reads==10);
    o.good(8*256,8);assert(o.reads==10); // Failed replacement does not poison other slots.
    s.disk=s.original;o.good(256,8);assert(o.reads==11);s.canaries();
    puts("PASS LRU hit refresh, eviction reauthentication and tamper rejection preserve other slots");
  }
  for(bool verifyFailure:{false,true}) {
    Shared s(12,8);Ops o(s);o.good(0,8);
    if(verifyFailure)o.badVerify=2;else o.badRead=2;
    auto r=o.run(256,8);assert(r.status==(verifyFailure?3u:2u) && !r.done);
    o.good(0,8);assert(o.reads==2);
    o.good(256,8);assert(o.reads==3);s.canaries();
    puts(verifyFailure?"PASS failed multi-slot verification never publishes bytes":"PASS partial DMA failure leaves unrelated authenticated slots reusable");
  }
  {
    Shared s(12,8);Ops o(s);o.absolute=4096;o.bytes=2*kDataMax+512;
    o.good(512,1);o.good(0,8);o.good(512,1);assert(o.reads==2);
    // All slots, including the tail record, must be discarded on geometry/storage changes.
    o.bytes-=512;o.good(0,8);assert(o.reads==3);
    o.recordBytes=4096;o.good(0,8);assert(o.reads==4);
    s.cache.extraBytes=4095;o.good(0,8);o.good(8,8);o.good(0,8);assert(o.reads==7);
    s.cache.extraBuffer=nullptr;s.cache.extraBytes=SIZE_MAX;o.good(0,8);assert(o.reads==8);
    s.canaries();
    puts("PASS multi-slot short tail, geometry invalidation and insufficient/null storage fallback");
  }
  {
    Shared s(12,10);Ops o(s);
    for(unsigned record=0;record<9;++record)o.good(record*256,8);
    unsigned valid=0;for(auto &entry:s.cache.entries){valid+=entry.valid;assert(entry.age<8);}
    assert(valid==8);auto r=o.run(0,8);assert(!r.status && o.reads==10);
    for(size_t i=7*kDataMax+16;i<s.extra.size();++i)assert(s.extra[i]==0xa7);
    s.canaries();
    puts("PASS oversized storage is capped at 8 slots with bounded ages and untouched excess bytes");
  }
  {
    Shared s(12,8);s.concurrent=true;Ops first(s),second(s);UfsWindowResult a{},b{};
    std::thread one([&]{a=first.run(0,8);});
    {std::unique_lock<std::mutex> g(s.control);s.changed.wait(g,[&]{return s.attempts==1;});}
    std::thread two([&]{b=second.run(256,8);});one.join();two.join();
    assert(!a.status && !b.status && !memcmp(first.output.data(),s.original.data(),4096));
    assert(!memcmp(second.output.data(),s.original.data()+kDataMax,4096));
    first.good(256,8);second.good(0,8);assert(first.reads+second.reads==2);
    assert(s.window.try_lock());s.window.unlock();s.canaries();
    puts("PASS concurrent multi-slot misses/hits retain each authenticated snapshot and release the lock");
  }
  puts("PASS ALL 21 authenticated-cache scenarios");
  return 0;
}
