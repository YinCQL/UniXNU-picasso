// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Shared production window reader. Ops owns synchronization, block I/O,
// integrity verification and copying to the caller. No callback runs under
// this guard: the IOKit wrapper completes the request after this returns.
static bool ufsWindowGeometry (uint64_t absolute, uint64_t bytes)
{
  constexpr uint64_t limit = (uint64_t (UINT32_MAX) + 1) * 4096;
  return bytes && !(bytes & 511) && !(absolute & 4095) && absolute < limit && bytes <= limit - absolute;
}
struct UfsWindowResult { IOReturn status; uint64_t done; };
template <class Ops> struct UfsWindowGuard {
  Ops &ops;
  explicit UfsWindowGuard (Ops &value) : ops(value) { ops.lock (); }
  ~UfsWindowGuard () { ops.unlock (); }
  UfsWindowGuard (const UfsWindowGuard &) = delete;
  UfsWindowGuard &operator= (const UfsWindowGuard &) = delete;
};
template <class Ops>
static UfsWindowResult ufsReadWindow (Ops &ops, uint64_t absolute, uint64_t bytes,
                                     uint64_t block, uint64_t count, uint8_t *bounce)
{
  if (!bounce || !ufsWindowGeometry (absolute, bytes) || !count || block >= bytes / 512 || count > bytes / 512 - block)
    return { kIOReturnBadArgument, 0 };
  UfsWindowGuard<Ops> guard (ops);
  uint64_t start = absolute + block * 512, remaining = count * 512;
  UfsWindowResult result = { kIOReturnSuccess, 0 };
  while (remaining) {
    uint64_t lba = (start + result.done) / 4096, skip = (start + result.done) % 4096;
    uint64_t blocks = (skip + remaining + 4095) / 4096;
    if (blocks > kDataMax / 4096) blocks = kDataMax / 4096;
    result.status = ops.read (lba, static_cast<uint32_t> (blocks), bounce);
    if (result.status != kIOReturnSuccess) break;
    // The window lock protects this same snapshot through verify AND copy.
    // The transport lock alone protects only the DMA-to-bounce copy.
    for (uint64_t index = 0; index < blocks; ++index) {
      uint64_t windowBlock = lba + index - absolute / 4096;
      uint64_t offset = windowBlock * 4096;
      if (offset >= bytes) { result.status = kIOReturnBadArgument; break; }
      uint64_t length = bytes - offset < 4096 ? bytes - offset : 4096;
      result.status = ops.verify (windowBlock, bounce + index * 4096, static_cast<size_t> (length));
      if (result.status != kIOReturnSuccess) break;
    }
    if (result.status != kIOReturnSuccess) break;
    uint64_t usable = blocks * 4096 - skip;
    if (usable > remaining) usable = remaining;
    uint64_t copied = ops.copy (result.done, bounce + skip, usable);
    if (copied > usable) { result.status = kIOReturnIOError; break; }
    result.done += copied;
    if (copied != usable) { result.status = kIOReturnIOError; break; }
    remaining -= usable;
  }
  return result;
}

struct UfsVerifiedReadStats {
  uint64_t requests = 0, requestedBytes = 0, hits = 0, misses = 0, readBytes = 0;
  uint64_t readTicks = 0, verifyTicks = 0;
};

// Bounded LRU of authenticated snapshots. Slot zero uses the existing bounce
// buffer; optional storage supplies up to seven more records. The window lock
// covers lookup, replacement, verification and copying. The owner policy is
// immutable for this lifetime; reset before changing it or these buffers.
static constexpr unsigned kUfsVerifiedCacheRecords = 8;
struct UfsVerifiedRecordEntry {
  bool valid = false;
  uint64_t record = 0;
  unsigned age = 0;
};
struct UfsVerifiedRecordCache {
  UfsVerifiedRecordEntry entries[kUfsVerifiedCacheRecords] = {};
  uint8_t *extraBuffer = nullptr;
  size_t extraBytes = 0;
  uint64_t absolute = 0, bytes = 0;
  uint32_t recordBytes = 0;
  const uint8_t *buffer = nullptr;
  const uint8_t *boundExtraBuffer = nullptr;
  size_t boundExtraBytes = 0;
  UfsVerifiedReadStats stats = {};
};

