// SPDX-License-Identifier: BSD-2-Clause-Patent
// OS properties follow the UniXNU display tutorial's AppleARMBacklight example.
// The hardware backend is Picasso's PM8150L WLED5, not Sheng's GENI/KTZ8866.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include "PicassoBacklightCore.h"

extern "C" bool picasso_backlight_current(uint32_t *);
extern "C" IOReturn picasso_backlight_level(uint32_t);

static IOService *publishedBrightness;
extern "C" bool picasso_brightness_is_service(IORegistryEntry *entry) {
    return entry && entry == __atomic_load_n(&publishedBrightness, __ATOMIC_ACQUIRE);
}

class PicassoBrightness : public IOService {
    OSDeclareDefaultStructors(PicassoBrightness)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *requestTimer = nullptr, *fadeTimer = nullptr;
    IOLock *requestLock = nullptr;
    bool requestAdded = false, fadeAdded = false, accepting = false, compatible = false;
    const OSMetaClass *appleClass = nullptr;
    uint64_t mailbox = 0;
    uint32_t level = 0, reports = 0, propertyReports = 0;
    picasso_backlight::Fade fade;

    static uint64_t nowMs() {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        return ns / 1000000;
    }
    bool publishLevel(uint32_t value) {
        OSDictionary *brightness = OSDictionary::withCapacity(3);
        OSDictionary *parameters = OSDictionary::withCapacity(1);
        OSNumber *minimum = OSNumber::withNumber(0ULL, 32);
        OSNumber *maximum = OSNumber::withNumber(picasso_backlight::max_level, 32);
        OSNumber *current = OSNumber::withNumber(value, 32);
        bool ok = brightness && parameters && minimum && maximum && current &&
            brightness->setObject("min", minimum) && brightness->setObject("max", maximum) &&
            brightness->setObject("value", current) && parameters->setObject("brightness", brightness) &&
            setProperty("IODisplayParameters", parameters);
        OSSafeReleaseNULL(minimum); OSSafeReleaseNULL(maximum); OSSafeReleaseNULL(current);
        OSSafeReleaseNULL(brightness); OSSafeReleaseNULL(parameters);
        return ok;
    }
    void failure(IOReturn error) {
        __atomic_store_n(&accepting, false, __ATOMIC_RELEASE);
        fadeTimer->cancelTimeout();
        setProperty("PicassoBrightnessError", uint64_t(uint32_t(error)), 32);
        IOLog("PICASSO_BRIGHTNESS stopped error=%x last_accepted=%u\n", error, level);
    }
    void step() {
        if (!__atomic_load_n(&accepting, __ATOMIC_ACQUIRE)) return;
        uint64_t now = nowMs();
        uint32_t next = fade.value(now);
        if (next != level) {
            IOReturn error = picasso_backlight_level(next);
            if (error == kIOReturnBusy) {
                // A completed, read-only preflight found changed configuration.
                // Do not retry on a timer, publish a level or poison the service:
                // a later OS request may revalidate it. Transport/partial-write
                // errors still stop the session in failure().
                fadeTimer->cancelTimeout();
                if (reports < 128 || !(reports & 63))
                    IOLog("PICASSO_BRIGHTNESS rejected target=%u error=%x retry=next_request\n", next, error);
                return;
            }
            if (error) { failure(error); return; }
            // The backend accepts this value or defers it while the display is
            // off. Its log distinguishes a deferred target from hardware readback.
            level = next;
            if (!publishLevel(level)) { failure(kIOReturnNoMemory); return; }
        }
        if (fade.active(now)) {
            IOReturn error = fadeTimer->setTimeoutMS(16);
            if (error) failure(error);
        }
    }
    static void fadeFired(OSObject *owner, IOTimerEventSource *) {
        static_cast<PicassoBrightness *>(owner)->step();
    }
    static void requestFired(OSObject *owner, IOTimerEventSource *) {
        auto *self = static_cast<PicassoBrightness *>(owner);
        if (!__atomic_load_n(&self->accepting, __ATOMIC_ACQUIRE)) return;
        uint64_t request = __atomic_exchange_n(&self->mailbox, 0ULL, __ATOMIC_ACQUIRE);
        if (!(request >> 63)) return;
        self->fadeTimer->cancelTimeout();
        uint32_t goal = uint32_t(request >> 16) & 0xffff;
        uint32_t ms = uint32_t(request) & 0xffff;
        self->fade.begin(self->level, goal, ms, nowMs());
        if (self->reports < 128 || (self->reports & 63) == 0)
            IOLog("PICASSO_BRIGHTNESS request=%u fade_ms=%u from=%u\n", goal, ms, self->level);
        ++self->reports;
        self->step();
    }
    void quiesce() {
        IOService *expected = this;
        __atomic_compare_exchange_n(&publishedBrightness, &expected, nullptr, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        // setProperties never waits for the work-loop gate while holding this
        // lock. Removing the sources waits out callbacks before freeing them.
        if (requestLock) IOLockLock(requestLock);
        __atomic_store_n(&accepting, false, __ATOMIC_RELEASE);
        if (requestTimer) requestTimer->cancelTimeout();
        if (requestLock) IOLockUnlock(requestLock);
        if (fadeTimer) fadeTimer->cancelTimeout();
        if (requestAdded) { loop->removeEventSource(requestTimer); requestAdded = false; }
        if (fadeAdded) { loop->removeEventSource(fadeTimer); fadeAdded = false; }
        // A callback already holding the gate could have armed a final timeout.
        if (requestTimer) requestTimer->cancelTimeout();
        if (fadeTimer) fadeTimer->cancelTimeout();
    }
public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");
        appleClass = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
        OSSafeReleaseNULL(name);
        bool serviceClass = false;
        for (const OSMetaClass *m = appleClass; m; m = m->getSuperClass())
            if (m == &IOService::gMetaClass) serviceClass = true;
        bool backendReady = picasso_backlight_current(&level);
        if (!serviceClass || !backendReady) {
            IOLog("PICASSO_BRIGHTNESS refused apple_class=%u backend_ready=%u\n", serviceClass, backendReady);
            return false;
        }
        requestLock = IOLockAlloc();
        loop = IOWorkLoop::workLoop();
        requestTimer = IOTimerEventSource::timerEventSource(this, requestFired);
        fadeTimer = IOTimerEventSource::timerEventSource(this, fadeFired);
        if (!requestLock || !loop || !requestTimer || !fadeTimer) return false;
        if (loop->addEventSource(requestTimer)) return false;
        requestAdded = true;
        if (loop->addEventSource(fadeTimer)) return false;
        fadeAdded = true;
        if (!publishLevel(level)) return false;
        // CoreBrightness requires both this property and AppleARMBacklight
        // conformance for its built-in-display/libEDR path. Do not publish only
        // half that interface. No Apple private methods or copied vtable are used.
        __atomic_store_n(&compatible, true, __ATOMIC_RELEASE);
        if (!setProperty("backlight-control", true)) return false;
        __atomic_store_n(&accepting, true, __ATOMIC_RELEASE);
        IOService *expected = nullptr;
        if (!__atomic_compare_exchange_n(&publishedBrightness, &expected, this, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return false;
        registerService();
        IOLog("PICASSO_BRIGHTNESS published range=0..4095 nonzero_min=42 initial=%u\n", level);
        return true;
    }
    IOReturn setProperties(OSObject *properties) override {
        auto *dict = OSDynamicCast(OSDictionary, properties);
        unsigned ticket = __atomic_fetch_add(&propertyReports, 1U, __ATOMIC_RELAXED);
        if (ticket < 128 || !(ticket & 63))
            IOLog("PICASSO_BRIGHTNESS properties ticket=%u dictionary=%u keys=%u\n", ticket, dict != nullptr, dict ? dict->getCount() : 0);
        if (!dict) return kIOReturnBadArgument;
        // CoreBrightness's DisplayBrightness/DisplayBrightnessSliderPosition
        // properties publish derived status. With no calibrated nits curve they
        // can stay zero while the actual brightness command changes. Never use
        // that telemetry as a second actuator (build170 incorrectly requested off).
        if (dict->getObject("DisplayBrightness")) return kIOReturnUnsupported;
        auto *number = OSDynamicCast(OSNumber, dict->getObject("brightness"));
        auto *period = OSDynamicCast(OSNumber, dict->getObject("FadePeriod"));
        auto *panel = OSDynamicCast(OSNumber, dict->getObject("backlight-level"));
        auto integer = [](OSNumber *n, uint64_t maximum) {
            return n && (n->numberOfBits() == 8 || n->numberOfBits() == 16 ||
                n->numberOfBits() == 32 || n->numberOfBits() == 64) &&
                n->unsigned64BitValue() <= maximum;
        };
        if (!integer(number, picasso_backlight::max_level) ||
            (dict->getObject("FadePeriod") && !integer(period, UINT32_MAX)) ||
            (dict->getObject("backlight-level") && !integer(panel, picasso_backlight::max_level)) ||
            dict->getCount() != 1U + (period ? 1U : 0U) + (panel ? 1U : 0U))
            return kIOReturnBadArgument;
        uint32_t raw = uint32_t(number->unsigned64BitValue());
        // CoreBrightness also uses -1.0 (0xffff0000) for an immediate update.
        uint32_t fixed = period ? uint32_t(period->unsigned64BitValue()) : 0;
        uint32_t ms = (fixed & 0x80000000U) ? 0 : uint32_t((uint64_t(fixed) * 1000) >> 16);
        if (ms > 5000) ms = 5000;
        if (!requestLock) return kIOReturnNotReady;
        IOLockLock(requestLock);
        IOReturn result = kIOReturnNotReady;
        if (__atomic_load_n(&accepting, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&mailbox, (1ULL << 63) | (uint64_t(picasso_backlight::clamp(uint32_t(raw))) << 16) | ms, __ATOMIC_RELEASE);
            // Acknowledges a queued request, not a completed hardware write.
            // Never synchronously runAction on backboardd's brightness thread.
            result = requestTimer->setTimeoutUS(1);
            if (result) __atomic_store_n(&mailbox, 0ULL, __ATOMIC_RELEASE);
        }
        IOLockUnlock(requestLock);
        if (result || ticket < 128 || !(ticket & 63))
            IOLog("PICASSO_BRIGHTNESS queued=%u target=%u fade_ms=%u error=%x protocol=brightness\n", result == 0,
                picasso_backlight::clamp(uint32_t(raw)), uint32_t(ms), result);
        return result;
    }
    void stop(IOService *provider) override { quiesce(); IOService::stop(provider); }
    void free() override {
        quiesce();
        __atomic_store_n(&compatible, false, __ATOMIC_RELEASE);
        OSSafeReleaseNULL(requestTimer); OSSafeReleaseNULL(fadeTimer); OSSafeReleaseNULL(loop);
        if (requestLock) { IOLockFree(requestLock); requestLock = nullptr; }
        IOService::free();
    }
};

// Same metaclass override as the reference's arm64e path, also safe on arm64.
// This supplies the OS property protocol; it is not an Apple hardware instance.
PicassoBrightness::MetaClass PicassoBrightness::gMetaClass;
const OSMetaClass * const PicassoBrightness::metaClass = &PicassoBrightness::gMetaClass;
const OSMetaClass * const PicassoBrightness::superClass = &IOService::gMetaClass;
PicassoBrightness::PicassoBrightness(const OSMetaClass *meta) : IOService(meta) {}
PicassoBrightness::~PicassoBrightness() {}
const OSMetaClass *PicassoBrightness::getMetaClass() const {
    return __atomic_load_n(&compatible, __ATOMIC_ACQUIRE) && appleClass ? appleClass : &gMetaClass;
}
OSMetaClassConstructorInit(PicassoBrightness, IOService, )
OSDefineDefaultStructors(PicassoBrightness, IOService)

extern "C" IOService *picasso_brightness_allocate() { return new PicassoBrightness; }
