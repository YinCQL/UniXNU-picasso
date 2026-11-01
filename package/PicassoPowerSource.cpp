// SPDX-License-Identifier: MIT
// Static battery source for the first userspace bring-up.
//
// SpringBoard looks up IOPMPowerSource and AppleHDQGasGaugeControl while
// initialising its power UI. Picasso does not yet have a working PMIC fuel
// gauge driver, so publish a complete, deterministic AC-powered source. This
// keeps shutdown policy away from zero/uninitialised battery state while the
// display and service startup path is qualified. It is not a battery driver.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

class PicassoPowerSource : public IOPMPowerSource {
  OSDeclareDefaultStructors(PicassoPowerSource)
public:
  bool start(IOService *provider) override;
};

OSDefineMetaClassAndStructors(PicassoPowerSource, IOPMPowerSource)

static void putNumber(IOPMPowerSource *source, const char *key, uint32_t value)
{
  const OSSymbol *symbol = OSSymbol::withCString(key);
  OSNumber *number = OSNumber::withNumber(value, 32);
  if (symbol != nullptr && number != nullptr) {
    source->setProperty(symbol, number);
  }
  if (number != nullptr) number->release();
  if (symbol != nullptr) symbol->release();
}

static void putBool(IOPMPowerSource *source, const char *key, bool value)
{
  const OSSymbol *symbol = OSSymbol::withCString(key);
  if (symbol != nullptr) {
    source->setProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
    symbol->release();
  }
}

bool PicassoPowerSource::start(IOService *provider)
{
  if (!IOPMPowerSource::start(provider)) {
    return false;
  }
  setName("picasso-power-source");

  // A complete source is published before registerService wakes powerd.
  setBatteryInstalled(true);
  setCurrentCapacity(5020);
  setMaxCapacity(5020);
  setVoltage(4200);
  setAmperage(0);
  setTimeRemaining(-1);
  setCycleCount(0);
  setExternalConnected(true);
  setExternalChargeCapable(true);
  setIsCharging(false);
  setAtWarnLevel(false);
  setAtCriticalLevel(false);

  putNumber(this, "AppleRawCurrentCapacity", 5020);
  putNumber(this, "AppleRawMaxCapacity", 5020);
  putNumber(this, "DesignCapacity", 5020);
  putNumber(this, "InstantAmperage", 0);
  putNumber(this, "Temperature", 2500);
  putBool(this, "FullyCharged", true);
  putBool(this, "AppleRawExternalConnected", true);
  putBool(this, "PicassoPowerSourceFallback", true);

  OSDictionary *legacy = OSDictionary::withCapacity(6);
  if (legacy != nullptr) {
    const char *keys[] = {
      kIOBatteryCurrentChargeKey, kIOBatteryCapacityKey, kIOBatteryVoltageKey,
      kIOBatteryAmperageKey, kIOBatteryCycleCountKey, kIOBatteryFlagsKey,
    };
    const uint32_t values[] = {
      5020, 5020, 4200, 0, 0,
      kIOBatteryInstalled | kIOBatteryChargerConnect,
    };
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
      OSNumber *number = OSNumber::withNumber(values[i], 32);
      if (number != nullptr) {
        legacy->setObject(keys[i], number);
        number->release();
      }
    }
    setLegacyIOBatteryInfo(legacy);
    legacy->release();
  }

  updateStatus();
  registerService(kIOServiceSynchronous);
  IOLog("picasso-power: static IOPMPowerSource published, 100%% AC, not charging\n");
  return true;
}

extern kmod_info_t g_kernel_kmod_info;
extern "C" void picasso_power_source_static_init(void)
    __asm__("__GLOBAL__sub_I_PicassoPowerSource.cpp");

extern "C" void picasso_power_source_run(void)
{
  static bool attempted;
  if (attempted) {
    return;
  }
  attempted = true;

  IOService *resources = IOService::getResourceService();
  if (resources == nullptr) {
    IOLog("picasso-power: IOResources unavailable\n");
    return;
  }

  const OSSymbol *name = OSSymbol::withCString("PicassoPowerSource");
  if (name == nullptr) {
    IOLog("picasso-power: class name allocation failed\n");
    return;
  }
  const OSMetaClass *registered = OSMetaClass::getMetaClassWithName(name);
  name->release();
  if (registered == nullptr) {
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (handle == nullptr) {
      IOLog("picasso-power: class registration begin failed\n");
      return;
    }
    picasso_power_source_static_init();
    if (OSMetaClass::postModLoad(handle) != kOSReturnSuccess ||
        PicassoPowerSource::gMetaClass.getSuperClass() != &IOPMPowerSource::gMetaClass) {
      IOLog("picasso-power: class registration failed\n");
      return;
    }
  }

  PicassoPowerSource *source = new PicassoPowerSource;
  if (source == nullptr || !source->init() ||
      !source->attach(resources) || !source->start(resources)) {
    IOLog("picasso-power: service publication failed\n");
    if (source != nullptr) source->release();
    return;
  }
  // Platform-lifetime service; retain it after successful registration.
}
