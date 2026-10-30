// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared production command path. Included inside the root transport namespace.
// The host harness replaces only MMIO, DMA synchronization, clock and lock primitives.
#ifndef NATIVE_UFS_TRACE
#define NATIVE_UFS_TRACE(...) do { if (gRootDiagnosticEnabled) IOLog(__VA_ARGS__); } while (0)
#endif
#ifndef NATIVE_UFS_READ_TRACE
#define NATIVE_UFS_READ_TRACE(...) NATIVE_UFS_TRACE(__VA_ARGS__)
#endif
#include "PollPause.h"

struct HardwareTransferPort {
  uint64_t deadline, lastClock;
  uint32_t tracePoll;
  // Polling only observes the submitted slot. Re-ringing or restarting the
  // request list here can race completion and reuse DMA still owned by the HC.
  void beginPoll (uint32_t poll) {
    tracePoll = poll;
    if (poll == 1) NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT poll=begin\n");
  }
  uint32_t doorbell () {
    uint32_t value = read32 (gHost.hc, 0x58);
    if (tracePoll == 1) NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT poll invalidate0 doorbell=%x\n", value);
    return value;
  }
  uint32_t interrupt () {
    uint32_t value = read32 (gHost.hc, 0x20);
    if (tracePoll == 1) NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT poll invalidate0 interrupt=%x\n", value);
    return value;
  }
  uint32_t completedOcs () {
    if (tracePoll == 1) NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT poll invalidate0 ocs=begin\n");
    cacheInvalidate (gHost.virt, 64);
    uint32_t value = *reinterpret_cast<volatile uint32_t *> (gHost.virt + 8) & 0xff;
    if (tracePoll == 1) NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT poll invalidate0 ocs=%x\n", value);
    return value;
  }
  bool expired () {
    if (tracePoll == 1) NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT clock=begin\n");
    uint64_t now = mach_absolute_time ();
    lastClock = now;
    if (tracePoll == 1) NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT clock=end now=%llx deadline=%llx\n", now, deadline);
    return now >= deadline;
  }
  void delay () {
    if (tracePoll == 1) NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT delay=begin mode=cpu-yield hints=%u\n", kUfsPollPauseHintCount);
    ufsPollPause (hostCpuRelax);
    if (tracePoll == 1) NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT delay=end mode=cpu-yield\n");
  }
};

static void putBe32 (uint8_t *at, uint32_t value)
{
  at[0] = static_cast<uint8_t> (value >> 24);
  at[1] = static_cast<uint8_t> (value >> 16);
  at[2] = static_cast<uint8_t> (value >> 8);
  at[3] = static_cast<uint8_t> (value);
}

static uint32_t loadBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

