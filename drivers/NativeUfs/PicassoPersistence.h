// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "PicassoRecord.h"
// Board-private records in the existing 1 GiB reserve, disjoint from Data/aux.
// NVRAM: first six 4K blocks of the otherwise-unused export region.
// Console: two 260 KiB slots at the beginning of the final 32 MiB log region.
// Legacy raw log/export APIs stay disabled (sheng-log is absent).
namespace {
struct PicassoRecordOps {
    bool read(uint64_t b,uint32_t n,void *p) {
        while(n){uint32_t k=n>32?32:n;if(hostRead4k(0,b,k,p)!=kIOReturnSuccess)return false;
            b+=k;n-=k;p=static_cast<uint8_t *>(p)+k*4096;}return true;
    }
    bool write(uint64_t b,uint32_t n,const void *p) {
        while(n){uint32_t k=n>32?32:n;if(hostWrite4k(0,b,k,p,true)!=kIOReturnSuccess)return false;
            b+=k;n-=k;p=static_cast<const uint8_t *>(p)+k*4096;}return true;
    }
    bool sync() {return hostSyncCache()==kIOReturnSuccess;}
};
IOLock *gPicassoRecordLock;
picasso_record::Record gPicassoNvram, gPicassoLog;
uint8_t gPicassoNvramImage[8192],gPicassoNvramScratch[8192];
bool gPicassoNvramLoaded;
extern "C" void picasso_nvram_periodic_sync(void) __attribute__((weak_import));
extern "C" uint32_t picasso_console_snapshot(void *,uint32_t,uint64_t *,uint64_t *) __attribute__((weak_import));
static void picassoLogThread(void *,wait_result_t) {
    uint8_t *scratch=static_cast<uint8_t *>(IOMallocZero(262144));
    uint8_t *data=static_cast<uint8_t *>(IOMallocZero(262144));
    if(!scratch || !data) {if(scratch)IOFree(scratch,262144);if(data)IOFree(data,262144);thread_terminate(current_thread());return;}
    PicassoRecordOps ops;
    uint64_t base=gStoreGeometry.last+1-kLogReserveBlocks;
    int opened=gPicassoLog.open(ops,base,2,262144,scratch,data);
    IOLog("PICASSO_LOG open=%d generation=%llu previous_bytes=%u\n",opened,gPicassoLog.generation,gPicassoLog.length);
    uint64_t previous=0;unsigned ticks=0;
    while(true) {
        IOSleep(10000);
        // Same periodic GPT recheck rule as Sheng BootLog: stop records if
        // the allocation changed underneath us. No transport reset/retry.
        if(++ticks%30==0 && native_ufs_store_gpt_check()) {
            IOLockLock(gPicassoRecordLock);gPicassoNvram.failed=true;IOLockUnlock(gPicassoRecordLock);
            IOLog("PICASSO_RECORDS GPT changed; stopped\n");break;
        }
        if(&picasso_nvram_periodic_sync)picasso_nvram_periodic_sync();
        if(!opened || !&picasso_console_snapshot)continue;
        uint64_t total=0,dropped=0;
        uint32_t n=picasso_console_snapshot(data,262144,&total,&dropped);
        if(!n || total==previous)continue;
        if(!gPicassoLog.save(ops,data,n,scratch)) {IOLog("PICASSO_LOG save failed; stopped\n");opened=0;continue;}
        previous=total;
        // No per-save console logging: that would create its own new payload.
    }
    IOFree(scratch,262144);IOFree(data,262144);
    thread_terminate(current_thread());
}
}
extern "C" volatile uint32_t native_data_persist_state;
extern "C" int picasso_persistence_start(void) {
    // A volatile seed fallback does not establish ownership of persistent windows.
    if(native_data_persist_state!=2 && native_data_persist_state!=3) {
        IOLog("PICASSO_RECORDS disabled Data state=%u\n",native_data_persist_state);return 0;
    }
    static bool attempted=false;
    if(attempted)return gPicassoRecordLock && gPicassoNvram.ready;
    attempted=true;
    uint32_t enabled=0;PE_parse_boot_argn("picasso-persist-records",&enabled,sizeof(enabled));
    if(enabled!=1 || storeMode()!=2 || logMode()!=0 || !gStoreGeometry.valid || native_ufs_store_gpt_check())return 0;
    uint64_t partition=gStoreGeometry.last-gStoreGeometry.first+1;
    if(reserveBlocks()<kLogReserveBlocks+kExportBlocks || reserveBlocks()>=partition ||
        partition-reserveBlocks()<(gStoreGeometry.imageBytes+4095)/4096+1024)return 0;
    uint64_t first=gStoreGeometry.last+1-kLogReserveBlocks-kExportBlocks;
    // host window registration and all transport commands share the controller lock.
    IOLockLock(gHost.lock);
    bool opened=hostLogOpen(first,kLogReserveBlocks+kExportBlocks);
    IOLockUnlock(gHost.lock);
    if(!opened || !(gPicassoRecordLock=IOLockAlloc()))return 0;
    PicassoRecordOps ops;
    int state=gPicassoNvram.open(ops,first,1,8192,gPicassoNvramScratch,gPicassoNvramImage);
    gPicassoNvramLoaded=state==1 && gPicassoNvram.length==8192;
    if(state==1 && !gPicassoNvramLoaded)gPicassoNvram.ready=false;
    IOLog("PICASSO_NVRAM open=%d generation=%llu restored=%u\n",state,gPicassoNvram.generation,gPicassoNvramLoaded);
    thread_t thread=THREAD_NULL;
    if(kernel_thread_start(picassoLogThread,nullptr,&thread)==KERN_SUCCESS)thread_deallocate(thread);
    return gPicassoNvram.ready;
}
extern "C" int picasso_nvram_load(void *out,uint32_t n) {
    if(!out || n!=8192 || !gPicassoRecordLock)return -1;
    IOLockLock(gPicassoRecordLock);
    int result=gPicassoNvramLoaded?1:gPicassoNvram.ready?0:-1;
    if(result==1)memcpy(out,gPicassoNvramImage,n);
    IOLockUnlock(gPicassoRecordLock);return result;
}
extern "C" int picasso_nvram_save(const void *data,uint32_t n) {
    if(!data || n!=8192 || !gPicassoRecordLock)return 0;
    IOLockLock(gPicassoRecordLock);
    PicassoRecordOps ops;
    bool ok=gPicassoNvram.save(ops,static_cast<const uint8_t *>(data),n,gPicassoNvramScratch);
    if(ok){memcpy(gPicassoNvramImage,data,n);gPicassoNvramLoaded=true;}
    IOLockUnlock(gPicassoRecordLock);
    IOLog("PICASSO_NVRAM save=%u generation=%llu\n",ok,gPicassoNvram.generation);
    return ok;
}

