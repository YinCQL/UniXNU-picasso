// SPDX-License-Identifier: BSD-2-Clause-Patent
// Runtime transport adapted from ShengUfs f020bd6. Bootstrap remains separate.
static UfsRootDiagnostics gSlotDiagnostics[kSlots];
static volatile uint32_t gCommandSequence = 0;
static IOLock *gSlotLock = nullptr, *gSubmitLock = nullptr;
static void rtQuarantineLocked (uint32_t reason = 6, uint32_t opcode = 0);
struct RuntimeSubmitGuard {
  RuntimeSubmitGuard () { IOLockLock (gSubmitLock); }
  ~RuntimeSubmitGuard () { IOLockUnlock (gSubmitLock); }
};
static void rtQuarantine (uint32_t reason = 6, uint32_t opcode = 0);
static uint32_t rtCompletionCount (unsigned);
static bool rtInterruptMode (void);
static void rtWaitCompletion (unsigned, uint32_t, uint64_t, uint64_t);
static void rtWaitDone (uint64_t);
static uint64_t rtDmaAddress (const void *);
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared production command path. Included inside the root transport namespace.
// The rt harness replaces only MMIO, DMA synchronization, clock and lock primitives.
// One command slot (RootTransportLayout.h): its request list entry, command descriptor and data area.
struct UfsSlot {
  unsigned index;
  uint32_t entry, bit;     // request list entry and its doorbell bit
  uint8_t *utrd, *ucd, *data;
  uint64_t ucdPhys, dataPhys;
};
static UfsRootDiagnostics &rtDiagnostic (const UfsSlot &slot) { return gSlotDiagnostics[slot.index]; }
static IOReturn rtRecord (const UfsSlot &slot, IOReturn result, uint32_t ocs)
{
  IOLockLock (gDiagnosticLock);
  rtDiagnostic (slot).result = static_cast<uint32_t> (result);
  rtDiagnostic (slot).ocs = ocs;
  gRootDiagnostic = rtDiagnostic (slot);
  IOReturn recorded = recordTransport (result, ocs);
  IOLockUnlock (gDiagnosticLock);
  return recorded;
}
// The geometry of every slot, set once at bring-up (rtSlotGeometry), and the request list's form: in a cached region (each entry
// cleaned and invalidated by its own line) or on its own write-combined page (no cache maintenance).
static UfsSlot gSlotGeom[kSlots];
static bool gListUncached = false;
static int8_t gEntrySlot[32] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                 -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
// Doorbell bits of the commands rung and not yet seen complete (read by the completion interrupt), and per slot the completions the
// interrupt has seen (what a waiting command sleeps on).
static volatile uint32_t gOutstanding = 0;
static volatile uint32_t gSlotGen[kSlots] = {};
static uint64_t gSlotDoneAt[kSlots] = {};

static void rtSlotGeometry (unsigned index, uint8_t *utrd, uint32_t entry, uint8_t *ucd, uint64_t ucdPhys, uint8_t *data,
                              uint64_t dataPhys)
{
  UfsSlot &slot = gSlotGeom[index];
  slot.index = index; slot.entry = entry; slot.bit = 1U << entry;
  slot.utrd = utrd; slot.ucd = ucd; slot.data = data; slot.ucdPhys = ucdPhys; slot.dataPhys = dataPhys;
  gEntrySlot[entry] = static_cast<int8_t> (index);
}

// Slots 0..count-1 of region A with the request list at its offset 0 (entry 2s).
static void rtSlotGeometryRegionA (unsigned count)
{
  for (unsigned e = 0; e < 32; ++e) gEntrySlot[e] = -1;
  gListUncached = false;
  for (unsigned s = 0; s < count && s < kRegionSlots; ++s)
    rtSlotGeometry (s, gHost.virt + ufsSlotUtrdOffset (s), ufsSlotEntry (s), gHost.virt + ufsSlotUcdOffset (s),
                      gHost.phys + ufsSlotUcdOffset (s), gHost.virt + ufsSlotDataOffset (s), gHost.phys + ufsSlotDataOffset (s));
}

static UfsSlot ufsSlot (unsigned index) { return gSlotGeom[index]; }

