// SPDX-License-Identifier: BSD-2-Clause-Patent
// Empty IOAcceleratorES for the firmware-backed CPU display.
//
// backboardd matches IOAcceleratorES before CoreAnimation composes a frame;
// with no such service the UI never draws, even though the GOP framebuffer
// works. Picasso has no Adreno backend yet, so this stub owns the name: it
// answers every user-client call with success and zeroed outputs and
// advertises no MetalPluginName, which leaves CoreAnimation on its software
// path. The same approach brought mondrian to the iOS main UI.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <sys/proc.h>

class PicassoAcceleratorUserClient : public IOUserClient {
    OSDeclareDefaultStructors(PicassoAcceleratorUserClient);
public:
    IOReturn clientClose() override;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
        IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) override;
};

OSDefineMetaClassAndStructors(PicassoAcceleratorUserClient, IOUserClient);

IOReturn PicassoAcceleratorUserClient::clientClose() {
    terminate();
    return kIOReturnSuccess;
}

IOReturn PicassoAcceleratorUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *, OSObject *, void *) {
    static unsigned calls;
    for (uint32_t i = 0; i < args->scalarOutputCount && i < 16; i++) args->scalarOutput[i] = 0;
    if (args->structureOutput && args->structureOutputSize) bzero(args->structureOutput, args->structureOutputSize);
    // The first selectors are enough to see what the software path still asks for.
    if (__atomic_fetch_add(&calls, 1, __ATOMIC_RELAXED) < 24) {
        char name[32] = "?";
        proc_selfname(name, sizeof(name));
        IOLog("PICASSO_ACCEL %s selector=%u in=%u out=%u answered by the empty accelerator\n",
            name, selector, args->scalarInputCount, args->scalarOutputCount);
    }
    return kIOReturnSuccess;
}

class IOAcceleratorES : public IOService {
    OSDeclareDefaultStructors(IOAcceleratorES);
public:
    bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

bool IOAcceleratorES::start(IOService *provider) {
    if (!IOService::start(provider)) return false;
    setProperty(kIOUserClientClassKey, "PicassoAcceleratorUserClient");
    setProperty("IOAcceleratorStub", kOSBooleanTrue);
    registerService();
    return true;
}

extern "C" IOService *picasso_accelerator_allocate(void) {
    return OSTypeAlloc(IOAcceleratorES);
}

// A fixed IOPMPowerSource: a full battery on a USB-C charger. powerd finds
// every IOPMPowerSource and builds the power sources list from its
// properties; with none, iOS takes the phone for one on battery, and
// SBDontDimOrLockOnAC (set for a board without input) never applies, so the
// lock screen goes dark. The battery itself is not read: Picasso has no fuel
// gauge driver yet. Shape after HackiOS's HakiosBattery.
class PicassoPowerSource : public IOPMPowerSource {
    OSDeclareDefaultStructors(PicassoPowerSource);
public:
    void publishFixedState();
private:
    void number(const char *key, unsigned long long value);
};

OSDefineMetaClassAndStructors(PicassoPowerSource, IOPMPowerSource);

void PicassoPowerSource::number(const char *key, unsigned long long value) {
    const OSSymbol *symbol = OSSymbol::withCString(key);
    OSNumber *n = OSNumber::withNumber(value, 32);
    if (symbol && n) setPSProperty(symbol, n);
    OSSafeReleaseNULL(n);
    OSSafeReleaseNULL(symbol);
}

void PicassoPowerSource::publishFixedState() {
    setBatteryInstalled(true);
    setExternalConnected(true);
    setExternalChargeCapable(true);
    setIsCharging(false);
    const OSSymbol *full = OSSymbol::withCString("FullyCharged");
    if (full) setPSProperty(full, kOSBooleanTrue);
    OSSafeReleaseNULL(full);
    setCurrentCapacity(100);
    setMaxCapacity(100);
    setVoltage(4400);
    setAmperage(0);
    number("AppleRawCurrentCapacity", 4500);
    number("AppleRawMaxCapacity", 4500);
    number("DesignCapacity", 4500);
    OSDictionary *adapter = OSDictionary::withCapacity(4);
    OSNumber *watts = OSNumber::withNumber(18ULL, 32), *family = OSNumber::withNumber(0xe0004009ULL, 32);  // USB-C
    if (adapter && watts && family) {
        adapter->setObject(kIOPMPSAdapterDetailsWattsKey, watts);
        adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, family);
        adapter->setObject("IsWireless", kOSBooleanFalse);
        const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
        if (key) setPSProperty(key, adapter);
        OSSafeReleaseNULL(key);
    }
    OSSafeReleaseNULL(watts); OSSafeReleaseNULL(family); OSSafeReleaseNULL(adapter);
    updateStatus();
}

extern "C" bool picasso_power_source_publish(void) {
    IOService *platform = IOService::getPlatform();
    PicassoPowerSource *source = OSTypeAlloc(PicassoPowerSource);
    if (!platform || !source || !source->init()) { OSSafeReleaseNULL(source); return false; }
    source->publishFixedState();  // the properties are there before anybody finds the service
    bool attached = source->attach(platform);
    if (attached) source->setName("PicassoPowerSource");
    bool started = attached && source->start(platform);
    if (started) source->registerService();
    else if (attached) source->detach(platform);
    OSSafeReleaseNULL(source);  // the registry keeps a published service
    return started;
}
