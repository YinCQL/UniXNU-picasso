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
#include <libkern/c++/OSSerialize.h>
#include <string.h>
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
    // OSNumber's 31/63-bit tags contain an IEEE binary64 value. The kernel
    // build forbids FP registers: obtain the documented serialization instead
    // of calling unsigned64BitValue (which truncates 0..1 to zero) or touching
    // OSNumber private layout. Only one number is serialized, never a graph.
    static IOReturn normalizedLevel(OSNumber *number, uint32_t *value) {
        if (!number) return kIOReturnBadArgument;
        unsigned bits = number->numberOfBits();
        if (bits != 31 && bits != 63) {
            uint64_t integer = number->unsigned64BitValue();
            if (integer > 1) return kIOReturnBadArgument;
            *value = uint32_t(integer) * picasso_backlight::max_level;
            return kIOReturnSuccess;
        }
        OSSerialize *serialized = OSSerialize::binaryWithCapacity(32);
        if (!serialized) return kIOReturnNoMemory;
        uint32_t words[4] = {};
        bool ok = number->serialize(serialized) && serialized->getLength() == sizeof(words);
        if (ok) memcpy(words, serialized->text(), sizeof(words));
        serialized->release();
        // OSSerializeBinary.h: signature, end-of-collection | number | bit count.
        if (!ok || words[0] != 0xd3 || words[1] != (0x84000000U | bits))
            return kIOReturnBadArgument;
        uint64_t ieee = uint64_t(words[2]) | (uint64_t(words[3]) << 32);
        if (!picasso_backlight::normalized(ieee, value)) return kIOReturnBadArgument;
        return kIOReturnSuccess;
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
        if (ticket < 8) {
            auto *display = OSDynamicCast(OSDictionary, dict->getObject("DisplayBrightness"));
            auto *brightness = display ? OSDynamicCast(OSNumber, display->getObject("Brightness")) : nullptr;
            if (display) IOLog("PICASSO_BRIGHTNESS normalized fields=%u number=%u bits=%u\n",
                display->getCount(), brightness != nullptr, brightness ? brightness->numberOfBits() : 0);
        }
        uint32_t raw = 0, ms = 0;
        const bool normalized = dict->getObject("DisplayBrightness") != nullptr;
        if (normalized) {
            // 20H392 __DisplayUpdateSlider publishes this dictionary through
            // IORegistryEntrySetCFProperty. Brightness is the normalized slider
            // position; Nits/EDR fields are telemetry, not WLED register values.
            if (dict->getCount() != 1) return kIOReturnBadArgument;
            auto *display = OSDynamicCast(OSDictionary, dict->getObject("DisplayBrightness"));
            auto *number = display ? OSDynamicCast(OSNumber, display->getObject("Brightness")) : nullptr;
            IOReturn error = normalizedLevel(number, &raw);
            if (error) return error;
        } else {
            auto *number = OSDynamicCast(OSNumber, dict->getObject("brightness"));
            auto *period = OSDynamicCast(OSNumber, dict->getObject("FadePeriod"));
            if (!number || number->numberOfBits() == 31 || number->numberOfBits() == 63 ||
                number->unsigned64BitValue() > picasso_backlight::max_level ||
                (dict->getObject("FadePeriod") && !period) ||
                (period && (period->numberOfBits() == 31 || period->numberOfBits() == 63 ||
                    period->unsigned64BitValue() > UINT32_MAX))) return kIOReturnBadArgument;
            raw = uint32_t(number->unsigned64BitValue());
            // CoreBrightness also uses -1.0 (0xffff0000) for an immediate update.
            uint32_t fixed = period ? uint32_t(period->unsigned64BitValue()) : 0;
            ms = (fixed & 0x80000000U) ? 0 : uint32_t((uint64_t(fixed) * 1000) >> 16);
            if (ms > 5000) ms = 5000;
        }
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
            IOLog("PICASSO_BRIGHTNESS queued=%u target=%u fade_ms=%u error=%x normalized=%u\n", result == 0,
                picasso_backlight::clamp(uint32_t(raw)), uint32_t(ms), result, normalized);
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