// Completion waits (rtWaitCompletion, supplied by the port): between two polls of a command, a port with the controller's interrupt
// sleeps until the interrupt sees this slot complete (or a short timeout) instead of spinning. `seen` is the slot's completion count
// read before the doorbell was polled, so a completion between that poll and the sleep is not missed.
struct RuntimeTransferPort {
  const UfsSlot &slot;
  uint64_t deadline, lastClock = 0;
  uint32_t tracePoll = 0;
  void beginPoll (uint32_t poll) { tracePoll = poll; }
  uint32_t seen = 0;
  uint64_t started = mach_absolute_time ();   // when the wait began: the port polls first and sleeps only past its spin budget
  uint32_t doorbell () { seen = rtCompletionCount (slot.index); return (read32 (gHost.hc, 0x58) & slot.bit) ? 1U : 0U; }
  uint32_t interrupt () { return read32 (gHost.hc, 0x20); }
  uint32_t completedOcs () {
    if (!gListUncached) cacheInvalidate (reinterpret_cast<const uint8_t *> (reinterpret_cast<uintptr_t> (slot.utrd) & ~uintptr_t (63)), 64);
    return *reinterpret_cast<volatile uint32_t *> (slot.utrd + 8) & 0xff;
  }
  bool expired () { lastClock = mach_absolute_time (); return lastClock >= deadline; }
  void delay () { rtWaitCompletion (slot.index, seen, started, deadline); }
};

static void rtPutBe32 (uint8_t *at, uint32_t value)
{
  at[0] = static_cast<uint8_t> (value >> 24);
  at[1] = static_cast<uint8_t> (value >> 16);
  at[2] = static_cast<uint8_t> (value >> 8);
  at[3] = static_cast<uint8_t> (value);
}

static uint32_t rtLoadBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

// Issue the UPIU already placed in the slot's command descriptor through its request list entry. `direction` is
// the UTRD data direction (0 none, 1 from rt, 2 to rt). Direction 1 is reachable only through rtWrite4k, which
// confines it to the persistent store window. Returns the overall command status
// (0 = completed) in *ocs. Timeout/fatal status quarantines the allocation. Commands of different slots run concurrently:
// each touches only its own entry, descriptor and data area, and rings only its own doorbell bit (writing 0 bits has no effect).
// rtSubmit's halves: rtIssue rings the doorbell (*deadline: when the command times out); rtAwait polls for its completion.
// One caller may have commands of several slots outstanding between them.
// Zero copy (Linux ufshcd and Apple's storage drivers DMA straight into the request's pages): a physical region description table
// built from the caller's buffer (rtBuildPrds) instead of the slot's data area. Up to kMaxPrds entries of up to 256 KiB each, at most
// kXferMax per command; the caller's buffer gets the cache maintenance the data area otherwise gets.
struct UfsPrd { uint64_t addr; uint32_t bytes; };
static constexpr unsigned kMaxPrds = (kUcdStride - kPrdtOffset) / 16;
static constexpr uint32_t kXferMax = 1U << 20, kPrdMax = 256U << 10;
// CPU-only PRDT scratch belongs to the slot lease. Keep the 1 KiB array
// off a filesystem caller's kernel stack, including while waiting for a slot.
alignas(64) static UfsPrd gSlotPrds[kSlots][kMaxPrds];
static bool gZeroCopy = false;
// Data commands by path, for the log: straight to or from the caller's pages, or through the slot's data area with a copy.
static volatile uint64_t gDirectCommands = 0, gStagedCommands = 0;
// Commands in flight, for the log: when each slot's doorbell rang (0 while the slot has no command out) and the command's opcode (the
// SCSI opcode, or the UPIU transaction type for others). The log names the oldest, so a command that does not complete is visible.
static volatile uint64_t gSlotIssuedAt[kSlots] = {};
static volatile uint8_t gSlotOpcode[kSlots] = {};

