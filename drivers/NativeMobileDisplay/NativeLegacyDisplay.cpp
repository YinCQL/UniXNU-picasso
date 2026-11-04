// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso CPU/GOP adaptation of NativeMobileDisplay 8666a995.
// Original family lifecycle and client methods are bound by the build audit.
#include "OriginalLegacyFramebufferABI.h"
#include "NativeLegacyRequest.h"
#include "PicassoGop.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
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
// The mode height userland sees; the panel can be taller (1080x2340 mode over a 1080x2400 panel).
extern "C" uint32_t native_legacy_display_mode_height(uint32_t panelHeight);
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
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
extern "C" void native_legacy_retirement_observed(IOService *);
// GPU scanout hand-off (boot-arg sheng-fb-gpu=1): a finished frame is offered
// to the privileged GPU client instead of being copied. The presenter's offer
// either takes the frame or refuses it, in which case this file keeps the CPU
// path unchanged. A failed GPU copy falls back for the rest of the kick.
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height,uint32_t destY);
extern "C" void native_legacy_swap_stage(unsigned stage);
extern "C" void native_legacy_gpu_fallback_observed(int status);
extern "C" {extern volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;}
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
class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
    // The GOP panel's geometry. getDisplayDimensions() reports the mode (a
    // shorter 2340-row phone canvas when the ADT asks for it), so every panel
    // store and bounds check uses these instead.
    uint32_t panelWidth=0,panelHeight=0;
    // Previous frame for native_legacy_copy_diff (rows packed at width*4, same bytes as one GOP frame). The client always
    // sends a full-frame dirty rect, so the only reusable dirty information is this shadow; IPC-backed dirty rects are a no-op here.
    void *copyShadow=nullptr;
    uint64_t copyShadowBytes=0;
    bool copyShadowValid=false;
    void releaseCopyShadow() {if(copyShadow)IOFree(copyShadow,copyShadowBytes);copyShadow=nullptr;copyShadowBytes=0;copyShadowValid=false;}
    uint32_t scanBudget=4;
    uint64_t frameCount=0;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // The frame currently owned by the GPU client (valid until its completion),
    // and whether that swap already retired when the client took it.
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
    uint64_t gpuLength=0;
    bool gpuBypass=false;
    bool gpuRetired=false;
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
    unsigned vsyncHz=0;
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
        vsyncNext+=vsyncInterval;
        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        unsigned hz=60;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
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
    // Called by the GPU client's completion (any thread): the swap finishes on
    // the framebuffer's work loop, where the queue and the vsync clients live.
    void gpuComplete(int status) {
        struct Args {NativeLegacyDisplay *display;int status;};
        Args *args=(Args *)IOMalloc(sizeof(Args));
        if(!args){finishGpuSwap(status);return;}
        args->display=this;args->status=status;
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=(Args *)a0;a->display->finishGpuSwap(a->status);IOFree(a,sizeof(Args));return kIOReturnSuccess;
        };
        if(loop && loop->runAction(action,this,args)==kIOReturnSuccess)return;
        IOFree(args,sizeof(Args));
        finishGpuSwap(status);
    }
    void free() override {stopVsync();releaseCopyShadow();if(logicalPM){PMstop();logicalPM=false;}primary=false;OSSafeReleaseNULL(bootMapping);IOMobileFramebufferLegacy::free();}
    void stop(IOService *provider) override {
        stopVsync();
        releaseCopyShadow();
        transferred=false;gpuRequest=nullptr;gpuRetired=false;for(auto &entry: validated)entry=nullptr;
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
    bool start(IOService *provider) override {
        if(!startDisplay(provider))return false;
        startVsync();return true;
    }
    bool startDisplay(IOService *provider) {
        if(!IOMobileFramebufferLegacy::start(provider))return false;
        if(primary && bootMapping)announcePoweredByFirmware();
        unsigned enabled=1;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=4;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
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
        unsigned enabled=1;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request))return;
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
            uint32_t bw=panelWidth,bh=panelHeight;
            if(!bw || !bh)getDisplayDimensions(&bw,&bh);
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
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
        uint32_t width=panelWidth,height=panelHeight;
        if(!width || !height)getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint64_t sourceStride=native_legacy_surface_stride(surface);
        uint64_t allocation=native_legacy_surface_allocation(surface);
        const uint32_t sourceWidth=native_legacy_surface_width(surface);
        const uint32_t sourceHeight=native_legacy_surface_height(surface);
        /* A phone canvas is shorter than the panel: 2340 rows of artwork over a 2400-row panel. Accept it, put it in
         * the middle and fill the bars with black instead of rejecting the swap (a rejected swap never completes and
         * the render server stops presenting). The GPU copy path keeps the exact-geometry fast path. */
        const bool letterbox=sourceWidth>=width && sourceHeight && sourceHeight<=height;
        if(!memory || !memory->metaCast("IOMemoryDescriptor") ||
           (sourceHeight<=height ? !native_legacy_surface_bounds(width,sourceHeight,sourceStride,allocation,memory->getLength())
                                 : 1) ||
           !letterbox ||
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
        /* The phone canvas is shorter than the panel: hand the GPU the source height and the top-bar row
         * offset and let the presenter fill both bars, instead of copying 10 MB per frame on the CPU. */
        const uint32_t gpuBarRows=sourceHeight<height?(height-sourceHeight>=60?60:height-sourceHeight):0;
        if(!gpuBypass && sourceHeight<=height && sourceWidth>=width &&
           native_legacy_gpu_offer(this,memory,sourceStride,width,sourceHeight,gpuBarRows)) {
            // The GPU writes the scanout memory behind the CPU's back: the
            // differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            copyShadowValid=false;gpuRequest=request;gpuLength=length;transferred=true;
            // Retire as soon as the client owns the frame (the presenter retains
            // it); the next frame waits for the copy because transferred stays
            // set until finishGpuSwap.
            unsigned early=1;PE_parse_boot_argn("sheng-early-retire",&early,sizeof early);
            if(early==1 && retireSwap(request,true)) {
                gpuRetired=true;native_legacy_early_retires=native_legacy_early_retires+1;
            }
            return;
        }
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
        if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        // The phone mode leaves 60 rows below the panel's 2400. Keep all of them at the top: the status bar then
        // starts 60 px down, clearing the top-right camera, and the UI canvas ends at the physical bottom edge.
        const uint32_t barRows=height>sourceHeight?(height-sourceHeight>=60?60:height-sourceHeight):0;
        if(!letterbox || !native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,sourceHeight)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        // Inspect the first four frames and a sparse periodic sample after
        // that. Full-frame scans are deliberately not done on every copy.
        ++frameCount;
        if(scanBudget || (frameCount%600)==0) {
            if(scanBudget)--scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,sourceHeight,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
        }
        /* offline-legacy-bench=2: on the first real frame, time reads of the swap's own source memory through the
         * swap's mapping and through a copyback mapping of the same descriptor, each twice (cold and warm), plus a
         * cached control buffer and a store to the real destination. Together with the frame copy that follows, this
         * separates source-read cost, attribute choice and scheduling delays from destination store cost. */
        unsigned bench=0;PE_parse_boot_argn("offline-legacy-bench",&bench,sizeof bench);
        if(bench>=2 && frameCount==1) {
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
        // The client's swap always covers the whole frame, so reuse the existing shadow-diff copy: a static frame stores no
        // device bytes and a small update stores only the changed 64-byte blocks. Fall back to the plain copy on any
        // unsupported geometry or an allocation failure, and invalidate the shadow whenever the plain path or a blank ran.
        uint64_t stored=length;
        int copied=0;
        if(!letterbox && (!copyShadow || copyShadowBytes<length)) {
            releaseCopyShadow();
            copyShadow=IOMalloc(length);
            if(copyShadow)copyShadowBytes=length;
        }
        if(!letterbox && copyShadow) {
            const uint64_t diff=native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height,copyShadow,copyShadowValid?1:0);
            if(diff!=UINT64_MAX){stored=diff;copyShadowValid=true;copied=1;}
            else copyShadowValid=false;
        }
        if(!copied) {
            stored=length;
            volatile unsigned char *panel=(volatile unsigned char *)bootMapping->getVirtualAddress();
            if(!native_legacy_copy_pixels((volatile void *)(panel+(uint64_t)barRows*video.v_rowBytes),
                bootMapping->getLength()-(uint64_t)barRows*video.v_rowBytes,video.v_rowBytes,
                reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,sourceHeight)) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            if(barRows) {
                (void)native_legacy_fill_pixels((volatile void *)panel,bootMapping->getLength(),
                    video.v_rowBytes,width,barRows,0xff000000U);
                const uint64_t bottom=(uint64_t)(barRows+sourceHeight)*video.v_rowBytes;
                (void)native_legacy_fill_pixels((volatile void *)(panel+bottom),
                    bootMapping->getLength()-bottom,video.v_rowBytes,width,height-sourceHeight-barRows,0xff000000U);
            }
            if(letterbox)copyShadowValid=false;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,stored);}
        OSSafeReleaseNULL(source);transferred=true;
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        finishTransfer(request);
    }
    // Hands the pending head to the unchanged family after the CPU copy.
    bool retireSwap(const OriginalLegacySwapRequest *request,bool force=false) {
        unsigned retire=0,cpuPresent=1;
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
        if(!retireSwap(request,force)){native_legacy_retirement_failed(this);return;}
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
        if(!request)return;
        gpuRequest=nullptr;
        if(gpuRetired) {
            gpuRetired=false;
            if(status==0)native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            else {native_legacy_early_failures=native_legacy_early_failures+1;native_legacy_gpu_fallback_observed(status);gpuBypass=true;}
            transferred=false;kickSwapQueue();return;
        }
        if(status==0) {
            native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
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
        unsigned enabled=1;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=panelWidth,height=panelHeight;
            if(!width || !height)getDisplayDimensions(&width,&height);
            /* A phone canvas (1080x2340 over a 1080x2400 panel) names its own smaller height in the swap; accept it
             * when it is at most the panel's and both rectangles cover exactly that canvas. The copy path centres it
             * and fills the bars. */
            const uint32_t requestWidth=word(816),requestHeight=word(820);
            const bool phoneCanvas=requestWidth==width && requestHeight>height/2 && requestHeight<=height;
            const uint32_t canvasWidth=phoneCanvas?requestWidth:width,canvasHeight=phoneCanvas?requestHeight:height;
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                for(unsigned offset: rectOffsets) {
                    uint32_t rect[4]={};memcpy(rect,bytes+offset,sizeof(rect));
                    rectangles=rectangles && rect[0]==0 && rect[1]==0 && rect[2]==canvasWidth && rect[3]==canvasHeight;
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                if(width && height && rectangles && options && requestWidth==canvasWidth && requestHeight==canvasHeight) {
                    reason=4;
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole canvas, is copied.
                    if(native_legacy_surface_width(surface)>=canvasWidth && native_legacy_surface_height(surface)>=canvasHeight &&
                       picasso_gop_format(native_legacy_surface_format(surface),native_legacy_surface_planes(surface)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(canvasWidth,canvasHeight,native_legacy_surface_stride(surface),
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
    bool startHardware() override {
        unsigned enabled=1;
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
            panelWidth=(uint32_t)video.v_width;panelHeight=(uint32_t)video.v_height;
            const uint32_t modeHeight=native_legacy_display_mode_height(panelHeight);
            installBootDimensions(panelWidth,modeHeight);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            valid=width==panelWidth && height==modeHeight;
            if(valid) {
                OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;primary=true; // Text/graphics is console policy, not display identity.
                // The observer validates the GOP mapping against the panel, not the mode: the panel is still
                // 2400 rows when userland is told the mode is a 2340-row phone canvas.
                valid=native_legacy_gop_mapping_observed(this,bootMapping,panelWidth,panelHeight);
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
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}
