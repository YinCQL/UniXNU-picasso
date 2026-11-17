// SPDX-License-Identifier: BSD-2-Clause-Patent
static unsigned gSlotCount = 1;
static uint32_t gSlotFree = 1;
static uint32_t gSlotsBusy = 0, gSlotsPeak = 0;
static uint64_t gSlotWaits = 0, gSlotWaitTicks = 0;
static unsigned gSlotWaitersNow = 0;
static uint32_t gPoisonReason = 0, gPoisonOpcode = 0;
static void rtLogRuntimeStats (void);
static void rtLogTableStats (void);
struct RuntimeWaiter { RuntimeWaiter *next; int slot; };
static RuntimeWaiter *gWaitHead = nullptr, *gWaitTail = nullptr;
static IOLock *gWriteLock = nullptr;
static volatile uint64_t gRuntimeReads = 0, gRuntimeWrites = 0, gRuntimeSyncs = 0;
static volatile uint64_t gRuntimeTicks = 0, gBusyRetries = 0;
static volatile uint32_t gRuntimeReadTrace = 0;
static bool gIrqMode = false;
static uint64_t gSpinTicks = 0;
static uint32_t gSpinUs = 200;
static volatile uint64_t gIrqs = 0, gIrqSleeps = 0;

static void rtQuarantineLocked (uint32_t reason, uint32_t opcode)
{
  if (!__atomic_load_n (&gPoisonReason, __ATOMIC_RELAXED)) {
    __atomic_store_n (&gPoisonOpcode, opcode, __ATOMIC_RELAXED);
    __atomic_store_n (&gPoisonReason, reason, __ATOMIC_RELEASE);
  }
  __atomic_store_n (&gHost.poisoned, true, __ATOMIC_RELEASE);
  __atomic_store_n (&gHost.ready, false, __ATOMIC_RELEASE);
  if (!gSlotLock) return;
  IOLockLock (gSlotLock);
  for (RuntimeWaiter *w = gWaitHead; w; w = w->next) {
    w->slot = -2;
    IOLockWakeup (gSlotLock, w, true);
  }
  gWaitHead = gWaitTail = nullptr;
  IOLockUnlock (gSlotLock);
}

static void rtQuarantine (uint32_t reason, uint32_t opcode)
{
  IOLockLock (gSubmitLock);
  rtQuarantineLocked (reason, opcode);
  IOLockUnlock (gSubmitLock);
}

static UfsSlot rtSlotTake (bool *pooled)
{
  *pooled = false;
  IOLockLock (gSlotLock);
  if (!__atomic_load_n (&gHost.ready, __ATOMIC_ACQUIRE) ||
      __atomic_load_n (&gHost.poisoned, __ATOMIC_ACQUIRE)) {
    IOLockUnlock (gSlotLock); return ufsSlot (0);
  }
  int index = -1;
  if (gSlotFree) {
    index = __builtin_ctz (gSlotFree); gSlotFree &= ~(1U << index);
  } else {
    ++gSlotWaits; ++gSlotWaitersNow;
    const uint64_t start = mach_absolute_time ();
    RuntimeWaiter waiter = { nullptr, -1 };
    if (gWaitTail) gWaitTail->next = &waiter; else gWaitHead = &waiter;
    gWaitTail = &waiter;
    while (waiter.slot == -1) IOLockSleep (gSlotLock, &waiter, THREAD_UNINT);
    index = waiter.slot; --gSlotWaitersNow;
    gSlotWaitTicks += ufsElapsedTicks (start, mach_absolute_time ());
  }
  if (index >= 0) { *pooled = true; if (++gSlotsBusy > gSlotsPeak) gSlotsPeak = gSlotsBusy; }
  IOLockUnlock (gSlotLock);
  return ufsSlot (index >= 0 ? static_cast<unsigned> (index) : 0);
}

