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

// One authenticated snapshot in the existing per-window bounce buffer. The
// window lock must cover lookup, replacement, verification AND copying. The
// owner policy and read-only window are immutable for this cache's lifetime;
// discard it before changing either or writing to the bounce buffer elsewhere.
struct UfsVerifiedRecordCache {
  bool valid = false;
  uint64_t absolute = 0, bytes = 0, record = 0;
  uint32_t recordBytes = 0;
  const uint8_t *buffer = nullptr;
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
    if (cache.valid && cache.absolute == absolute && cache.bytes == bytes &&
        cache.recordBytes == recordBytes && cache.record == record && cache.buffer == bounce) {
      ++cache.stats.hits;
    } else {
      // A failed/partial DMA read or failed authentication must not leave a
      // cache hit pointing at bytes that replaced the previous good record.
      cache.valid = false;
      ++cache.stats.misses;
      cache.stats.readBytes += blocks * 4096; // attempted transport bytes
      result.status = ops.read (absolute / 4096 + record * (recordBytes / 4096),
                                static_cast<uint32_t> (blocks), bounce);
      if (result.status != kIOReturnSuccess) break;
      result.status = ops.verify (record, bounce, static_cast<size_t> (recordLength));
      if (result.status != kIOReturnSuccess) break;
      cache.absolute = absolute;
      cache.bytes = bytes;
      cache.recordBytes = recordBytes;
      cache.record = record;
      cache.buffer = bounce;
      cache.valid = true;
    }
    uint64_t usable = recordLength - recordOffset;
    if (usable > remaining) usable = remaining;
    uint64_t copied = ops.copy (result.done, bounce + recordOffset, usable);
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
