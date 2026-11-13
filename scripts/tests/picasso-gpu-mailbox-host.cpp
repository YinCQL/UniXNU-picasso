// SPDX-License-Identifier: MIT
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
using IOReturn=int;using IOOptionBits=unsigned;using wait_result_t=int;using thread_t=void *;
enum {kIOReturnSuccess=0,kIOReturnNotReady=1,kIOReturnNoResources=2,KERN_SUCCESS=0,kIOMapAnywhere=8};
struct IOMemoryDescriptor {unsigned refs=1;void retain(){++refs;}};
static std::mutex lock;
static struct {std::mutex *lock;} g{&lock};
static void IOLockLock(std::mutex *m){m->lock();}
static void IOLockUnlock(std::mutex *m){m->unlock();}
static unsigned starts,deallocations;
static bool startFailed;
static void nativeAdrenoMailboxPoller(void *,wait_result_t){}
static int kernel_thread_start(void (*fn)(void *,wait_result_t),void *ctx,thread_t *t){
    assert(fn==nativeAdrenoMailboxPoller && !ctx);++starts;
    if(startFailed)return 1;
    *t=reinterpret_cast<void *>(uintptr_t(1));return KERN_SUCCESS;
}
static void thread_deallocate(thread_t t){assert(t);++deallocations;}
static IOMemoryDescriptor page,*gMailbox=&page;
static bool gPollerStarted;
static struct {uint32_t mb_maps;} native_adreno_metal_diag;
class AGXDeviceUserClient {public:bool live_=true;IOReturn mailboxMemory(IOOptionBits *,IOMemoryDescriptor **);};
static AGXDeviceUserClient *gMailboxOwner;
#include "mailbox.inc"
int main(){
    AGXDeviceUserClient owner,stranger;gMailboxOwner=&owner;
    IOMemoryDescriptor *memory=nullptr;IOOptionBits options=123;
    assert(stranger.mailboxMemory(&options,&memory)==kIOReturnNotReady && !memory && starts==0);
    owner.live_=false;assert(owner.mailboxMemory(&options,&memory)==kIOReturnNotReady && starts==0);
    owner.live_=true;gMailbox=nullptr;assert(owner.mailboxMemory(&options,&memory)==kIOReturnNotReady && starts==0);
    gMailbox=&page;startFailed=true;
    assert(owner.mailboxMemory(&options,&memory)==kIOReturnNoResources && !memory && options==123);
    assert(!gPollerStarted && starts==1 && page.refs==1 && deallocations==0);
    startFailed=false;std::vector<std::thread> workers;
    for(unsigned i=0;i<32;++i)workers.emplace_back([&]{
        IOMemoryDescriptor *m=nullptr;IOOptionBits o=0;
        assert(owner.mailboxMemory(&o,&m)==kIOReturnSuccess && m==&page && o==kIOMapAnywhere);
    });
    for(auto &t:workers)t.join();
    assert(starts==2 && deallocations==1 && gPollerStarted && page.refs==33 && native_adreno_metal_diag.mb_maps==32);
    gMailboxOwner=nullptr;assert(owner.mailboxMemory(&options,&memory)==kIOReturnNotReady && !memory);
    gMailboxOwner=&stranger;assert(stranger.mailboxMemory(&options,&memory)==kIOReturnSuccess && starts==2);
    assert(page.refs==34 && native_adreno_metal_diag.mb_maps==33);
    puts("PASS owner eligibility, thread-start failure/retry, 32 concurrent maps, close and replacement owner");
}
