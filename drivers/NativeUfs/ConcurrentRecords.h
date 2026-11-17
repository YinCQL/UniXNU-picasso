// SPDX-License-Identifier: BSD-2-Clause-Patent
// Cache entries own complete authenticated records. No table lock or pool lock
// spans transport I/O, SHA-256 or a caller copy; refs pins the exact snapshot.
struct UfsConcurrentEntry { uint64_t record, stamp; unsigned state, refs; };
struct UfsConcurrentCache {
  UfsConcurrentEntry entries[kUfsVerifiedCacheRecords] = {};
  uint64_t clock = 0;
  unsigned count = 1;
  UfsVerifiedReadStats stats = {};
};
template <class Ops>
static UfsWindowResult ufsReadConcurrentRecords (Ops &ops, uint64_t absolute, uint64_t bytes,
  uint64_t block, uint64_t count, uint8_t *bounce, uint8_t *extra, uint32_t recordBytes,
  UfsConcurrentCache &cache, UfsVerifiedReadStats *report)
{
  if (report) *report = {};
  if (!bounce || !ufsWindowGeometry (absolute, bytes) || !recordBytes || recordBytes > kDataMax ||
      (recordBytes & 4095) || !count || block >= bytes / 512 || count > bytes / 512 - block)
    return { kIOReturnBadArgument, 0 };
  ops.lock ();
  ++cache.stats.requests; cache.stats.requestedBytes += count * 512;
  ops.unlock ();
  UfsWindowResult result = { kIOReturnSuccess, 0 };
  uint64_t start = block * 512, remaining = count * 512;
  while (remaining) {
    const uint64_t offset = start + result.done, record = offset / recordBytes, skip = offset % recordBytes;
    const uint64_t length = bytes - record * recordBytes < recordBytes ? bytes - record * recordBytes : recordBytes;
    const uint32_t blocks = static_cast<uint32_t> ((length + 4095) / 4096);
    unsigned selected = 0;
    bool hit = false;
    ops.lock ();
    for (;;) {
      bool filling = false;
      selected = cache.count;
      for (unsigned i = 0; i < cache.count; ++i) {
        const auto &e = cache.entries[i];
        if (e.state && e.record == record) {
          if (e.state == 1) filling = true;
          else { selected = i; hit = true; }
          break;
        }
      }
      if (filling) { ops.sleep (&cache); continue; }
      if (hit) { ++cache.stats.hits; break; }
      uint64_t oldest = UINT64_MAX;
      for (unsigned i = 0; i < cache.count; ++i) {
        const auto &e = cache.entries[i];
        if (!e.refs && e.state != 1 && e.stamp < oldest) { oldest = e.stamp; selected = i; if (!oldest) break; }
      }
      if (selected == cache.count) { ops.sleep (&cache); continue; }
      auto &e = cache.entries[selected];
      e.record = record; e.state = 1; e.stamp = 0;
      ++cache.stats.misses; cache.stats.readBytes += uint64_t (blocks) * 4096;
      break;
    }
    auto &e = cache.entries[selected];
    ++e.refs; e.stamp = ++cache.clock;
    uint8_t *snapshot = selected ? extra + size_t (selected - 1) * recordBytes : bounce;
    ops.unlock ();
    if (!hit) {
      result.status = ops.read (absolute / 4096 + record * (recordBytes / 4096), blocks, snapshot);
      if (result.status == kIOReturnSuccess) result.status = ops.verify (record, snapshot, static_cast<size_t> (length));
      ops.lock ();
      e.state = result.status == kIOReturnSuccess ? 2 : 0;
      if (!e.state) e.stamp = 0;
      ops.wakeup (&cache); ops.unlock ();
    }
    if (result.status == kIOReturnSuccess) {
      const uint64_t usable = length - skip < remaining ? length - skip : remaining;
      const uint64_t copied = ops.copy (result.done, snapshot + skip, usable);
      if (copied > usable) result.status = kIOReturnIOError;
      else { result.done += copied; if (copied != usable) result.status = kIOReturnIOError; else remaining -= usable; }
    }
    ops.lock (); --e.refs; ops.wakeup (&cache); ops.unlock ();
    if (result.status != kIOReturnSuccess) break;
  }
  ops.lock ();
  const uint64_t requests = cache.stats.requests;
  if (report && requests >= 64 && requests <= 1048576 && !(requests & (requests - 1))) *report = cache.stats;
  ops.unlock ();
  return result;
}
