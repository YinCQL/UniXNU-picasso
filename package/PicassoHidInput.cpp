// SPDX-License-Identifier: BSD-2-Clause-Patent
// Owned keyboard/consumer/digitizer device using the retained original family.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "PicassoHidPlan.h"
#include "TouchPublish.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
IOService *native_hid_input_allocate(void);
kern_return_t OSKextLoadKextWithIdentifier(const char *);
volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
volatile uint32_t native_hid_input_command;
volatile uint32_t native_touch_writer_lock;
void native_novatek_touch_probe(void);
}

static IOService *inputDevice;
static thread_call_t inputCall;
static unsigned presented, homeSent, homeLimit;
static uint64_t homeDue;
static bool planArmed;
static PicassoHidPlan gesture;
static void (*touchSink)(void *);
static void *touchContext;
static unsigned reports[8];
static unsigned failures[8];
static int results[8];
static unsigned autoStop;

extern "C" void picasso_hid_content_presented(void) {
    unsigned expected = 0;
    __atomic_compare_exchange_n(&autoStop, &expected, 1, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

extern "C" void picasso_hid_physical_contact(void) {
    __atomic_store_n(&autoStop, 2, __ATOMIC_RELEASE);
}

static uint64_t milliseconds() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}

extern "C" void native_touch_set_sink(void (*sink)(void *), void *context) {
    touchSink = sink; touchContext = context;
}

extern "C" void native_touch_notify(void) {
    if (touchSink) touchSink(touchContext);
}

// No physical button producer is linked on this board. Resolve its optional
// HID hook under -no_weak_imports; a future button driver can override it.
extern "C" __attribute__((weak)) void native_buttons_set_sink(void (*)(void *, uint32_t), void *) {}

extern "C" void native_hid_input_observed(unsigned event, int result) {
    if (event >= 8) return;
    __atomic_store_n(&results[event], result, __ATOMIC_RELEASE);
    unsigned n = __atomic_add_fetch(&reports[event], 1, __ATOMIC_ACQ_REL);
    if (event < 3)
        IOLog("PICASSO_HID event=%u sample=%u result=%x\n", event, n, result);
    else if ((event == 4 || event == 5 || event == 6) && result) {
        // Count failures separately: successful touch reports must not exhaust
        // the diagnostic budget before memory pressure causes a later error.
        unsigned failed = __atomic_add_fetch(&failures[event], 1, __ATOMIC_RELAXED);
        if (failed <= 8)
            IOLog("PICASSO_HID event=%u sample=%u result=%x failure_sample=%u\n", event, n, result, failed);
    }
}

static IORegistryEntry *eventDriver(IORegistryEntry *parent, unsigned depth) {
    if (!depth) return nullptr;
    OSIterator *it = parent->getChildIterator(gIOServicePlane);
    if (!it) return nullptr;
    IORegistryEntry *found = nullptr;
    while (OSObject *object = it->getNextObject()) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, object);
        if (!entry) continue;
        if (entry->metaCast("IOHIDEventDriver")) { found = entry; found->retain(); }
        else found = eventDriver(entry, depth - 1);
        if (found) break;
    }
    it->release();
    return found;
}

extern "C" void native_hid_live_checkpoint(IOService *device) {
    // The combined keyboard collection must match first; classify the attached
    // event service as a built-in digitizer only after matching, once per service.
    static IORegistryEntry *published;
    IORegistryEntry *driver = eventDriver(device, 4);
    if (!driver) return;
    if (device->getProperty("Built-In") && published != driver) {
        driver->setProperty("Built-In", kOSBooleanTrue);
        driver->setProperty("DisplayIntegrated", kOSBooleanTrue);
        driver->setProperty("PrimaryUsagePage", (unsigned long long)0x0d, 32);
        driver->setProperty("PrimaryUsage", (unsigned long long)4, 32);
        if (IORegistryEntry *interface = driver->getParentEntry(gIOServicePlane)) {
            interface->setProperty("Built-In", kOSBooleanTrue);
            interface->setProperty("DisplayIntegrated", kOSBooleanTrue);
            interface->setProperty("PrimaryUsagePage", (unsigned long long)0x0d, 32);
            interface->setProperty("PrimaryUsage", (unsigned long long)4, 32);
        }
        published = driver;
        IOLog("PICASSO_HID digitizer event service ready\n");
    }
    driver->release();
}

extern "C" bool native_hid_input_requested(void) {
    if (__atomic_exchange_n(&native_hid_input_command, 0, __ATOMIC_ACQ_REL) == 1) return true;
    if (__atomic_load_n(&autoStop, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&planArmed, __ATOMIC_ACQUIRE) || homeSent >= homeLimit || milliseconds() < homeDue) return false;
    ++homeSent; homeDue = milliseconds() + 60000;
    return true;
}

extern "C" void picasso_hid_frame_presented(void) {
    __atomic_add_fetch(&presented, 1, __ATOMIC_RELAXED);
}

static void notifyPublished(void *) { native_touch_notify(); }