// Embedded-table path: read and authenticate one complete record before any
// requested subrange leaves the driver. recordBytes must fit the shared DMA
// buffer and be a multiple of the driver's native 4-KiB transport unit. A hit
// reuses those same authenticated bytes, never a "verified" bit for a new read.
template <class Ops>
static UfsWindowResult ufsReadVerifiedRecords (Ops &ops, uint64_t absolute, uint64_t bytes,
                                               uint64_t block, uint64_t count, uint8_t *bounce,
                                               uint32_t recordBytes, UfsVerifiedRecordCache &cache,
                                               UfsVerifiedReadStats *report = nullptr)
{
  if (report) *report = {};
  if (!bounce || !ufsWindowGeometry (absolute, bytes) || !recordBytes ||
      recordBytes > kDataMax || (recordBytes & 4095) || !count ||
      block >= bytes / 512 || count > bytes / 512 - block)
    return { kIOReturnBadArgument, 0 };
  UfsWindowGuard<Ops> guard (ops);
  // Invalidate every slot when geometry or storage changes, including when
  // an optional allocation becomes unavailable. No stale snapshot can match.
  if (cache.absolute != absolute || cache.bytes != bytes || cache.recordBytes != recordBytes ||
      cache.buffer != bounce || cache.boundExtraBuffer != cache.extraBuffer ||
      cache.boundExtraBytes != cache.extraBytes) {
    for (auto &entry : cache.entries) entry = {};
    cache.absolute = absolute;
    cache.bytes = bytes;
    cache.recordBytes = recordBytes;
    cache.buffer = bounce;
    cache.boundExtraBuffer = cache.extraBuffer;
    cache.boundExtraBytes = cache.extraBytes;
  }
  size_t extraRecords = cache.extraBuffer ? cache.extraBytes / recordBytes : 0;
  unsigned slots = extraRecords >= kUfsVerifiedCacheRecords - 1 ? kUfsVerifiedCacheRecords :
                   1 + static_cast<unsigned> (extraRecords);
  ++cache.stats.requests;
  cache.stats.requestedBytes += count * 512;
  uint64_t start = block * 512, remaining = count * 512;
  UfsWindowResult result = { kIOReturnSuccess, 0 };
  while (remaining) {
    uint64_t windowOffset = start + result.done;
    uint64_t record = windowOffset / recordBytes;
    uint64_t recordOffset = windowOffset - record * recordBytes;
    uint64_t recordLength = bytes - record * recordBytes;
    if (recordLength > recordBytes) recordLength = recordBytes;
    uint64_t blocks = (recordLength + 4095) / 4096;
    unsigned selected = slots;
    for (unsigned i = 0; i < slots; ++i) {
      if (cache.entries[i].valid && cache.entries[i].record == record) { selected = i; break; }
    }
    bool hit = selected != slots;
    if (!hit) {
      selected = 0;
      for (unsigned i = 0; i < slots; ++i) {
        if (!cache.entries[i].valid) { selected = i; break; }
        if (cache.entries[i].age > cache.entries[selected].age) selected = i;
      }
    }
    auto &entry = cache.entries[selected];
    uint8_t *snapshot = selected ? cache.extraBuffer + (selected - 1) * size_t (recordBytes) : bounce;
    if (hit) {
      ++cache.stats.hits;
    } else {
      // A failed/partial DMA read or failed authentication must not leave a
      // cache hit pointing at bytes that replaced the previous good record.
      entry.valid = false;
      ++cache.stats.misses;
      cache.stats.readBytes += blocks * 4096; // attempted transport bytes
      result.status = ops.read (absolute / 4096 + record * (recordBytes / 4096),
                                static_cast<uint32_t> (blocks), snapshot);
      if (result.status != kIOReturnSuccess) break;
      result.status = ops.verify (record, snapshot, static_cast<size_t> (recordLength));
      if (result.status != kIOReturnSuccess) break;
      entry.record = record;
      entry.valid = true;
    }
    // Ages remain bounded by the slot count; no timestamp can wrap on a long
    // boot. Hits refresh recency as well as successful authenticated misses.
    unsigned previousAge = hit ? entry.age : kUfsVerifiedCacheRecords;
    for (unsigned i = 0; i < slots; ++i) {
      if (i != selected && cache.entries[i].valid && cache.entries[i].age < previousAge)
        ++cache.entries[i].age;
    }
    entry.age = 0;
    uint64_t usable = recordLength - recordOffset;
    if (usable > remaining) usable = remaining;
    uint64_t copied = ops.copy (result.done, snapshot + recordOffset, usable);
    if (copied > usable) { result.status = kIOReturnIOError; break; }
    result.done += copied;
    if (copied != usable) { result.status = kIOReturnIOError; break; }
    remaining -= usable;
  }
  // At most 15 reports per window (64..1048576 requests, powers of two).
  // Snapshot under the lock; the caller prints after the guard is released.
  uint64_t requests = cache.stats.requests;
  if (report && requests >= 64 && requests <= 1048576 && !(requests & (requests - 1)))
    *report = cache.stats;
  return result;
}
