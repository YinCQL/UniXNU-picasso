// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../ArmCpu/ArmCpuSpi.h"
#include "RuntimeInterruptWait.h"
extern "C" IOReturn armcpu_spi_attach (uint32_t, bool, void (*) (void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" IOReturn armcpu_spi_mask (uint32_t, bool) __attribute__((weak_import));
// A bring-up caller may run before ArmCpu publishes GIC readiness. Retry
// on a separate stack; never hold up root publication or an I/O caller.
static bool gIrqAttached = false;
static IOReturn runtimeTryInterrupt (void)
{
  RuntimeSubmitGuard guard;
  if (__atomic_load_n (&gHost.poisoned, __ATOMIC_ACQUIRE)) return kIOReturnIOError;
  if (__atomic_load_n (&gOutstanding, __ATOMIC_ACQUIRE) || read32 (gHost.hc, 0x58)) return kIOReturnBusy;
  if (!gIrqAttached) {
    uint32_t readback = 0; uint64_t router = 0;
    IOReturn result = armcpu_spi_attach (297, false, runtimeInterrupt, nullptr, &readback, &router);
    IOLog ("NATIVE_UFS_IRQ attach result=%08x readback=%x router=%llx\n", static_cast<unsigned> (result), readback, router);
    if (result != kIOReturnSuccess) return result;
    gIrqAttached = true;
    result = armcpu_spi_mask (297, true);
    if (result != kIOReturnSuccess) return result;
    if (!ArmCpuSpiLevelConfigured (readback)) return kIOReturnIOError;
  }
  hostWrite (0x20, 1); // no in-flight command; discard only stale completion
  __atomic_store_n (&gIrqMode, true, __ATOMIC_RELEASE);
  hostWrite (0x24, 1);
  IOReturn result = armcpu_spi_mask (297, false);
  if (result != kIOReturnSuccess) {
    hostWrite (0x24, 0); __atomic_store_n (&gIrqMode, false, __ATOMIC_RELEASE); return result;
  }
  IOLog ("NATIVE_UFS_IRQ enabled intid=297 spin_us=%u\n", gSpinUs);
  return kIOReturnSuccess;
}
static void runtimeInterruptThread (void *, wait_result_t)
{
  IOReturn result = kIOReturnNotReady;
  for (unsigned attempt = 0; attempt < 200; ++attempt) {
    result = runtimeTryInterrupt ();
    if (result == kIOReturnSuccess || (result != kIOReturnNotReady && result != kIOReturnBusy)) break;
    IOSleep (10);
  }
  if (result != kIOReturnSuccess) IOLog ("NATIVE_UFS_IRQ fallback result=%08x mode=polling\n", static_cast<unsigned> (result));
  thread_terminate (current_thread ());
}
static void runtimeReportLink (void);
static void runtimeStatsThread (void *, wait_result_t)
{
  for (unsigned sample=0;sample<8;++sample) { IOSleep (15000); rtLogRuntimeStats (); }
  thread_terminate (current_thread ());
}
static void runtimeStartStats (void)
{
  thread_t thread=nullptr;
  const kern_return_t result=kernel_thread_start (runtimeStatsThread, nullptr, &thread);
  if (result==KERN_SUCCESS) thread_deallocate (thread);
  IOLog ("NATIVE_UFS_STATS scheduled result=%d samples=8 interval_ms=15000\n", result);
}
static void runtimeEnableInterrupt (void)
{
  runtimeReportLink ();
  uint32_t direct = 1;
  PE_parse_boot_argn ("native-ufs-direct", &direct, sizeof (direct));
  gZeroCopy = direct != 0;
  IOLog ("NATIVE_UFS_DMA direct=%u prdt_scratch=per_slot bytes=%u\n", gZeroCopy, static_cast<unsigned> (sizeof (gSlotPrds[0])));
  uint32_t enabled = 1;
  PE_parse_boot_argn ("native-ufs-irq", &enabled, sizeof (enabled));
  PE_parse_boot_argn ("native-ufs-spin-us", &gSpinUs, sizeof (gSpinUs));
  if (gSpinUs > 10000) gSpinUs = 10000;
  nanoseconds_to_absolutetime (uint64_t (gSpinUs) * 1000, &gSpinTicks);
  if (!enabled || !&armcpu_spi_attach || !&armcpu_spi_mask) {
    IOLog ("NATIVE_UFS_IRQ disabled requested=%u api=%u/%u\n", enabled, &armcpu_spi_attach != nullptr, &armcpu_spi_mask != nullptr); return;
  }
  IOReturn result = runtimeTryInterrupt ();
  if (result == kIOReturnSuccess) return;
  if (result != kIOReturnNotReady && result != kIOReturnBusy) {
    IOLog ("NATIVE_UFS_IRQ fallback result=%08x mode=polling\n", static_cast<unsigned> (result)); return;
  }
  thread_t thread = nullptr;
  kern_return_t dispatched = kernel_thread_start (runtimeInterruptThread, nullptr, &thread);
  if (dispatched == KERN_SUCCESS) thread_deallocate (thread);
  IOLog ("NATIVE_UFS_IRQ deferred result=%08x thread=%d max_attempts=200 interval_ms=10\n", static_cast<unsigned> (result), dispatched);
}

static bool rtReachable (uint64_t phys, uint64_t bytes, bool wide)
{
  if (!phys || (!wide && (phys >= 0x100000000ULL || bytes > 0x100000000ULL - phys))) return false;
  const uint32_t id = read32 (gHost.smmu, 0x28);
  for (unsigned shift = 0; shift <= 8; shift += 4) {
    unsigned bits = ufsSmmuAddressBits ((id >> shift) & 15);
    if (!bits || phys >= (1ULL << bits) || bytes > (1ULL << bits) - phys) return false;
  }
  return true;
}
static uint64_t rtDmaAddress (const void *va)
{
  const uint64_t pa = ml_vtophys (reinterpret_cast<vm_offset_t> (va));
  return rtReachable (pa, 4096, (read32 (gHost.hc, 0) & 0x01000000) != 0) ? pa : 0;
}
// UIC has its own command registers and mutex; ordinary data submissions
// never wait for the diagnostic poll. Only the short dispatch uses submitLock.
#include "RuntimeDme.h"
static IOLock *gUicLock = nullptr;
static bool gUicPoisoned = false;
struct RuntimeDmePort {
  bool canQuery () { return preemption_enabled () && !panic_active () && gUicLock && gSubmitLock; }
  bool enter () { return IOLockTryLock (gUicLock); }
  void leave () { IOLockUnlock (gUicLock); }
  bool submit () { return IOLockTryLock (gSubmitLock); }
  void unsubmit () { IOLockUnlock (gSubmitLock); }
  bool usable () { return __atomic_load_n (&gHost.ready, __ATOMIC_ACQUIRE) &&
    !__atomic_load_n (&gHost.poisoned, __ATOMIC_ACQUIRE) && !gUicPoisoned; }
  bool idle () { return !__atomic_load_n (&gOutstanding, __ATOMIC_ACQUIRE) &&
    !read32 (gHost.hc, 0x58) && !(read32 (gHost.hc, 0x24) & (1U << 10)); }
  uint32_t read (unsigned at) { return read32 (gHost.hc, at); }
  void write (unsigned at, uint32_t value) { hostWrite (at, value); }
  uint64_t now () { return mach_absolute_time (); }
  uint64_t deadline () { uint64_t end=0; clock_interval_to_deadline (500000, kMicrosecondScale, &end); return end; }
  void pause () { IOSleep (1); }
  void poison () { gUicPoisoned = true; }
};
extern "C" int native_ufs_dme_get (uint32_t attribute, uint32_t selector, uint32_t *value)
{
  RuntimeDmePort port;
  return ufsDmeGet (port, attribute, selector, value) == UfsDmeResult::Success;
}
// A single initial snapshot runs before root publication or competing I/O.
// Failed diagnostic reads never prevent root bring-up or alter the link.
static void runtimeReportLink (void)
{
  uint32_t txGear=~0U, rxGear=~0U, txLanes=~0U, rxLanes=~0U, series=~0U, power=~0U;
  unsigned valid=0;
  const uint32_t attrs[] = {0x1568,0x1583,0x1560,0x1580,0x156a,0x1571};
  uint32_t *const values[] = {&txGear,&rxGear,&txLanes,&rxLanes,&series,&power};
  for (unsigned i=0;i<6;++i) {
    if (native_ufs_dme_get (attrs[i], 0, values[i])) valid |= 1U << i;
    if (gUicPoisoned) break;
  }
  IOLog ("NATIVE_UFS_LINK valid=%x tx_gear=%u rx_gear=%u tx_lanes=%u rx_lanes=%u hs_series=%u power=%x uic_poison=%u readonly=1\n",
    valid,txGear,rxGear,txLanes,rxLanes,series,power,gUicPoisoned);
}

static IOBufferMemoryDescriptor *gListDma = nullptr, *gRegionB = nullptr;
static OSSharedPtr<IOMemoryMap> gListMap;
static IOBufferMemoryDescriptor *rtContiguous (uint32_t bytes, bool wide, uint8_t **virt, uint64_t *phys, IOOptionBits cache = 0)
{
  auto *dma = IOBufferMemoryDescriptor::inTaskWithOptions (kernel_task,
    static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone) | cache, bytes, 4096);
  if (!dma) return nullptr;
  IOByteCount contiguous = 0;
  *virt = static_cast<uint8_t *> (dma->getBytesNoCopy ());
  *phys = dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  if (!*virt || (*phys & 4095) || contiguous < bytes || !rtReachable (*phys, bytes, wide)) { dma->release (); return nullptr; }
  bzero (*virt, bytes); cacheCivac (*virt, bytes); return dma;
}
static bool runtimeSlotsPrepare (unsigned count, unsigned entries, bool wide)
{
  gSlotLock = IOLockAlloc (); gWriteLock = IOLockAlloc (); gSubmitLock = IOLockAlloc ();
  gUicLock = IOLockAlloc ();
  if (!gSlotLock || !gWriteLock || !gSubmitLock) return false;
  rtSlotGeometryRegionA (count);
  uint8_t *cached = nullptr; uint64_t listPhys = 0;
  auto *list = count > 1 ? rtContiguous (4096, wide, &cached, &listPhys, kIOMapWriteCombineCache) : nullptr;
  if (list) {
    OSSharedPtr<IOMemoryMap> map (list->createMappingInTask (kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
    auto *v = map ? reinterpret_cast<uint8_t *> (map->getVirtualAddress ()) : nullptr;
    if (v) {
      bzero (v, 4096); __asm__ volatile ("dsb sy" ::: "memory");
      gListDma = list; gListMap = map; gListUncached = true; gUtrlPhys = listPhys;
      for (unsigned e = 0; e < 32; ++e) gEntrySlot[e] = -1;
      for (unsigned s = 0; s < count; ++s)
        rtSlotGeometry (s, v + 32 * s, s, gHost.virt + ufsSlotUcdOffset (s), gHost.phys + ufsSlotUcdOffset (s),
                        gHost.virt + ufsSlotDataOffset (s), gHost.phys + ufsSlotDataOffset (s));
      uint8_t *b = nullptr; uint64_t bp = 0;
      if (count == 16 && entries >= 32 && (gRegionB = rtContiguous (kRegionBBytes, wide, &b, &bp))) {
        for (unsigned s = 0; s < 16; ++s)
          rtSlotGeometry (16 + s, v + 32 * (16 + s), 16 + s, b + s * kUcdStride, bp + s * kUcdStride,
                          b + kRegionBDataOffset + s * kDataMax, bp + kRegionBDataOffset + s * kDataMax);
        count = 32;
      }
    } else list->release ();
  }
  gSlotCount = count;
  // Leave one descriptor unused for an eventual dedicated panic writer.
  // There is no forced reuse of a normal request, even in single-slot mode.
  gSlotFree = count > 1 ? (1U << (count - 1)) - 1 : 1;
  IOLog ("NATIVE_UFS_SLOTS count=%u usable=%u list_wc=%u\n", count, count > 1 ? count - 1 : 1, gListUncached);
  return true;
}