// Ownership marker is outside the auxiliary payload, bound to location and seed.
extern "C" uint8_t native_ram_block_seed_sha256[32];
static bool picassoAuxClaim(uint64_t first,uint64_t blocks) {
    if(!gPicassoRecordLock || blocks!=65536)return false;
    IOLockLock(gPicassoRecordLock);
    if(gAuxRegionBlocks) {
        bool same=gAuxRegionFirst==first && gAuxRegionBlocks==blocks;
        IOLockUnlock(gPicassoRecordLock);return same;
    }
    PicassoRecordOps ops;
    uint8_t *allocation=static_cast<uint8_t *>(IOMallocZero(8192));
    if(!allocation){IOLockUnlock(gPicassoRecordLock);return false;}
    uint8_t *data=allocation,*scratch=allocation+4096;
    picasso_record::Record owner;
    uint64_t marker=gStoreGeometry.last+1-kLogReserveBlocks-kExportBlocks+6;
    int state=owner.open(ops,marker,3,4096,scratch,data);
    uint8_t expected[48]={};picasso_record::put64(expected,first);picasso_record::put64(expected+8,blocks);
    memcpy(expected+16,native_ram_block_seed_sha256,32);
    bool ok=state==1 && owner.length==48 && !memcmp(data,expected,48);
    if(state==2) {
        uint8_t *scan=static_cast<uint8_t *>(IOMalloc(32*4096));
        ok=scan!=nullptr;
        for(uint64_t b=0;ok && b<blocks;b+=32)ok=ops.read(first+b,32,scan) && picasso_record::zero(scan,32*4096);
        if(scan)IOFree(scan,32*4096);
        if(ok)ok=!native_ufs_store_gpt_check() && owner.save(ops,expected,48,scratch);
    }
    if(ok) {IOLockLock(gHost.lock);ok=hostAuxOpen(first,blocks);IOLockUnlock(gHost.lock);}
    if(ok) {gAuxRegionFirst=first;__atomic_store_n(&gAuxRegionBlocks,blocks,__ATOMIC_RELEASE);}
    IOFree(allocation,8192);IOLockUnlock(gPicassoRecordLock);return ok;
}
