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
#include "PicassoDataInit.h"
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
namespace InitTest {
static Medium medium;
static struct {uint64_t first,last,imageBytes;uint32_t headerCrc,entriesCrc;} gStoreGeometry={2883584,15330298,8380219392ULL,0x3c17a352,0xd82988c2};
static uint64_t gStoreRegionFirst=9825114,gStoreRegionBlocks=5243041;
static constexpr uint64_t kLogReserveBlocks=8192,kExportBlocks=65536;
static picasso_data_init::Journal gPicassoDataInit;
static uint8_t gPicassoInitScratch[4096],gPicassoInitSaved[4096];
static bool authorized=true,gptBad=false,windowBad=false;
static uint64_t reserveBlocks(){return 262144;}
static bool PE_parse_boot_argn(const char *,void *p,unsigned){*(uint32_t *)p=authorized;return true;}
static int native_ufs_store_gpt_check(){return gptBad;}
static bool picassoRecordWindowOpen(){return !windowBad;}
static void IOLog(const char *,...){}
struct PicassoRecordOps {
    bool read(uint64_t b,uint32_t n,void *p){return medium.read(b,n,p);}
    bool write(uint64_t b,uint32_t n,const void *p){return medium.write(b,n,p);}
    bool sync(){return medium.sync();}
};
#include "init-begin.inc"
#include "init-finish.inc"
static unsigned run(){
    unsigned cases=0;uint8_t seed[32]={0x19};medium.limit=15330299;
    authorized=false;assert(!picasso_data_init_begin(5242880,seed) && !medium.writes);++cases;
    authorized=true;gptBad=true;assert(!picasso_data_init_begin(5242880,seed) && !medium.writes);++cases;gptBad=false;
    ++gStoreRegionFirst;assert(!picasso_data_init_begin(5242880,seed) && !medium.writes);++cases;--gStoreRegionFirst;
    assert(!picasso_data_init_begin(1048576,seed) && !medium.writes);++cases;
    assert(picasso_data_init_begin(5242880,seed)==1);++cases;
    Medium pending=medium;
    assert(picasso_data_init_begin(5242880,seed)==1 && picasso_data_init_finish());++cases;
    Medium completed=medium;auto writes=medium.writes;
    // A remaining replacement bootarg cannot reset an already published disk.
    assert(picasso_data_init_begin(5242880,seed)==2 && picasso_data_init_finish());writes=medium.writes;++cases;
    seed[0]^=1;assert(!picasso_data_init_begin(5242880,seed) && medium.writes==writes);seed[0]^=1;++cases;
    authorized=false;assert(picasso_data_init_begin(5242880,seed)==2);authorized=true;++cases;
    for(unsigned step=1;step<=6;++step)for(bool tear:{false,true}) {
        medium=pending;assert(picasso_data_init_begin(5242880,seed)==1);
        medium.fail=medium.operations+step;medium.tear=tear;
        assert(!picasso_data_init_finish());medium.fail=0;
        int state=picasso_data_init_begin(5242880,seed);
        assert(state==1 || state==2);assert(picasso_data_init_finish());++cases;
    }
    // No pending header: a torn initial journal must refuse, not infer consent.
    for(unsigned step=1;step<=3;++step)for(bool tear:{false,true}) {
        medium=Medium{};medium.limit=15330299;medium.fail=4+step;medium.tear=tear;
        assert(!picasso_data_init_begin(5242880,seed));medium.fail=0;
        int state=picasso_data_init_begin(5242880,seed);assert(state==0 || state==1);++cases;
    }
    for(unsigned slot=0;slot<2;++slot) {
        medium=completed;medium.disk[15256581+slot*2][0]^=1;
        assert(picasso_data_init_begin(5242880,seed)==2 && picasso_data_init_finish());++cases;
    }
    medium=completed;
    // Crash during Data reset: journal stays pending until overlay is durable.
    Medium data;data.limit=persist::windowBlocks(5242880);
    data.disk[14806002-9825114][0]=0xa5;const auto old=data.disk;
    std::vector<uint8_t> bits(160*4096),scratch(32*4096);persist::Overlay overlay;
    // Every reset I/O boundary, including a torn final header. No guest sees
    // this disk until BOTH complete journal slots are durable.
    for(unsigned step=1;step<=165;++step) {
        Medium interrupted=data;interrupted.fail=1+step;interrupted.tear=true;
        persist::Overlay attempt;
        assert(attempt.open(interrupted,5242880,seed,false,bits.data(),true)==persist::Open::Failed);
        interrupted.fail=0;persist::Overlay retry;
        assert(retry.open(interrupted,5242880,seed,false,bits.data(),true)==persist::Open::Fresh);
        assert(retry.present==0);++cases;
    }
    assert(overlay.open(data,5242880,seed,false,bits.data(),true)==persist::Open::Fresh);++cases;
    for(const auto &item:old)assert(data.disk[item.first]==item.second); // bitmap hides retired contents
    SeedDisk disk={};disk.bytes=20ULL<<30;Buffer buf;buf.data.assign(4096,0x62);bool error=false;
    assert(directDiskIO(disk,overlay,data,scratch.data(),32,true,(20ULL<<30)-4096,4096,buf,&error)==4096 && !error);++cases;
    persist::Overlay resumed;assert(resumed.open(data,5242880,seed,false,bits.data())==persist::Open::Resumed);
    buf.data.assign(4096,0);assert(directDiskIO(disk,resumed,data,scratch.data(),32,false,(20ULL<<30)-4096,4096,buf,&error)==4096 && !error);
    for(auto b:buf.data)assert(b==0x62);++cases;
    // Completed journal selects reset=false: damaged and changed-seed overlays
    // cannot be made fresh just because the boot argument remains set.
    data.disk[0][60]^=1;auto before=data.writes;persist::Overlay broken;
    assert(broken.open(data,5242880,seed,false,bits.data())==persist::Open::Kept && data.writes==before);++cases;
    return cases;
}
}
namespace FallbackTest {
static uint32_t native_data_persist_state;
static uint64_t native_ram_block_resident_bytes;
static uint32_t native_ram_block_ready=0,native_ram_block_attach_requested=1;
static uint32_t native_data_persist_header=9,native_data_persist_gpt=0,native_data_persist_gpt_after=0;
static uint64_t native_data_persist_pages=0,native_data_persist_loaded=0,native_data_persist_first_lba=0,native_data_persist_present=0;
static Medium store;
static int picasso_data_init_begin(uint64_t pages,const uint8_t *seed){return InitTest::picasso_data_init_begin(pages,seed);}
static int picasso_data_init_finish(){return InitTest::picasso_data_init_finish();}
static unsigned gptCalls=0,terminated=0,publications=0;
using wait_result_t=int;
static void *current_thread(){return nullptr;}
static void thread_terminate(void *){++terminated;}
static void IOSleep(unsigned){assert(false && "unexpected worker wait");}
static unsigned native_ufs_store_ready(){return 1;}
static unsigned native_ufs_store_mode(){return 2;}
static unsigned native_ufs_store_gpt_check(){++gptCalls;return 0;}
static int native_ufs_store_open(uint64_t blocks,uint64_t *first,const char **why){assert(blocks==store.limit);*first=blocks==5243041?9825114:14019546;*why="";return 1;}
static int native_ufs_store_read(uint64_t b,uint32_t n,void *p){return store.read(b,n,p);}
static int native_ufs_store_write(uint64_t b,uint32_t n,const void *p){return store.write(b,n,p);}
static int native_ufs_store_sync(){return store.sync();}
#ifdef _MSC_VER
#define __ATOMIC_ACQUIRE 1
#ifndef __ATOMIC_RELEASE
#define __ATOMIC_RELEASE 2
#endif
static uint32_t __atomic_load_n(uint32_t *p,int){return *p;}
static void __atomic_store_n(uint32_t *p,uint32_t n,int){*p=n;}
static uint64_t __atomic_fetch_add(uint64_t *p,uint64_t n,int){auto old=*p;*p+=n;return old;}
#endif
static bool allocationFail=false;
static unsigned outstanding=0;
static void *IOMallocZero(uint64_t n) { if(allocationFail)return nullptr;void *p=calloc(1,size_t(n));if(p)++outstanding;return p; }
static void IOFree(void *p,uint64_t) { if(p){assert(outstanding);--outstanding;free(p);} }
static void IOLog(const char *,...) {}
struct NativeRamBlock {
    bool isBig=true,direct=false,directFailed=false,online=false,stopping=false,volatileFallback=false;
    static constexpr uint64_t kVolatileResidentLimit=2ULL<<30;
    SeedDisk big={};RamSparseDisk sparse={};persist::Overlay overlay;
    void *gate=this;uint8_t *memory=nullptr;uint64_t capacity=(4ULL<<30)/512;
    uint8_t seedHash[32]={1};uint8_t *overlayBits=nullptr,*stage=nullptr;
    static constexpr uint32_t kStagePages=32;
    bool storeAttached=false;unsigned releases=0;
    static bool discardRequested(){return false;}
    static bool initializeBlankRequested(){return true;}
    struct StoreOps {
        bool scanProgress=false;
        bool read(uint64_t b,uint32_t n,void *p){return native_ufs_store_read(b,n,p);}
        bool write(uint64_t b,uint32_t n,const void *p){return native_ufs_store_write(b,n,p);}
        bool sync(){return native_ufs_store_sync();}
    };
    void persistFailed(){overlay.active=false;native_data_persist_state=5;}
    void release(){assert(!locks);++releases;}
    void registerService(){assert(!locks && online && native_ram_block_ready==1);++publications;}
    #include "fallback-available.inc"
    #include "fallback-seed-info.inc"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpointer-bool-conversion"
#endif
    #include "fallback-attach.inc"
    #include "fallback-worker.inc"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
    bool importSeed(uint8_t *data,uint64_t bytes) {
        memory=data;uint64_t logical=bytes;bool packedInput=true;
        #include "seed-import.inc"
        memory=nullptr;capacity=logical/512;return true;
    }
    #include "fallback-qualify.inc"
    #include "fallback-allocate.inc"
    #include "fallback-prepare.inc"
    NativeRamBlock(uint64_t size=4ULL<<30) {
        // Two nonzero extents, exercising the production packed-import branch.
        std::vector<uint8_t> packed(64+8192);
        memcpy(packed.data(),"DBRDISK1",8);persist::put32(packed.data()+8,1);persist::put32(packed.data()+12,4096);
        persist::put64(packed.data()+16,size);persist::put64(packed.data()+24,2);
        persist::put64(packed.data()+32,3*4096);persist::put64(packed.data()+40,4096);
        persist::put64(packed.data()+48,size-4096);persist::put64(packed.data()+56,4096);
        memset(packed.data()+64,0x30,4096);memset(packed.data()+64+4096,0x31,4096);
        assert(importSeed(packed.data(),packed.size()));
        assert(isBig && sparse.bytes==size && !sparse.pages && !sparse.resident);
    }
    ~NativeRamBlock() {
        IOFree(overlayBits,persist::bitmapBlocks(capacity*512/4096)*4096);IOFree(stage,kStagePages*4096);
        for(uint64_t i=0;i<big.count;++i)IOFree(big.page[i],4096);
        IOFree(big.index,16);IOFree(big.page,2*sizeof(uint8_t *));
        if(sparse.pages){for(uint64_t i=0;i<sparse.bytes/4096;++i)IOFree(sparse.pages[i],4096);IOFree(sparse.pages,sparse.bytes/4096*sizeof(uint8_t *));}
    }
};
struct Ops {
    NativeRamBlock &ram;Buffer &buffer;
    uint8_t *allocate(){return ram.allocateSparsePage();}
    uint64_t read(uint64_t at,void *p,uint64_t n){return buffer.read(at,p,n);}
    uint64_t copy(uint64_t at,const void *p,uint64_t n){return buffer.copy(at,p,n);}
};
static unsigned run() {
    unsigned cases=0;
    persist::Overlay candidate;candidate.header=1;candidate.scan=persist::BlankScan::Nonzero;
    assert(NativeRamBlock::acceptsVolatileFallback(2,persist::Open::Kept,candidate,0));++cases;
    for(unsigned mode=0;mode<2;++mode){assert(!NativeRamBlock::acceptsVolatileFallback(mode,persist::Open::Kept,candidate,0));++cases;}
    assert(!NativeRamBlock::acceptsVolatileFallback(2,persist::Open::Kept,candidate,1));++cases;
    for(auto status:{persist::Open::Failed,persist::Open::Resumed,persist::Open::Fresh,persist::Open::Blank}) {
        assert(!NativeRamBlock::acceptsVolatileFallback(2,status,candidate,0));++cases;
    }
    for(int header:{0,2,3,-1}){candidate.header=header;assert(!NativeRamBlock::acceptsVolatileFallback(2,persist::Open::Kept,candidate,0));++cases;}
    candidate.header=1;
    for(auto scan:{persist::BlankScan::ReadFailed,persist::BlankScan::NoBuffer,persist::BlankScan::Zero,persist::BlankScan::NotRun}) {
        candidate.scan=scan;assert(!NativeRamBlock::acceptsVolatileFallback(2,persist::Open::Kept,candidate,0));++cases;
    }
    {
        NativeRamBlock ram;auto *original=ram.big.page[1];allocationFail=true;
        assert(!ram.prepareVolatileFallback() && ram.isBig && ram.big.page[1]==original && !ram.sparse.pages);++cases;
        allocationFail=false;ram.online=true;assert(!ram.prepareVolatileFallback());ram.online=false;++cases;
        ram.directFailed=true;assert(!ram.prepareVolatileFallback());ram.directFailed=false;++cases;
        assert(ram.prepareVolatileFallback() && !ram.isBig && ram.volatileFallback && !ram.big.page && ram.sparse.find((4ULL<<30)/4096-1)==original);
        assert(native_data_persist_state==8 && native_ram_block_resident_bytes==8192 && !ram.overlay.active);++cases;
        assert(!ram.prepareVolatileFallback());++cases;
        Buffer buffer;buffer.data.resize(4096);Ops ops={ram,buffer};bool oom=false;
        assert(ramSparseIO(ram.sparse,false,(4ULL<<30)-4096,4096,ops,oom)==4096 && !oom);
        for(auto c:buffer.data)assert(c==0x31);++cases;
        buffer.data.assign(512,0x79);
        assert(ramSparseIO(ram.sparse,true,(4ULL<<30)-4096+512,512,ops,oom)==512 && !oom);
        buffer.data.assign(4096,0);assert(ramSparseIO(ram.sparse,false,(4ULL<<30)-4096,4096,ops,oom)==4096);
        for(unsigned i=0;i<4096;++i)assert(buffer.data[i]==(i>=512 && i<1024?0x79:0x31));++cases;
        assert(ramSparseIO(ram.sparse,false,0,4096,ops,oom)==4096);for(auto c:buffer.data)assert(!c);++cases;
        auto resident=ram.sparse.resident;ram.sparse.resident=ram.kVolatileResidentLimit;
        assert(!ramSparseIO(ram.sparse,true,0,4096,ops,oom) && oom && !ram.sparse.find(0));++cases;
        assert(ramSparseIO(ram.sparse,true,3*4096,4096,ops,oom)==4096 && !oom);++cases;
        ram.sparse.resident=resident;allocationFail=true;
        assert(!ramSparseIO(ram.sparse,true,0,4096,ops,oom) && oom);allocationFail=false;++cases;
        assert(ramSparseIO(ram.sparse,true,0,4096,ops,oom)==4096 && !oom && ram.sparse.resident==resident+4096);++cases;
        assert(!ramSparseIO(ram.sparse,true,4ULL<<30,1,ops,oom));++cases;
    }
    assert(!outstanding);
    {
        NativeRamBlock ram;ram.big.index[1]=ram.big.index[0];
        assert(!ram.prepareVolatileFallback() && ram.isBig && !ram.sparse.pages && ram.big.page[0]);++cases;
    }
    assert(!outstanding);
    {
        NativeRamBlock ram;auto *page=ram.big.page[0];ram.sparse.bytes=2ULL<<30;
        assert(!ram.prepareVolatileFallback() && ram.isBig && ram.big.page[0]==page && !ram.sparse.pages);++cases;
    }
    {
        NativeRamBlock ram;ram.sparse.resident=4096;
        assert(!ram.prepareVolatileFallback() && ram.isBig && !ram.sparse.pages);++cases;
    }
    {
        NativeRamBlock ram;ram.sparse.bytes=0;
        assert(ram.prepareVolatileFallback() && ram.sparse.bytes==(4ULL<<30));++cases;
    }
    // Run actual attachStore/startPersistent/seedInfo over a nonempty window:
    // no persistent writes, exactly one publication, unchanged seed identity.
    {
        NativeRamBlock ram;store=Medium{};store.limit=persist::windowBlocks((4ULL<<30)/4096);
        store.disk[786456][0]=0x55;gptCalls=terminated=publications=0;native_ram_block_ready=0;
        auto *firstPage=ram.big.page[0];uint8_t digest[32];memcpy(digest,ram.seedHash,32);
        NativeRamBlock::startPersistent(&ram,0);
        assert(native_data_persist_state==8 && ram.volatileFallback && ram.online && !ram.direct && !ram.isBig);
        assert(publications==1 && terminated==1 && ram.releases==1 && !locks && !store.writes && !store.syncs && gptCalls==2);++cases;
        assert(ram.overlay.scan==persist::BlankScan::Nonzero && ram.overlay.scanBlock==786456 && ram.sparse.find(3)==firstPage);++cases;
        uint64_t base=1,bytes=0;uint8_t hash[32]={};
        assert(ram.seedInfo(&base,&bytes,hash) && !base && bytes==(4ULL<<30) && !memcmp(hash,digest,32));++cases;
        auto reads=store.reads;ram.attachStore();assert(store.reads==reads && !store.writes && ram.online);++cases;
    }
    // Entire production import -> journal -> overlay -> worker -> seedInfo.
    InitTest::medium=Medium{};InitTest::medium.limit=15330299;
    store=Medium{};store.limit=5243041;store.disk[14806002-9825114][0]=0xa5;
    for(unsigned boot=0;boot<4;++boot) {
        NativeRamBlock ram(20ULL<<30);native_ram_block_ready=0;publications=terminated=0;
        if(boot==2)ram.seedHash[0]^=1;
        if(boot==3)store.disk[0][60]^=1;
        auto before=store.writes;
        NativeRamBlock::startPersistent(&ram,0);
        if(boot<2) {
            assert(ram.online && ram.direct && !ram.volatileFallback && publications==1 && native_ram_block_ready==1);
            assert(native_data_persist_state==(boot?2U:3U));
            uint64_t base=9,size=0;uint8_t identity[32];
            assert(ram.seedInfo(&base,&size,identity) && base==0 && size==(20ULL<<30));
            Buffer b;b.data.assign(4096,boot?0:0x71);bool error=false;NativeRamBlock::StoreOps ops;
            assert(directDiskIO(ram.big,ram.overlay,ops,ram.stage,32,boot==0,(20ULL<<30)-4096,4096,b,&error)==4096 && !error);
            for(auto v:b.data)assert(v==0x71);
            if(boot)assert(store.writes==before);
        } else {
            assert(!ram.online && !ram.direct && !ram.volatileFallback && !publications && native_ram_block_ready==2);
            assert(store.writes==before);
        }
        assert(terminated==1 && ram.releases==1 && !locks);++cases;
    }
    assert(!outstanding);return cases;
}
}
int main() {
    const unsigned initCases=InitTest::run();
    unsigned cases=AuxTest::run()+FallbackTest::run();
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
    assert(f.scan==persist::BlankScan::Nonzero && f.scanBlock==foreign.limit-1 && f.scanOffset==4095 && f.scannedBlocks==foreign.limit-1);
    assert(f.scanCrc==persist::crc32(foreign.disk[foreign.limit-1].data(),4096));++cases;
    Medium head;head.limit=medium.limit;head.disk[0][127]=5;persist::Overlay h;
    assert(h.open(head,pages,seed,false,bits.data(),false,true,stage.data(),32)==persist::Open::Kept);
    assert(h.scanBlock==0 && h.scanOffset==127 && !head.writes);++cases;
    Medium unreadable;unreadable.limit=medium.limit;unreadable.fail=3;persist::Overlay failedScan;
    assert(failedScan.open(unreadable,pages,seed,false,bits.data(),false,true,stage.data(),32)==persist::Open::Failed);
    assert(failedScan.scan==persist::BlankScan::ReadFailed && failedScan.scanBlock==32 && failedScan.scannedBlocks==32 && !unreadable.writes);++cases;
    assert(ov.scan==persist::BlankScan::Zero && ov.scannedBlocks==medium.limit);++cases;
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
    printf("PASS persistence: %u large-disk, recovery, foreign-data and torn-write cases\n",cases+initCases);
}