// Issue the UPIU already placed at gHost.virt + kUcdOffset through request slot 0. `direction` is
// the UTRD data direction (0 none, 1 from host, 2 to host). Direction 1 is reachable only through hostWrite4k, which
// confines it to the persistent store window. Returns the overall command status
// (0 = completed) in *ocs. Timeout/fatal status quarantines the allocation.
static IOReturn hostSubmit (uint32_t direction, uint32_t dataBytes, uint32_t timeoutUs, uint32_t *ocs)
{
  *ocs = 15;
  if (!timeoutUs || dataBytes > kDataMax || (dataBytes & 3) ||
      (dataBytes ? (direction != 1 && direction != 2) : direction != 0)) return kIOReturnBadArgument;
  if (gHost.poisoned) return kIOReturnNotReady;
  uint32_t doorbell = read32 (gHost.hc, 0x58);
  uint32_t interrupt = read32 (gHost.hc, 0x20);
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT begin db=%x is=%x\n", doorbell, interrupt);
  if (doorbell != 0) {
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (kIOReturnBusy, *ocs);
  }
  if (interrupt & kUfsFatalStatus) {
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (kIOReturnIOError, *ocs);
  }
  gRootDiagnostic.command++;
  gRootDiagnostic.type = gHost.virt[kUcdOffset];
  gRootDiagnostic.lun = gHost.virt[kUcdOffset + 2];
  gRootDiagnostic.opcode = gHost.virt[kUcdOffset + (gRootDiagnostic.type == 1 ? 16 : 12)];
  gRootDiagnostic.scsi = 0xffff;
  gRootDiagnostic.response = 0;
  gRootDiagnostic.protocol = 0;
  gRootDiagnostic.residual = 0;
  gRootDiagnostic.sense = 0;
  uint8_t *utrd = gHost.virt;
  uint64_t ucdPhys = gHost.phys + kUcdOffset;
  uint32_t *words = reinterpret_cast<uint32_t *> (utrd);
  for (unsigned index = 0; index < 8; index++) {
    words[index] = 0;
  }
  words[0] = 0x10000000U | (direction << 25);
  words[2] = 0x0fU;
  words[4] = static_cast<uint32_t> (ucdPhys);
  words[5] = static_cast<uint32_t> (ucdPhys >> 32);
  words[6] = (512U >> 2) | ((kRspOffset >> 2) << 16);
  words[7] = (dataBytes ? 1U : 0U) | ((kPrdtOffset >> 2) << 16);
  if (dataBytes) {
    uint64_t dataPhys = gHost.phys + kDataOffset;
    uint32_t *prd = reinterpret_cast<uint32_t *> (gHost.virt + kUcdOffset + kPrdtOffset);
    prd[0] = static_cast<uint32_t> (dataPhys);
    prd[1] = static_cast<uint32_t> (dataPhys >> 32);
    prd[2] = 0;
    prd[3] = dataBytes - 1;
  }
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT utrd=end\n");
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT cache_clean=begin\n");
  cacheCivac (gHost.virt, kDataOffset + dataBytes);
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT cache_clean=end\n");
  hostWrite (0x20, 1);  // Acknowledge completion only; preserve error evidence.
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT is_ack=end\n");
  HardwareTransferPort port;
  port.tracePoll = 0;
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT deadline=begin timeout_us=%u\n", timeoutUs);
  clock_interval_to_deadline (timeoutUs, kMicrosecondScale, &port.deadline);
  port.lastClock = mach_absolute_time ();
  gRootDiagnostic.startTick = port.lastClock;
  gRootDiagnostic.deadlineTick = port.deadline;
  gRootDiagnostic.timeoutUs = timeoutUs;
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT deadline=end value=%llx\n", port.deadline);
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT doorbell=begin\n");
  hostWrite (0x58, 1);
  gRootDiagnostic.submitted = 1;
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT doorbell=end\n");
  UfsTransferPoll state;
  UfsTransferEnd end = pollUfsTransfer (port, state);
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT poll=end result=%u polls=%u ocs=%x db=%x is=%x\n",
                    static_cast<unsigned> (end), state.polls, state.ocs,
                    state.doorbellSeen, state.interruptSeen);
  gRootDiagnostic.polls = state.polls;
  gRootDiagnostic.dbSeen = state.doorbellSeen;
  gRootDiagnostic.isSeen = state.interruptSeen;
  gRootDiagnostic.dbLast = state.doorbellLast;
  gRootDiagnostic.isLast = state.interruptLast;
  gRootDiagnostic.deadlineSample = state.deadlineSample;
  gRootDiagnostic.lastTick = port.lastClock;
  *ocs = state.ocs;
  if (end != UfsTransferEnd::Completed) {
    // No cancel/reset/retry: the controller may still own the buffer, even
    // with DB=0. Keep it allocated and prohibit all subsequent CPU writes.
    gHost.poisoned = true;
    gHost.ready = false;
    return recordTransport (end == UfsTransferEnd::Timeout ? kIOReturnTimeout : kIOReturnIOError, *ocs);
  }
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT response invalidate=begin\n");
  cacheInvalidate (gHost.virt, kDataOffset + dataBytes);
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT response invalidate=end\n");
  gRootDiagnostic.response = loadBe32 (gHost.virt + kUcdOffset + kRspOffset);
  NATIVE_UFS_TRACE ("NATIVE_UFS_SUBMIT response read=end word0=%08x\n", gRootDiagnostic.response);
  return recordTransport (*ocs == 0 ? kIOReturnSuccess : kIOReturnIOError, *ocs);
}

