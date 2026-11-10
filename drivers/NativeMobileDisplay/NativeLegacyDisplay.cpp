// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso CPU/GOP adaptation of NativeMobileDisplay 8666a995.
// Original family lifecycle and client methods are bound by the build audit.
#include "OriginalLegacyFramebufferABI.h"
#include "NativeLegacyRequest.h"
#include "NativeLegacyBootLogo.h"
#include "PicassoGop.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <kern/clock.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned,void *,void *);
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *,uint32_t,uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_client_enter(task_t,uint32_t);
extern "C" void native_legacy_client_observed(IOReturn,IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned,IOReturn,unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *),native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy);
extern "C" void native_legacy_swap_validation_observed(bool,unsigned,const void *,const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn,const void *,uint64_t);
extern "C" void native_legacy_swap_geometry_observed(const uint32_t *,const uint32_t *,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
extern "C" void native_legacy_retirement_observed(IOService *);
// GPU scanout hand-off (boot-arg sheng-fb-gpu=1): a finished frame is offered
// to the privileged GPU client instead of being copied. The presenter's offer
// either takes the frame or refuses it, in which case this file keeps the CPU
// path unchanged. A failed GPU copy falls back for the rest of the kick.
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height,uint32_t destY);
extern "C" void native_legacy_swap_stage(unsigned stage);
// The GPU's newest fence handed out and newest one retired (NativeAdrenoPublish). Direct scanout waits for
// the compositor's frame to finish before flipping to its surface; absent (CPU-only backend) it never waits.
extern "C" bool native_adreno_fences(uint32_t *submitted,uint32_t *retired) __attribute__((weak_import));
extern "C" {extern volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;}
extern "C" void native_legacy_gpu_fallback_observed(int status);
extern "C" void native_legacy_retirement_failed(IOService *);
// Display refresh tick (boot-arg sheng-vsync=<Hz>, 0 or absent: off). The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. The GOP scanout has no vblank interrupt, so a
// work-loop timer stands in for it while at least one client has vsync notifications enabled. Events: 0 armed, 1 sent, 2 idle (no client),
// 3 late (deadline passed, cadence restarted), 4 started, 5 refresh properties reported at that rate.
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
extern "C" uint64_t mach_continuous_time(void);
/* Per-frame timeline for the GPU scanout hand-off (boot arg picasso-frame-trace=1; default off, no output
 * and no work when absent). One line per completed GPU-served frame:
 *   offer_ns  when the compositor's swap became an offer to the GPU client (kickOnce)
 *   done_ns   when the client reported the copy complete (finishGpuSwap)
 *   svc_us    done - offer: how long the frame was in the GPU path
 *   gap_us    offer - the previous frame's done: how long the display had no frame to serve
 * The userspace side of the same frames (import/encode+commit/GPU wait) is in the plugin's PF line and the
 * telemetry table's per-command-buffer rows (CDC `gpu-table`). */
static void picasso_frame_trace(uint64_t offer_ticks)
{
    extern volatile uint64_t native_gop_last_take_tick,native_gop_last_complete_tick;
    static int enabled=-1;
    if(enabled<0){uint32_t value=0;enabled=(PE_parse_boot_argn("picasso-frame-trace",&value,sizeof value)&&value)?1:0;}
    if(!enabled || !offer_ticks)return;
    static uint64_t lastDone=0,count=0;
    const uint64_t done=mach_absolute_time();
    const uint64_t take=native_gop_last_take_tick,complete=native_gop_last_complete_tick;
    uint64_t offer_ns=0,done_ns=0,svc_ns=0,gap_ns=0,take_ns=0,comp_ns=0,hand_ns=0;
    absolutetime_to_nanoseconds(offer_ticks,&offer_ns);
    absolutetime_to_nanoseconds(done,&done_ns);
    absolutetime_to_nanoseconds(done-offer_ticks,&svc_ns);
    if(lastDone && offer_ns>lastDone)gap_ns=offer_ns-lastDone;
    lastDone=done_ns;
    /* Split offer-to-done: scheduling (offer to the client taking it, including the page mapping), the client's
     * present (take to its completion), and this handler (completion to the display driver seeing it). */
    if(take>offer_ticks)absolutetime_to_nanoseconds(take-offer_ticks,&take_ns);
    if(complete>take)absolutetime_to_nanoseconds(complete-take,&comp_ns);
    if(done>complete)absolutetime_to_nanoseconds(done-complete,&hand_ns);
    printf("PICASSO_FRAME n=%llu offer_ns=%llu done_ns=%llu svc_us=%llu gap_us=%llu take_us=%llu comp_us=%llu hand_us=%llu\n",
        (unsigned long long)++count,(unsigned long long)offer_ns,(unsigned long long)done_ns,
        (unsigned long long)(svc_ns/1000),(unsigned long long)(gap_ns/1000),
        (unsigned long long)(take_ns/1000),(unsigned long long)(comp_ns/1000),(unsigned long long)(hand_ns/1000));
}
/* ---- Direct scanout (sheng-dpu-scanout) ------------------------------------------------
 * Ported from sheng's GopFramebuffer/NativeLegacyDisplay.cpp and NativeUfs.cpp. The display
 * pipe's address registers are 32-bit and every kernel allocation is above 4 GiB, so the
 * compositor's surfaces are mapped through the apps SMMU bank the boot loader assigned to the
 * display stream into kDpuSlots windows of 32 MiB at kDpuWindow. 0x80000000..0xffffffff stays
 * identity mapped (two 1 GiB blocks) so the boot loader's scanout at 0xa0000000 keeps working. */
