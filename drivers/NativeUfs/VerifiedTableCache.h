// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Bounded cache of verified-root hash table pages.
//
// A window's hash table (one SHA-256 per 4096-byte image block, 128 entries per 4096-byte table page) is authenticated once
// against the owner-signed header while its pages are hashed; only those page hashes stay in memory (read-only). A block
// verification takes its table entry from a cached table page, or reads that page from the table object on LUN 0, checks it
// against the kept page hash and caches it. A table page that does not match is never cached and its blocks are refused.
//
// The cache is preallocated and wired: a fill allocates nothing and reads through the transport directly, never through a
// window device, so a page-in under memory pressure cannot wait on pageout or on a verified read of itself. The cache lock is
// not held across the read; a request that needs a page another request is filling sleeps until that fill ends.
//
// Ops supplies lock (), unlock (), sleep (event) (drops and retakes the lock), wakeup (event),
// readPage (table, page, dst) (one 4096-byte read, true on success) and sha256 (data, bytes, out) (true on success).
struct VrootTable {
  const uint8_t *pageHashes;   // SHA-256 of each table page (its table bytes only for the last one); read-only
  uint32_t *where;             // cache entry + 1 that holds each table page, 0 for none; guarded by the cache lock
  uint64_t tableBytes;         // block count * 32
  uint64_t pages;              // table pages
  uint64_t firstLba;           // LUN 0 block of the first table page
};

enum : uint32_t { kVrootEmpty = 0, kVrootFilling = 1, kVrootValid = 2 };
struct VrootCacheEntry {
  const VrootTable *table;
  uint64_t page;
  uint64_t used;               // LRU stamp; 0 while empty
  uint32_t state;
};
// Counters change under the cache lock; the board's log reads them without it.
struct VrootCacheStats {
  uint64_t hits;               // entries served from a cached table page
  uint64_t misses;             // lookups that found no cached page and started a fill
  uint64_t reads;              // table page reads that completed
  uint64_t evictions;          // cached pages replaced by another
  uint64_t waits;              // sleeps for a page another request was filling, or for a free entry
  uint64_t failures;           // fills refused: read error or a page that does not match its kept hash
};
struct VrootTableCache {
  uint8_t *pages;              // count * 4096 bytes
  VrootCacheEntry *entries;
  uint32_t count;
  uint64_t clock;
  VrootCacheStats stats;
};

static inline bool vrootSame (const uint8_t *a, const uint8_t *b, size_t n)
{
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

// Copies the table entry of `block` to `hash`. False when the block is outside the table or its table page cannot be read or
// does not match its kept hash; the caller then refuses the block.
template <class Ops>
static bool vrootTableEntry (VrootTableCache &cache, Ops &ops, const VrootTable &table, uint64_t block, uint8_t hash[32])
{
  if (!cache.count || !cache.pages || !cache.entries || !table.where || !table.pageHashes || block >= table.tableBytes / 32) return false;
  const uint64_t page = block / 128, at = (block % 128) * 32;
  if (page >= table.pages) return false;
  ops.lock ();
  for (;;) {
    const uint32_t held = table.where[page];
    if (held > cache.count) { ops.unlock (); return false; }
    if (held) {
      VrootCacheEntry &entry = cache.entries[held - 1];
      if (entry.table != &table || entry.page != page) { ops.unlock (); return false; }
      if (entry.state == kVrootValid) {
        entry.used = ++cache.clock;
        memcpy (hash, cache.pages + uint64_t (held - 1) * 4096 + at, 32);
        ++cache.stats.hits;
        ops.unlock ();
        return true;
      }
      ++cache.stats.waits;
      ops.sleep (&entry);
      continue;
    }
    // The least recently used entry that is not being filled; empty entries (stamp 0) go first.
    uint32_t victim = UINT32_MAX;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < cache.count; ++i) {
      const VrootCacheEntry &entry = cache.entries[i];
      if (entry.state != kVrootFilling && entry.used < oldest) { oldest = entry.used; victim = i; if (!oldest) break; }
    }
    if (victim == UINT32_MAX) {
      ++cache.stats.waits;
      ops.sleep (&cache);
      continue;
    }
    VrootCacheEntry &entry = cache.entries[victim];
    if (entry.state == kVrootValid) {
      entry.table->where[entry.page] = 0;
      ++cache.stats.evictions;
    }
    entry.table = &table;
    entry.page = page;
    entry.used = 0;
    entry.state = kVrootFilling;
    table.where[page] = victim + 1;
    ++cache.stats.misses;
    ops.unlock ();

    uint8_t *data = cache.pages + uint64_t (victim) * 4096;
    const uint64_t left = table.tableBytes - page * 4096;
    const size_t bytes = left < 4096 ? static_cast<size_t> (left) : 4096;
    uint8_t digest[32];
    const bool read = ops.readPage (table, page, data);
    const bool ok = read && ops.sha256 (data, bytes, digest) && vrootSame (digest, table.pageHashes + page * 32, 32);

    ops.lock ();
    if (read) ++cache.stats.reads;
    if (ok) {
      entry.state = kVrootValid;
      entry.used = ++cache.clock;
      memcpy (hash, data + at, 32);
    } else {
      table.where[page] = 0;
      entry.table = nullptr;
      entry.used = 0;
      entry.state = kVrootEmpty;
      ++cache.stats.failures;
    }
    ops.wakeup (&entry);
    ops.wakeup (&cache);
    ops.unlock ();
    return ok;
  }
}

// Forgets every page of `table` before its storage is released. No request may use the table any more.
template <class Ops>
static void vrootTableDrop (VrootTableCache &cache, Ops &ops, const VrootTable &table)
{
  ops.lock ();
  for (uint32_t i = 0; i < cache.count; ++i) {
    VrootCacheEntry &entry = cache.entries[i];
    while (entry.table == &table && entry.state == kVrootFilling) ops.sleep (&entry);
    if (entry.table == &table) {
      table.where[entry.page] = 0;
      entry.table = nullptr;
      entry.used = 0;
      entry.state = kVrootEmpty;
    }
  }
  ops.wakeup (&cache);
  ops.unlock ();
}
