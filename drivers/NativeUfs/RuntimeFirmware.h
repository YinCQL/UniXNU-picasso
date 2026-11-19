// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only discovery, after RootCommands.h. Qualcomm's stock SM7250 ufs.h /
// ufshcd.c define general LU indices 0..7, READ_DESC=1, UNIT=2 and these offsets.
// Query descriptors arrive inside the response UPIU, without a data PRDT.
static int runtimeFirmwareGeometry(uint8_t lun, uint64_t *blocks)
{
  *blocks = 0;
  if (lun >= 8) return -22;
  if (!gHost.lock) return -11;
  bool pooled = false;
  const UfsSlot slot = rtSlotTake (&pooled);
  if (!pooled) return -11;
  int error = -5;
  do {
    if (!__atomic_load_n (&gHost.ready, __ATOMIC_ACQUIRE) || __atomic_load_n (&gHost.poisoned, __ATOMIC_ACQUIRE) || !rtClearUcd(slot)) break;
    uint8_t *cmd = slot.ucd;
    cmd[0] = 0x16; cmd[3] = static_cast<uint8_t> (slot.entry); cmd[5] = 1;
    cmd[12] = 1; cmd[13] = 2; cmd[14] = lun; cmd[19] = 255;
    uint32_t ocs = 15;
    if (rtSubmit(slot, 0, 0, 200000, &ocs) != kIOReturnSuccess ||
        rtResponseHeader(slot, 0x36, 0, static_cast<uint8_t> (slot.entry)) != kIOReturnSuccess) break;
    const uint8_t *rsp = cmd + kRspOffset;
    if (rsp[12] != 1 || rsp[13] != 2 || rsp[14] != lun || rsp[15]) {
      rtProtocolError(slot, 5); break;
    }
    unsigned length = (unsigned(rsp[10]) << 8) | rsp[11];
    if (rsp[6]) {
      // INVALID_INDEX is an absent general LU. Other query errors are not absence.
      if (length) { rtProtocolError(slot, 4); break; }
      error = rsp[6] == 0xfc ? -2 : -5;
      break;
    }
    if (length < 0x23 || length > 255 || ((unsigned(rsp[18]) << 8) | rsp[19]) != length) {
      rtProtocolError(slot, 4); break;
    }
    const uint8_t *desc = rsp + 32;
    if (desc[0] != length || desc[1] != 2 || desc[2] != lun || desc[3] > 1) {
      error = -22; break;
    }
    if (!desc[3]) { error = -2; break; }
    if (desc[10] != 12) { error = -95; break; }
    uint64_t count = (uint64_t(rtLoadBe32(desc + 11)) << 32) | rtLoadBe32(desc + 15);
    if (count < 6 || count > UINT32_MAX) { error = -95; break; }
    // READ CAPACITY(10) independently confirms the active SCSI block geometry.
    // Consume at most two UNIT ATTENTION responses; no reset or write command.
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
      uint8_t cdb[10] = {0x25};
      uint32_t scsi = 0;
      rtDiagnostic(slot).sense = 0;
      IOReturn result = rtScsiDir(slot, lun, cdb, sizeof cdb, 2, 8, 200000, &scsi);
      if (result == kIOReturnSuccess) {
        const uint8_t *capacity = slot.data;
        if (rtLoadBe32(capacity + 4) != 4096 || uint64_t(rtLoadBe32(capacity)) + 1 != count) error = -22;
        else { *blocks = count; error = 0; }
        break;
      }
      if (__atomic_load_n (&gHost.poisoned, __ATOMIC_ACQUIRE) || scsi != 2 || (rtDiagnostic(slot).sense >> 16) != 6) break;
    }
  } while (false);
  rtSlotGive (slot, pooled);
  return error;
}
