// SPDX-License-Identifier: BSD-2-Clause-Patent
static uint32_t rtCompletionCount (unsigned slot) { return __atomic_load_n (&gSlotGen[slot], __ATOMIC_ACQUIRE); }
static bool rtInterruptMode (void) { return gIrqMode; }
static void rtWaitCompletion (unsigned slot, uint32_t seen, uint64_t started, uint64_t deadline)
{
  const uint64_t now = mach_absolute_time ();
  // Keep the board's qualified yield polling in bootstrap and when blocking
  // is unavailable. WFE would require a qualified periodic event source.
  if (!gIrqMode || !preemption_enabled () || now - started < gSpinTicks || now >= deadline) {
    ufsPollPause (hostCpuRelax); return;
  }
  uint64_t ns = 0;
  absolutetime_to_nanoseconds (deadline - now, &ns);
  uint32_t us = ns / 1000 > 1000 ? 1000 : static_cast<uint32_t> (ns / 1000);
  if (!us) { hostCpuRelax (); return; }
  assert_wait_timeout ((event_t) (uintptr_t) &gSlotGen[slot], THREAD_UNINT, us, NSEC_PER_USEC);
  if (rtCompletionCount (slot) != seen) { clear_wait (current_thread (), THREAD_AWAKENED); return; }
  __atomic_fetch_add (&gIrqSleeps, 1, __ATOMIC_RELAXED);
  thread_block (THREAD_CONTINUE_NULL);
}
static void rtWaitDone (uint64_t started) { __atomic_fetch_add (&gRuntimeTicks, mach_absolute_time () - started, __ATOMIC_RELAXED); }
static void runtimeInterrupt (void *)
{
  if (read32 (gHost.hc, 0x20) & 1) hostWrite (0x20, 1);
  __atomic_fetch_add (&gIrqs, 1, __ATOMIC_RELAXED);
  uint32_t done = __atomic_load_n (&gOutstanding, __ATOMIC_ACQUIRE) & ~read32 (gHost.hc, 0x58);
  while (done) {
    const unsigned entry = __builtin_ctz (done); done &= done - 1;
    const int slot = gEntrySlot[entry]; if (slot < 0) continue;
    __atomic_add_fetch (&gSlotGen[slot], 1, __ATOMIC_RELEASE);
    thread_wakeup ((event_t) (uintptr_t) &gSlotGen[slot]);
  }
}
extern "C" IOReturn armcpu_spi_attach (uint32_t, bool, void (*) (void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" IOReturn armcpu_spi_mask (uint32_t, bool) __attribute__((weak_import));
static void runtimeEnableInterrupt (void)
{
  // Qualify the runtime transport in polling mode before enabling the IRQ.
  uint32_t direct = 0;
  PE_parse_boot_argn ("native-ufs-direct", &direct, sizeof (direct));
  gZeroCopy = direct != 0;
  IOLog ("NATIVE_UFS_DMA direct=%u prdt_scratch=per_slot bytes=%u\n", gZeroCopy, static_cast<unsigned> (sizeof (gSlotPrds[0])));
  uint32_t enabled = 0;
  PE_parse_boot_argn ("native-ufs-irq", &enabled, sizeof (enabled));
  PE_parse_boot_argn ("native-ufs-spin-us", &gSpinUs, sizeof (gSpinUs));
  if (gSpinUs > 10000) gSpinUs = 10000;
  nanoseconds_to_absolutetime (uint64_t (gSpinUs) * 1000, &gSpinTicks);
  if (!enabled || !&armcpu_spi_attach || !&armcpu_spi_mask) return;
  if (armcpu_spi_attach (297, false, runtimeInterrupt, nullptr, nullptr, nullptr) != kIOReturnSuccess) return;
  hostWrite (0x24, 1);
  if (armcpu_spi_mask (297, false) != kIOReturnSuccess) { hostWrite (0x24, 0); return; }
  gIrqMode = true;
  IOLog ("NATIVE_UFS_IRQ enabled intid=297 spin_us=%u\n", gSpinUs);
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
