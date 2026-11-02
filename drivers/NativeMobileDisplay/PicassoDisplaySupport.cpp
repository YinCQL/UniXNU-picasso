// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware-backed CPU display. No Qualcomm display registers are changed.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSKext.h>
#include <libkern/OSKextLibPrivate.h>
#include <kern/clock.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "PicassoGop.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
IOService *native_legacy_display_allocate(void);
void picasso_keystore_probe(void) __attribute__((weak));
bool picasso_aks_ready(void) __attribute__((weak));
}

struct NativeLegacyMetaStorage;
extern NativeLegacyMetaStorage NativeLegacyDisplay_gMetaClass
    __asm("__ZN19NativeLegacyDisplay10gMetaClassE");
OSReturn oskext_start_direct(OSKext *kext, bool start_dependencies)
    __asm__("__ZN6OSKext5startEb");

static IOService *pendingDisplay;
static IOService *activeDisplay;
static IOService *activeNub;
static bool initialized, screenReleased;
static unsigned reportCounts[12][2];
static IORegistryEntry *displayChosen;
static bool scaleAdded, rotationAdded;
static struct {
    uint64_t submissions, accepted, rejected, rejectReasons[8];
    uint64_t scans, blankFrames, nonblackFrames;
    uint64_t transfers, transferFailures, retireSuccesses, retireFailures;
    uint64_t copies, copyBytes, lastCopyTicks, lastCopyBytes;
    uint64_t lastSubmitTick, lastFrameTick, lastTransferTick, lastRetireTick;
    uint64_t lastPixels, lastMinX, lastMinY, lastMaxX, lastMaxY;
    uint64_t vsyncTicks;
    uint32_t lastTransferStatus;
} displayStats;

extern "C" bool native_legacy_display_probe_ready(void);
extern "C" bool native_legacy_surface_ready(void);
extern "C" int boot_progress_prepare_scanout(void) __attribute__((weak_import));

extern "C" kern_return_t native_start_kext_and_report(const char *identifier,
    const char *class_name) {
    auto kext = OSKext::lookupKextWithIdentifier(identifier);
    if (!kext) {
        printf("NATIVE KEXT START: lookup failed id=%s\n", identifier);
        return kOSKextReturnNotFound;
    }
    printf("NATIVE KEXT START: begin id=%s loaded=%u started=%u\n",
        identifier, kext->isLoaded() ? 1u : 0u, kext->isStarted() ? 1u : 0u);
    kern_return_t result = kext->isLoaded() ?
        oskext_start_direct(kext, true) :
        OSKext::loadKextWithIdentifier(identifier);
    const OSSymbol *symbol = OSSymbol::withCString(class_name);
    const OSMetaClass *meta = symbol ?
        OSMetaClass::getMetaClassWithName(symbol) : nullptr;
    OSSafeReleaseNULL(symbol);
    const OSMetaClass *super = meta ? meta->getSuperClass() : nullptr;
    printf("NATIVE KEXT START: result=%d meta=%p class=%s size=%u super=%s\n",
        result, meta, meta ? meta->getClassName() : "missing",
        meta ? meta->getClassSize() : 0,
        super ? super->getClassName() : "missing");
    return result;
}