static constexpr uint32_t kDpuWindow=0x10000000u,kDpuSlots=4,kDpuSlotBytes=32u<<20,kDpuSlotTables=kDpuSlotBytes>>21;
static constexpr uint32_t kDpuFaults=0xc00001feu;
static IOMemoryMap *gDpuSmmuMap=nullptr;
static volatile uint32_t *gDpuSmmu=nullptr;
static uint64_t *gDpuTables=nullptr;
static volatile uint32_t *gDpuCb=nullptr;
static IOLock *gDpuLock=nullptr;
static uint32_t gDpuState=0,gDpuBank=0;
static uint32_t dpuRead(uint32_t offset) {return gDpuSmmu?gDpuSmmu[offset/4]:0xffffffffu;}
__attribute__((unused)) static void dpuWrite(uint32_t offset,uint32_t value) {if(gDpuSmmu)gDpuSmmu[offset/4]=value;}
static void dpuClean(const void *start,size_t length) {
    for(size_t off=0;off<length;off+=64)__asm__ volatile("dc civac, %0"::"r"(static_cast<const uint8_t *>(start)+off):"memory");
    __asm__ volatile("dsb sy":::"memory");
}
static bool dpuTlbFlush(volatile uint32_t *cb) {
    cb[0x618/4]=0;cb[0x7f0/4]=0;
    __asm__ volatile("dsb sy":::"memory");
    for(unsigned spin=0;spin<100000;++spin)if(!(cb[0x7f4/4]&1))return true;
    return false;
}
static void dpuEnable(volatile uint32_t *cb,uint32_t bank,uint32_t oldSctlr) {
    const size_t bytes=(2+kDpuSlots*kDpuSlotTables)*4096;
    IOBufferMemoryDescriptor *tables=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
        (IOOptionBits)kIODirectionInOut|(IOOptionBits)kIOMemoryPhysicallyContiguous|(IOOptionBits)kIOMemoryMapperNone,
        bytes,4096);
    gDpuLock=IOLockAlloc();
    if(!tables || !gDpuLock) {gDpuState=0xdead0002u;return;}
    uint64_t *l1=(uint64_t *)tables->getBytesNoCopy();
    const uint64_t pa=tables->getPhysicalSegment(0,nullptr,kIOMemoryMapperNone);
    bzero(l1,bytes);
    const uint64_t attrs=(1ULL<<10)|(1ULL<<6);
    l1[0]=pa|3;
    l1[2]=0x80000000ULL|attrs|1;
    l1[3]=0xc0000000ULL|attrs|1;
    for(uint32_t t=0;t<kDpuSlots*kDpuSlotTables;++t)l1[(kDpuWindow>>21)+t]=(pa+(2+t)*4096ULL)|3;
    dpuClean(l1,bytes);
    const uint32_t old[5]={cb[0x20/4],cb[0x24/4],cb[0x10/4],cb[0x30/4],cb[0x38/4]};
    cb[0x20/4]=(uint32_t)pa;cb[0x24/4]=(uint32_t)(pa>>32);
    cb[0x10/4]=5u|(7u<<15);
    cb[0x30/4]=32u|(1u<<23);
    cb[0x38/4]=0x44;
    __asm__ volatile("dsb sy":::"memory");
    const auto undo=[&](uint32_t why) {
        cb[0x0/4]=oldSctlr;
        __asm__ volatile("dsb sy; isb":::"memory");
        cb[0x20/4]=old[0];cb[0x24/4]=old[1];cb[0x10/4]=old[2];cb[0x30/4]=old[3];cb[0x38/4]=old[4];
        const uint32_t fsr=cb[0x58/4];
        if(fsr&(1u<<30))cb[0x8/4]=1;
        if(fsr&kDpuFaults)cb[0x58/4]=fsr;
        dpuTlbFlush(cb);
        gDpuState=why;
    };
    if(cb[0x20/4]!=(uint32_t)pa || cb[0x30/4]!=(32u|(1u<<23))) {undo(0xdead0003u);return;}
    if(!dpuTlbFlush(cb)) {undo(0xdead0004u);return;}
    cb[0x0/4]=1u|(1u<<5)|(1u<<6);
    __asm__ volatile("dsb sy; isb":::"memory");
    uint32_t fsr=0,us=0;
    for(;us<50000;us+=500) {
        fsr=cb[0x58/4];
        if(fsr&kDpuFaults)break;
        IODelay(500);
    }
    if(fsr&kDpuFaults) {undo(0xdead0005u);return;}
    gDpuTables=l1;gDpuCb=cb;gDpuBank=bank;gDpuState=0x80000000u|bank;
}
static bool dpuStart() {
    if(gDpuTables)return true;
    IOMemoryDescriptor *md=IOMemoryDescriptor::withPhysicalAddress(0x15000000,0xd0000,kIODirectionInOut);
    gDpuSmmuMap=md?md->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
    if(md)md->release();
    if(!gDpuSmmuMap) {printf("PICASSO_DPU smmu map failed\n");return false;}
    gDpuSmmu=(volatile uint32_t *)gDpuSmmuMap->getVirtualAddress();
    const uint32_t idr0=dpuRead(0x20),idr1=dpuRead(0x24);
    const unsigned groups=(idr0&255)>128?128:(idr0&255),banks=idr1&255;
    const unsigned pages=1u<<(((idr1>>28)&7)+1);
    printf("PICASSO_DPU smmu scr0=%x idr0=%x idr1=%x groups=%u banks=%u pages=%u\n",
        dpuRead(0x0),idr0,idr1,groups,banks,pages);
    if(!groups || !banks || banks>128 || pages>128 || (pages+banks)*4096u>0xd0000u) {gDpuState=0xdead0001u;return false;}
    uint32_t bank=0xffffffffu,sctlr=0,cbar=0,s2cr=0,smr=0;
    for(unsigned i=0;i<groups;i++) {
        const uint32_t value=dpuRead(0x800+4*i);
        if(!(value&0x80000000u))continue;
        const uint32_t idLow=value&0x7fff,maskHigh=(value>>16)&0x7fff,maskLow=value&0xffff;
        if(((0xb40u^idLow)&0x7fff&~maskHigh) && ((0xb40u^maskHigh)&0xffff&~maskLow))continue;
        s2cr=dpuRead(0xc00+4*i);bank=s2cr&255;smr=value;
        if(bank>=banks)continue;
        cbar=dpuRead(0x1000+4*bank);sctlr=dpuRead((pages+bank)*4096);
        break;
    }
    printf("PICASSO_DPU stream smr=%x s2cr=%x bank=%u cbar=%x sctlr=%x\n",smr,s2cr,bank,cbar,sctlr);
    if(bank>=banks) {gDpuState=0xdead0001u;return false;}
    if(((s2cr>>16)&3)!=0 || ((cbar>>16)&3)!=1 || (sctlr&1)) {gDpuState=0xdead0001u;return false;}
    dpuEnable(gDpuSmmu+(pages+bank)*(4096/4),bank,sctlr);
    printf("PICASSO_DPU smmu state=%x\n",gDpuState);
    return gDpuTables!=nullptr;
}
static uint32_t dpuMap(unsigned slot,const uint64_t *pages,unsigned count) {
    if(!gDpuTables || slot>=kDpuSlots || count>kDpuSlotBytes/4096)return 0;
    uint64_t *l3=gDpuTables+1024+(size_t)slot*kDpuSlotTables*512;
    const uint64_t attrs=(1ULL<<10)|(1ULL<<6);
    for(unsigned i=0;i<kDpuSlotTables*512;++i) {
        if(i<count && (pages[i]&0xfff))return 0;
        l3[i]=i<count?(pages[i]|attrs|3):0;
    }
    dpuClean(l3,kDpuSlotTables*4096);
    IOLockLock(gDpuLock);
    const bool synced=dpuTlbFlush(gDpuCb);
    IOLockUnlock(gDpuLock);
    return synced?kDpuWindow+slot*kDpuSlotBytes:0;
}
class NativeLegacyDisplay;
static NativeLegacyDisplay *gDirectDisplay=nullptr;
class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
    /* Zero-copy flip (sheng-sde-flip=1): the MDP/SDE register block, mapped once. The wf335 probe
     * found the firmware's scanout on VIG0 (SSPP 0x5000, CTL0 0x2000) with SRC0_ADDR holding the
     * GOP's physical address, i.e. the display reads physical memory directly. */
    IOMemoryMap *sdeMap=nullptr;
    volatile uint32_t *sdeRegs=nullptr;
    /* sheng-sde-selftest=1: a scanout buffer this driver allocates itself, physically contiguous, so the SDE
     * flip can be proven before the compositor's own (scattered) IOSurfaces are solved. Same panel geometry,
     * same stride as the client's surface. */
    IOBufferMemoryDescriptor *sdeShadow=nullptr;
    uint64_t sdeShadowStride=0;
    uint32_t sdeShadowPa=0;
    /* sheng-dpu-scanout=2: the display pipe fetches the compositor's own surface through the
     * display SMMU window. Work-loop state only (kick, latch timer, fence timer). */
    struct DirectSlot {OSObject *surface;IOMemoryDescriptor *memory;uint32_t iova;uint64_t used;};
    static constexpr unsigned kDirectSlots=4;
    DirectSlot directSlots[kDirectSlots]={};
    uint64_t *directPages=nullptr;
    IOTimerEventSource *latchTimer=nullptr;
    const OriginalLegacySwapRequest *latchRequest=nullptr;
    uint64_t latchStart=0,directClock=0;
    int shownSlot=-1,pendingSlot=-1;
    uint32_t shownStride=0;
    bool directOn=false,directBoot=true;
    const OriginalLegacySwapRequest *fenceFor=nullptr;
    uint32_t fenceTarget=0;
    bool fenceCaptured=false;
    IOTimerEventSource *fenceTimer=nullptr;
    uint64_t fenceSince=0;
    unsigned directFlips=0,directMaps=0,directMapFails=0,directLatchTimeouts=0,directFallbacks=0;
    uint64_t directLatchUs=0,directLatchMax=0;
    uint64_t directLastFlip=0,directFenceUs=0,directPeriodUs=0,directLatchLastUs=0;
    bool latchNeedsFinish=false;
    uint32_t bootStride=0;
    // The GOP panel's geometry, fixed from the boot video mode in startDisplay(). This line publishes
    // the full panel height (2400 rows) as the mode, so there are no bars and the surface matches it.
    uint32_t panelWidth=0,panelHeight=0;
    uint32_t scanBudget=4;
    NativeLegacyBootLogo bootLogo;
    bool logoConfigured=false;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // Submission, completion and refresh can kick concurrently. Preserve nested
    // retirement callbacks while serializing queue pumps from other threads.
    IORecursiveLock *kickLock=nullptr;
    bool benchmarked=false;
    // One bounded cached copy of visible pixels, independent of IOSurface lifetime.
    void *copyShadow=nullptr;
    uint64_t copyShadowBytes=0,copyShadowStride=0;
    uintptr_t copyShadowDestination=0;
    uint32_t copyShadowWidth=0,copyShadowHeight=0;
    bool copyShadowValid=false,copyShadowTried=false;
    void resetCopyShadow() {
        if(copyShadow)IOFree(copyShadow,copyShadowBytes);
        copyShadow=nullptr;copyShadowBytes=copyShadowStride=0;
        copyShadowDestination=0;copyShadowWidth=copyShadowHeight=0;
        copyShadowValid=copyShadowTried=false;
    }
    uint64_t copySurface(volatile void *destination,uint64_t destinationBytes,uint64_t destinationStride,
        const void *source,uint64_t sourceBytes,uint64_t sourceStride,uint32_t width,uint32_t height) {
        if(!native_legacy_copy_layout(destination,destinationBytes,destinationStride,
            source,sourceBytes,sourceStride,width,height))return UINT64_MAX;
        if(copyShadowDestination!=(uintptr_t)destination || copyShadowStride!=destinationStride ||
            copyShadowWidth!=width || copyShadowHeight!=height) {
            resetCopyShadow();copyShadowDestination=(uintptr_t)destination;copyShadowStride=destinationStride;
            copyShadowWidth=width;copyShadowHeight=height;
        }
        uint64_t length=(uint64_t)width*height*4;
        if(!copyShadowTried) {
            copyShadowTried=true;
            // 1080x2400 uses 10,368,000 bytes. Never grow this with RAM pressure
            // or retry a failed allocation on every frame.
            if(length<=16ULL*1024*1024 && !(width&1) && !((uintptr_t)destination&7) && !(destinationStride&7)) {
                copyShadow=IOMalloc(length);if(copyShadow)copyShadowBytes=length;
            }
        }
        uint64_t stored=native_legacy_copy_diff(destination,destinationBytes,destinationStride,
            source,sourceBytes,sourceStride,width,height,copyShadow,copyShadowValid);
        if(stored==UINT64_MAX) {
            copyShadowValid=false;
            return native_legacy_copy_pixels(destination,destinationBytes,destinationStride,
                source,sourceBytes,sourceStride,width,height)?length:UINT64_MAX;
        }
        copyShadowValid=true;return stored;
    }
    // Keep the original swap and IOSurface owned until GPU completion. Merely
    // retaining the descriptor does not stop the producer reusing its pixels.
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
    uint64_t gpuLength=0;
    bool gpuBypass=false;
    // Offer tick of the in-flight GPU frame, for picasso_frame_trace().
    uint64_t gpuStartTick=0;
    // Preserve every accepted request until the original family retires it.
    // A full table rejects the new request without evicting a live entry.
    const OriginalLegacySwapRequest *validated[8]={};
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {for(auto &entry: validated)if(entry==request)entry=nullptr;}
    bool addValidated(const OriginalLegacySwapRequest *request) {
        if(isValidated(request))return true;
        for(auto &entry: validated)if(!entry){entry=request;return true;}
        return false;
    }
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue at +5592 never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer=nullptr;
    uint64_t vsyncInterval=0,vsyncNext=0;
    unsigned vsyncHz=0,pumpTicks=0;
    bool vsyncArmed=false;
    /* wf131 measured the compositor kicking the swap queue three times with the client queue
     * at +5592 still empty every time, so the tick that stands in for the absent panel
     * subclass never armed and userspace never received a vsync. The kick itself proves a
     * display client exists, and the original sender walks the client queue (a no-op when it
     * is empty), so treat a kick as "wanted" for the rest of the boot: wf132. */
    bool vsyncKicked=false;
    bool vsyncWanted() const {
        const void *head=nullptr;memcpy(&head,reinterpret_cast<const unsigned char *>(this)+5592,sizeof(head));return head;
    }
    bool vsyncActive() const { return vsyncWanted() || vsyncKicked; }
    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync() {
        /* wf131: the tick only arms for a registered vsync client; print the reason the first
         * few times so "no vsync ever" is distinguishable from "vsync client never registered". */
        static unsigned armPrints;
        if(armPrints<8){++armPrints;printf("PICASSO_DISPLAY vsync: arm timer=%p armed=%d wanted=%d kicked=%d\n",vsyncTimer,int(vsyncArmed),int(vsyncWanted()),int(vsyncKicked));}
        if(!vsyncTimer || vsyncArmed || !vsyncActive())return;
        vsyncArmed=true;vsyncNext=mach_absolute_time()+vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);
    }
    static void vsyncFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();}
    void vsyncTick() {
        vsyncArmed=false;
        if(!vsyncTimer)return;
        if(!vsyncActive()){native_legacy_vsync_observed(2,vsyncHz);return;}
        const uint64_t now=mach_absolute_time();
        native_legacy_vsync_notify(this,now,mach_continuous_time(),vsyncInterval);
        native_legacy_vsync_observed(1,vsyncHz);
        // A lost submission kick must not require another user frame to recover.
        // Pump once a second; the presenter owns timeout and completion identity.
        if(vsyncHz && (++pumpTicks%vsyncHz)==0)kickSwapQueue();
        vsyncNext+=vsyncInterval;
        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!hz || hz>240 || !primary || !bootMapping || vsyncTimer)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        nanoseconds_to_absolutetime(1000000000ull/hz,&vsyncInterval);
        vsyncHz=hz;vsyncTimer=timer;native_legacy_vsync_observed(4,hz);
        // A client may already have registered: arm under the gate, like the registration does.
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(!vsyncTimer)return;
        vsyncTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