static void rtSlotGive (const UfsSlot &slot, bool pooled)
{
  if (!pooled) return;
  IOLockLock (gSlotLock);
  --gSlotsBusy;
  // A poisoned host never reuses a descriptor or a direct-DMA snapshot.
  if (!__atomic_load_n (&gHost.poisoned, __ATOMIC_ACQUIRE)) {
    if (RuntimeWaiter *w = gWaitHead) {
      gWaitHead = w->next; if (!gWaitHead) gWaitTail = nullptr;
      w->slot = static_cast<int> (slot.index); IOLockWakeup (gSlotLock, w, true);
    } else gSlotFree |= 1U << slot.index;
  }
  IOLockUnlock (gSlotLock);
}

#ifndef NATIVE_UFS_HOST_RUNTIME_PLATFORM
#include "RuntimePlatform.h"
#endif

static unsigned rtBuildPrds (const void *buffer, uint64_t bytes, UfsPrd *prd)
{
  if (!gZeroCopy || (reinterpret_cast<uintptr_t> (buffer) & 4095) || !bytes || (bytes & 4095) || bytes > kXferMax) return 0;
  unsigned count = 0;
  for (uint64_t at = 0; at < bytes; at += 4096) {
    const uint64_t pa = rtDmaAddress (static_cast<const uint8_t *> (buffer) + at);
    if (!pa || (pa & 4095)) return 0;
    if (count && prd[count - 1].addr + prd[count - 1].bytes == pa && prd[count - 1].bytes + 4096 <= kPrdMax)
      prd[count - 1].bytes += 4096;
    else { if (count == kMaxPrds) return 0; prd[count++] = { pa, 4096 }; }
  }
  return count;
}
static IOReturn rtScsiRetry (const UfsSlot &slot, uint8_t lun, const uint8_t *cdb, uint32_t direction,
                           uint32_t bytes, uint32_t timeout, const UfsPrd *prd = nullptr, unsigned prds = 0, const void *buffer = nullptr)
{
  uint64_t totalDeadline = 0;
  clock_interval_to_deadline (timeout, kMicrosecondScale, &totalDeadline);
  for (unsigned attempt = 0; ; ++attempt) {
    uint64_t now = mach_absolute_time (), ns = 0;
    if (now >= totalDeadline) return kIOReturnTimeout; // no command owns DMA here
    absolutetime_to_nanoseconds (totalDeadline - now, &ns);
    const uint32_t left = ns / 1000 > timeout ? timeout : static_cast<uint32_t> (ns / 1000);
    if (!left) return kIOReturnTimeout;
    uint32_t scsi = 0xffff;
    IOReturn result = rtScsiDir (slot, lun, cdb, 10, direction, bytes, left, &scsi, prd, prds, buffer);
    if (__atomic_load_n (&gHost.poisoned, __ATOMIC_ACQUIRE) || result != kIOReturnNotReady || (scsi != 8 && scsi != 0x28) || attempt >= 50) return result;
    __atomic_fetch_add (&gBusyRetries, 1, __ATOMIC_RELAXED);
    uint64_t pause = 0; clock_interval_to_deadline (attempt < 4 ? 50U << attempt : 1000U, kMicrosecondScale, &pause);
    while (mach_absolute_time () < pause && mach_absolute_time () < totalDeadline) ufsPollPause (hostCpuRelax);
  }
}
static IOReturn runtimeRead4k (uint8_t lun, uint64_t lba, uint32_t blocks, void *dst, bool owned)
{
  if (!dst || !blocks || blocks > kDataMax / 4096 || lba > 0xffffffffULL - (blocks - 1)) return kIOReturnBadArgument;
  const uint32_t trace = __atomic_fetch_add (&gRuntimeReadTrace, 1, __ATOMIC_RELAXED);
  if (trace < 8) IOLog ("NATIVE_UFS_RUNTIME_READ begin sample=%u lun=%u lba=%llx blocks=%u owned=%u\n", trace + 1, lun, lba, blocks, owned);
  bool pooled = false; const UfsSlot slot = rtSlotTake (&pooled);
  if (trace < 8) IOLog ("NATIVE_UFS_RUNTIME_READ slot sample=%u pooled=%u index=%u entry=%u list_wc=%u\n", trace + 1, pooled, slot.index, slot.entry, gListUncached);
  if (!pooled) return kIOReturnNotReady;
  UfsPrd *prd = gSlotPrds[slot.index];
  const unsigned prds = owned ? rtBuildPrds (dst, uint64_t (blocks) * 4096, prd) : 0;
  __atomic_fetch_add (prds ? &gDirectCommands : &gStagedCommands, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add (&gRuntimeReads, 1, __ATOMIC_RELAXED);
  uint8_t cdb[10] = { 0x28, 0, uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                     0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  IOReturn result = rtScsiRetry (slot, lun, cdb, 2, blocks * 4096U, 1000000, prds ? prd : nullptr, prds, prds ? dst : nullptr);
  if (trace < 8) IOLog ("NATIVE_UFS_RUNTIME_READ end sample=%u result=%08x scsi=%x polls=%u db=%x is=%x\n", trace + 1, static_cast<unsigned> (result), rtDiagnostic (slot).scsi, rtDiagnostic (slot).polls, rtDiagnostic (slot).dbLast, rtDiagnostic (slot).isLast);
  if (result == kIOReturnSuccess && !prds) memcpy (dst, slot.data, blocks * 4096U);
  rtSlotGive (slot, pooled);
  return result;
}
static IOReturn runtimeWrite4k (uint8_t lun, uint64_t lba, uint32_t blocks, const void *src, bool fua)
{
  if (!src || !blocks || blocks > kDataMax / 4096 || lun != 0 || !hostInWindow (lba, blocks)) return kIOReturnBadArgument;
  IOLockLock (gWriteLock); // order every persistence/log write with SYNCHRONIZE CACHE
  bool pooled = false; const UfsSlot slot = rtSlotTake (&pooled);
  IOReturn result = kIOReturnNotReady;
  if (pooled) {
    __atomic_fetch_add (&gRuntimeWrites, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add (&gStagedCommands, 1, __ATOMIC_RELAXED);
    {
      RuntimeSubmitGuard submitGuard;
      if (__atomic_load_n (&gHost.poisoned, __ATOMIC_ACQUIRE)) {
        rtSlotGive (slot, pooled); IOLockUnlock (gWriteLock); return kIOReturnNotReady;
      }
      memcpy (slot.data, src, blocks * 4096U);
    }
    uint8_t cdb[10] = { 0x2a, uint8_t (fua ? 8 : 0), uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                       0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
    result = rtScsiRetry (slot, 0, cdb, 1, blocks * 4096U, 2000000);
    rtSlotGive (slot, pooled);
  }
  IOLockUnlock (gWriteLock); return result;
}
static IOReturn runtimeSyncCache (void)
{
  if (!gStoreOpen && !gLogOpen && !gAuxOpen) return kIOReturnNotReady;
  IOLockLock (gWriteLock);
  bool pooled = false; const UfsSlot slot = rtSlotTake (&pooled);
  IOReturn result = kIOReturnNotReady;
  if (pooled) {
    __atomic_fetch_add (&gRuntimeSyncs, 1, __ATOMIC_RELAXED);
    uint8_t cdb[10] = { 0x35 }; result = rtScsiRetry (slot, 0, cdb, 0, 0, 10000000);
    rtSlotGive (slot, pooled);
  }
  IOLockUnlock (gWriteLock); return result;
}
#include "RuntimeFirmware.h"
// Pool data is copied only under its lock; reject a busy snapshot without
// blocking an I/O thread. The rest uses atomic loads and is approximate.
extern "C" int native_ufs_stats_text (char *out, size_t bytes)
{
  if (!out || !bytes) return 0;
  unsigned peak=0, takers=0, busyNow=0; uint64_t waits=0, waitTicks=0;
  const bool pool = preemption_enabled () && gSlotLock && IOLockTryLock (gSlotLock);
  if (pool) {
    peak=gSlotsPeak; takers=gSlotWaitersNow; busyNow=gSlotsBusy; waits=gSlotWaits; waitTicks=gSlotWaitTicks;
    IOLockUnlock (gSlotLock);
  }
  const auto count = [] (const volatile uint64_t &v) { return static_cast<unsigned long long> (__atomic_load_n (&v, __ATOMIC_RELAXED)); };
  const auto us = [] (uint64_t ticks) { uint64_t ns=0; absolutetime_to_nanoseconds (ticks, &ns); return static_cast<unsigned long long> (ns / 1000); };
  uint64_t oldest=0; unsigned oldSlot=0, oldOpcode=0; const uint64_t now=mach_absolute_time ();
  for (unsigned i=0;i<gSlotCount;++i) {
    const uint64_t at=__atomic_load_n (&gSlotIssuedAt[i], __ATOMIC_RELAXED);
    if (at && ufsElapsedTicks (at, now)>oldest) { oldest=ufsElapsedTicks (at, now); oldSlot=i; oldOpcode=__atomic_load_n (&gSlotOpcode[i], __ATOMIC_RELAXED); }
  }
  const uint32_t poison=__atomic_load_n (&gPoisonReason, __ATOMIC_ACQUIRE);
  return snprintf (out, bytes,
    "NATIVE_UFS_RUNTIME_STATS slots=%u pool_snapshot=%u peak=%u waits=%llu irq=%u irqs=%llu sleeps=%llu direct=%llu staged=%llu busy=%llu r/w/s=%llu/%llu/%llu ticks=%llu\n"
    "NATIVE_UFS_WAIT done_lt50_100_200_400_ge400_us=%llu/%llu/%llu/%llu/%llu pauses=%llu pause_us=%llu wakes=%llu wake_us=%llu wake_max_us=%llu sleep_timeouts=%llu takers=%u inuse=%u slot_wait_us=%llu old_us=%llu old_op=%x old_slot=%u poison=%u poison_op=%x\n"
    "NATIVE_UFS_API rd=%llu/%llu/%llu/%llu/%llu wr=%llu/%llu/%llu/%llu/%llu sync=%llu/%llu/%llu/%llu/%llu win=%llu/%llu/%llu/%llu/%llu snapshot_wait=%llu/%llu units=ops/bytes/us/max_us/failures\n",
    gSlotCount,pool,peak,static_cast<unsigned long long>(waits),rtInterruptMode(),count(gIrqs),count(gIrqSleeps),count(gDirectCommands),count(gStagedCommands),count(gBusyRetries),count(gRuntimeReads),count(gRuntimeWrites),count(gRuntimeSyncs),count(gRuntimeTicks),
    count(gWaitRanges[0]),count(gWaitRanges[1]),count(gWaitRanges[2]),count(gWaitRanges[3]),count(gWaitRanges[4]),count(gWaitPauses),us(count(gWaitPauseTicks)),count(gIrqWakes),us(count(gIrqWakeTicks)),us(count(gIrqWakeMax)),count(gIrqTimeouts),takers,busyNow,us(waitTicks),us(oldest),oldOpcode,oldSlot,poison,poison?__atomic_load_n(&gPoisonOpcode,__ATOMIC_RELAXED):0,
    count(gStoreReadMetrics.ops),count(gStoreReadMetrics.bytes),us(count(gStoreReadMetrics.ticks)),us(count(gStoreReadMetrics.maximum)),count(gStoreReadMetrics.failures),
    count(gStoreWriteMetrics.ops),count(gStoreWriteMetrics.bytes),us(count(gStoreWriteMetrics.ticks)),us(count(gStoreWriteMetrics.maximum)),count(gStoreWriteMetrics.failures),
    count(gStoreSyncMetrics.ops),count(gStoreSyncMetrics.bytes),us(count(gStoreSyncMetrics.ticks)),us(count(gStoreSyncMetrics.maximum)),count(gStoreSyncMetrics.failures),
    count(gWindowMetrics.ops),count(gWindowMetrics.bytes),us(count(gWindowMetrics.ticks)),us(count(gWindowMetrics.maximum)),count(gWindowMetrics.failures),count(gSnapshotWaits),us(count(gSnapshotWaitTicks)));
}
// Only this dedicated, finite worker formats the larger snapshot on its stack.
static void rtLogRuntimeStats (void)
{
  char text[1536]; native_ufs_stats_text (text, sizeof (text)); IOLog ("%s", text);
  rtLogTableStats ();
}
