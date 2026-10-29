// SPDX-License-Identifier: MIT
// Keep the original IOWatchdog policy, entitled UserClient and APFS global.
// Only its hardware backend is replaced with the SM7250 watchdog owner.
#include "PicassoWatchdogAbi.h"
#include "PicassoWatchdogBackend.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
// The builder removes this object's automatic __mod_init_func and exposes
// this exact compiler-generated initializer without changing its code.
void picasso_watchdog_static_init(void) __asm__("__GLOBAL__sub_I_PicassoWatchdog.cpp");
}

class PicassoWatchdog : public IOWatchdog {
  OSDeclareDefaultStructors(PicassoWatchdog)

private:
  IOWorkLoop *watchdogLoop = nullptr;
  IOTimerEventSource *watchdogTimer = nullptr;
  bool timerAdded = false;
  bool backendClaimed = false;
  bool policyReady = false;
  bool hardwareEnabled = false;
  uint64_t successfulChecks = 0;
  uint32_t clientOpenLogCount = 0;

  static void timerFired(OSObject *owner, IOTimerEventSource *sender);
  void scheduleNextCheck();

protected:
  void enableWatchdogHardware() override;
  void disableWatchdogHardware() override;
  void petWatchdogHardware() override;

public:
  bool start(IOService *provider) override;
  void free() override;
  IOWorkLoop *getWorkLoop() const override;
  IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                        OSDictionary *properties,
                        LIBKERN_RETURNS_RETAINED IOUserClient **handler) override;
};

// Only the native subclass is registered. Its superclass is the original
// IOWatchdog::gMetaClass, supplied by the checked original-image binding.
OSDefineMetaClassAndStructors(PicassoWatchdog, IOWatchdog)

static PicassoWatchdog *gWatchdog;
static uint32_t gRunAttempted;

IOWorkLoop *PicassoWatchdog::getWorkLoop() const
{
  return watchdogLoop;
}

IOReturn PicassoWatchdog::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                      OSDictionary *properties, IOUserClient **handler)
{
  // Keep every original type/root/entitlement/exclusive-open result and the
  // returned original UserClient. This wrapper only records bounded evidence.
  IOReturn result = IOWatchdog::newUserClient(owningTask, securityID, type, properties, handler);
  uint32_t index = __atomic_fetch_add(&clientOpenLogCount, 1, __ATOMIC_RELAXED);
  if (index < 8) {
    IOUserClient *accepted = result == kIOReturnSuccess && handler ? *handler : nullptr;
    IOLog("picasso-watchdog: original newUserClient type=%u result=%08x accepted=%u\n",
          type, static_cast<unsigned>(result), accepted != nullptr);
  }
  return result;
}

void PicassoWatchdog::scheduleNextCheck()
{
  // Callers hold watchdogLoop's recursive gate. No timeout is queued until
  // all original state and locks are initialized and the backend is enabled.
  if (watchdogTimer->setTimeoutMS(1000) != kIOReturnSuccess) {
    panic("picasso-watchdog: cannot schedule policy check after ownership transfer");
  }
}

void PicassoWatchdog::enableWatchdogHardware()
{
  watchdogLoop->closeGate();
  if (!backendClaimed || !picasso_watchdog_backend_enable(this)) {
    panic("picasso-watchdog: hardware enable failed after ownership transfer");
  }
  hardwareEnabled = true;
  if (policyReady) {
    watchdogTimer->enable();
    scheduleNextCheck();
  }
  watchdogLoop->openGate();
}

void PicassoWatchdog::disableWatchdogHardware()
{
  watchdogLoop->closeGate();
  // The original base stop invokes this hook. Cancel under the same gate as
  // timerFired so no callback can pet after the disable. The permanent owner
  // remains installed; client close does not reach this path or restore USB.
  watchdogTimer->cancelTimeout();
  watchdogTimer->disable();
  if (!backendClaimed || !picasso_watchdog_backend_disable(this)) {
    panic("picasso-watchdog: hardware disable failed after ownership transfer");
  }
  hardwareEnabled = false;
  watchdogLoop->openGate();
}