public:
    /* Copy the surface on screen into the boot memory so the CDC snapshot (which reads the GOP
     * mapping) shows what the panel shows while direct scanout owns the pipe. */
    void directSnapshot() {
        if(!directOn || directBoot || shownSlot<0 || !bootMapping || !shownStride)return;
        IOMemoryDescriptor *source=directSlots[shownSlot].memory;
        PE_Video video={};uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        if(!source || PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress() ||
           video.v_width!=width || video.v_height!=height)return;
        uint8_t *out=(uint8_t *)bootMapping->getVirtualAddress();
        for(uint32_t y=0;y<height && uint64_t(y+1)*video.v_rowBytes<=bootMapping->getLength();++y)
            if(source->readBytes(uint64_t(y)*shownStride,out+uint64_t(y)*video.v_rowBytes,uint64_t(width)*4)!=uint64_t(width)*4)return;
        __asm__ volatile("dsb sy":::"memory");
    }
    // Called by the GPU client's completion (any thread): the swap finishes on the framebuffer's work loop, the
    // loop the swap queue and the vsync clients live on. The family's command gate is not reachable from this
    // subclass, so the state transitions are made idempotent instead: every path out of finishGpuSwap clears
    // transferred (a latch left set with gpuRequest cleared stopped every later kick and froze the compositor).
    void gpuComplete(int status) {
        struct Args {NativeLegacyDisplay *display;int status;};
        Args args={this,status};
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=(Args *)a0;
            if(!a->display->kickLock)return kIOReturnNotReady;
            IORecursiveLockLock(a->display->kickLock);
            a->display->finishGpuSwap(a->status);
            IORecursiveLockUnlock(a->display->kickLock);
            return kIOReturnSuccess;
        };
        // runAction is synchronous: the stack arguments stay live until the
        // framebuffer gate is acquired. Never mutate the swap queue ungated
        // on an allocation failure or a teardown callback.
        if(loop && loop->runAction(action,this,&args)==kIOReturnSuccess)return;
        native_legacy_retirement_failed(this);
    }
    void free() override {stopVsync();stopDirect();resetCopyShadow();OSSafeReleaseNULL(sdeShadow);sdeShadowStride=0;OSSafeReleaseNULL(sdeMap);sdeRegs=nullptr;if(kickLock){IORecursiveLockFree(kickLock);kickLock=nullptr;}if(logicalPM){PMstop();logicalPM=false;}primary=false;OSSafeReleaseNULL(bootMapping);IOMobileFramebufferLegacy::free();}
    void stop(IOService *provider) override {
        stopVsync();
        stopDirect();
        resetCopyShadow();
        transferred=false;gpuRequest=nullptr;gpuStartTick=0;for(auto &entry: validated)entry=nullptr;
        if(logicalPM){PMstop();logicalPM=false;}
        primary=false;IOMobileFramebufferLegacy::stop(provider);OSSafeReleaseNULL(bootMapping);
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840,
    // the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part of this
    // port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is the
    // original state setter (virtual 354, the one the original code calls with this same request shape) with only the powered word named.
    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this,&request,false);
    }
    static uint32_t sdeRead(volatile uint32_t *base,uint32_t offset) { return base[offset/4]; }
    // sheng-sde-probe=1: read the SDE (display controller) register blocks that describe the firmware's
    // scanout, so a direct flip (zero copy) can be built from the registers the boot loader actually
    // programmed instead of the vendor driver's layout. Read-only; the MDP is powered while the panel scans.
    void sdeProbe() {
        static int enabled=-1;
        if(enabled<0) { uint32_t value=0; enabled=(PE_parse_boot_argn("sheng-sde-probe",&value,sizeof value) && value)?1:0; }
        if(enabled!=1 || !bootMapping)return;
        const uint64_t gop=bootMapping->getPhysicalAddress();
        const uint32_t gopLo=(uint32_t)gop,gopHi=(uint32_t)(gop>>32);
        printf("PICASSO_SDE gop_pa=%llx len=%llu panel=%ux%u\n",(unsigned long long)gop,
            (unsigned long long)bootMapping->getLength(),panelWidth,panelHeight);
        IOMemoryDescriptor *mdp=IOMemoryDescriptor::withPhysicalAddress(0xae00000,0x90000,kIODirectionInOut);
        IOMemoryMap *map=mdp?mdp->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        if(mdp)mdp->release();
        if(!map || !map->getVirtualAddress()) {OSSafeReleaseNULL(map);printf("PICASSO_SDE mdp map failed\n");return;}
        volatile uint32_t *m=(volatile uint32_t *)map->getVirtualAddress();
        printf("PICASSO_SDE version=%08x top",sdeRead(m,0));
        for(uint32_t o=0;o<0x40;o+=4)printf(" %x",sdeRead(m,0x1000+o));
        printf("\n");
        const uint32_t sspp[5]={0x5000,0x7000,0x25000,0x27000,0x29000};
        static const uint32_t ssppOffs[12]={0x0,0x4,0x8,0xc,0x10,0x14,0x18,0x1c,0x30,0x34,0x38,0x3c};
        for(unsigned i=0;i<5;i++) {
            printf("PICASSO_SDE sspp%u",i);
            for(unsigned k=0;k<12;k++) {
                const uint32_t value=sdeRead(m,sspp[i]+ssppOffs[k]);
                printf(" %x",value);
                if(value==gopLo || value==gopHi)printf("(gop)");
            }
            printf("\n");
        }
        const uint32_t ctl[4]={0x2000,0x2200,0x2400,0x2600};
        for(unsigned i=0;i<4;i++) {
            printf("PICASSO_SDE ctl%u",i);
            for(uint32_t o=0;o<=0x30;o+=4)printf(" %x",sdeRead(m,ctl[i]+o));
            printf("\n");
        }
        printf("PICASSO_SDE mixer0");
        for(uint32_t o=0;o<=0x40;o+=4)printf(" %x",sdeRead(m,0x45000+o));
        printf("\n");
        printf("PICASSO_SDE pp0");
        for(uint32_t o=0;o<=0x30;o+=4)printf(" %x",sdeRead(m,0x71000+o));
        printf("\n");
        printf("PICASSO_SDE intf1");
        for(uint32_t o=0;o<=0x40;o+=4)printf(" %x",sdeRead(m,0x6b800+o));
        printf("\n");
        map->release();
        IOMemoryDescriptor *vbif=IOMemoryDescriptor::withPhysicalAddress(0xaeb0000,0x1040,kIODirectionInOut);
        IOMemoryMap *vmap=vbif?vbif->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        if(vbif)vbif->release();
        if(vmap && vmap->getVirtualAddress()) {
            volatile uint32_t *v=(volatile uint32_t *)vmap->getVirtualAddress();
            printf("PICASSO_SDE vbif");
            for(uint32_t o=0;o<=0x40;o+=4)printf(" %x",sdeRead(v,o));
            printf("\n");
        }
        OSSafeReleaseNULL(vmap);
    }
    bool start(IOService *provider) override {
        if(!startDisplay(provider))return false;
        startVsync();sdeProbe();sdeDiag();startDirect();return true;
    }
    bool startDisplay(IOService *provider) {
        if(!IOMobileFramebufferLegacy::start(provider))return false;
        if(!kickLock)kickLock=IORecursiveLockAlloc();
        if(!kickLock){IOMobileFramebufferLegacy::stop(provider);return false;}
        if(primary && bootMapping)announcePoweredByFirmware();
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
        if(enabled!=1 || phase!=4 || !provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        PMinit();logicalPM=initialized;
        if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;}
        provider->joinPMtree(this);
        IOReturn status=registerPowerDriver(this,states,2);native_legacy_pm_observed(1,status,0);
        if(status==kIOReturnSuccess){status=changePowerStateToPriv(1);native_legacy_pm_observed(2,status,1);}
        if(status!=kIOReturnSuccess){PMstop();logicalPM=false;IOMobileFramebufferLegacy::stop(provider);return false;}
        return true;
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        if(!logicalPM)return IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(ordinal>1 || !bootMapping){native_legacy_pm_observed(3,kIOReturnUnsupported,ordinal);return kIOReturnUnsupported;}
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        IOReturn status=IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        native_legacy_pm_observed(4,status,ordinal);return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        // Nothing in the family publishes IOMFBDisplayRefresh (its platform subclass does), and without it the render server keeps its own
        // 1/60 s heartbeat (int-210: refresh=0). Publish it before any client can find the display, only when sheng-vsync asks for more.
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(primary && hz>60 && hz<=240)native_legacy_set_refresh_properties(this);
        IOService::registerService(options);native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override {return primary;}
    // The render server paces its display timer by the minimum refresh interval in IOMFBDisplayRefresh (int-209: no vsync client ever
    // registered, frames came every 16.7 ms), and the generic slot 301 fixes it at 1/60 s. With sheng-vsync=<Hz> it reports the panel's
    // own rate instead; the maximum interval and the step keep the original values. Words in 32.32 seconds, then their mach times.
    // The original maximum interval is 1/24 s (int-240), which offers the render server a 24 to sheng-vsync Hz range. The panel has no
    // variable refresh, so sheng-vsync-fixed=1 reports the maximum interval equal to the minimum: one fixed rate.
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(hz<=60 || hz>240)return true;
        unsigned locked=0;PE_parse_boot_argn("sheng-vsync-fixed",&locked,sizeof(locked));
        const uint64_t interval=(1ull<<32)/hz;
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        if(fixed || locked==1){words[2]=interval;words[3]=machTime;}
        native_legacy_vsync_observed(5,hz);
        return true;
    }
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking=false,rekick=false;
    void kickSwapQueue() override {
        if(!kickLock)return;
        IORecursiveLockLock(kickLock);
        kickSwapLocked();
        IORecursiveLockUnlock(kickLock);
    }
    void kickSwapLocked() {
        /* wf131: whether the compositor's queued swap ever reaches the kick, and whether a
         * vsync client is registered at that moment. wf132: the kick also establishes that a
         * display client exists, which is what the tick needs to run. */
        static unsigned kickPrints;
        if(kickPrints<8){++kickPrints;printf("PICASSO_DISPLAY vsync: kick wanted=%d armed=%d\n",int(vsyncWanted()),int(vsyncArmed));}
        vsyncKicked=true;
        armVsync();
        if(kicking){rekick=true;return;}
        kicking=true;
        do {
            rekick=false;
            kickOnce();
            gpuBypass=false;
        } while(rekick && !transferred);
        kicking=false;
    }
    void kickOnce() {
        // The GPU client owns one frame until it completes it; the presenter's flip timeout normally takes a
        // stalled frame back. If that timer was never created (no work loop) or the completion never reached this
        // driver, do not wait forever: take the frame back here and use the CPU path for this kick.
        if(transferred && gpuStartTick) {
            uint64_t held=0;absolutetime_to_nanoseconds(mach_absolute_time()-gpuStartTick,&held);
            if(held>=1500000000ull) {
                gpuRequest=nullptr;gpuLength=0;transferred=false;gpuBypass=true;gpuStartTick=0;
                native_legacy_gpu_fallback_observed(kIOReturnTimeout);
            }
        }
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request))return;
        if(!logoConfigured) {
            unsigned hold=0;PE_parse_boot_argn("picasso-boot-logo-hold",&hold,sizeof(hold));
            bootLogo.enabled=hold==1;logoConfigured=true;
        }
        uint64_t nowNs=0;absolutetime_to_nanoseconds(mach_absolute_time(),&nowNs);
        uint64_t nowMs=nowNs/1000000;
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // This backend accepts synchronous CPU surfaces only. No GPU fence
        // completion or panel vblank is claimed.
        OSObject *surface=nullptr;
        memcpy(&surface,reinterpret_cast<const unsigned char *>(request)+2624,sizeof(surface));
        if(!surface) {
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank={};
            uint32_t bw=0,bh=0;getDisplayDimensions(&bw,&bh);
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            if(bootLogo.holdBlank(nowMs)) {
                // Consume this validated blank without erasing the existing
                // boot logo. Retire normally so the compositor keeps moving.
                transferred=true;
                native_legacy_frame_observed(2,UINT64_MAX,0,0,0,0);
                native_legacy_transfer_observed(kIOReturnSuccess,request,0);
                finishTransfer(request);
                return;
            }
            directToBoot();
            copyShadowValid=false;
            if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),blank.v_rowBytes,bw,bh,0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            copyShadowValid=false;   // the GOP is solid black now; the shadow no longer describes the panel
            transferred=true;
            native_legacy_frame_observed(1,0,0,0,0,0);
            native_legacy_transfer_observed(kIOReturnSuccess,request,blankLength);
            finishTransfer(request);
            return;
        }
        if(!surface->metaCast("IOSurface"))return;
        IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
        PE_Video video={};
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint64_t sourceStride=native_legacy_surface_stride(surface);
        uint64_t allocation=native_legacy_surface_allocation(surface);
        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !native_legacy_surface_bounds(width,height,sourceStride,allocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !picasso_gop_format(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) ||
           PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress() ||
           video.v_width!=width || video.v_height!=height || video.v_depth!=32 || video.v_rotate ||
           !native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        // The diagnostic renderer repaints the page over the scanout until it
        // is told the system owns the display; every accepted frame needs that,
        // GPU-copied or not (int-093). A failed prepare is retried next kick.
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
        }
        /* sheng-dpu-scanout=2: the display fetches the compositor's surface itself. The swap waits
         * for the GPU to finish the frame (the compositor submits when its command buffer is
         * scheduled), flips, and retires at once; the pipe's latch completes asynchronously and
         * only gates the next flip, so the compositor can commit on the next vsync instead of
         * waiting for the latch. */
        if(directOn && !gpuBypass && native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height) {
            if(latchRequest && !(sdeRegs[0x2000/4+0x18/4]&1u)) completeLatch();
            if(latchRequest) {transferred=true;return;}
            {
                const uint64_t fenceStart=mach_absolute_time();
                if(!gpuWrote(request)) {transferred=true;return;}
                uint64_t fenceNs=0;absolutetime_to_nanoseconds(mach_absolute_time()-fenceStart,&fenceNs);
                const int slot=directSlot(surface,memory);
                if(slot>=0) {
                    sdeFlipTo(directSlots[slot].iova,(uint32_t)sourceStride);
                    shownStride=(uint32_t)sourceStride;
                    directBoot=false;pendingSlot=slot;latchRequest=request;
                    latchStart=mach_absolute_time();latchTimer->setTimeoutUS(500);
                    ++directFlips;directFenceUs=fenceNs/1000;
                    const uint64_t now=mach_absolute_time();
                    if(directLastFlip){uint64_t ns=0;absolutetime_to_nanoseconds(now-directLastFlip,&ns);directPeriodUs=ns/1000;}
                    directLastFlip=now;
                    native_legacy_swap_stage(1);
                    native_legacy_frame_observed(3,0,0,0,0,0);
                    /* Early retire: the surface on screen stays mapped in its slot and the
                     * compositor's other drawables are free, so the next frame may commit now. */
                    if(retireSwap(request,true)) {transferred=false;native_legacy_early_retires=native_legacy_early_retires+1;}
                    else {transferred=true;latchNeedsFinish=true;}
                    if((directFlips%30)==0)directFrameStats();
                    if((directFlips%120)==0)directStats();
                    if(!transferred)kickSwapQueue();
                    return;
                }
                ++directFallbacks;
            }
        }
        directToBoot();
        /* sheng-sde-flip=1: zero-copy scanout. The display controller reads the compositor's own
         * surface (the same physical-address model the boot loader uses), so the GPU present/copy,
         * its commit and its fence wait all leave the frame. Falls through to the copy paths when the
         * surface is not an exact, contiguous panel-sized buffer. */
        unsigned flip=0;PE_parse_boot_argn("sheng-sde-flip",&flip,sizeof flip);
        if(flip==1 && sdeFlip(memory,width,height,sourceStride)) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            copyShadowValid=false;
            native_legacy_swap_stage(3);
            native_legacy_frame_observed(3,0,0,0,0,0);
            native_legacy_transfer_observed(kIOReturnSuccess,request,0);
            unsigned early=1;PE_parse_boot_argn("sheng-early-retire",&early,sizeof early);
            if(early==1 && retireSwap(request,true)) native_legacy_early_retires=native_legacy_early_retires+1;
            return;
        }
        /* sheng-sde-selftest=1: the same flip against a contiguous buffer this driver allocated itself
         * (see sdeSelfTest), to prove the SDE path while the compositor's surfaces are still scattered. */
        unsigned selftest=0;PE_parse_boot_argn("sheng-sde-selftest",&selftest,sizeof selftest);
        if(selftest==1 && sdeSelfTest(memory,width,height,sourceStride)) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            copyShadowValid=false;
            native_legacy_swap_stage(3);
            native_legacy_frame_observed(3,0,0,0,0,0);
            native_legacy_transfer_observed(kIOReturnSuccess,request,0);
            unsigned early=1;PE_parse_boot_argn("sheng-early-retire",&early,sizeof early);
            if(early==1 && retireSwap(request,true)) native_legacy_early_retires=native_legacy_early_retires+1;
            return;
        }
        if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height,0)) {
            // The GPU writes the scanout memory behind the CPU's back: the
            // differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            copyShadowValid=false;gpuRequest=request;gpuLength=length;transferred=true;
            gpuStartTick=mach_absolute_time();
            // The completion callback retires only after the consumer has
            // finished reading. Failed imports/copies retry this request on CPU.
            return;
        }
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
        if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        // Continue scanning only through the bounded logo phase. Every copied
        // frame is counted below, even when its pixels are no longer scanned.
        uint64_t scan[5]={UINT64_MAX,0,0,0,0};
        bool scanned=false;
        if(scanBudget || bootLogo.needsScan(nowMs)) {
            if(scanBudget)--scanBudget;
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan)) {
                scanned=true;
            }
        }
        /* offline-legacy-bench=2: on the first real frame, time reads of the swap's own source memory through the
         * swap's mapping and through a copyback mapping of the same descriptor, each twice (cold and warm), plus a
         * cached control buffer and a store to the real destination. Together with the frame copy that follows, this
         * separates source-read cost, attribute choice and scheduling delays from destination store cost. */
        unsigned bench=0;PE_parse_boot_argn("offline-legacy-bench",&bench,sizeof bench);
        if(bench>=2 && !benchmarked) {
            benchmarked=true;
            const uint64_t benchBytes=1u<<20,benchWords=benchBytes/8;
            const volatile uint64_t *srcWords=(const volatile uint64_t *)source->getVirtualAddress();
            volatile uint64_t *fb=(volatile uint64_t *)bootMapping->getVirtualAddress();
            IOMemoryMap *alt=memory->map(kIOMapAnywhere|kIOMapReadOnly|kIOMapCopybackCache);
            uint64_t *ctrl=(uint64_t *)IOMalloc(benchBytes);
            uint64_t t0=0,t1=0,sum=0;
            if(ctrl)for(uint64_t i=0;i<benchWords;++i)ctrl[i]=i*0x9e3779b97f4a7c15ull;
            for(unsigned pass=0;pass<2;++pass) {
                sum=0;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(t0)::"memory");
                for(uint64_t i=0;i<benchWords;++i)sum+=srcWords[i];
                __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t1)::"memory");
                printf("PICASSO_DISPLAY bench2 src pass=%u ticks=%llu sum=%llx opts=%x\n",pass,t1-t0,sum,(unsigned)source->getMapOptions());
            }
            if(alt && alt->getVirtualAddress()) {
                const volatile uint64_t *altWords=(const volatile uint64_t *)alt->getVirtualAddress();
                for(unsigned pass=0;pass<2;++pass) {
                    sum=0;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(t0)::"memory");
                    for(uint64_t i=0;i<benchWords;++i)sum+=altWords[i];
                    __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t1)::"memory");
                    printf("PICASSO_DISPLAY bench2 alt pass=%u ticks=%llu sum=%llx opts=%x\n",pass,t1-t0,sum,(unsigned)alt->getMapOptions());
                }
            } else printf("PICASSO_DISPLAY bench2 alt unavailable\n");
            if(ctrl) {
                __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(t0)::"memory");
                for(uint64_t i=0;i<benchWords;++i)ctrl[i]=i*0xd1b54a32d192ed03ull;
                __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t1)::"memory");
                printf("PICASSO_DISPLAY bench2 ctrlstore ticks=%llu\n",t1-t0);
                sum=0;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(t0)::"memory");
                for(uint64_t i=0;i<benchWords;++i)sum+=ctrl[i];
                __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t1)::"memory");
                printf("PICASSO_DISPLAY bench2 ctrlread ticks=%llu sum=%llx\n",t1-t0,sum);
            }
            __asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(t0)::"memory");
            for(uint64_t i=0;i<benchWords;++i)fb[i]=0xdeadbeef00000000ull|i;
            __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t1)::"memory");
            printf("PICASSO_DISPLAY bench2 fbstore ticks=%llu\n",t1-t0);
            OSSafeReleaseNULL(alt);if(ctrl)IOFree(ctrl,benchBytes);
        }
        uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
        uint64_t stored=copySurface(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height);
        if(stored==UINT64_MAX) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,stored);}
        OSSafeReleaseNULL(source);transferred=true;
        if(scanned) {
            bool held=bootLogo.holding;
            bootLogo.observe(nowMs,width,height,scan);
            if(held!=bootLogo.holding)
                IOLog("PICASSO_DISPLAY boot_logo holding=%u time_ms=%llu\n",bootLogo.holding,nowMs);
        }
        native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        // A frame finished on the CPU path: no GPU offer was accepted, so gpuStartTick is 0 and
        // picasso_frame_trace() emits nothing (a CPU-served frame has no offer/take/completion split).
        picasso_frame_trace(gpuStartTick);
        finishTransfer(request);
    }
    // Hands the pending head to the unchanged family after the CPU copy.
    bool retireSwap(const OriginalLegacySwapRequest *request,bool force=false) {
        unsigned retire=0,cpuPresent=0;
        PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
        PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
        if(!(retire==1 || cpuPresent==1 || force))return false;
        if(!activateOnlyPendingSwap(request))return false;
        removeValidated(request);
        IOMobileFramebufferLegacy::completeCurrentSwap();
        native_legacy_retirement_observed(this);
        return true;
    }
    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    void finishTransfer(const OriginalLegacySwapRequest *request,bool force=false) {
        // Only after the real copy and store barrier: select the actual pending head and let the unchanged family release it and wake
        // waiters. This reports memory scanout completion, not a panel vblank or hardware fence.
        if(!retireSwap(request,force)){native_legacy_retirement_failed(this);transferred=false;return;}
        // The unchanged family has retired this generation and released its current reference. Permit the next independently validated
        // single pending frame. Keep the latch set while completion can reenter this method, and whenever retirement remains disabled.
        transferred=false;
        kickSwapQueue();
    }
    // The GPU client reported its copy (status 0) or lost the frame. Success
    // retires the swap like a finished CPU copy; anything else reinstates the
    // CPU copy for the next kick.
    void finishGpuSwap(int status) {
        const OriginalLegacySwapRequest *request=gpuRequest;
        const uint64_t offer=gpuStartTick;
        gpuStartTick=0;
        if(!request)return;
        gpuRequest=nullptr;
        if(status==0) {
            native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            picasso_frame_trace(offer);
            finishTransfer(request,true);return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred=false;gpuBypass=true;kickSwapQueue();
    }
    IOReturn collectCompletionReports(uint32_t,void *,uint32_t *count,uint32_t) override {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if(count)*count=0;
        return kIOReturnUnsupported;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override {
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes=reinterpret_cast<const unsigned char *>(request);
        auto word=[&](unsigned offset){uint32_t value=0;memcpy(&value,bytes+offset,4);return value;};
        OSObject *surface=nullptr;unsigned reason=1;bool accepted=false;
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                uint32_t rects[2][4]={};
                for(unsigned i=0;i<2;++i) {
                    uint32_t *rect=rects[i];memcpy(rect,bytes+rectOffsets[i],sizeof(rects[i]));
                    // iOS 16 CoreAnimation leaves a full-screen layer's rectangles zero.
                    bool unset=!rect[0] && !rect[1] && !rect[2] && !rect[3];
                    rectangles=rectangles && (unset || (rect[0]==0 && rect[1]==0 && rect[2]==width && rect[3]==height));
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                // The same userland leaves the destination size zero as well.
                bool size=(word(816)==width && word(820)==height) || (!word(816) && !word(820));
                native_legacy_swap_geometry_observed(rects[0],rects[1],word(804),word(808),word(812),word(816),word(820));
                if(width && height && rectangles && options && size) {
                    reason=4;
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied.
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       picasso_gop_format(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,native_legacy_surface_stride(surface),
                           native_legacy_surface_allocation(surface),memory->getLength())) {
                        reason=0;accepted=true;
                    } else {
                        native_legacy_surface_rejected(native_legacy_surface_width(surface),native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface),native_legacy_surface_planes(surface),native_legacy_surface_stride(surface),
                            native_legacy_surface_allocation(surface),memory?memory->getLength():0);
                    }
                }
            }
        }
        if(!accepted && reason==2 && !surface && enabled==1) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4]={};memcpy(none,bytes+2624,sizeof(none));
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
            if(!none[0] && !none[1] && !none[2] && !none[3] && width && height && options &&
               word(816)==width && word(820)==height && native_legacy_blank_state(word(792),word(796),word(800))) {
                reason=5;accepted=true;
            }
        }
        if(accepted && !addValidated(request)){accepted=false;reason=6;}
        if(!accepted)removeValidated(request);
        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override {
        if(!pitch)return kIOReturnBadArgument;
        *pitch=0;IOService *provider=getProvider();if(!provider)return kIOReturnNotReady;
        OSObject *property=provider->getProperty("dot-pitch");
        if(!property)return kIOReturnNotFound;
        OSData *data=OSDynamicCast(OSData,property);
        if(!data || data->getLength()!=sizeof(*pitch))return kIOReturnBadArgument;
        memcpy(pitch,data->getBytesNoCopy(),sizeof(*pitch));return kIOReturnSuccess;
    }
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    IOReturn supportedFrameInfo(uint32_t,uint32_t *count,char *key,uint32_t *mask) override {
        if(!count || !key || !mask)return kIOReturnBadArgument;
        *count=0;*mask=0;bzero(key,64);return kIOReturnUnsupported;
    }
    IOReturn newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) override {
        native_legacy_client_enter(task,type);
        IOReturn status=IOMobileFramebufferLegacy::newUserClient(task,security,type,client);
        native_legacy_client_observed(status,status==kIOReturnSuccess && client?*client:nullptr);return status;
    }
    bool sdeEnsureMapped() {
        if(sdeRegs)return true;
        IOMemoryDescriptor *mdp=IOMemoryDescriptor::withPhysicalAddress(0xae00000,0x90000,kIODirectionInOut);
        if(!mdp)return false;
        IOMemoryMap *map=mdp->map(kIOMapAnywhere|kIOMapInhibitCache);
        mdp->release();
        if(!map)return false;
        if(!map->getVirtualAddress()){map->release();return false;}
        sdeMap=map;sdeRegs=(volatile uint32_t *)map->getVirtualAddress();
        return true;
    }
    /* Program the active pipe (VIG0 = SSPP 0x5000, CTL0 = 0x2000; the boot loader's own scanout, found
     * by the wf335 probe): same geometry, new base and stride, one CTL flush. The hardware latches the
     * update at the next frame boundary. */
    void sdeProgram(uint32_t pa,uint32_t width,uint32_t height,uint64_t stride) {
        const uint32_t base=0x5000;
        sdeRegs[base/4+0x00/4]=((uint32_t)height<<16)|width;   /* SSPP_SRC_SIZE */
        sdeRegs[base/4+0x0c/4]=((uint32_t)height<<16)|width;   /* SSPP_OUT_SIZE */
        sdeRegs[base/4+0x14/4]=pa;                             /* SSPP_SRC0_ADDR */
        sdeRegs[base/4+0x24/4]=(uint32_t)stride;               /* SSPP_SRC_YSTRIDE0 */
        __asm__ volatile("dsb st":::"memory");
        sdeRegs[0x2000/4+0x18/4]=1u;                           /* CTL_FLUSH, VIG0 */
    }
    /* How the client's surface is laid out, once: descriptor class/length and the physical run structure of
     * the first frames. The SDE reads one linear range, so a fully scattered 4 KB surface cannot be scanned;
     * the longest run says whether anything larger is available. */
    void sdeDescribeSurface(IOMemoryDescriptor *memory,uint64_t needed) {
        static unsigned prints;
        if(prints>=2)return;
        ++prints;
        const OSMetaClass *meta=memory->getMetaClass();
        printf("PICASSO_SDE surface class=%s len=%llu needed=%llu\n",
            meta?meta->getClassName():"?",(unsigned long long)memory->getLength(),(unsigned long long)needed);
        uint64_t offset=0,longest=0,longestAt=0;unsigned runs=0,pages=0;
        while(offset<needed && pages<2700) {
            IOByteCount segment=0;
            const addr64_t pa=memory->getPhysicalSegment(offset,&segment,0);
            if(!pa || !segment)break;
            if(segment>longest){longest=segment;longestAt=offset;}
            offset+=segment;++runs;++pages;
        }
        printf("PICASSO_SDE surface runs=%u bytes=%llu longest=%llu at=%llu\n",
            runs,(unsigned long long)offset,(unsigned long long)longest,(unsigned long long)longestAt);
    }
    bool sdeFlip(IOMemoryDescriptor *memory,uint32_t width,uint32_t height,uint64_t stride) {
        if(!sdeEnsureMapped() || !primary || !bootMapping)return false;
        if(width!=panelWidth || height!=panelHeight) {
            static unsigned sizePrints;
            if(sizePrints<4){++sizePrints;printf("PICASSO_SDE flip size %ux%u panel %ux%u\n",width,height,panelWidth,panelHeight);}
            return false;
        }
        if((stride&3) || stride<(uint64_t)width*4)return false;
        const uint64_t needed=(uint64_t)(height-1)*stride+(uint64_t)width*4;
        IOByteCount segment=0;
        const addr64_t pa=memory->getPhysicalSegment(0,&segment,0);
        if(!pa || segment<needed || pa>(addr64_t)0xffffffffu || pa+needed<(addr64_t)pa) {
            static unsigned skipPrints;
            if(skipPrints<8){++skipPrints;printf("PICASSO_SDE flip skip pa=%llx seg=%llu needed=%llu\n",(unsigned long long)pa,(unsigned long long)segment,(unsigned long long)needed);}
            sdeDescribeSurface(memory,needed);
            return false;
        }
        {
            static unsigned flipPrints;
            if(flipPrints<8){++flipPrints;printf("PICASSO_SDE flip pa=%llx stride=%llu %ux%u\n",(unsigned long long)pa,(unsigned long long)stride,width,height);}
        }
        sdeProgram((uint32_t)pa,width,height,stride);
        return true;
    }
    /* sheng-sde-selftest=1: copy the client's frame into a scanout buffer this driver allocated itself,
     * physically contiguous, and point the SDE at that buffer. The copy is the same 10.4 MB copy the GOP
     * path does; the point is that the SDE now scans memory this driver owns, which proves the flip's
     * address translation, format and stride without needing the compositor's surface to be contiguous. */
    bool sdeSelfTest(IOMemoryDescriptor *memory,uint32_t width,uint32_t height,uint64_t stride) {
        if(!sdeEnsureMapped() || !primary || !bootMapping)return false;
        if(width!=panelWidth || height!=panelHeight)return false;
        if((stride&3) || stride<(uint64_t)width*4)return false;
        const uint64_t needed=(uint64_t)(height-1)*stride+(uint64_t)width*4;
        if(!sdeShadow) {
            /* cpm_allocate finds the run in the free lists; keep the first one that fits the SDE's 32-bit
             * address register (retry a few times, releasing buffers that land above 4 GB). */
            for(unsigned attempt=0;attempt<6 && !sdeShadow;++attempt) {
                IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withOptions(
                    (IOOptionBits)kIOMemoryPhysicallyContiguous|(IOOptionBits)kIODirectionInOut,needed,4096);
                if(!buffer) {printf("PICASSO_SDE selftest alloc failed attempt=%u\n",attempt);continue;}
                IOByteCount segment=0;
                const addr64_t pa=buffer->getPhysicalSegment(0,&segment,0);
                if(pa && segment>=needed && pa+needed<=(addr64_t)0xffffffffu) {
                    sdeShadow=buffer;sdeShadowStride=stride;sdeShadowPa=(uint32_t)pa;
                    printf("PICASSO_SDE selftest buffer pa=%llx len=%llu stride=%llu\n",
                        (unsigned long long)pa,(unsigned long long)buffer->getLength(),(unsigned long long)stride);
                } else {
                    printf("PICASSO_SDE selftest reject pa=%llx seg=%llu\n",(unsigned long long)pa,(unsigned long long)segment);
                    buffer->release();
                }
            }
            if(!sdeShadow) {printf("PICASSO_SDE selftest no contiguous buffer\n");return false;}
        }
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
        if(!source || !source->getVirtualAddress() || source->getLength()<needed) {OSSafeReleaseNULL(source);return false;}
        memcpy(sdeShadow->getBytesNoCopy(),reinterpret_cast<const void *>(source->getVirtualAddress()),(size_t)needed);
        source->release();
        __asm__ volatile("dsb sy":::"memory");
        sdeProgram(sdeShadowPa,width,height,stride);
        return true;
    }
    /* sheng-sde-probe=1 also runs these two capability checks, once, at display start:
     *  - can the kernel hand out a physically contiguous scanout-sized buffer (the flip's fallback)?
     *  - what is the apps SMMU (0x15000000) doing with the display's stream 0xb40: does the SDE read
     *    physical addresses, or IOVAs translated by this SMMU? */
    void sdeDiag() {
        static int enabled=-1;
        if(enabled<0) { uint32_t value=0; enabled=(PE_parse_boot_argn("sheng-sde-probe",&value,sizeof value) && value)?1:0; }
        if(enabled!=1)return;
        const vm_size_t sizes[3]={1u<<20,12u<<20,64u<<20};
        for(unsigned i=0;i<3;i++) {
            IOBufferMemoryDescriptor *buffer=IOBufferMemoryDescriptor::withOptions(
                (IOOptionBits)kIOMemoryPhysicallyContiguous|(IOOptionBits)kIODirectionInOut,sizes[i],4096);
            if(!buffer) {printf("PICASSO_SDE contig %llu failed\n",(unsigned long long)sizes[i]);continue;}
            IOByteCount segment=0;
            const addr64_t pa=buffer->getPhysicalSegment(0,&segment,0);
            printf("PICASSO_SDE contig %llu ok pa=%llx seg=%llu len=%llu\n",
                (unsigned long long)sizes[i],(unsigned long long)pa,
                (unsigned long long)segment,(unsigned long long)buffer->getLength());
            buffer->release();
        }
        /* The apps SMMU, read-only: which stream-match entry covers the display's stream (DT: SID 0xb40,
         * mask 0x402), which context bank it uses, and whether that bank translates. Modeled on the sheng
         * DPU port (NativeUfs.cpp): sMR at 0x800+4i, s2CR at 0xc00+4i, CBAR at 0x1000+4b, CBA2R 0x1800+4b,
         * CBn at (pages+b)*4096 with pages from IDR1. */
        IOMemoryDescriptor *smmu=IOMemoryDescriptor::withPhysicalAddress(0x15000000,0xd0000,kIODirectionIn);
        IOMemoryMap *map=smmu?smmu->map(kIOMapAnywhere|kIOMapInhibitCache):nullptr;
        if(smmu)smmu->release();
        if(!map || !map->getVirtualAddress()) {OSSafeReleaseNULL(map);printf("PICASSO_SMMU map failed\n");return;}
        volatile uint32_t *s=(volatile uint32_t *)map->getVirtualAddress();
        const uint32_t scr0=s[0],idr0=s[0x20/4],idr1=s[0x24/4];
        const unsigned groups=(idr0&255)>128?128:(idr0&255),banks=idr1&255;
        const unsigned pages=1u<<(((idr1>>28)&7)+1);
        printf("PICASSO_SMMU scr0=%x idr0=%x idr1=%x groups=%u banks=%u pages=%u gfsr=%x\n",
            scr0,idr0,idr1,groups,banks,pages,s[0x58/4]);
        // This probe uses 4 KiB pages and must keep every context bank inside its mapping.
        if((idr1&0x80000000u) || !banks || banks>128 || pages>128 || (pages+banks)*4096u>0xd0000u) {
            map->release();return;
        }
        unsigned valid=0;
        for(unsigned i=0;i<groups;i++) {
            const uint32_t smr=s[(0x800+4*i)/4];
            if(!(smr&0x80000000u))continue;
            ++valid;
            const uint32_t idLow=smr&0x7fff,maskHigh=(smr>>16)&0x7fff,maskLow=smr&0xffff;
            /* sheng's layout: ID in 15:0, MASK in 30:16. ARM SMMU-500 TRM: ID in 30:16, MASK in 15:0. */
            const bool hitLow=((0xb40u^idLow)&0x7fff&~maskHigh)==0;
            const bool hitHigh=((0xb40u^maskHigh)&0xffff&~maskLow)==0;
            if(!hitLow && !hitHigh)continue;
            const uint32_t s2cr=s[(0xc00+4*i)/4],bank=s2cr&255;
            printf("PICASSO_SMMU match smr[%u]=%x layout=%s s2cr=%x bank=%u type=%u\n",
                i,smr,hitLow?"id-low":"id-high",s2cr,bank,(s2cr>>16)&3);
            if(bank>=banks)continue;
            volatile uint32_t *cb=s+(pages+bank)*(4096/4);
            printf("PICASSO_SMMU cbar=%x cba2r=%x sctlr=%x ttbr0=%x%08x tcr2=%x tcr=%x mair=%x fsr=%x\n",
                s[(0x1000+4*bank)/4],s[(0x1800+4*bank)/4],cb[0x00/4],
                cb[0x24/4],cb[0x20/4],cb[0x10/4],cb[0x30/4],cb[0x38/4],cb[0x58/4]);
        }
        printf("PICASSO_SMMU valid-smrs=%u\n",valid);
        map->release();
    }
    /* Point the active pipe at a 32-bit address in the display window (or the boot memory) and
     * flush VIG0; the pipe latches at the next frame boundary. */
    void sdeFlipTo(uint32_t iova,uint32_t stride) {
        sdeRegs[0x5000/4+0x14/4]=iova;
        sdeRegs[0x5000/4+0x24/4]=stride&0xffff;
        __asm__ volatile("dsb sy":::"memory");
        sdeRegs[0x2000/4+0x18/4]=1u;
        __asm__ volatile("dsb sy":::"memory");
    }
    /* The slot that holds this surface, mapping it into a free window (never the one on screen or
     * the one waiting to latch) the first time. */
    int directSlot(OSObject *surface,IOMemoryDescriptor *memory) {
        ++directClock;
        for(unsigned k=0;k<kDirectSlots;++k)if(directSlots[k].surface==surface && directSlots[k].memory==memory){directSlots[k].used=directClock;return int(k);}
        int pick=-1;
        for(unsigned k=0;k<kDirectSlots;++k) {
            if(int(k)==shownSlot || int(k)==pendingSlot)continue;
            if(pick<0 || !directSlots[k].surface || (directSlots[pick].surface && directSlots[k].used<directSlots[pick].used))pick=int(k);
        }
        if(pick<0)return -1;
        DirectSlot &slot=directSlots[pick];
        if(slot.surface) {slot.memory->complete();OSSafeReleaseNULL(slot.memory);OSSafeReleaseNULL(slot.surface);}
        if(memory->getLength()>8192ull*4096 || memory->prepare()!=kIOReturnSuccess){++directMapFails;return -1;}
        unsigned count=0;bool ok=true;
        for(IOByteCount offset=0;offset<memory->getLength() && ok;) {
            IOByteCount length=0;
            const addr64_t pa=memory->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
            if(!pa || (pa&0xfff) || !length){ok=false;break;}
            for(IOByteCount at=0;at<length && offset+at<memory->getLength();at+=4096)directPages[count++]=pa+at;
            offset+=length;
        }
        const uint32_t iova=ok?dpuMap(unsigned(pick),directPages,count):0;
        if(!iova){memory->complete();++directMapFails;return -1;}
        surface->retain();memory->retain();
        slot={surface,memory,iova,directClock};
        ++directMaps;
        return pick;
    }
    /* The compositor submits a frame's swap when its command buffer is scheduled, not when the GPU
     * finished it: wait for the fences handed out before the swap arrived. */
    bool gpuWrote(const OriginalLegacySwapRequest *request) {
        if(fenceFor!=request) {fenceFor=request;fenceTarget=0;fenceCaptured=false;fenceSince=0;}
        uint32_t submitted=0,retired=0;
        if(!&native_adreno_fences || !native_adreno_fences(&submitted,&retired)) {fenceFor=nullptr;return true;}
        if(!fenceCaptured) {fenceTarget=submitted;fenceCaptured=true;}
        if(int32_t(retired-fenceTarget)>=0) {fenceFor=nullptr;return true;}
        if(!fenceSince)fenceSince=mach_absolute_time();
        if(fenceTimer)fenceTimer->setTimeoutUS(200);
        return false;
    }
    static void latchFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->latchTick();}
    /* Finish a latch whose flush bit has cleared (or that timed out). The swap was already retired
     * when the flip was written; this only advances the shown slot and releases a held swap. */
    void completeLatch() {
        if(!latchRequest)return;
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-latchStart,&ns);
        if(ns>=50000000ull)++directLatchTimeouts;
        const uint32_t us=uint32_t(ns/1000);
        if(us>directLatchMax)directLatchMax=us;
        directLatchUs+=us;directLatchLastUs=us;
        shownSlot=pendingSlot;pendingSlot=-1;
        const OriginalLegacySwapRequest *request=latchRequest;latchRequest=nullptr;
        if(latchNeedsFinish) {latchNeedsFinish=false;finishTransfer(request,true);return;}
        transferred=false;
        kickSwapQueue();
    }
    void latchTick() {
        if(!latchRequest)return;
        uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time()-latchStart,&ns);
        if((sdeRegs[0x2000/4+0x18/4]&1u) && ns<50000000ull){latchTimer->setTimeoutUS(500);return;}
        completeLatch();
    }
    static void fenceFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->fenceTick();}
    void fenceTick() {transferred=false;kickSwapQueue();}
    /* Back to the boot loader's memory (the blank frame and the copy paths draw there). */
    void directToBoot() {
        if(!directOn || directBoot || !bootMapping)return;
        sdeFlipTo((uint32_t)bootMapping->getPhysicalAddress(),bootStride);
        directBoot=true;shownSlot=-1;
    }
    void startDirect() {
        unsigned mode=0;PE_parse_boot_argn("sheng-dpu-scanout",&mode,sizeof mode);
        if(mode<1 || !primary || !sdeEnsureMapped())return;
        if(!dpuStart()) {printf("PICASSO_DPU smmu not ready state=%x\n",gDpuState);return;}
        if(mode<2)return;
        if(!&native_adreno_fences) {printf("PICASSO_DPU no fence interface\n");return;}
        directPages=(uint64_t *)IOMallocData(8192*sizeof(uint64_t));
        IOWorkLoop *loop=getWorkLoop();
        latchTimer=loop?IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::latchFired):nullptr;
        fenceTimer=loop?IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::fenceFired):nullptr;
        if(!directPages || !latchTimer || !fenceTimer ||
           loop->addEventSource(latchTimer)!=kIOReturnSuccess || loop->addEventSource(fenceTimer)!=kIOReturnSuccess) {
            if(latchTimer){loop->removeEventSource(latchTimer);OSSafeReleaseNULL(latchTimer);}
            if(fenceTimer){loop->removeEventSource(fenceTimer);OSSafeReleaseNULL(fenceTimer);}
            if(directPages){IOFreeData(directPages,8192*sizeof(uint64_t));directPages=nullptr;}
            printf("PICASSO_DPU start failed pages=%p latch=%p fence=%p\n",directPages,latchTimer,fenceTimer);
            return;
        }
        directOn=true;directBoot=true;gDirectDisplay=this;
        printf("PICASSO_DPU direct scanout on bank=%u window=%x boot=%llx stride=%u\n",
            gDpuBank,kDpuWindow,(unsigned long long)bootMapping->getPhysicalAddress(),bootStride);
    }
    void stopDirect() {
        if(!directOn)return;
        directToBoot();
        for(auto &slot:directSlots) {if(slot.surface){slot.memory->complete();OSSafeReleaseNULL(slot.memory);OSSafeReleaseNULL(slot.surface);}slot={};}
        IOWorkLoop *loop=getWorkLoop();
        if(latchTimer){if(loop)loop->removeEventSource(latchTimer);OSSafeReleaseNULL(latchTimer);}
        if(fenceTimer){if(loop)loop->removeEventSource(fenceTimer);OSSafeReleaseNULL(fenceTimer);}
        if(directPages){IOFreeData(directPages,8192*sizeof(uint64_t));directPages=nullptr;}
        latchRequest=nullptr;pendingSlot=-1;shownSlot=-1;directOn=false;gDirectDisplay=nullptr;
    }
    void directFrameStats() {
        printf("PICASSO_DPU_FRAME n=%u period_us=%llu fence_us=%llu latch_us=%llu flips=%u\n",
            directFlips,(unsigned long long)directPeriodUs,(unsigned long long)directFenceUs,
            (unsigned long long)directLatchLastUs,directFlips);
    }
    void directStats() {
        static unsigned prints;
        if(prints>=200)return;
        ++prints;
        printf("PICASSO_DPU flips=%u maps=%u mapfails=%u fallbacks=%u latch_timeouts=%u latch_us_total=%llu latch_us_max=%llu\n",
            directFlips,directMaps,directMapFails,directFallbacks,directLatchTimeouts,
            (unsigned long long)directLatchUs,(unsigned long long)directLatchMax);
    }
    bool startHardware() override {
        unsigned enabled=0;
        PE_parse_boot_argn("offline-legacy-gop",&enabled,sizeof(enabled));
        if(enabled!=1)return false;
        PE_Video video={};IOService *provider=getProvider();
        if(!provider || PE_current_console(&video) ||
           !picasso_gop_video(video))return false;
        /* The scanout store dominates a frame: 1080x2400x4 = 10.4 MB per copy. An inhibit-cache (strongly ordered)
         * mapping turns every 8-byte store into its own bus transaction (~130 ms per frame at 19.2 MHz cntpct, measured
         * as PICASSO_DISPLAY event=10). Write-combining lets the CPU merge sequential stores into 64-byte lines, which
         * is the standard attribute for a framebuffer the display controller also reads. */
        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapWriteCombineCache);
        if(!mapping)return false;
        bool valid=mapping->getVirtualAddress() && mapping->getPhysicalAddress()==video.v_baseAddr &&
                   mapping->getLength()==video.v_rowBytes*video.v_height;
        if(valid) {
            /* This line publishes the panel's own height as the mode (2400 rows): the compositor's
             * drawable is panel-sized, so direct scanout can point the pipe at it without bars. */
            panelWidth=(uint32_t)video.v_width;panelHeight=(uint32_t)video.v_height;
            bootStride=(uint32_t)video.v_rowBytes;
            installBootDimensions(panelWidth,panelHeight);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            valid=width==video.v_width && height==video.v_height;
            /* The panel's own geometry, for the direct-scanout path: the line publishes the full panel
             * height as the mode (2400 rows), so the compositor's drawable is panel-sized and matches. */
            panelWidth=width;panelHeight=height;
            if(valid) {
                resetCopyShadow();OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;primary=true; // Text/graphics is console policy, not display identity.
                valid=native_legacy_gop_mapping_observed(this,bootMapping,width,height);
                /* offline-legacy-bench=1: measure the three sides of a frame copy once, through this exact mapping.
                 * The 10.4 MB frame copy costs ~130 ms here (PICASSO_DISPLAY event=10); these one-megabyte timings
                 * separate destination stores, destination reads and a cached-to-cached memcpy. */
                unsigned bench=0;PE_parse_boot_argn("offline-legacy-bench",&bench,sizeof bench);
                if(valid && bench==1) {
                    volatile uint64_t *fb=(volatile uint64_t *)bootMapping->getVirtualAddress();
                    uint64_t *a=(uint64_t *)IOMalloc(1u<<20),*b=(uint64_t *)IOMalloc(1u<<20);
                    if(a && b) {
                        uint64_t t0=0,t1=0;
                        for(uint64_t i=0;i<(1u<<17);++i)a[i]=i*0x9e3779b97f4a7c15ull;
                        __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t0)::"memory");
                        for(uint64_t i=0;i<(1u<<17);++i)b[i]=a[i];
                        __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t1)::"memory");
                        printf("PICASSO_DISPLAY bench memcpy1m ticks=%llu\n",t1-t0);
                        __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t0)::"memory");
                        for(uint64_t i=0;i<(1u<<17);++i)fb[i]=0xdeadbeef00000000ull|i;
                        __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t1)::"memory");
                        printf("PICASSO_DISPLAY bench fbwrite1m ticks=%llu\n",t1-t0);
                        __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t0)::"memory");
                        uint64_t sum=0;for(uint64_t i=0;i<(1u<<17);++i)sum+=fb[i];
                        __asm__ volatile("dsb sy\n\tisb\n\tmrs %0, cntpct_el0":"=r"(t1)::"memory");
                        printf("PICASSO_DISPLAY bench fbread1m ticks=%llu sum=%llx\n",t1-t0,sum);
                    }
                    if(a)IOFree(a,1u<<20);if(b)IOFree(b,1u<<20);
                }
            }
        }
        OSSafeReleaseNULL(mapping);return valid;
    }
#define LEAF(n) __attribute__((noreturn,noinline)) void opaque##n() override { native_legacy_backend_guard(n,this,__builtin_return_address(0)); }
    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
    LEAF(345)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}
/* CDC snapshot hook: with direct scanout the GOP mapping the snapshot reads is stale; copy the
 * surface the pipe is showing into it first. */
extern "C" void native_legacy_direct_snapshot(void) {if(gDirectDisplay)gDirectDisplay->directSnapshot();}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