static IOReturn rtIssue (const UfsSlot &slot, uint32_t direction, uint32_t dataBytes, uint32_t timeoutUs, uint64_t *deadline,
                           uint32_t *ocs, const UfsPrd *prd = nullptr, unsigned prds = 0, const void *buffer = nullptr)
{
  RuntimeSubmitGuard submitGuard;
  *ocs = 15;
  if (prds > kMaxPrds || (prds && (!prd || !buffer))) return kIOReturnBadArgument;
  if (!timeoutUs || dataBytes > (prds ? kXferMax : kDataMax) || (dataBytes & 3) ||
      (dataBytes ? (direction != 1 && direction != 2) : direction != 0)) return kIOReturnBadArgument;
  if (__atomic_load_n (&gHost.poisoned, __ATOMIC_ACQUIRE)) return kIOReturnNotReady;
  if (read32 (gHost.hc, 0x58) & slot.bit) {
    rtQuarantineLocked (1, slot.ucd[16]);
    return rtRecord (slot, kIOReturnBusy, *ocs);
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    rtQuarantineLocked (2, slot.ucd[16]);
    return rtRecord (slot, kIOReturnIOError, *ocs);
  }
  rtDiagnostic (slot).command = __atomic_add_fetch (&gCommandSequence, 1, __ATOMIC_RELAXED);
  rtDiagnostic (slot).type = slot.ucd[0];
  rtDiagnostic (slot).lun = slot.ucd[2];
  rtDiagnostic (slot).opcode = slot.ucd[rtDiagnostic (slot).type == 1 ? 16 : 12];
  rtDiagnostic (slot).scsi = 0xffff;
  rtDiagnostic (slot).response = 0;
  rtDiagnostic (slot).protocol = 0;
  rtDiagnostic (slot).residual = 0;
  rtDiagnostic (slot).sense = 0;
  uint8_t *utrd = slot.utrd;
  uint64_t ucdPhys = slot.ucdPhys;
  uint32_t *words = reinterpret_cast<uint32_t *> (utrd);
  for (unsigned index = 0; index < 8; index++) {
    words[index] = 0;
  }
  // I (bit 24): the completion sets IS.UTRCS and raises the interrupt (UFSHCI UTRD dword 0; Linux ufshcd UTP_REQ_DESC_INT_CMD).
  words[0] = 0x10000000U | (direction << 25) | (rtInterruptMode () ? 1U << 24 : 0U);
  words[2] = 0x0fU;
  words[4] = static_cast<uint32_t> (ucdPhys);
  words[5] = static_cast<uint32_t> (ucdPhys >> 32);
  words[6] = (512U >> 2) | ((kRspOffset >> 2) << 16);
  const unsigned entries = dataBytes ? (prds ? prds : 1U) : 0U;
  words[7] = entries | ((kPrdtOffset >> 2) << 16);
  if (dataBytes) {
    uint32_t *table = reinterpret_cast<uint32_t *> (slot.ucd + kPrdtOffset);
    uint64_t sum = 0;
    for (unsigned i = 0; i < entries; ++i) {
      const uint64_t address = prds ? prd[i].addr : slot.dataPhys;
      const uint32_t bytes = prds ? prd[i].bytes : dataBytes;
      if (!bytes || bytes > kPrdMax || (bytes & 3) || (address & 3)) return kIOReturnBadArgument;
      table[4 * i] = static_cast<uint32_t> (address);
      table[4 * i + 1] = static_cast<uint32_t> (address >> 32);
      table[4 * i + 2] = 0;
      table[4 * i + 3] = bytes - 1;
      sum += bytes;
    }
    if (sum != dataBytes) return kIOReturnBadArgument;
  }
  if (!gListUncached) cacheCivac (slot.utrd, 32);
  cacheCivac (slot.ucd, kPrdtOffset + 16 * (entries ? entries : 1U));
  if (dataBytes) cacheCivac (prds ? buffer : slot.data, dataBytes);
  if (!rtInterruptMode ()) hostWrite (0x20, 1);  // Acknowledge completion only; preserve error evidence. With interrupts the handler does.
  clock_interval_to_deadline (timeoutUs, kMicrosecondScale, deadline);
  __atomic_or_fetch (&gOutstanding, slot.bit, __ATOMIC_RELEASE);
  __atomic_store_n (&gSlotOpcode[slot.index], slot.ucd[slot.ucd[0] == 1 ? 16 : 12], __ATOMIC_RELAXED);
  __atomic_store_n (&gSlotIssuedAt[slot.index], mach_absolute_time (), __ATOMIC_RELAXED);
  hostWrite (0x58, slot.bit);
  return kIOReturnSuccess;
}