void PicassoWatchdog::petWatchdogHardware()
{
  watchdogLoop->closeGate();
  if (!backendClaimed || !hardwareEnabled || !picasso_watchdog_backend_pet(this)) {
    panic("picasso-watchdog: hardware pet rejected after policy check");
  }
  watchdogLoop->openGate();
}

void PicassoWatchdog::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
  PicassoWatchdog *self = static_cast<PicassoWatchdog *>(owner);
  // IOTimerEventSource delivers this action with watchdogLoop's gate held.
  if (sender != self->watchdogTimer || !self->policyReady || !self->hardwareEnabled) {
    return;
  }

  // The original function never pets hardware. It retains the original
  // 600-second first deadline, 90-second checkin deadline, APFS defang and
  // shutdown policy. A panic never reaches the following hardware pet.
  picasso_original_iowatchdog_policy_check(self);
  self->petWatchdogHardware();
  ++self->successfulChecks;
  if (self->successfulChecks == 1) {
    IOLog("picasso-watchdog: first original policy check and hardware pet complete\n");
  }
  self->scheduleNextCheck();
}

bool PicassoWatchdog::start(IOService *provider)
{
  if (provider == nullptr || watchdogLoop != nullptr || backendClaimed) {
    return false;
  }
  watchdogLoop = IOWorkLoop::workLoop();
  if (watchdogLoop == nullptr) {
    return false;
  }
  watchdogTimer = IOTimerEventSource::timerEventSource(
      kIOTimerEventSourceOptionsPriorityKernelHigh, this, timerFired);
  if (watchdogTimer == nullptr ||
      watchdogLoop->addEventSource(watchdogTimer) != kIOReturnSuccess) {
    return false;
  }
  timerAdded = true;

  // No timer has been scheduled. Every native resource is ready before the
  // irreversible owner transfer and before original policy publishes us.
  watchdogLoop->closeGate();
  if (!picasso_watchdog_backend_claim(this)) {
    watchdogLoop->openGate();
    return false;
  }
  backendClaimed = true;

  // These are the two flag inputs at the original Apple call site. The native
  // provider is the real platform, with no fabricated Apple DT/MMIO property.
  uintptr_t flags = PE_i_can_has_debugger(nullptr) ? 1 : 0;
  if (provider->getProperty("extended-iokit-quiesce-timeout") == nullptr) {
    flags |= 2;
  }
  if (!picasso_original_iowatchdog_policy_start(this, provider, 0, flags)) {
    panic("picasso-watchdog: original policy start failed after ownership transfer");
  }
  if (!hardwareEnabled) {
    // An explicitly disabled original policy cannot satisfy this platform's
    // required watchdog owner. Do not turn it into unconditional boot pets.
    panic("picasso-watchdog: original policy did not enable required hardware");
  }
  policyReady = true;
  scheduleNextCheck();
  watchdogLoop->openGate();

  IOLog("picasso-watchdog: original IOWatchdog policy published, size=%lu flags=%lu; "
        "SM7250 owner active, hardware=15s policy=600/90s poll=1s\n",
        static_cast<unsigned long>(sizeof(IOWatchdog)), static_cast<unsigned long>(flags));
  // Current board boots cpus=1. A stalled timer/CPU stops hardware feed. The
  // Apple per-CPU cross-call implementation is not compatible with SM7250;
  // this driver does not claim per-core hang detection for future SMP boots.
  return true;
}

void PicassoWatchdog::free()
{
  if (backendClaimed) {
    // The original global, UserClient and backend owner may reference this
    // instance for the rest of the boot. Successful starts keep their initial
    // reference forever; failed starts may be freed only before claim.
    panic("picasso-watchdog: attempted to free permanent hardware owner");
  }
  if (watchdogTimer != nullptr) {
    watchdogTimer->cancelTimeout();
    if (timerAdded) {
      watchdogLoop->removeEventSource(watchdogTimer);
    }
    watchdogTimer->release();
    watchdogTimer = nullptr;
  }
  if (watchdogLoop != nullptr) {
    watchdogLoop->release();
    watchdogLoop = nullptr;
  }
  IOWatchdog::free();
}

static const OSMetaClass *lookupMetaClass(const char *className)
{
  const OSSymbol *name = OSSymbol::withCString(className);
  if (name == nullptr) {
    return nullptr;
  }
  const OSMetaClass *meta = OSMetaClass::getMetaClassWithName(name);
  name->release();
  return meta;
}