static bool hostClearUcd (void)
{
  NATIVE_UFS_TRACE ("NATIVE_UFS_UCD clear=begin\n");
  if (gHost.poisoned) {
    NATIVE_UFS_TRACE ("NATIVE_UFS_UCD clear=failed reason=poisoned\n");
    return false;
  }
  // Preflight may reject before a new command is submitted. Do not label the
  // previous command's timing/poll samples as belonging to this attempt.
  gRootDiagnostic.submitted = 0;
  gRootDiagnostic.polls = gRootDiagnostic.dbSeen = gRootDiagnostic.isSeen = 0;
  gRootDiagnostic.dbLast = gRootDiagnostic.isLast = gRootDiagnostic.deadlineSample = 0;
  gRootDiagnostic.startTick = gRootDiagnostic.lastTick = gRootDiagnostic.deadlineTick = 0;
  gRootDiagnostic.timeoutUs = 0;
  // Check before touching the command/response area, not just before ringing.
  uint32_t doorbell = read32 (gHost.hc, 0x58);
  if (doorbell != 0) {
    gHost.poisoned = true;
    gHost.ready = false;
    recordTransport (kIOReturnBusy, 15);
    NATIVE_UFS_TRACE ("NATIVE_UFS_UCD clear=failed reason=busy db=%x\n", doorbell);
    return false;
  }
  uint32_t interrupt = read32 (gHost.hc, 0x20);
  if (interrupt & kUfsFatalStatus) {
    gHost.poisoned = true;
    gHost.ready = false;
    recordTransport (kIOReturnIOError, 15);
    NATIVE_UFS_TRACE ("NATIVE_UFS_UCD clear=failed reason=fatal is=%x\n", interrupt);
    return false;
  }
  uint32_t *words = reinterpret_cast<uint32_t *> (gHost.virt + kUcdOffset);
  for (unsigned index = 0; index < (kPrdtOffset + 16) / 4; index++) {
    words[index] = 0;
  }
  NATIVE_UFS_TRACE ("NATIVE_UFS_UCD clear=end\n");
  return true;
}

static IOReturn hostProtocolError (uint32_t reason)
{
  gRootDiagnostic.protocol = reason;
  gHost.ready = false;
  gHost.poisoned = true;
  return recordTransport (kIOReturnIOError, gRootDiagnostic.ocs);
}

// Validate a completed response before interpreting zero-filled status bytes.
// Protocol codes: 1 type, 2 task tag, 3 LUN, 4 length/EHS, 5 query echo,
// 6 short/overrun transfer, 7 nonzero NOP result.
static IOReturn hostResponseHeader (uint8_t type, uint8_t lun, uint8_t tag)
{
  const uint8_t *rsp = gHost.virt + kUcdOffset + kRspOffset;
  if (rsp[0] != type) return hostProtocolError (1);
  if (rsp[3] != tag) return hostProtocolError (2);
  if (rsp[2] != lun) return hostProtocolError (3);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (rsp[8] || length > 512 - 32) return hostProtocolError (4);
  return kIOReturnSuccess;
}

static IOReturn hostNop (void)
{
  NATIVE_UFS_TRACE ("NATIVE_UFS_NOP begin\n");
  if (!hostClearUcd ()) {
    NATIVE_UFS_TRACE ("NATIVE_UFS_NOP clear=failed\n");
    return kIOReturnNotReady;
  }
  gHost.virt[kUcdOffset + 3] = 1;
  uint32_t ocs = 15;
  IOReturn result = hostSubmit (0, 0, 200000, &ocs);
  NATIVE_UFS_TRACE ("NATIVE_UFS_NOP submit_result=%x ocs=%x\n",
                    static_cast<unsigned> (result), ocs);
  if (result != kIOReturnSuccess) return result;
  result = hostResponseHeader (0x20, 0, 1);
  if (result != kIOReturnSuccess) return result;
  const uint8_t *rsp = gHost.virt + kUcdOffset + kRspOffset;
  if (rsp[10] || rsp[11]) return hostProtocolError (4);
  if (rsp[6] || rsp[7]) return hostProtocolError (7);
  return kIOReturnSuccess;
}