// `direction` is the command's UTRD data direction. Only data the device wrote (2) is invalidated after the command: the device does
// not change the data of a write (1), which was cleaned to memory before the doorbell, so its cache lines need nothing (Linux's
// arch_sync_dma_for_cpu returns at once for DMA_TO_DEVICE). An invalidate there would also drop a line a CPU wrote to the caller's
// page while the command ran.
static IOReturn rtAwait (const UfsSlot &slot, uint32_t dataBytes, uint64_t deadline, uint32_t *ocs, const void *buffer = nullptr,
                           unsigned direction = 2)
{
  RuntimeTransferPort port = { slot, deadline, 0 };
  UfsTransferPoll state;
  UfsTransferEnd end = pollUfsTransfer (port, state);
  if (end == UfsTransferEnd::Completed) {
    __atomic_store_n (&gSlotIssuedAt[slot.index], 0, __ATOMIC_RELAXED);
    __atomic_and_fetch (&gOutstanding, ~slot.bit, __ATOMIC_RELEASE);
    rtWaitDone (port.started);
  }
  rtDiagnostic (slot).submitted = 1;
  rtDiagnostic (slot).startTick = port.started;
  rtDiagnostic (slot).lastTick = port.lastClock;
  rtDiagnostic (slot).deadlineTick = deadline;
  rtDiagnostic (slot).dbLast = state.doorbellLast;
  rtDiagnostic (slot).isLast = state.interruptLast;
  rtDiagnostic (slot).deadlineSample = state.deadlineSample;
  rtDiagnostic (slot).polls = state.polls;
  rtDiagnostic (slot).dbSeen = state.doorbellSeen;
  rtDiagnostic (slot).isSeen = state.interruptSeen;
  *ocs = state.ocs;
  if (end != UfsTransferEnd::Completed) {
    // No cancel/reset/retry: the controller may still own the buffer, even
    // with DB=0. Keep it allocated and prohibit all subsequent CPU writes.
    rtQuarantine (end == UfsTransferEnd::Timeout ? 3 : 4, rtDiagnostic (slot).opcode);
    return rtRecord (slot, end == UfsTransferEnd::Timeout ? kIOReturnTimeout : kIOReturnIOError, *ocs);
  }
  cacheInvalidate (slot.ucd, kPrdtOffset);
  if (dataBytes && direction != 1) cacheInvalidate (buffer ? buffer : slot.data, dataBytes);
  rtDiagnostic (slot).response = rtLoadBe32 (slot.ucd + kRspOffset);
  return rtRecord (slot, *ocs == 0 ? kIOReturnSuccess : kIOReturnIOError, *ocs);
}

static IOReturn rtSubmit (const UfsSlot &slot, uint32_t direction, uint32_t dataBytes, uint32_t timeoutUs, uint32_t *ocs)
{
  uint64_t deadline = 0;
  IOReturn result = rtIssue (slot, direction, dataBytes, timeoutUs, &deadline, ocs);
  return result != kIOReturnSuccess ? result : rtAwait (slot, dataBytes, deadline, ocs, nullptr, direction);
}

static bool rtClearUcd (const UfsSlot &slot)
{
  RuntimeSubmitGuard submitGuard;
  if (__atomic_load_n (&gHost.poisoned, __ATOMIC_ACQUIRE)) return false;
  // Check before touching the command/response area, not just before ringing.
  if (read32 (gHost.hc, 0x58) & slot.bit) {
    rtQuarantineLocked (1, 0);
    rtRecord (slot, kIOReturnBusy, 15);
    return false;
  }
  if (read32 (gHost.hc, 0x20) & kUfsFatalStatus) {
    rtQuarantineLocked (2, 0);
    rtRecord (slot, kIOReturnIOError, 15);
    return false;
  }
  rtDiagnostic (slot) = UfsRootDiagnostics {};
  uint32_t *words = reinterpret_cast<uint32_t *> (slot.ucd);
  for (unsigned index = 0; index < (kPrdtOffset + 16) / 4; index++) {
    words[index] = 0;
  }
  return true;
}

static IOReturn rtProtocolError (const UfsSlot &slot, uint32_t reason)
{
  rtDiagnostic (slot).protocol = reason;
  rtQuarantine (5, rtDiagnostic (slot).opcode);
  return rtRecord (slot, kIOReturnIOError, rtDiagnostic (slot).ocs);
}

// Validate a completed response before interpreting zero-filled status bytes.
// Protocol codes: 1 type, 2 task tag, 3 LUN, 4 length/EHS, 5 query echo,
// 6 short/overrun transfer, 7 nonzero NOP result.
static IOReturn rtResponseHeader (const UfsSlot &slot, uint8_t type, uint8_t lun, uint8_t tag)
{
  const uint8_t *rsp = slot.ucd + kRspOffset;
  if (rsp[0] != type) return rtProtocolError (slot, 1);
  if (rsp[3] != tag) return rtProtocolError (slot, 2);
  if (rsp[2] != lun) return rtProtocolError (slot, 3);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (rsp[8] || length > 512 - 32) return rtProtocolError (slot, 4);
  return kIOReturnSuccess;
}

