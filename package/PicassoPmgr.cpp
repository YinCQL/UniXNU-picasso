// SPDX-License-Identifier: MIT
// Advertise an ApplePMGRNub to the unchanged AMFI.
//
// AMFI's ConfigurationSettings finds the power manager through
// IOService::serviceMatching("ApplePMGRNub") + waitForMatchingService, then ORs bit 2 into the
// always-on state it reads back. The target helpers behind that call load the provider from
// nub+0x88 and, when the field is NULL, return 0 / return without writing. Publishing a bare
// ApplePMGRNub therefore satisfies the latch without implementing Apple's PMGR and without
// patching AMFI: the target kernelcache stays unmodified.
//
// The call site is the sheng-platform-power.patch hook in bsd_init (CONFIG_XNU_SHENG_PMGR=1),
// which runs before AMFI's configuration settings are initialised.
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>

extern "C" void sheng_pmgr_run(void);

// Bring-up evidence for the boot-progress panel and the UFS log: 0 nothing tried, 1 class absent,
// 2 allocation failed, 3 published, 4 discovery check failed.
volatile uint32_t picasso_pmgr_step;
volatile uint32_t picasso_pmgr_class_size;
volatile uint64_t picasso_pmgr_nub;

static const OSMetaClass *nubMetaClass(void)
{
  const OSSymbol *name = OSSymbol::withCString("ApplePMGRNub");
  if (name == nullptr) {
    return nullptr;
  }
  const OSMetaClass *meta = OSMetaClass::getMetaClassWithName(name);
  name->release();
  return meta;
}

extern "C" void sheng_pmgr_run(void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn("sheng-pmgr", &mode, sizeof(mode)) || (mode == 0)) {
    return;
  }
  picasso_pmgr_step = 1;
  const OSMetaClass *meta = nubMetaClass();
  if (meta == nullptr) {
    // The prelinked kext may not have registered its classes yet at this point; load it explicitly
    // (a no-op when it is already loaded) and look the class up again.
    OSKext::loadKextWithIdentifier("com.apple.driver.ApplePMGR");
    meta = nubMetaClass();
  }
  if (meta == nullptr) {
    IOLog("picasso-pmgr: ApplePMGRNub class absent\n");
    return;
  }
  picasso_pmgr_class_size = meta->getClassSize();
  IOService *nub = static_cast<IOService *>(meta->alloc());
  if (nub == nullptr) {
    picasso_pmgr_step = 2;
    IOLog("picasso-pmgr: ApplePMGRNub allocation failed\n");
    return;
  }
  if (!nub->init()) {
    picasso_pmgr_step = 2;
    IOLog("picasso-pmgr: ApplePMGRNub init failed\n");
    nub->release();
    return;
  }
  IOService *platform = IOService::getPlatform();
  if ((platform != nullptr) && !nub->attach(platform)) {
    picasso_pmgr_step = 2;
    IOLog("picasso-pmgr: ApplePMGRNub attach failed\n");
    nub->release();
    return;
  }
  // No provider is set: the field AMFI dereferences stays NULL, which is the path that returns 0
  // instead of reaching into Apple's PMGR object.
  nub->registerService(kIOServiceSynchronous);
  picasso_pmgr_nub = reinterpret_cast<uint64_t>(nub);
  picasso_pmgr_step = 3;
  IOService *found = nullptr;
  OSDictionary *matching = IOService::serviceMatching("ApplePMGRNub");
  if (matching != nullptr) {
    // OSPtr is the raw pointer in this configuration; waitForMatchingService returns it retained.
    found = IOService::waitForMatchingService(matching, 1000000);
    matching->release();
  }
  picasso_pmgr_step = (found == nub) ? 4 : 3;
  IOLog("picasso-pmgr: ApplePMGRNub size=%u nub=%p found=%p\n", picasso_pmgr_class_size, nub, found);
  if (found != nullptr) {
    found->release();
  }
}