// Run a SCSI command. direction 0: no data, 1: host -> device from the DMA data area (already filled by the caller), 2: device
// -> host into the DMA data area. Callers other than hostWrite4k never pass direction 1.
static IOReturn hostScsiDir (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, unsigned direction, uint32_t dataBytes,
                             uint32_t timeoutUs, uint32_t *scsiStatus)
{
  *scsiStatus = 0xffff;
  if (!cdb || !cdbLength || cdbLength > 16 || dataBytes > kDataMax || direction > 2 ||
      (dataBytes & 3) || (dataBytes ? !direction : direction)) return kIOReturnBadArgument;
  if (!hostClearUcd ()) return kIOReturnNotReady;
  uint8_t *ucd = gHost.virt + kUcdOffset;
  ucd[0] = 0x01;                      // COMMAND UPIU
  ucd[1] = direction == 2 ? 0x40 : direction == 1 ? 0x20 : 0x00;   // R: data in, W: data out
  ucd[2] = lun;
  putBe32 (ucd + 12, dataBytes);
  for (unsigned index = 0; index < cdbLength; index++) {
    ucd[16 + index] = cdb[index];
  }
  uint32_t ocs = 0xf;
  IOReturn status = hostSubmit (direction ? direction : 0U, dataBytes, timeoutUs, &ocs);
  if (status != kIOReturnSuccess) return status;
  status = hostResponseHeader (0x21, lun, 0);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = ucd + kRspOffset;
  *scsiStatus = (uint32_t (rsp[6]) << 8) | rsp[7];
  gRootDiagnostic.scsi = *scsiStatus;
  gRootDiagnostic.residual = loadBe32 (rsp + 12);
  unsigned length = (unsigned (rsp[10]) << 8) | rsp[11];
  if (length) {
    unsigned senseLength = (unsigned (rsp[32]) << 8) | rsp[33];
    if (length < 2 || senseLength > length - 2) return hostProtocolError (4);
    const uint8_t *sense = rsp + 34;
    if (senseLength >= 14 && ((sense[0] & 0x7f) == 0x70 || (sense[0] & 0x7f) == 0x71))
      gRootDiagnostic.sense = (uint32_t (sense[2] & 15) << 16) | (uint32_t (sense[12]) << 8) | sense[13];
    else if (senseLength >= 4 && ((sense[0] & 0x7f) == 0x72 || (sense[0] & 0x7f) == 0x73))
      gRootDiagnostic.sense = (uint32_t (sense[1] & 15) << 16) | (uint32_t (sense[2]) << 8) | sense[3];
  }
  if (!*scsiStatus && ((rsp[1] & 0x60) || gRootDiagnostic.residual)) return hostProtocolError (6);
  IOReturn result = *scsiStatus == 0 ? kIOReturnSuccess : kIOReturnNotReady;
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  return result;
}

// The original read-only entry point: data, when present, always flows device -> host.
static IOReturn hostScsi (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t dataBytes,
                          uint32_t timeoutUs, uint32_t *scsiStatus)
{
  if (dataBytes && !toHost) { *scsiStatus = 0xffff; return kIOReturnBadArgument; }
  return hostScsiDir (lun, cdb, cdbLength, dataBytes ? 2U : 0U, dataBytes, timeoutUs, scsiStatus);
}