extern "C" kern_return_t native_register_display_and_report(void) {
    if (picasso_keystore_probe) picasso_keystore_probe();
    if (picasso_aks_ready) {
        printf("PICASSO_AKS ready=%u at CDC register\n",
            picasso_aks_ready() ? 1u : 0u);
    }
    static bool registered = false;
    const OSMetaClass *meta = reinterpret_cast<const OSMetaClass *>(
        &NativeLegacyDisplay_gMetaClass);
    if (registered) {
        const OSMetaClass *super = meta->getSuperClass();
        printf("NATIVE DISPLAY REG: already registered meta=%p super=%s size=%u\n",
            meta, super ? super->getClassName() : "missing",
            meta->getClassSize());
        return kOSReturnSuccess;
    }
    if (native_legacy_display_probe_ready()) {
        const OSMetaClass *super = meta->getSuperClass();
        printf("NATIVE DISPLAY REG: early probe already initialized meta=%p super=%s size=%u\n",
            meta, super ? super->getClassName() : "missing",
            meta->getClassSize());
        registered = true;
        return kOSReturnSuccess;
    }
    printf("NATIVE DISPLAY REG: kmod name=%s\n", g_kernel_kmod_info.name);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    printf("NATIVE DISPLAY REG: preModLoad handle=%p\n", handle);
    if (!handle) {
        return kOSReturnError;
    }
    native_legacy_display_static_init();
    OSReturn status = OSMetaClass::postModLoad(handle);
    const OSMetaClass *super = meta->getSuperClass();
    printf("NATIVE DISPLAY REG: postModLoad=%d meta=%p super=%s size=%u\n",
        status, meta, super ? super->getClassName() : "missing",
        meta->getClassSize());
    if (status == kOSReturnSuccess) {
        registered = true;
    }
    return status;
}

extern "C" kern_return_t native_publish_display_and_report(int level) {
    IOService *display = native_legacy_display_allocate();
    printf("NATIVE DISPLAY: allocate object=%p\n", display);
    if (!display) {
        return kOSReturnError;
    }
    const OSMetaClass *object_meta = display->getMetaClass();
    printf("NATIVE DISPLAY: meta=%p super=%p size=%u\n", object_meta,
        object_meta ? object_meta->getSuperClass() : nullptr,
        object_meta ? object_meta->getClassSize() : 0);
    bool legacy = display->metaCast("IOMobileFramebufferLegacy") != nullptr;
    bool framebuffer = display->metaCast("IOMobileFramebuffer") != nullptr;
    bool service = display->metaCast("IOService") != nullptr;
    printf("NATIVE DISPLAY: cast legacy=%u framebuffer=%u service=%u\n",
        legacy, framebuffer, service);
    if (!legacy || !framebuffer || !service) {
        display->release();
        return kOSKextReturnValidation;
    }
    bool initialized_ok = display->init();
    printf("NATIVE DISPLAY: init=%u\n", initialized_ok ? 1u : 0u);
    if (!initialized_ok) {
        display->release();
        return kOSReturnError;
    }
    if (level < 1) {
        display->release();
        printf("NATIVE DISPLAY: level=0 complete, released\n");
        return kOSReturnSuccess;
    }
    if (!native_legacy_surface_ready()) {
        printf("NATIVE DISPLAY: IOCoreSurfaceRoot unavailable; continuing to provider probe\n");
    }
    IOService *parent = IOService::getResourceService();
    IORegistryEntry *treeParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    IOService *nub = nullptr;
    OSObject *armio_object = OSMetaClass::allocClassWithName("AppleARMIODevice");
    if (armio_object) {
        nub = OSDynamicCast(IOService, armio_object);
        if (nub) {
            printf("NATIVE DISPLAY: AppleARMIODevice alloc=%p class=%s\n",
                nub, nub->getMetaClass()->getClassName());
        } else {
            OSSafeReleaseNULL(armio_object);
        }
    }
    if (!nub) {
        nub = new IOPlatformDevice;
        printf("NATIVE DISPLAY: AppleARMIODevice unavailable; fallback IOPlatformDevice=%p\n",
            nub);
    }
    PE_Video video = {};
    bool videoValid = !PE_current_console(&video) && picasso_gop_video(video);
    IODeviceMemory *memory = videoValid ?
        IODeviceMemory::withRange(video.v_baseAddr,
            (IOByteCount)video.v_rowBytes * video.v_height) : nullptr;
    OSArray *ranges = OSArray::withCapacity(1);
    const char type[] = "display-subsystem";
    OSData *typeData = OSData::withBytes(type, sizeof(type));
    printf("NATIVE DISPLAY: parent=%p armio-tree=%p video=%u base=%lx width=%lu height=%lu stride=%lu depth=%lu\n",
        parent, treeParent, videoValid ? 1u : 0u, video.v_baseAddr,
        video.v_width, video.v_height, video.v_rowBytes, video.v_depth);
    bool nubAttached = false, nubStarted = false, dtAttached = false;
    bool attached = false, started = false;
    if (nub && memory && ranges && treeParent && typeData && parent &&
        nub->init() && ranges->setObject(memory)) {
        nub->setDeviceMemory(ranges);
        nub->setName("NativeGOPFramebuffer");
        nubAttached = nub->attach(parent);
        if (nubAttached) nubStarted = nub->start(parent);
        if (nubStarted && nub->setProperty("device_type", typeData)) {
            nub->setName("gop-display", gIODTPlane);
            dtAttached = nub->attachToParent(treeParent, gIODTPlane);
            SInt32 score = 0;
            if (dtAttached && display->probe(nub, &score) == display)
                attached = display->attach(nub);
            if (attached) started = display->start(nub);
        }
    }
    printf("NATIVE DISPLAY: nub attach=%u start=%u dt=%u display attach=%u start=%u\n",
        nubAttached ? 1u : 0u, nubStarted ? 1u : 0u, dtAttached ? 1u : 0u,
        attached ? 1u : 0u, started ? 1u : 0u);
    if (started) {
        display->registerService(kIOServiceSynchronous);
        printf("NATIVE DISPLAY: registered provider=%p state=0x%x\n",
            display->getProvider(), display->getState());
        activeDisplay = display;
        activeNub = nub;
    } else {
        if (attached) display->detach(nub);
        if (dtAttached) nub->detachFromParent(treeParent, gIODTPlane);
        if (nubStarted) nub->stop(parent);
        if (nubAttached) nub->detach(parent);
        display->release();
        OSSafeReleaseNULL(nub);
    }
    OSSafeReleaseNULL(typeData);
    OSSafeReleaseNULL(ranges);
    OSSafeReleaseNULL(memory);
    OSSafeReleaseNULL(treeParent);
    return started ? kOSReturnSuccess : kOSReturnError;
}

