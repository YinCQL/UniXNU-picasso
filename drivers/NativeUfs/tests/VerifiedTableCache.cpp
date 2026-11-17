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
#include <openssl/sha.h>
#include "../VerifiedTableCache.h"
struct Ops {
  std::mutex &m;std::condition_variable &changed;std::vector<uint8_t>&source;
  std::unique_lock<std::mutex> guard;std::atomic<unsigned>&reads;
  bool fail=false;
  Ops(std::mutex&x,std::condition_variable&c,std::vector<uint8_t>&s,std::atomic<unsigned>&n):m(x),changed(c),source(s),guard(x,std::defer_lock),reads(n){}
  void lock(){guard.lock();}void unlock(){guard.unlock();}void sleep(void *){changed.wait(guard);}void wakeup(void *){changed.notify_all();}
  bool readPage(const VrootTable&,uint64_t page,void*dst){assert(!guard.owns_lock());++reads;memcpy(dst,source.data()+page*4096,4096);return !fail;}
  bool sha256(const void*d,size_t n,uint8_t*out){SHA256(static_cast<const unsigned char*>(d),n,out);return true;}
};
int main(){
  std::mutex m;std::condition_variable cv;std::atomic<unsigned> reads{0};
  std::vector<uint8_t> source(3*4096),hashes(3*32),pages(4096);uint32_t where[3]={};VrootCacheEntry entries[1]={};
  const uint64_t bytes=2*4096+32;
  for(size_t i=0;i<source.size();++i)source[i]=uint8_t(i*17+i/4096);
  for(unsigned p=0;p<3;++p)SHA256(source.data()+p*4096,p==2?32:4096,hashes.data()+p*32);
  VrootTable table{hashes.data(),where,bytes,3,0};VrootTableCache cache{pages.data(),entries,1,0,{}};
  Ops a(m,cv,source,reads),b(m,cv,source,reads);uint8_t out[32];
  assert(vrootTableEntry(cache,a,table,0,out)&&!memcmp(out,source.data(),32));
  source[0]^=1;assert(vrootTableEntry(cache,a,table,1,out)); // pinned cached table bytes
  assert(vrootTableEntry(cache,a,table,256,out)); // partial final table page
  assert(!vrootTableEntry(cache,a,table,0,out)&&where[0]==0&&cache.stats.failures==1);
  source[0]^=1;a.fail=true;assert(!vrootTableEntry(cache,a,table,0,out));a.fail=false;
  std::thread x([&]{uint8_t h[32];assert(vrootTableEntry(cache,a,table,2,h));});
  std::thread y([&]{uint8_t h[32];assert(vrootTableEntry(cache,b,table,3,h));});x.join();y.join();
  assert(!vrootTableEntry(cache,a,table,257,out));
  vrootTableDrop(cache,a,table);assert(where[0]==0&&where[1]==0&&where[2]==0);
  puts("PASS page digest/tamper and read failure refusal, partial tail, eviction, shared fill and table teardown");
}
