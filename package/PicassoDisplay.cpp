// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware-backed CPU display. No Qualcomm display registers are changed.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "PicassoGop.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
void picasso_keystore_probe(void);
IOService *native_legacy_display_allocate(void);
kern_return_t OSKextLoadKextWithIdentifier(const char *);
}

static IOService *pendingDisplay;
static IOService *activeDisplay;
static IOPlatformDevice *activeNub;
static bool initialized, screenReleased;
static unsigned reportCounts[12][2];
static IORegistryEntry *displayChosen;
static bool scaleAdded, rotationAdded;

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
    IOLog("PICASSO_DISPLAY event=%u sample=%u a=%llx b=%llx c=%llx failure=%u\n",
        event, sample, a, b, c, failed);
}

extern "C" bool native_legacy_prepare_scanout(void) {
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
    report(6, ok, reason, 0, !ok);
}
extern "C" void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format,
    uint32_t planes, uint64_t stride, uint64_t allocation, uint64_t length) {
    static unsigned count;
    if (takeReport(&count)) IOLog("PICASSO_DISPLAY surface rejected width=%llu height=%llu format=%x planes=%u stride=%llu allocation=%llu length=%llu\n",
        width, height, format, planes, stride, allocation, length);
}
extern "C" void native_legacy_transfer_observed(IOReturn status, const void *, uint64_t bytes) { report(7, status, bytes, 0, status != kIOReturnSuccess); }
extern "C" void native_legacy_retirement_failed(IOService *) { report(9, 0, 0, 0, true); }
extern "C" void native_legacy_retirement_observed(IOService *) { report(9, 1, 0, 0); }
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes) { report(10, ticks, bytes, 0); }
extern "C" void native_legacy_vsync_observed(unsigned event, unsigned hz) { report(11, event, hz, 0); }
extern "C" void native_legacy_frame_observed(unsigned kind, uint64_t pixels, uint64_t, uint64_t, uint64_t, uint64_t) {
    static unsigned count;
    if (takeReport(&count)) IOLog("PICASSO_DISPLAY frame kind=%u nonblack=%llu\n", kind, pixels);
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

// Honor the ADT's iPhone scale while keeping the existing 2x default when no
// scale is supplied. Reject malformed or unsupported properties unchanged.
static bool matchDisplayScale(IORegistryEntry *entry, uint32_t &scale, bool &added) {
    OSObject *old = entry->getProperty("display-scale");
    if (!old) {
        scale = 2;
        return matchWord(entry, "display-scale", scale, added);
    }
    OSData *data = OSDynamicCast(OSData, old);
    if (!data || data->getLength() != sizeof(scale)) return false;
    memcpy(&scale, data->getBytesNoCopy(), sizeof(scale));
    return scale == 2 || scale == 3;
}

extern "C" bool native_legacy_gop_mapping_observed(IOService *, IOMemoryMap *map, uint32_t width, uint32_t height) {
    PE_Video video = {};
    if (PE_current_console(&video) || map->getPhysicalAddress() != video.v_baseAddr ||
        width != video.v_width || height != video.v_height) return false;
    if (displayChosen) return false;
    displayChosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    // Scale is UI policy, not physical DPI; the actual scanout geometry is unchanged.
    uint32_t scale = 0;
    bool scaleOK = displayChosen && matchDisplayScale(displayChosen, scale, scaleAdded);
    bool rotationOK = scaleOK && matchWord(displayChosen, "display-rotation", 0, rotationAdded);
    bool ok = scaleOK && rotationOK;
    if (!ok) finishDisplayPolicy(false);
    IOLog("PICASSO_DISPLAY mapped ok=%u base=%llx bytes=%llx width=%u height=%u stride=%lu depth=%lu console=%lu scale=%u scale_ok=%u rotation_ok=%u\n",
        (unsigned)ok, map->getPhysicalAddress(), map->getLength(), width, height,
        video.v_rowBytes, video.v_depth, video.v_display, scale, (unsigned)scaleOK, (unsigned)rotationOK);
    return ok;
}

extern "C" void native_legacy_display_probe(void) {
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
    picasso_keystore_probe();
    IOService *display = pendingDisplay;
    if (!display) return;
    pendingDisplay = nullptr;
    const kern_return_t status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    bool found = surface != nullptr;
    OSSafeReleaseNULL(surface); OSSafeReleaseNULL(matching);
    report(0, 2, status, found);
    PE_Video video = {};
    if (status || !found || PE_current_console(&video) ||
        !picasso_gop_video(video)) {
        IOLog("PICASSO_DISPLAY handoff rejected base=%lx width=%lu height=%lu stride=%lu depth=%lu rotation=%u\n",
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