extern "C" bool native_legacy_display_probe_ready(void) {
    return initialized && (pendingDisplay != nullptr || activeDisplay != nullptr);
}

static unsigned takeReport(unsigned *counter) {
    unsigned value = __atomic_load_n(counter, __ATOMIC_RELAXED);
    while (value < 4) {
        if (__atomic_compare_exchange_n(counter, &value, value + 1, false,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return value + 1;
    }
    return 0;
}

// Bounded per-event output, including failures, without logging every frame.
static void report(unsigned event, uint64_t a, uint64_t b, uint64_t c, bool failed = false) {
    if (event >= 12) return;
    unsigned sample = takeReport(&reportCounts[event][failed ? 1 : 0]);
    if (!sample) return;
    printf("PICASSO_DISPLAY event=%u sample=%u a=%llx b=%llx c=%llx failure=%u\n",
        event, sample, a, b, c, failed);
}

static uint64_t absoluteNanoseconds(uint64_t ticks) {
    uint64_t value = 0;
    absolutetime_to_nanoseconds(ticks, &value);
    return value;
}

static void reportDisplayState(const char *trigger) {
    const uint64_t now = mach_absolute_time();
    const uint64_t lastSubmit = __atomic_load_n(&displayStats.lastSubmitTick, __ATOMIC_RELAXED);
    const uint64_t lastFrame = __atomic_load_n(&displayStats.lastFrameTick, __ATOMIC_RELAXED);
    const uint64_t lastTransfer = __atomic_load_n(&displayStats.lastTransferTick, __ATOMIC_RELAXED);
    const uint64_t lastRetire = __atomic_load_n(&displayStats.lastRetireTick, __ATOMIC_RELAXED);
    printf("PICASSO_DISPLAY_STATE trigger=%s now_ns=%llu submit=%llu accept=%llu reject=%llu "
        "reasons=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu scans=%llu blank=%llu nonblack=%llu "
        "transfer=%llu transfer_fail=%llu retire=%llu retire_fail=%llu copies=%llu copy_bytes=%llu "
        "last_submit_ns=%llu last_frame_ns=%llu last_transfer_ns=%llu last_retire_ns=%llu "
        "last_copy_ticks=%llu last_copy_bytes=%llu last_pixels=%llu bbox=%llu,%llu-%llu,%llu "
        "last_status=%x vsync_ticks=%llu\n",
        trigger, (unsigned long long)absoluteNanoseconds(now),
        (unsigned long long)__atomic_load_n(&displayStats.submissions, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.accepted, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.rejected, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.rejectReasons[0], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.rejectReasons[1], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.rejectReasons[2], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.rejectReasons[3], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.rejectReasons[4], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.rejectReasons[5], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.rejectReasons[6], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.rejectReasons[7], __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.scans, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.blankFrames, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.nonblackFrames, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.transfers, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.transferFailures, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.retireSuccesses, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.retireFailures, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.copies, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.copyBytes, __ATOMIC_RELAXED),
        (unsigned long long)absoluteNanoseconds(lastSubmit),
        (unsigned long long)absoluteNanoseconds(lastFrame),
        (unsigned long long)absoluteNanoseconds(lastTransfer),
        (unsigned long long)absoluteNanoseconds(lastRetire),
        (unsigned long long)__atomic_load_n(&displayStats.lastCopyTicks, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.lastCopyBytes, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.lastPixels, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.lastMinX, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.lastMinY, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.lastMaxX, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.lastMaxY, __ATOMIC_RELAXED),
        __atomic_load_n(&displayStats.lastTransferStatus, __ATOMIC_RELAXED),
        (unsigned long long)__atomic_load_n(&displayStats.vsyncTicks, __ATOMIC_RELAXED));
}

extern "C" void native_legacy_display_state_report(void) {
    reportDisplayState("wait");
}

extern "C" bool native_legacy_prepare_scanout(void) {
    // The boot-page monitor writes directly into the same GOP mapping. Yield it
    // before a system frame takes the screen; boot-diag-resume reclaims it when
    // userland stops producing new scanouts.
    if (boot_progress_prepare_scanout && !boot_progress_prepare_scanout()) {
        return false;
    }
    if (!screenReleased) {
        // Release the real XNU text console before the first system frame.
        // CDC owns its independent console ring and remains available.
        if (PE_initialize_console(nullptr, kPEReleaseScreen)) return false;
        screenReleased = true;
        report(8, 1, 0, 0);
    }
    return true;
}

extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot, void *, void *caller) {
    panic("PICASSO_DISPLAY unsupported backend slot=%u caller=%p", slot, caller);
}
extern "C" void native_legacy_registration_observed(IOService *display) {
    report(1, reinterpret_cast<uintptr_t>(display), display->metaCast("IOMobileFramebuffer") != nullptr, 0);
}
extern "C" void native_legacy_client_enter(task_t, uint32_t type) { report(3, type, 0, 0); }
extern "C" void native_legacy_client_observed(IOReturn status, IOUserClient *client) {
    report(4, status, client != nullptr, 0, status != kIOReturnSuccess);
}
extern "C" void native_legacy_pm_observed(unsigned stage, IOReturn status, unsigned long state) {
    report(5, stage, status, state, status != kIOReturnSuccess);
}
extern "C" void native_legacy_swap_validation_observed(bool ok, unsigned reason, const void *, const void *) {
    const uint64_t total = __atomic_add_fetch(&displayStats.submissions, 1, __ATOMIC_RELAXED);
    if (ok) __atomic_add_fetch(&displayStats.accepted, 1, __ATOMIC_RELAXED);
    else __atomic_add_fetch(&displayStats.rejected, 1, __ATOMIC_RELAXED);
    if (reason < 8) __atomic_add_fetch(&displayStats.rejectReasons[reason], 1, __ATOMIC_RELAXED);
    __atomic_store_n(&displayStats.lastSubmitTick, mach_absolute_time(), __ATOMIC_RELAXED);
    report(6, ok, reason, 0, !ok);
    if ((total % 64) == 0) reportDisplayState("submissions");
}
extern "C" void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format,
    uint32_t planes, uint64_t stride, uint64_t allocation, uint64_t length) {
    static unsigned count;
    if (takeReport(&count)) printf("PICASSO_DISPLAY surface rejected width=%llu height=%llu format=%x planes=%u stride=%llu allocation=%llu length=%llu\n",
        width, height, format, planes, stride, allocation, length);
}
extern "C" void native_legacy_transfer_observed(IOReturn status, const void *, uint64_t bytes) {
    __atomic_add_fetch(&displayStats.transfers, 1, __ATOMIC_RELAXED);
    if (status != kIOReturnSuccess) __atomic_add_fetch(&displayStats.transferFailures, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&displayStats.lastTransferStatus, static_cast<uint32_t>(status), __ATOMIC_RELAXED);
    __atomic_store_n(&displayStats.lastTransferTick, mach_absolute_time(), __ATOMIC_RELAXED);
    report(7, status, bytes, 0, status != kIOReturnSuccess);
}
extern "C" void native_legacy_retirement_failed(IOService *) {
    __atomic_add_fetch(&displayStats.retireFailures, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&displayStats.lastRetireTick, mach_absolute_time(), __ATOMIC_RELAXED);
    report(9, 0, 0, 0, true);
}
extern "C" void native_legacy_retirement_observed(IOService *) {
    __atomic_add_fetch(&displayStats.retireSuccesses, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&displayStats.lastRetireTick, mach_absolute_time(), __ATOMIC_RELAXED);
    report(9, 1, 0, 0);
}
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes) {
    __atomic_add_fetch(&displayStats.copies, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&displayStats.copyBytes, bytes, __ATOMIC_RELAXED);
    __atomic_store_n(&displayStats.lastCopyTicks, ticks, __ATOMIC_RELAXED);
    __atomic_store_n(&displayStats.lastCopyBytes, bytes, __ATOMIC_RELAXED);
    // Keep more than the generic four samples: the copy cost alternates between two source surfaces, so a handful of
    // consecutive frames is what separates a real per-surface cost from a scheduling delay.
    static unsigned copySamples;
    if (copySamples < 32) {
        unsigned sample = __atomic_add_fetch(&copySamples, 1, __ATOMIC_RELAXED);
        printf("PICASSO_DISPLAY event=10 sample=%u a=%llx b=%llx c=%llu failure=0\n",
            sample, ticks, bytes, __atomic_load_n(&displayStats.copies, __ATOMIC_RELAXED));
    }
}
extern "C" void native_legacy_vsync_observed(unsigned event, unsigned hz) {
    if (event == 1) {
        const uint64_t total = __atomic_add_fetch(&displayStats.vsyncTicks, 1, __ATOMIC_RELAXED);
        if ((total % 600) == 0) reportDisplayState("vsync");
    }
    report(11, event, hz, 0);
}
extern "C" void native_legacy_frame_observed(unsigned kind, uint64_t pixels,
    uint64_t minx, uint64_t miny, uint64_t maxx, uint64_t maxy) {
    if (kind == 0) {
        __atomic_add_fetch(&displayStats.scans, 1, __ATOMIC_RELAXED);
        if (pixels) __atomic_add_fetch(&displayStats.nonblackFrames, 1, __ATOMIC_RELAXED);
        else __atomic_add_fetch(&displayStats.blankFrames, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&displayStats.lastFrameTick, mach_absolute_time(), __ATOMIC_RELAXED);
        __atomic_store_n(&displayStats.lastPixels, pixels, __ATOMIC_RELAXED);
        __atomic_store_n(&displayStats.lastMinX, minx, __ATOMIC_RELAXED);
        __atomic_store_n(&displayStats.lastMinY, miny, __ATOMIC_RELAXED);
        __atomic_store_n(&displayStats.lastMaxX, maxx, __ATOMIC_RELAXED);
        __atomic_store_n(&displayStats.lastMaxY, maxy, __ATOMIC_RELAXED);
    }
    static unsigned count;
    if (takeReport(&count)) printf("PICASSO_DISPLAY frame kind=%u nonblack=%llu\n", kind, pixels);
}