static IOReturn rtScsiIssue (const UfsSlot &slot, uint8_t lun, const uint8_t *cdb, unsigned cdbLength, unsigned direction,
                               uint32_t dataBytes, uint32_t timeoutUs, uint64_t *deadline, uint32_t *scsiStatus,
                               const UfsPrd *prd = nullptr, unsigned prds = 0, const void *buffer = nullptr)
{
  *scsiStatus = 0xffff;
  if (!cdb || !cdbLength || cdbLength > 16 || dataBytes > (prds ? kXferMax : kDataMax) || direction > 2 ||
      (dataBytes & 3) || (dataBytes ? !direction : direction)) return kIOReturnBadArgument;
  if (!rtClearUcd (slot)) return kIOReturnNotReady;
  uint8_t *ucd = slot.ucd;
  const uint8_t tag = static_cast<uint8_t> (slot.entry);
  ucd[0] = 0x01;                      // COMMAND UPIU
  ucd[1] = direction == 2 ? 0x40 : direction == 1 ? 0x20 : 0x00;   // R: data in, W: data out
  ucd[2] = lun;
  ucd[3] = tag;
  rtPutBe32 (ucd + 12, dataBytes);
  for (unsigned index = 0; index < cdbLength; index++) {
    ucd[16 + index] = cdb[index];
  }
  uint32_t ocs = 0xf;
  return rtIssue (slot, direction ? direction : 0U, dataBytes, timeoutUs, deadline, &ocs, prd, prds, buffer);
}

static IOReturn rtScsiAwait (const UfsSlot &slot, uint8_t lun, uint32_t dataBytes, uint64_t deadline, uint32_t *scsiStatus,
                               const void *buffer = nullptr, unsigned direction = 2)
{
  *scsiStatus = 0xffff;
  uint32_t ocs = 0xf;
  IOReturn status = rtAwait (slot, dataBytes, deadline, &ocs, buffer, direction);
  if (status != kIOReturnSuccess) return status;
  const uint8_t tag = static_cast<uint8_t> (slot.entry);
  status = rtResponseHeader (slot, 0x21, lun, tag);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = slot.ucd + kRspOffset;
  *scsiStatus = (uint32_t (rsp[6]) << 8) | rsp[7];
  rtDiagnostic (slot).scsi = *scsiStatus;
  rtDiagnostic (slot).residual = rtLoadBe32 (rsp + 12);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (length) {
    unsigned senseLength = (unsigned (rsp[32]) << 8) | rsp[33];
    if (length < 2 || senseLength > length - 2) return rtProtocolError (slot, 4);
    const uint8_t *sense = rsp + 34;
    if (senseLength >= 14 && ((sense[0] & 0x7f) == 0x70 || (sense[0] & 0x7f) == 0x71))
      rtDiagnostic (slot).sense = (uint32_t (sense[2] & 15) << 16) | (uint32_t (sense[12]) << 8) | sense[13];
    else if (senseLength >= 4 && ((sense[0] & 0x7f) == 0x72 || (sense[0] & 0x7f) == 0x73))
      rtDiagnostic (slot).sense = (uint32_t (sense[1] & 15) << 16) | (uint32_t (sense[2]) << 8) | sense[3];
  }
  if (!*scsiStatus && ((rsp[1] & 0x60) || rtDiagnostic (slot).residual)) return rtProtocolError (slot, 6);
  IOReturn result = *scsiStatus == 0 ? kIOReturnSuccess : kIOReturnNotReady;
  rtDiagnostic (slot).result = static_cast<uint32_t> (result);
  return result;
}

static IOReturn rtScsiDir (const UfsSlot &slot, uint8_t lun, const uint8_t *cdb, unsigned cdbLength, unsigned direction,
                             uint32_t dataBytes, uint32_t timeoutUs, uint32_t *scsiStatus,
                             const UfsPrd *prd = nullptr, unsigned prds = 0, const void *buffer = nullptr)
{
  uint64_t deadline = 0;
  IOReturn status = rtScsiIssue (slot, lun, cdb, cdbLength, direction, dataBytes, timeoutUs, &deadline, scsiStatus, prd, prds, buffer);
  return status != kIOReturnSuccess ? status
                                    : rtScsiAwait (slot, lun, dataBytes, deadline, scsiStatus, prds ? buffer : nullptr, direction);
}


#include "RuntimeQueue.h"
