// SPDX-License-Identifier: BSD-2-Clause-Patent
// Only the CRC-validated LUN 0 modem window. Caller owns its 4K bounce buffer;
// hostRead4k serializes DMA and refuses a quarantined controller under its lock.
extern "C" int native_ufs_modem_size(uint64_t *bytes) {
  if (!bytes) return -22;
  *bytes = 0;
  if (!native_ufs_store_ready()) return -11;
  if (!gModemFirst || gModemLast < gModemFirst || gModemLast > UINT32_MAX) return -2;
  *bytes = (gModemLast - gModemFirst + 1) * 4096ULL;
  return 0;
}

extern "C" int native_ufs_modem_read(uint64_t offset, void *out, uint32_t bytes, void *bounce) {
  uint64_t size;
  int error = native_ufs_modem_size(&size);
  if (error) return error;
  if (!out || !bounce || out == bounce || bytes > 65536 || offset > size || bytes > size - offset) return -22;
  auto *dst = static_cast<uint8_t *>(out);
  while (bytes) {
    uint32_t within = static_cast<uint32_t>(offset & 4095);
    uint32_t take = bytes < 4096 - within ? bytes : 4096 - within;
    if (hostRead4k(0, gModemFirst + offset / 4096, 1, bounce) != kIOReturnSuccess) return -5;
    memcpy(dst, static_cast<uint8_t *>(bounce) + within, take);
    dst += take; bytes -= take; offset += take;
  }
  return 0;
}