// Query read request (opcode 0x03 READ ATTRIBUTE / 0x05 READ FLAG). Returns the value field.
static IOReturn hostQueryRead (uint8_t opcode, uint8_t idn, uint32_t *value)
{
  if (!value || (opcode != 3 && opcode != 5)) return kIOReturnBadArgument;
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUERY begin opcode=%02x idn=%02x\n", opcode, idn);
  if (!hostClearUcd ()) {
    NATIVE_UFS_TRACE ("NATIVE_UFS_QUERY clear=failed\n");
    return kIOReturnNotReady;
  }
  uint8_t *ucd = gHost.virt + kUcdOffset;
  ucd[0] = 0x16;                      // QUERY REQUEST
  ucd[3] = 1;
  ucd[5] = 0x01;                      // standard read request (header dword 1, byte 1)
  ucd[12] = opcode;
  ucd[13] = idn;
  uint32_t ocs = 0xf;
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUERY submit=begin\n");
  IOReturn status = hostSubmit (0, 0, 200000, &ocs);
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUERY submit=end result=%08x ocs=%x\n",
                    static_cast<unsigned> (status), ocs);
  if (status != kIOReturnSuccess) {
    return status;
  }
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUERY response header=begin\n");
  status = hostResponseHeader (0x36, 0, 1);
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUERY response header=end result=%08x protocol=%u\n",
                    static_cast<unsigned> (status), gRootDiagnostic.protocol);
  if (status != kIOReturnSuccess) return status;
  const uint8_t *rsp = ucd + kRspOffset;
  if (rsp[10] || rsp[11]) {
    NATIVE_UFS_TRACE ("NATIVE_UFS_QUERY response reject=length\n");
    return hostProtocolError (4);
  }
  if (rsp[12] != opcode || rsp[13] != idn || rsp[14] || rsp[15]) {
    NATIVE_UFS_TRACE ("NATIVE_UFS_QUERY response reject=echo\n");
    return hostProtocolError (5);
  }
  if (rsp[6] != 0) {
    NATIVE_UFS_TRACE ("NATIVE_UFS_QUERY response reject=result code=%02x\n", rsp[6]);
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnError);
    return kIOReturnError;
  }
  *value = loadBe32 (rsp + 20);
  NATIVE_UFS_TRACE ("NATIVE_UFS_QUERY response=end value=%x\n", *value);
  return kIOReturnSuccess;
}

// Accessed under gHost.lock. Keep boot read boundaries bounded even if APFS
// performs many requests while looking for the root volume.
static unsigned gRootReadTraceCount = 0;

