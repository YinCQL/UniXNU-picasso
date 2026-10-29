// SPDX-License-Identifier: MIT
#ifndef PICASSO_WATCHDOG_ABI_H
#define PICASSO_WATCHDOG_ABI_H

#include <IOKit/IOService.h>

// Declaration of the retained 21H461 class, not a second implementation or
// metaclass registration. prepare-picasso-watchdog-bindings.py verifies the
// original image before resolving its structors, overrides and class data.
class IOWatchdog : public IOService {
  OSDeclareAbstractStructors(IOWatchdog)

private:
  // Accessed exclusively by the retained binary implementation.
  unsigned char originalPolicyState[0x70] __attribute__((unused));

public:
  void stop(IOService *provider) override;
  IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                        OSDictionary *properties,
                        LIBKERN_RETURNS_RETAINED IOUserClient **handler) override;

protected:
  // Local semantic names for the original pure virtual slots. The recovered
  // ABI is void(this), in this exact order; the original source names are not
  // known. Offsets from the instance vptr: 0x540, 0x548, 0x550.
  virtual void enableWatchdogHardware() = 0;
  virtual void disableWatchdogHardware() = 0;
  virtual void petWatchdogHardware() = 0;
};

static_assert(sizeof(IOService) == 0x88, "21H461 IOService layout changed");
static_assert(sizeof(IOWatchdog) == 0xf8, "21H461 IOWatchdog layout changed");

// Explicit machine-ABI aliases to nonvirtual original routines. At the sole
// original start call site, x2 is zero and is not read by the target routine.
extern "C" bool picasso_original_iowatchdog_policy_start(
    IOWatchdog *self, IOService *provider, uintptr_t unusedZero, uintptr_t flags);
extern "C" void picasso_original_iowatchdog_policy_check(IOWatchdog *self);

extern "C" void picasso_watchdog_run(void);

#endif