static void publishContact(unsigned count, int y) {
    const int x = 15240, id = 1;
    nvt_touch_publication publication = {&native_touch_seq, &native_touch_count,
        native_touch_xs, native_touch_ys, native_touch_ids, 12, notifyPublished, nullptr, &native_touch_writer_lock};
    nvt_touch_publish_if(&publication, &x, &y, &id, count, &autoStop);
}

static void attachInput() {
    int load = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    int match = load ? load : OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",
        true, false, kOSKextExcludeNone, kOSKextExcludeNone, nullptr);
    const OSSymbol *name = OSSymbol::withCString("IOHIDDevice");
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    IOLog("PICASSO_HID family=%d matching=%d class_size=%u\n", load, match, meta ? meta->getClassSize() : 0);
    if (load || match || !meta || meta->getClassSize() != 208) return;
    OSDictionary *matching = IOService::serviceMatching("IOHIDResource");
    IOService *resource = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (resource) {
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (handle) {
            native_hid_static_init();
            int status = OSMetaClass::postModLoad(handle);
            IOLog("PICASSO_HID metaclass=%d\n", status);
            if (!status) {
                IOService *device = native_hid_input_allocate();
                bool valid = device && device->metaCast("IOHIDDevice") && device->init();
                bool attached = valid && device->attach(resource);
                bool started = attached && device->start(resource);
                IOLog("PICASSO_HID allocated=%u attached=%u started=%u\n", valid, attached, started);
                if (started) inputDevice = device;
                else { if (attached) device->detach(resource); OSSafeReleaseNULL(device); }
            }
        }
    }
    OSSafeReleaseNULL(resource); OSSafeReleaseNULL(matching);
}

static void inputTick(thread_call_param_t, thread_call_param_t) {
    static bool attempted;
    if (!attempted) { attempted = true; attachInput(); }
    if (!inputDevice) return;
    const uint64_t now = milliseconds();
    const bool ready = touchSink && __atomic_load_n(&reports[3], __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&results[3], __ATOMIC_ACQUIRE) == 1;
    static bool touchStarted, stopPrinted;
    if (ready && !touchStarted) { touchStarted = true; native_novatek_touch_probe(); }
    unsigned stopped = __atomic_load_n(&autoStop, __ATOMIC_ACQUIRE);
    if (stopped && !stopPrinted) {
        stopPrinted = true;
        IOLog("PICASSO_HID automatic input stopped reason=%s time_ms=%llu\n",
            stopped == 2 ? "physical-contact" : "visible-content", now);
    }
    if (stopped == 2) {
        // The physical report replaces virtual fingers; do not lift the user's contact.
        gesture = {}; return;
    }
    if (stopped) gesture.stop();
    if (!stopped && !planArmed && ready && __atomic_load_n(&presented, __ATOMIC_RELAXED) >= 2) {
        unsigned count = 0;
        PE_parse_boot_argn("offline-hid-auto-home-count", &count, sizeof(count));
        homeLimit = count > 8 ? 8 : count;
        unsigned swipes = 0;
        PE_parse_boot_argn("offline-hid-swipe", &swipes, sizeof(swipes));
        unsigned homeDelay = 120, swipeDelay = 125;
        PE_parse_boot_argn("offline-hid-auto-home", &homeDelay, sizeof(homeDelay));
        PE_parse_boot_argn("offline-hid-swipe-delay", &swipeDelay, sizeof(swipeDelay));
        if (homeDelay > 300) homeDelay = 300;
        if (swipeDelay > 300) swipeDelay = 300;
        gesture.arm(now, swipes, swipeDelay * 1000); homeDue = now + homeDelay * 1000;
        __atomic_store_n(&planArmed, true, __ATOMIC_RELEASE);
        IOLog("PICASSO_HID plan armed time_ms=%llu home=%u swipe=%u home_delay_s=%u swipe_delay_s=%u\n",
            now, homeLimit, gesture.limit, homeDelay, swipeDelay);
    }
    bool wasActive = gesture.active;
    auto action = gesture.poll(now, native_touch_hid_reports,
        __atomic_load_n(&results[6], __ATOMIC_ACQUIRE) == 0, ready);
    if (action != PicassoHidPlan::Idle) publishContact(action == PicassoHidPlan::Contact ? 1 : 0,
        action == PicassoHidPlan::Contact ? gesture.y() : 1200);
    if (wasActive && !gesture.active)
        IOLog("PICASSO_HID swipe=%u reports=%u result=%x\n", gesture.sent, native_touch_hid_reports,
            __atomic_load_n(&results[6], __ATOMIC_ACQUIRE));
    if ((stopped || planArmed) && !gesture.active && gesture.sent >= gesture.limit) return;
    uint64_t deadline = 0;
    clock_interval_to_deadline(gesture.active ? 16 : 1000, kMillisecondScale, &deadline);
    thread_call_enter_delayed(inputCall, deadline);
}

extern "C" void picasso_hid_start_later(void) {
    unsigned enabled = 0;
    PE_parse_boot_argn("offline-hid-input", &enabled, sizeof(enabled));
    if (enabled != 1 || inputCall) return;
    inputCall = thread_call_allocate(inputTick, nullptr);
    if (inputCall) thread_call_enter(inputCall);
    else IOLog("PICASSO_HID could not allocate input worker\n");
}