static void finishDisplayPolicy(bool keep) {
    if (displayChosen && !keep) {
        if (scaleAdded) displayChosen->removeProperty("display-scale");
        if (rotationAdded) displayChosen->removeProperty("display-rotation");
    }
    OSSafeReleaseNULL(displayChosen);
    scaleAdded = rotationAdded = false;
}

static bool matchWord(IORegistryEntry *entry, const char *key, uint32_t value, bool &added) {
    OSObject *old = entry->getProperty(key);
    if (old) {
        OSData *data = OSDynamicCast(OSData, old);
        uint32_t actual = 0;
        if (!data || data->getLength() != sizeof(actual)) return false;
        memcpy(&actual, data->getBytesNoCopy(), sizeof(actual));
        return actual == value;
    }
    OSData *data = OSData::withBytes(&value, sizeof(value));
    bool ok = data && entry->setProperty(key, data);
    OSSafeReleaseNULL(data);
    added = ok;
    return ok;
}

// The original framebuffer start path expects the IOSurface family to have
// published IOCoreSurfaceRoot before it can attach its client. Keep this
// bounded and cached: a CDC-triggered probe must not repeat a five-second wait
// or turn a missing optional service into an unbounded boot dependency.
extern "C" bool native_legacy_surface_ready(void) {
    static unsigned state; // 0 unknown/retryable, 1 ready
    if (state == 1) return true;
    OSReturn status = OSKext::loadKextWithIdentifier(
        "com.apple.iokit.IOSurface", true, false,
        kOSKextExcludeNone, kOSKextExcludeAll, nullptr);
    OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface = matching ?
        IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    bool found = surface != nullptr;
    OSSafeReleaseNULL(surface);
    OSSafeReleaseNULL(matching);
    if (!found) {
        // IOSurfaceRoot matches IOResources only after IOBSD is published. Creating
        // it here makes a second instance race the normal match and registers the
        // iosurface sysctl twice (slot 189). Wait again, but never create it.
        printf("PICASSO_DISPLAY: IOCoreSurfaceRoot unavailable; waiting for normal match\n");
        matching = IOService::nameMatching("IOCoreSurfaceRoot");
        surface = matching ?
            IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
        found = surface != nullptr;
        OSSafeReleaseNULL(surface);
        OSSafeReleaseNULL(matching);
    }
    report(0, 2, status, found);
    state = found ? 1 : 0;
    return found;
}

