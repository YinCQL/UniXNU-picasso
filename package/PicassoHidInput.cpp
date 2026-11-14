// SPDX-License-Identifier: BSD-2-Clause-Patent
// Owned keyboard/consumer/digitizer device using the retained original family.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
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
volatile uint32_t native_hid_input_events[8];
volatile uint32_t native_touch_writer_lock;
void native_novatek_touch_probe(void);
void picasso_buttons_start(void);
}

static IOService *inputDevice;
static thread_call_t inputCall;
static unsigned presented, homeSent, homeLimit;
static uint64_t homeDue;
static bool planArmed;
static PicassoHidPlan gesture;
static void (*touchSink)(void *);
static void *touchContext;
static int results[8];
static unsigned reportErrors[8];
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

// The producer retains the callback's IOService across dispatch. Clearing the
// sink stops new dispatches; NativeHidInput serializes its stop with callbacks
// on the work loop, so a retained in-flight callback cannot access freed state.
static IOLock *buttonLock;
static bool (*buttonSink)(void *, uint32_t);
static IOService *buttonContext;
extern "C" void native_buttons_set_sink(bool (*sink)(void *, uint32_t), void *context) {
    // First registration is on the serial board start path, before the worker.
    if (!buttonLock) buttonLock = IOLockAlloc();
    if (!buttonLock) { IOLog("PICASSO_BUTTONS refused reason=sink-lock\n"); return; }
    IOService *next = sink ? static_cast<IOService *>(context) : nullptr;
    if (next) next->retain();
    IOLockLock(buttonLock);
    IOService *old = buttonContext;
    buttonContext = next; buttonSink = sink;
    IOLockUnlock(buttonLock);
    OSSafeReleaseNULL(old);
}
extern "C" bool picasso_buttons_available(void) {
    if (!buttonLock) return false;
    IOLockLock(buttonLock);
    bool ready = buttonSink && buttonContext;
    IOLockUnlock(buttonLock);
    return ready;
}
extern "C" bool picasso_buttons_deliver(uint32_t mask) {
    if (!buttonLock) return false;
    IOLockLock(buttonLock);
    auto sink = buttonSink;
    IOService *context = buttonContext;
    if (context) context->retain();
    IOLockUnlock(buttonLock);
    bool delivered = sink && context && sink(context, mask);
    OSSafeReleaseNULL(context);
    return delivered;
}
extern "C" void picasso_hid_physical_button(void) {
    unsigned expected = 0;
    __atomic_compare_exchange_n(&autoStop, &expected, 3, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

extern "C" void native_hid_input_observed(unsigned event, int result) {
    if (event >= 8) return;
    __atomic_store_n(&results[event], result, __ATOMIC_RELEASE);
    unsigned n = __atomic_add_fetch(&native_hid_input_events[event], 1, __ATOMIC_ACQ_REL);
    const bool reportError = (event == 4 || event == 5 || event == 6) && result &&
        __atomic_add_fetch(&reportErrors[event], 1, __ATOMIC_RELAXED) <= 8;
    if (event < 3 || reportError)
        IOLog("PICASSO_HID event=%u sample=%u result=%x\n", event, n, result);
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

static unsigned hidNumber(OSObject *object) {
    OSNumber *number = OSDynamicCast(OSNumber, object);
    return number ? number->unsigned32BitValue() : 0;
}

static void logHidIdentity(IORegistryEntry *entry, const char *node) {
    if (!entry) return;
    OSArray *pairs = OSDynamicCast(OSArray, entry->getProperty("DeviceUsagePairs"));
    OSDictionary *first = pairs && pairs->getCount() ? OSDynamicCast(OSDictionary, pairs->getObject(0)) : nullptr;
    IOLog("PICASSO_HID identity node=%s registry=%llx page=%x usage=%x pairs=%u first=%x/%x built_in=%u integrated=%u\n",
        node, entry->getRegistryEntryID(), hidNumber(entry->getProperty("PrimaryUsagePage")),
        hidNumber(entry->getProperty("PrimaryUsage")), pairs ? pairs->getCount() : 0,
        first ? hidNumber(first->getObject("DeviceUsagePage")) : 0,
        first ? hidNumber(first->getObject("DeviceUsage")) : 0,
        entry->getProperty("Built-In") == kOSBooleanTrue,
        entry->getProperty("DisplayIntegrated") == kOSBooleanTrue);
}

extern "C" void native_hid_live_checkpoint(IOService *device) {
    // Classification is now derived from the descriptor before registration.
    // Observe all three nodes without racing the family's property publication
    // or rewriting a service that userspace has already discovered.
    static uint64_t published[2]; // one combined device, or the two opt-in split devices
    IORegistryEntry *driver = eventDriver(device, 4);
    if (!driver) return;
    const uint64_t identity = driver->getRegistryEntryID();
    unsigned slot = 0;
    while (slot < 2 && published[slot] && published[slot] != identity) ++slot;
    if (slot < 2 && !published[slot]) {
        published[slot] = identity;
        logHidIdentity(device, "device");
        logHidIdentity(driver->getParentEntry(gIOServicePlane), "interface");
        logHidIdentity(driver, "event-service");
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
    // Novatek owns IDs 1..10. A physical takeover must release the synthetic
    // contact, not continue its gesture and inherited edge state as ID 1.
    const int x = 15240, id = 255;
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
    const bool hidReady = __atomic_load_n(&native_hid_input_events[3], __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&results[3], __ATOMIC_ACQUIRE) == 1;
    const bool ready = touchSink && hidReady;
    static bool touchStarted, buttonsStarted, stopPrinted;
    if (ready && !touchStarted) { touchStarted = true; native_novatek_touch_probe(); }
    if (hidReady && !buttonsStarted) { buttonsStarted = true; picasso_buttons_start(); }
    unsigned stopped = __atomic_load_n(&autoStop, __ATOMIC_ACQUIRE);
    if (stopped && !stopPrinted) {
        stopPrinted = true;
        IOLog("PICASSO_HID automatic input stopped reason=%s time_ms=%llu\n",
            stopped == 2 ? "physical-contact" : stopped == 3 ? "physical-button" : "visible-content", now);
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