static int countMatchingWatchdogs(IOService *expected)
{
  OSDictionary *matching = IOService::serviceMatching("IOWatchdog");
  if (matching == nullptr) {
    return -1;
  }
  OSIterator *iterator = IOService::getMatchingServices(matching);
  matching->release();
  if (iterator == nullptr) {
    return 0;
  }
  int count = 0;
  bool foundExpected = expected == nullptr;
  while (OSObject *object = iterator->getNextObject()) {
    ++count;
    foundExpected |= object == expected;
  }
  bool valid = iterator->isValid();
  iterator->release();
  return valid && foundExpected ? count : -1;
}

extern "C" void picasso_watchdog_run(void)
{
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&gRunAttempted, &expected, 1, false,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    return;
  }

  OSReturn loadResult = OSKextLoadKextWithIdentifier("com.apple.driver.AppleARMWatchdogTimer");
  if (loadResult != kOSReturnSuccess) {
    IOLog("picasso-watchdog: original module load failed result=%08x\n", static_cast<unsigned>(loadResult));
    return;
  }
  const OSMetaClass *meta = lookupMetaClass("IOWatchdog");
  const OSMetaClass *clientMeta = lookupMetaClass("IOWatchdogUserClient");
  if (meta != &IOWatchdog::gMetaClass || meta->getClassSize() != sizeof(IOWatchdog) ||
      meta->getSuperClass() != &IOService::gMetaClass || clientMeta == nullptr ||
      clientMeta->getClassSize() != 0xe8 || clientMeta->getSuperClass() != &IOUserClient::gMetaClass) {
    IOLog("picasso-watchdog: original IOWatchdog metaclass/size unavailable or mismatched\n");
    return;
  }
  if (countMatchingWatchdogs(nullptr) != 0) {
    IOLog("picasso-watchdog: existing or unenumerable watchdog service; refusing duplicate policy global\n");
    return;
  }
  if (lookupMetaClass("PicassoWatchdog") != nullptr) {
    panic("picasso-watchdog: native metaclass initialized before original parent was qualified");
  }
  void *loadHandle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (loadHandle == nullptr) {
    IOLog("picasso-watchdog: native preModLoad failed\n");
    return;
  }
  picasso_watchdog_static_init();
  OSReturn registerResult = OSMetaClass::postModLoad(loadHandle);
  if (registerResult != kOSReturnSuccess || lookupMetaClass("PicassoWatchdog") != &PicassoWatchdog::gMetaClass ||
      PicassoWatchdog::gMetaClass.getSuperClass() != meta) {
    IOLog("picasso-watchdog: native postModLoad/class validation failed result=%08x\n",
          static_cast<unsigned>(registerResult));
    return;
  }
  IOService *platform = IOService::getPlatform();
  if (platform == nullptr) {
    IOLog("picasso-watchdog: platform provider unavailable\n");
    return;
  }
  PicassoWatchdog *service = new PicassoWatchdog;
  if (service == nullptr) {
    IOLog("picasso-watchdog: allocation failed\n");
    return;
  }
  if (!service->init()) {
    service->release();
    IOLog("picasso-watchdog: initialization failed before hardware claim\n");
    return;
  }
  if (!service->attach(platform)) {
    service->release();
    IOLog("picasso-watchdog: attach failed before hardware claim\n");
    return;
  }
  if (!service->start(platform)) {
    service->detach(platform);
    service->release();
    IOLog("picasso-watchdog: timer setup or hardware claim failed\n");
    return;
  }
  gWatchdog = service; // Intentionally retain the initial reference permanently.

  OSDictionary *matching = IOService::serviceMatching("IOWatchdog");
  // Original policy uses registerService(0), whose matching job may still be
  // asynchronous. Wait outside the gate with the timer already running.
  IOService *found = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
  if (matching != nullptr) {
    matching->release();
  }
  bool matched = found == service;
  if (found != nullptr) {
    found->release();
  }
  if (!matched || countMatchingWatchdogs(service) != 1) {
    panic("picasso-watchdog: original IOWatchdog instance enumeration failed");
  }
  IOLog("picasso-watchdog: original IOWatchdog matching verified for native instance\n");
}