extern "C" bool native_legacy_gop_mapping_observed(IOService *, IOMemoryMap *map, uint32_t width, uint32_t height) {
    PE_Video video = {};
    if (PE_current_console(&video) || map->getPhysicalAddress() != video.v_baseAddr ||
        width != video.v_width || height != video.v_height) return false;
    if (displayChosen) return false;
    displayChosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    // Two scanout pixels per UI point. This is UI policy, not physical DPI.
    bool ok = displayChosen && matchWord(displayChosen, "display-scale", 2, scaleAdded) &&
        matchWord(displayChosen, "display-rotation", 0, rotationAdded);
    if (!ok) finishDisplayPolicy(false);
    printf("PICASSO_DISPLAY mapped ok=%u base=%llx bytes=%llx width=%u height=%u stride=%lu depth=%lu console=%lu scale=2\n",
        ok, map->getPhysicalAddress(), map->getLength(), width, height,
        video.v_rowBytes, video.v_depth, video.v_display);
    return ok;
}

extern "C" void native_legacy_display_probe(void) {
    if (picasso_keystore_probe) picasso_keystore_probe();
    if (picasso_aks_ready) printf("PICASSO_AKS ready=%u at display probe\n", picasso_aks_ready() ? 1u : 0u);
    if (initialized) return;
    initialized = true;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) { report(0, 1, kIOReturnNoMemory, 0); return; }
    native_legacy_display_static_init();
    OSReturn status = OSMetaClass::postModLoad(handle);
    report(0, 1, status, 0);
    if (status) return;
    IOService *display = native_legacy_display_allocate();
    if (!display || !display->metaCast("IOMobileFramebufferLegacy") || !display->init()) {
        OSSafeReleaseNULL(display); return;
    }
    pendingDisplay = display;
}