// Read `blocks` (<= kDataMax / 4096) 4096-byte blocks of LUN `lun` starting at `lba` into dst.
static IOReturn hostRead4k (uint8_t lun, uint64_t lba, uint32_t blocks, void *dst)
{
  if (!dst || !blocks || blocks > kDataMax / 4096 || lba > 0xffffffffULL - (blocks - 1)) {
    return kIOReturnBadArgument;
  }
  if (!gHost.lock) return kIOReturnNotReady;
  IOLockLock (gHost.lock);
  // A preceding request may have quarantined the host while this reader
  // waited for the lock. Do not touch its DMA or misreport a parameter error.
  if (!gHost.ready || gHost.poisoned) {
    IOLockUnlock (gHost.lock);
    return kIOReturnNotReady;
  }
  const bool traceRead = gRootReadTraceCount < 8;
  if (traceRead) {
    ++gRootReadTraceCount;
    NATIVE_UFS_READ_TRACE ("NATIVE_UFS_READ begin sample=%u lun=%u lba=%llx blocks=%u\n",
                          gRootReadTraceCount, lun, static_cast<unsigned long long> (lba), blocks);
  }
  const uint32_t commandBefore = gRootDiagnostic.command;
  gRootDiagnostic.lba = lba;
  uint8_t cdb[10] = { 0x28, 0, uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                      0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  uint32_t scsi = 0;
  IOReturn status = hostScsi (lun, cdb, 10, true, blocks * 4096U, 1000000, &scsi);
  if (status == kIOReturnSuccess) {
    memcpy (dst, gHost.virt + kDataOffset, blocks * 4096U);
  }
  if (traceRead || status != kIOReturnSuccess) {
    if (gRootDiagnostic.command != commandBefore) {
      NATIVE_UFS_READ_TRACE ("NATIVE_UFS_READ end result=%08x lba=%llx blocks=%u command=%u polls=%u ocs=%x scsi=%x protocol=%u\n",
                            static_cast<unsigned> (status), static_cast<unsigned long long> (lba), blocks,
                            gRootDiagnostic.command, gRootDiagnostic.polls, gRootDiagnostic.ocs,
                            scsi, gRootDiagnostic.protocol);
    } else {
      NATIVE_UFS_READ_TRACE ("NATIVE_UFS_READ end result=%08x lba=%llx blocks=%u command_started=0 scsi=%x\n",
                            static_cast<unsigned> (status), static_cast<unsigned long long> (lba), blocks, scsi);
    }
  }
  if (traceRead && gRootReadTraceCount == 8) {
    NATIVE_UFS_READ_TRACE ("NATIVE_UFS_READ sample limit reached; further successful reads are quiet\n");
  }
  IOLockUnlock (gHost.lock);
  return status;
}

// ---- Persistent store and log windows ------------------------------------------------------------------------------
// The only place this driver writes. At most two windows of 4096-byte LBAs on LUN 0 (the Data store and the diagnostic log),
// each opened once by its integration after it has proved that the window lies inside the userdata partition and clear of the
// root image; every write is checked against them again here, so nothing outside them can be written even if a caller is wrong.
static bool gStoreOpen = false, gLogOpen = false;
static uint64_t gStoreFirst = 0, gStoreBlocks = 0, gLogFirst = 0, gLogBlocks = 0;

static bool hostLogOpen (uint64_t first, uint64_t blocks)
{
  if (gLogOpen || !blocks || first > 0xffffffffULL || blocks > 0xffffffffULL - first) return false;
  if (gStoreOpen && first < gStoreFirst + gStoreBlocks && gStoreFirst < first + blocks) return false;
  gLogFirst = first;
  gLogBlocks = blocks;
  gLogOpen = true;
  return true;
}

static bool hostInWindow (uint64_t lba, uint32_t blocks)
{
  if (gStoreOpen && lba >= gStoreFirst && lba - gStoreFirst < gStoreBlocks && blocks <= gStoreBlocks - (lba - gStoreFirst)) return true;
  return gLogOpen && lba >= gLogFirst && lba - gLogFirst < gLogBlocks && blocks <= gLogBlocks - (lba - gLogFirst);
}

static bool hostStoreOpen (uint64_t first, uint64_t blocks)
{
  if (gStoreOpen || !blocks || first > 0xffffffffULL || blocks > 0xffffffffULL - first) return false;
  if (gLogOpen && first < gLogFirst + gLogBlocks && gLogFirst < first + blocks) return false;
  gStoreFirst = first;
  gStoreBlocks = blocks;
  gStoreOpen = true;
  return true;
}

// WRITE(10) of `blocks` 4096-byte blocks from src to lba on LUN 0; the range must lie inside one open window.
static IOReturn hostWrite4k (uint8_t lun, uint64_t lba, uint32_t blocks, const void *src, bool fua = false)
{
  if (!src || !blocks || blocks > kDataMax / 4096 || lun != 0 || !hostInWindow (lba, blocks)) {
    return kIOReturnBadArgument;
  }
  if (!gHost.lock) return kIOReturnNotReady;
  IOLockLock (gHost.lock);
  if (!gHost.ready || gHost.poisoned) {
    IOLockUnlock (gHost.lock);
    return kIOReturnNotReady;
  }
  gRootDiagnostic.lba = lba;
  memcpy (gHost.virt + kDataOffset, src, blocks * 4096U);
  uint8_t cdb[10] = { 0x2a, uint8_t (fua ? 0x08 : 0), uint8_t (lba >> 24), uint8_t (lba >> 16), uint8_t (lba >> 8), uint8_t (lba),
                      0, uint8_t (blocks >> 8), uint8_t (blocks), 0 };
  uint32_t scsi = 0;
  IOReturn status = hostScsiDir (0, cdb, 10, 1, blocks * 4096U, 2000000, &scsi);
  IOLockUnlock (gHost.lock);
  return status;
}

// SYNCHRONIZE CACHE(10) for the whole LUN: everything written so far is on the medium when this returns.
static IOReturn hostSyncCache (void)
{
  if (!gHost.lock || (!gStoreOpen && !gLogOpen)) return kIOReturnNotReady;
  IOLockLock (gHost.lock);
  if (!gHost.ready || gHost.poisoned) {
    IOLockUnlock (gHost.lock);
    return kIOReturnNotReady;
  }
  uint8_t cdb[10] = { 0x35 };
  uint32_t scsi = 0;
  IOReturn status = hostScsiDir (0, cdb, 10, 0, 0, 10000000, &scsi);
  IOLockUnlock (gHost.lock);
  return status;
}

