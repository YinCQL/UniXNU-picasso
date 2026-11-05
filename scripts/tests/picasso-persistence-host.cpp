// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <map>
#include <vector>
#include <array>
#include "PersistOverlay.h"
#include "DirectDisk.h"
#include "PicassoRecord.h"
struct Medium {
    std::map<uint64_t,std::array<uint8_t,4096>> disk;
    uint64_t limit=0,reads=0,readBlocks=0,writes=0,syncs=0,operations=0,fail=0;
    bool tear=false;
    bool read(uint64_t b,uint32_t n,void *p) {
        assert(n && b<limit && n<=limit-b);++reads;readBlocks+=n;
        if(++operations==fail)return false;
        for(uint32_t i=0;i<n;++i) {
            auto it=disk.find(b+i);if(it==disk.end())memset((uint8_t *)p+i*4096,0,4096);
            else memcpy((uint8_t *)p+i*4096,it->second.data(),4096);
        }return true;
    }
    bool write(uint64_t b,uint32_t n,const void *p) {
        assert(n && b<limit && n<=limit-b);++writes;
        if(++operations==fail) {
            if(tear)memcpy(disk[b].data(),p,123);
            return false;
        }
        for(uint32_t i=0;i<n;++i)memcpy(disk[b+i].data(),(const uint8_t *)p+i*4096,4096);
        return true;
    }
    bool sync() {++syncs;return ++operations!=fail;}
};
struct Buffer {
    std::vector<uint8_t> data;
    uint64_t read(uint64_t o,void *p,uint64_t n){assert(o+n<=data.size());memcpy(p,data.data()+o,size_t(n));return n;}
    uint64_t copy(uint64_t o,const void *p,uint64_t n){assert(o+n<=data.size());memcpy(data.data()+o,p,size_t(n));return n;}
};
using IOReturn=int;
constexpr int kIOReturnNotReady=1,kIOReturnSuccess=0,kIOReturnIOError=2;
static unsigned native_data_persist_syncs=0;
static unsigned locks=0,syncCalls=0;
static bool syncOK=true;
static void IOLockLock(void *) {assert(!locks);++locks;}
static void IOLockUnlock(void *) {assert(locks==1);--locks;}
static bool native_ufs_store_sync() {++syncCalls;assert(locks==1);return syncOK;}
#ifdef _MSC_VER
#define __ATOMIC_RELAXED 0
static unsigned __atomic_fetch_add(unsigned *p,unsigned n,int) {auto old=*p;*p+=n;return old;}
#endif
struct SyncHarness {
    void *gate=this;bool online=true,isBig=true,direct=true,directFailed=false;
    unsigned failures=0;
    void persistFailed(){++failures;}
    #include "sync.inc"
};
namespace AuxTest {
static Medium medium;
static unsigned depth=0;
struct IOLock {};
static IOLock recordLock,hostLock;
static IOLock *gPicassoRecordLock=&recordLock;
static struct {IOLock *lock;} gHost={&hostLock};
static struct {uint64_t last;} gStoreGeometry={200000};
static uint64_t gAuxRegionFirst=0,gAuxRegionBlocks=0;
static constexpr uint64_t kLogReserveBlocks=8192,kExportBlocks=65536;
static uint8_t native_ram_block_seed_sha256[32]={7};
static bool gptFail=false,allocationFail=false;
static void IOLockLock(IOLock *l) {assert((l==&recordLock && depth==0)||(l==&hostLock && depth==1));++depth;}
static void IOLockUnlock(IOLock *l) {assert((l==&recordLock && depth==1)||(l==&hostLock && depth==2));--depth;}
static void *IOMalloc(size_t n){return allocationFail?nullptr:malloc(n);}
static void *IOMallocZero(size_t n){return allocationFail?nullptr:calloc(1,n);}
static void IOFree(void *p,size_t){free(p);}
static int native_ufs_store_gpt_check(){return gptFail?1:0;}
static bool hostAuxOpen(uint64_t,uint64_t){assert(depth==2);return true;}
struct PicassoRecordOps {
    bool read(uint64_t b,uint32_t n,void *p){assert(depth==1);return medium.read(b,n,p);}
    bool write(uint64_t b,uint32_t n,const void *p){assert(depth==1);return medium.write(b,n,p);}
    bool sync(){assert(depth==1);return medium.sync();}
};
#ifdef _MSC_VER
#define __ATOMIC_RELEASE 3
static void __atomic_store_n(uint64_t *p,uint64_t n,int){*p=n;}
#endif
#include "aux-claim.inc"
static unsigned run() {
    constexpr uint64_t first=1000,blocks=65536;
    medium.limit=200001;
    assert(picassoAuxClaim(first,blocks) && gAuxRegionFirst==first && gAuxRegionBlocks==blocks && !depth);
    uint64_t reads=medium.reads,writes=medium.writes;
    assert(picassoAuxClaim(first,blocks) && medium.reads==reads && medium.writes==writes);
    assert(!picassoAuxClaim(first+1,blocks));
    Medium saved=medium;gAuxRegionBlocks=0;
    assert(picassoAuxClaim(first,blocks) && medium.reads==reads+4 && medium.writes==writes);
    gAuxRegionBlocks=0;native_ram_block_seed_sha256[0]^=1;
    assert(!picassoAuxClaim(first,blocks) && medium.writes==writes);native_ram_block_seed_sha256[0]^=1;
    medium=Medium{};medium.limit=200001;medium.disk[first+blocks-1][4095]=1;
    assert(!picassoAuxClaim(first,blocks) && !medium.writes && !depth);
    medium=Medium{};medium.limit=200001;gptFail=true;
    assert(!picassoAuxClaim(first,blocks) && !medium.writes && !depth);gptFail=false;
    allocationFail=true;assert(!picassoAuxClaim(first,blocks) && !depth);allocationFail=false;
    medium=saved;medium.fail=medium.operations+1;
    assert(!picassoAuxClaim(first,blocks) && !depth);
    return 9;
}
}
int main() {
    unsigned cases=AuxTest::run();
    SyncHarness sync;
    assert(!sync.doSynchronizeCache() && syncCalls==1 && native_data_persist_syncs==1 && !locks);++cases;
    syncOK=false;assert(sync.doSynchronizeCache()==kIOReturnIOError && sync.directFailed && sync.failures==1 && !locks);++cases;
    unsigned calls=syncCalls;assert(sync.doSynchronizeCache()==kIOReturnIOError && syncCalls==calls);++cases;
    sync.direct=false;assert(sync.doSynchronizeCache()==kIOReturnNotReady && syncCalls==calls);++cases;
    sync.online=false;assert(sync.doSynchronizeCache()==kIOReturnNotReady && !locks);++cases;
    const uint64_t pages=(4ULL<<30)/4096;
    assert(persist::windowBlocks(pages)==1048609);++cases;
    uint8_t seed[32]={1};std::vector<uint8_t> bits(size_t(persist::bitmapBlocks(pages)*4096)),stage(32*4096);
    Medium medium;medium.limit=persist::windowBlocks(pages);
    persist::Overlay ov;
    assert(ov.open(medium,pages,seed,false,bits.data(),false,true,stage.data(),32)==persist::Open::Fresh);
    assert(medium.readBlocks==medium.limit+1 && medium.writes==34 && medium.syncs==3);++cases;
    SeedDisk disk={};disk.bytes=4ULL<<30;
    Buffer buffer;buffer.data.resize(8192,0x39);bool error=false;
    assert(directDiskIO(disk,ov,medium,stage.data(),32,true,(4ULL<<30)-8192,8192,buffer,&error)==8192 && !error);++cases;
    std::vector<uint8_t> bits2(bits.size());persist::Overlay next;
    assert(next.open(medium,pages,seed,false,bits2.data())==persist::Open::Resumed && next.present==2);
    buffer.data.assign(8192,0);
    assert(directDiskIO(disk,next,medium,stage.data(),32,false,(4ULL<<30)-8192,8192,buffer,&error)==8192 && !error);
    for(auto b:buffer.data)assert(b==0x39);++cases;
    // 512-byte merge into an existing page retains all neighbouring bytes.
    buffer.data.assign(512,0x72);
    assert(directDiskIO(disk,next,medium,stage.data(),32,true,(4ULL<<30)-4096+512,512,buffer,&error)==512);
    buffer.data.assign(4096,0);assert(directDiskIO(disk,next,medium,stage.data(),32,false,(4ULL<<30)-4096,4096,buffer,&error)==4096);
    for(unsigned i=0;i<4096;++i)assert(buffer.data[i]==(i>=512 && i<1024?0x72:0x39));++cases;
    assert(!directDiskIO(disk,next,medium,stage.data(),32,false,4ULL<<30,1,buffer,&error));++cases;
    // Wrong seed is preserved, never automatically reformatted.
    uint8_t different[32]={2};uint64_t before=medium.writes;persist::Overlay wrong;
    assert(wrong.open(medium,pages,different,false,bits.data(),false,true,stage.data(),32)==persist::Open::Kept && medium.writes==before);++cases;
    // A blank header with even one foreign byte in the LAST page is not empty.
    Medium foreign;foreign.limit=medium.limit;foreign.disk[foreign.limit-1][4095]=1;persist::Overlay f;
    assert(f.open(foreign,pages,seed,false,bits.data(),false,true,stage.data(),32)==persist::Open::Kept && !foreign.writes);++cases;
    // Reset fault at each mutation: old header is invalidated before bitmap edits.
    Medium small;small.limit=persist::windowBlocks(16);uint8_t sb[4096],page[4096];memset(page,0x55,sizeof page);
    persist::Overlay initial;assert(initial.open(small,16,seed,false,sb,true)==persist::Open::Fresh);
    assert(initial.writePage(small,4,page));
    for(unsigned n=1;n<=6;++n) {
        Medium m=small;m.fail=m.operations+1+n;persist::Overlay reset;
        assert(reset.open(m,16,seed,false,sb,true)==persist::Open::Failed);
        m.fail=0;persist::Overlay reopened;auto status=reopened.open(m,16,seed,true,sb);
        assert(status==persist::Open::Blank || (status==persist::Open::Resumed && (reopened.present==0 || reopened.present==1)));++cases;
    }
    // Dual records: every failed/partial step retains old or fully committed data.
    Medium records;records.limit=20;picasso_record::Record rec;
    uint8_t out[8192]={},recordScratch[8192],value[8192];memset(value,0x41,sizeof value);
    assert(rec.open(records,2,1,8192,recordScratch,out)==2 && !records.writes);
    assert(rec.save(records,value,8192,recordScratch));++cases;
    Medium baseline=records;
    for(unsigned n=1;n<=3;++n)for(unsigned torn=0;torn<2;++torn) {
        Medium m=baseline;picasso_record::Record current;
        assert(current.open(m,2,1,8192,recordScratch,out)==1 && out[0]==0x41);
        m.fail=m.operations+n;m.tear=torn!=0;memset(value,0x42,sizeof value);
        assert(!current.save(m,value,8192,recordScratch) && current.failed);
        auto count=m.writes;assert(!current.save(m,value,8192,recordScratch) && count==m.writes);
        m.fail=0;picasso_record::Record after;
        assert(after.open(m,2,1,8192,recordScratch,out)==1);
        assert(out[0]==0x41 || out[0]==0x42);for(auto b:out)assert(b==out[0]);++cases;
    }
    Medium hidden;hidden.limit=20;hidden.disk[7][3000]=1;picasso_record::Record no;
    assert(no.open(hidden,2,1,8192,recordScratch,out)==0 && !hidden.writes);++cases;
    Medium corrupt=baseline;corrupt.disk[3][0]^=1;picasso_record::Record bad;
    assert(bad.open(corrupt,2,1,8192,recordScratch,out)==0 && corrupt.writes==baseline.writes);++cases;
    printf("PASS persistence: %u large-disk, recovery, foreign-data and torn-write cases\n",cases);
}
