// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// All metric writers and readers use atomic accesses. Snapshots are approximate
// across counters; they never participate in transport or persistence decisions.
static uint64_t ufsElapsedTicks (uint64_t start, uint64_t end) { return end > start ? end - start : 0; }
static inline unsigned ufsWaitRange (uint64_t ns) { return ns < 50000 ? 0 : ns < 100000 ? 1 : ns < 200000 ? 2 : ns < 400000 ? 3 : 4; }
static void ufsMetricMax (uint64_t &value, uint64_t sample)
{
  uint64_t seen = __atomic_load_n (&value, __ATOMIC_RELAXED);
  while (sample > seen && !__atomic_compare_exchange_n (&value, &seen, sample, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
struct UfsOpMetrics { uint64_t ops=0, bytes=0, ticks=0, maximum=0, failures=0; };
static void ufsMetricAccount (UfsOpMetrics &m, uint64_t bytes, uint64_t ticks, bool success)
{
  __atomic_fetch_add (&m.ops, 1, __ATOMIC_RELAXED);
  __atomic_fetch_add (&m.bytes, bytes, __ATOMIC_RELAXED);
  __atomic_fetch_add (&m.ticks, ticks, __ATOMIC_RELAXED);
  ufsMetricMax (m.maximum, ticks);
  if (!success) __atomic_fetch_add (&m.failures, 1, __ATOMIC_RELAXED);
}
struct UfsMetricSpan {
  UfsOpMetrics &metrics; uint64_t (*clock) (); uint64_t started, bytes; bool success=false, active=true;
  UfsMetricSpan (UfsOpMetrics &m, uint64_t (*c) (), uint64_t b) : metrics(m), clock(c), started(c()), bytes(b) {}
  ~UfsMetricSpan () { if (active) ufsMetricAccount (metrics, bytes, ufsElapsedTicks (started, clock ()), success); }
};
static UfsOpMetrics gStoreReadMetrics, gStoreWriteMetrics, gStoreSyncMetrics, gWindowMetrics;
static uint64_t gSnapshotWaits=0, gSnapshotWaitTicks=0;
static uint64_t gWaitRanges[5]={}, gWaitPauses=0, gWaitPauseTicks=0;
static uint64_t gIrqWakes=0, gIrqWakeTicks=0, gIrqWakeMax=0, gIrqTimeouts=0;
