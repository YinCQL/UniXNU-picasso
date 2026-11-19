// SPDX-License-Identifier: BSD-2-Clause-Patent
static uint32_t rtCompletionCount (unsigned slot) { return __atomic_load_n (&gSlotGen[slot], __ATOMIC_ACQUIRE); }
static bool rtInterruptMode (void) { return __atomic_load_n (&gIrqMode, __ATOMIC_ACQUIRE); }
#include "RuntimeCompletionWait.h"
struct RuntimeCompletionPort {
  unsigned slot; uint32_t initialSeen;
  uint64_t now () { return mach_absolute_time (); }
  bool interruptMode () { return __atomic_load_n (&gIrqMode, __ATOMIC_ACQUIRE); }
  bool canBlock () { return preemption_enabled (); } // XNU also checks CPU interrupts.
  uint64_t spinTicks () { return gSpinTicks; }
  uint64_t nanoseconds (uint64_t ticks) { uint64_t ns = 0; absolutetime_to_nanoseconds (ticks, &ns); return ns; }
  void pause () {
    const uint64_t start = mach_absolute_time (); ufsPollPause (hostCpuRelax);
    __atomic_fetch_add (&gWaitPauses, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add (&gWaitPauseTicks, ufsElapsedTicks (start, mach_absolute_time ()), __ATOMIC_RELAXED);
  }
  void relax () { hostCpuRelax (); }
  bool arm (uint32_t us) {
    __atomic_store_n (&gSlotDoneAt[slot], 0, __ATOMIC_RELEASE);
    return assert_wait_timeout ((event_t) (uintptr_t) &gSlotGen[slot], THREAD_UNINT, us, NSEC_PER_USEC) == THREAD_WAITING;
  }
  uint32_t generation () { return rtCompletionCount (slot); }
  void cancel () { clear_wait (current_thread (), THREAD_AWAKENED); }
  void block () {
    const uint32_t seen = initialSeen;
    __atomic_fetch_add (&gIrqSleeps, 1, __ATOMIC_RELAXED); thread_block (THREAD_CONTINUE_NULL);
    if (rtCompletionCount (slot) == seen) { __atomic_fetch_add (&gIrqTimeouts, 1, __ATOMIC_RELAXED); return; }
    const uint64_t done = __atomic_load_n (&gSlotDoneAt[slot], __ATOMIC_ACQUIRE);
    if (!done) return;
    const uint64_t late = ufsElapsedTicks (done, mach_absolute_time ());
    __atomic_fetch_add (&gIrqWakes, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add (&gIrqWakeTicks, late, __ATOMIC_RELAXED); ufsMetricMax (gIrqWakeMax, late);
  }
};
static void rtWaitCompletion (unsigned slot, uint32_t seen, uint64_t started, uint64_t deadline)
{
  RuntimeCompletionPort port = { slot, seen };
  ufsRuntimeWaitCompletion (port, seen, started, deadline);
}
static void rtWaitDone (uint64_t started) {
  const uint64_t elapsed = ufsElapsedTicks (started, mach_absolute_time ()); uint64_t ns=0;
  absolutetime_to_nanoseconds (elapsed, &ns);
  __atomic_fetch_add (&gRuntimeTicks, elapsed, __ATOMIC_RELAXED);
  __atomic_fetch_add (&gWaitRanges[ufsWaitRange (ns)], 1, __ATOMIC_RELAXED);
}
static void runtimeInterrupt (void *)
{
  if (read32 (gHost.hc, 0x20) & 1) hostWrite (0x20, 1);
  __atomic_fetch_add (&gIrqs, 1, __ATOMIC_RELAXED);
  uint32_t done = __atomic_load_n (&gOutstanding, __ATOMIC_ACQUIRE) & ~read32 (gHost.hc, 0x58);
  while (done) {
    const unsigned entry = __builtin_ctz (done); done &= done - 1;
    const int slot = gEntrySlot[entry]; if (slot < 0) continue;
    uint64_t empty=0;
    __atomic_compare_exchange_n (&gSlotDoneAt[slot], &empty, mach_absolute_time (), false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    __atomic_add_fetch (&gSlotGen[slot], 1, __ATOMIC_RELEASE);
    thread_wakeup ((event_t) (uintptr_t) &gSlotGen[slot]);
  }
}
