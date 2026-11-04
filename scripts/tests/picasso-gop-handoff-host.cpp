#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#ifdef _MSC_VER
#define __ATOMIC_RELAXED 0
template<class T,class V> static T __atomic_fetch_add(volatile T *p,V value,int) {T before=*p;*p=before+static_cast<T>(value);return before;}
#endif

using IOReturn=int;
using task_t=void *;
constexpr int kIOReturnSuccess=0,kIOReturnUnsupported=1,kIOReturnNoMemory=2,
    kIOReturnNotReady=3,kIOReturnTimeout=4,kIOReturnBadArgument=5,kIOReturnAborted=6;
constexpr unsigned kIOMapAnywhere=1,kMillisecondScale=1,THREAD_ABORTSAFE=0,THREAD_AWAKENED=0;
volatile uint64_t native_gop_gpu_offers,native_gop_gpu_failures,native_gop_gpu_completions,native_gop_gpu_abandoned,native_gop_wait_stats[7];
volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;
static unsigned callbacks;
static int lastStatus;
static void native_legacy_gpu_complete(void *,int status) {++callbacks;lastStatus=status;}
static void native_legacy_gpu_fallback_observed(int status) {lastStatus=status;}
static void native_legacy_swap_stage(unsigned) {}
static bool PE_parse_boot_argn(const char *,unsigned *,unsigned) {return false;}
struct OSObject {};
struct IORecursiveLock {std::recursive_mutex mutex;};
static void IORecursiveLockLock(IORecursiveLock *lock) {lock->mutex.lock();}
static void IORecursiveLockUnlock(IORecursiveLock *lock) {lock->mutex.unlock();}
struct IOWorkLoop {
    unsigned calls=0;
    IOReturn runAction(IOReturn (*action)(OSObject *,void *,void *,void *,void *),OSObject *owner,void *args) {
        ++calls;return action(owner,args,nullptr,nullptr,nullptr);
    }
};
static uint64_t mockNow=1;
static uint64_t mach_absolute_time() {return mockNow;}
static void absolutetime_to_nanoseconds(uint64_t value,uint64_t *out) {*out=value;}
static void clock_interval_to_deadline(uint32_t,unsigned,uint64_t *out) {*out=1;}
struct IOLock {int held=0;};
static void IOLockLock(IOLock *gate) {assert(!gate->held);gate->held=1;}
static void IOLockUnlock(IOLock *gate) {assert(gate->held);gate->held=0;}
static void IOLockWakeup(IOLock *,void *,bool) {}
static int IOLockSleepDeadline(IOLock *,void *,uint64_t,int) {return 1;}
#define OSSafeReleaseNULL(x) do {if(x){(x)->release();(x)=nullptr;}} while(0)
struct IOMemoryMap {
    unsigned refs=0;
    uint64_t address=0x10000;
    void release() {assert(refs);--refs;}
    uint64_t getVirtualAddress() const {return address;}
    uint64_t getLength() const {return 4096;}
};
struct IOMemoryDescriptor {
    unsigned refs=1;
    bool fail=false;
    IOMemoryMap view;
    void retain() {++refs;}
    void release() {assert(refs);--refs;}
    IOMemoryMap *createMappingInTask(task_t,unsigned,unsigned) {if(fail)return nullptr;++view.refs;return &view;}
};
struct NGPSwap {uint64_t sequence=0,address=0,bytes=0,stride=0;uint32_t width=0,height=0,format=0,reserved=0;};
constexpr unsigned NGP_BGRX8888=1;
struct IOTimerEventSource {
    unsigned arms=0,cancels=0;
    void wakeAtTime(uint64_t) {++arms;}
    void cancelTimeout() {++cancels;}
};
struct Presenter : OSObject {
    IOLock lock;
    IOLock *gate=&lock;
    bool gpuMode=true,online=true,gpuClient=false;
    struct {bool offered=false,delivered=false;uint64_t sequence=0;void *display=nullptr;IOMemoryDescriptor *memory=nullptr;IOMemoryMap *map=nullptr;uint64_t stride=0;uint32_t width=0,height=0,destY=0;} swap;
    static constexpr uint32_t SwapTimeoutMs=500;
    static constexpr uint64_t GpuTaskLapseNs=2000000000ULL;
    IOTimerEventSource timer;
    IOTimerEventSource *watchdog=&timer;
    uint64_t swapSince=0,gpuTaskSince=0;
    unsigned watchdogPrints=0;
    task_t gpuTask=nullptr;
    void armWatchdogLocked() {if(watchdog)watchdog->wakeAtTime(1);}
    void cancelWatchdog() {if(watchdog)watchdog->cancelTimeout();}
    uint64_t swapSequence=0;
    // PRESENTER_METHODS
};
struct OriginalLegacySwapRequest {};
struct Display;
using NativeLegacyDisplay=Display;
struct Display : OSObject {
    IORecursiveLock queueLock;
    IORecursiveLock *kickLock=&queueLock;
    Presenter *presenter=nullptr;
    IOMemoryDescriptor *memory=nullptr;
    uint64_t sourceStride=64,length=4096,gpuLength=0;
    uint32_t width=16,height=64;
    const OriginalLegacySwapRequest *pendingRequest=nullptr,*gpuRequest=nullptr;
    bool gpuBypass=false,transferred=false,gpuRetired=false,copyShadowValid=true;
    unsigned retires=0,cpuRetries=0;
    unsigned gateFailures=0;
    IOWorkLoop workLoop;
    bool hasLoop=true;
    IOWorkLoop *getWorkLoop() {return hasLoop?&workLoop:nullptr;}
    void native_legacy_retirement_failed(Display *) {++gateFailures;}
    unsigned pixels=0x12345678;
    bool retireSwap(const OriginalLegacySwapRequest *,bool) {++retires;pixels=0xdeadbeef;return true;}
    void finishTransfer(const OriginalLegacySwapRequest *q,bool force) {assert(q==pendingRequest && force);retireSwap(q,true);transferred=false;}
    void kickSwapQueue() {assert(!transferred && gpuBypass);++cpuRetries;}
    static void native_legacy_transfer_observed(int,const void *,uint64_t) {}
    bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *m,uint64_t stride,uint32_t w,uint32_t h,unsigned y) {return presenter->offer(display,m,stride,w,h,y);}
    void offer() {
        const OriginalLegacySwapRequest *request=pendingRequest;
        // DISPLAY_OFFER
    }
    // DISPLAY_COMPLETION
    // DISPLAY_GATE
};
int main() {
    Presenter p;
    IOMemoryDescriptor memory;
    OriginalLegacySwapRequest request;
    Display d;d.presenter=&p;d.memory=&memory;d.pendingRequest=&request;
    NGPSwap out;
    assert(!p.offer(nullptr,&memory,64,16,64,0));
    assert(!p.offer(&d,nullptr,64,16,64,0));
    assert(!p.offer(&d,&memory,64,16,64,0)); // no waiting GPU client
    assert(p.waitSwap(nullptr,0,&out)==kIOReturnTimeout);
    assert(!p.swap.offered && memory.refs==1);
    assert(!p.gpuClient);
    p.gpuClient=true; // a serving client is now waiting for the next frame
    memory.fail=true;d.offer();
    assert(d.transferred && p.swap.offered && memory.refs==2);
    assert(d.retires==0 && d.pixels==0x12345678); // producer still owns these pixels
    assert(p.waitSwap(nullptr,0,&out)==kIOReturnNoMemory);
    assert(!p.swap.offered && memory.refs==1 && callbacks==1 && lastStatus==kIOReturnNoMemory);
    d.finishGpuSwap(lastStatus);
    assert(!d.transferred && d.cpuRetries==1 && d.retires==0);
    memory.fail=false;d.gpuBypass=false;d.offer();
    assert(p.waitSwap(nullptr,0,&out)==kIOReturnSuccess);
    assert(out.address && memory.view.refs==1 && memory.refs==2);
    assert(p.completeSwap(out.sequence-1,0)==kIOReturnBadArgument);
    assert(p.swap.offered && memory.view.refs==1);
    assert(p.completeSwap(out.sequence,0)==kIOReturnSuccess);
    assert(memory.refs==1 && !memory.view.refs && callbacks==2);
    d.gpuComplete(0);
    assert(d.retires==1 && !d.transferred && d.workLoop.calls==1);
    d.finishGpuSwap(0);assert(d.retires==1); // duplicate completion is inert
    d.gpuBypass=false;d.offer();p.abandonSwap();
    assert(!p.swap.offered && memory.refs==1 && callbacks==3 && lastStatus==kIOReturnAborted);
    d.finishGpuSwap(lastStatus);assert(d.cpuRetries==2 && d.retires==1);
    p.gpuClient=true;d.gpuBypass=false;d.offer();memory.view.address=0;
    assert(p.waitSwap(nullptr,0,&out)==kIOReturnNoMemory);
    assert(memory.refs==1 && !memory.view.refs && !p.swap.offered && callbacks==4);
    p.abandonSwap();assert(callbacks==4);
    d.hasLoop=false;d.gpuComplete(0);assert(d.gateFailures==1 && d.workLoop.calls==1);
    // No timer means no GPU ownership; the CPU retains responsibility.
    p.gpuClient=true;p.watchdog=nullptr;d.gpuBypass=false;
    assert(!p.offer(&d,&memory,64,16,64,0) && memory.refs==1);
    p.watchdog=&p.timer;
    // Exercise the production watchdog before and after its deadline.
    assert(p.offer(&d,&memory,64,16,64,0));
    const auto expiredSequence=p.swap.sequence;
    mockNow=250000001;Presenter::watchdogFired(&p,&p.timer);
    assert(p.swap.offered && memory.refs==2 && callbacks==4);
    mockNow=500000001;Presenter::watchdogFired(&p,&p.timer);
    assert(!p.swap.offered && !p.gpuClient && memory.refs==1 && callbacks==5 && lastStatus==kIOReturnTimeout);
    assert(p.completeSwap(expiredSequence,0)==kIOReturnSuccess && callbacks==5);
    // Teardown releases an offered frame without calling a stopped display.
    p.gpuClient=true;assert(p.offer(&d,&memory,64,16,64,0));
    p.abandonDisplay(&d);
    assert(!p.swap.offered && memory.refs==1 && callbacks==5);
    // Keep old-path helpers referenced so the same fixture compiles a baseline.
    (void)&PE_parse_boot_argn;
    puts("PASS: producer lifetime, failed/zero-address imports, CPU retry, valid/stale/duplicate completion, abandon, balanced references");
}
