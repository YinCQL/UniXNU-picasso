// SPDX-License-Identifier: MIT
// Volatile boot-stage storage for the original watchdogd's PMU property API.
// This is a software service, not a PMIC, battery or power-control driver.
// Values are held in the registry for this boot only; other operations fail.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/task.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>

class PicassoBootState : public IOService {
  OSDeclareDefaultStructors(PicassoBootState)
private:
  uint32_t stageLogCount = 0;
public:
  bool start(IOService *provider) override;
  IOReturn setProperties(OSObject *properties) override;
};

OSDefineMetaClassAndStructors(PicassoBootState, IOService)

bool PicassoBootState::start(IOService *provider)
{
  if (!IOService::start(provider)) {
    return false;
  }
  setName("picasso-boot-state");
  if (!setProperty("IOPMUPrimary", 1ULL, 32) ||
      !setProperty("IOPMUBootStage", 0ULL, 32) ||
      !setProperty("PicassoBootStageStorage", "volatile-software") ||
      !setProperty("HardwarePMUImplemented", false)) {
    IOService::stop(provider);
    return false;
  }
  registerService(kIOServiceSynchronous);
  return true;
}

IOReturn PicassoBootState::setProperties(OSObject *properties)
{
  IOReturn privilege = IOUserClient::clientHasPrivilege(
      current_task(), kIOClientPrivilegeAdministrator);
  if (privilege != kIOReturnSuccess) {
    return privilege;
  }
  OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);
  if (dictionary == nullptr) {
    return kIOReturnBadArgument;
  }
  if (dictionary->getCount() != 1 || dictionary->getObject("IOPMUBootStage") == nullptr) {
    return kIOReturnUnsupported;
  }
  OSNumber *stage = OSDynamicCast(OSNumber, dictionary->getObject("IOPMUBootStage"));
  if (stage == nullptr) {
    return kIOReturnBadArgument;
  }
  // IORegistryEntry installs a retained value under its property lock. Store
  // an independent number, so changing the caller's object cannot alter it.
  const uint64_t value = stage->unsigned64BitValue();
  if (value > 255) {
    return kIOReturnBadArgument;
  }
  if (!setProperty("IOPMUBootStage", value, 32)) {
    return kIOReturnNoMemory;
  }
  uint32_t expected = 0;
  if (value == 255 && __atomic_compare_exchange_n(&stageLogCount, &expected, 1, false,
                                                  __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    IOLog("picasso-boot-state: watchdogd boot stage=255 stored in volatile registry\n");
  }
  return kIOReturnSuccess;
}

extern kmod_info_t g_kernel_kmod_info;
extern "C" void picasso_boot_state_static_init(void)
    __asm__("__GLOBAL__sub_I_PicassoBootState.cpp");

static OSDictionary *primaryBootStateMatching(void)
{
  OSDictionary *matching = IOService::serviceMatching("IOService");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSNumber *primary = OSNumber::withNumber(1, 32);
  bool valid = matching && properties && primary &&
      properties->setObject("IOPMUPrimary", primary) &&
      matching->setObject("IOPropertyMatch", properties);
  if (primary) primary->release();
  if (properties) properties->release();
  if (!valid) {
    if (matching) matching->release();
    panic("picasso-boot-state: cannot allocate primary matching dictionary");
  }
  return matching;
}

extern "C" void picasso_boot_state_run(void)
{
  static bool attempted;
  if (attempted) {
    return;
  }
  attempted = true; // Called once, on the serial BSD initialization path.
  OSDictionary *matching = primaryBootStateMatching();
  IOService *existing = IOService::copyMatchingService(matching);
  matching->release();
  if (existing) {
    existing->release();
    IOLog("picasso-boot-state: existing primary service retained\n");
    return;
  }
  const OSSymbol *name = OSSymbol::withCString("PicassoBootState");
  if (name == nullptr) {
    panic("picasso-boot-state: cannot allocate class name");
  }
  const OSMetaClass *early = OSMetaClass::getMetaClassWithName(name);
  name->release();
  if (early) {
    panic("picasso-boot-state: unexpected automatic class initialization");
  }
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (handle == nullptr) {
    panic("picasso-boot-state: cannot begin class registration");
  }
  picasso_boot_state_static_init();
  if (OSMetaClass::postModLoad(handle) != kOSReturnSuccess ||
      PicassoBootState::gMetaClass.getSuperClass() != &IOService::gMetaClass) {
    panic("picasso-boot-state: class registration failed");
  }
  IOService *platform = IOService::getPlatform();
  PicassoBootState *service = new PicassoBootState;
  if (platform == nullptr || service == nullptr || !service->init() ||
      !service->attach(platform) || !service->start(platform)) {
    panic("picasso-boot-state: volatile boot-stage service could not start");
  }
  matching = primaryBootStateMatching();
  IOService *found = IOService::copyMatchingService(matching);
  matching->release();
  bool matched = found == service;
  if (found) found->release();
  if (!matched) {
    panic("picasso-boot-state: primary property matching failed");
  }
  // Deliberately retain this platform-lifetime service and its stored value.
  IOLog("picasso-boot-state: primary boot-stage API published, storage=volatile-software\n");
}