extern "C" void native_legacy_display_late_probe(void) {
    if (picasso_keystore_probe) picasso_keystore_probe();
    IOService *display = pendingDisplay;
    if (!display) return;
    pendingDisplay = nullptr;
    const OSReturn status = OSKext::loadKextWithIdentifier(
        "com.apple.iokit.IOSurface", true, false,
        kOSKextExcludeNone, kOSKextExcludeAll, nullptr);
    OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    bool found = surface != nullptr;
    OSSafeReleaseNULL(surface); OSSafeReleaseNULL(matching);
    report(0, 2, status, found);
    PE_Video video = {};
    if (status || !found || PE_current_console(&video) ||
        !picasso_gop_video(video)) {
        printf("PICASSO_DISPLAY handoff rejected base=%lx width=%lu height=%lu stride=%lu depth=%lu rotation=%u\n",
            video.v_baseAddr, video.v_width, video.v_height, video.v_rowBytes, video.v_depth, video.v_rotate);
        display->release(); return;
    }
    IOService *parent = IOService::getResourceService();
    IOPlatformDevice *nub = new IOPlatformDevice;
    IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
    OSArray *ranges = OSArray::withCapacity(1);
    IORegistryEntry *treeParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
    const char type[] = "display-subsystem";
    OSData *typeData = OSData::withBytes(type, sizeof(type));
    bool nubAttached = false, nubStarted = false, dtAttached = false, attached = false, started = false;
    if (nub && memory && ranges && treeParent && typeData && parent && nub->init() && ranges->setObject(memory)) {
        nub->setDeviceMemory(ranges); nub->setName("NativeGOPFramebuffer");
        nubAttached = nub->attach(parent);
        if (nubAttached) nubStarted = nub->start(parent);
        if (nubStarted && nub->setProperty("device_type", typeData)) {
            nub->setName("gop-display", gIODTPlane);
            dtAttached = nub->attachToParent(treeParent, gIODTPlane);
            SInt32 score = 0;
            if (dtAttached && display->probe(nub, &score) == display) attached = display->attach(nub);
            if (attached) started = display->start(nub);
        }
    }
    report(0, 3, started, dtAttached);
    finishDisplayPolicy(started);
    OSSafeReleaseNULL(typeData); OSSafeReleaseNULL(ranges); OSSafeReleaseNULL(memory);
    if (started) { activeDisplay = display; activeNub = nub; }
    else {
        if (attached) display->detach(nub);
        if (dtAttached) nub->detachFromParent(treeParent, gIODTPlane);
        if (nubStarted) nub->stop(parent);
        if (nubAttached) nub->detach(parent);
        OSSafeReleaseNULL(nub); display->release();
    }
    OSSafeReleaseNULL(treeParent);
}
