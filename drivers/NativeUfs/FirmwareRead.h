// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only discovery after RootLayout.h. Uses its endian/CRC helpers, but
// caller-owned scratch: root publication and persistence may be active here.
#include "FirmwareWindow.h"

static int firmwareGpt(uint8_t lun, uint64_t capacity, uint8_t *scratch, UfsFirmwareWindow *window)
{
  if (hostRead4k(lun, 1, 1, scratch) != kIOReturnSuccess) return -5;
  if (le32(scratch) != 0x20494645U || le32(scratch + 4) != 0x54524150U) return -2;
  uint32_t headerBytes = le32(scratch + 12), count = le32(scratch + 80);
  uint64_t entries = le64(scratch + 72), first = le64(scratch + 40), last = le64(scratch + 48);
  uint64_t backup = le64(scratch + 32);
  if (le32(scratch + 8) != 0x10000 || headerBytes < 92 || headerBytes > 4096 ||
      le32(scratch + 20) || le64(scratch + 24) != 1 || le32(scratch + 84) != 128 ||
      rootCrc32(scratch, headerBytes, true) != le32(scratch + 16)) return -22;
  uint32_t crc = le32(scratch + 88);
  if (entries < 2 || entries > 64 || !count || count > 128) return -22;
  uint32_t blocks = (count * 128 + 4095) / 4096;
  if (capacity < 6 || capacity > UINT32_MAX || backup != capacity - 1 ||
      first < entries + blocks || last < first || last >= backup || backup - last <= blocks) return -22;
  if (hostRead4k(lun, entries, blocks, scratch) != kIOReturnSuccess) return -5;
  if (rootCrc32(scratch, count * 128) != crc) return -22;
  unsigned matches = 0;
  UfsFirmwareWindow candidate = {};
  for (unsigned i = 0; i < count; ++i) {
    const uint8_t *entry = scratch + i * 128;
    if (!rootEntryUsed(entry)) continue;
    uint64_t start = le64(entry + 32), end = le64(entry + 40);
    if (start < first || end < start || end > last) return -22;
    for (unsigned j = 0; j < i; ++j) {
      const uint8_t *other = scratch + j * 128;
      if (rootEntryUsed(other) && start <= le64(other + 40) && le64(other + 32) <= end) return -22;
    }
    static const char name[] = "modem";
    bool match = true;
    for (unsigned c = 0; c < 36; ++c) {
      uint8_t expected = c < sizeof name ? static_cast<uint8_t>(name[c]) : 0;
      if (entry[56 + c * 2] != expected || entry[57 + c * 2]) match = false;
    }
    if (match) { ++matches; candidate = {start, end, lun}; }
  }
  if (matches > 1) return -17;
  if (!matches) return -2;
  *window = candidate;
  return 0;
}

extern "C" int native_ufs_modem_open(UfsFirmwareWindow *window, void *scratch, uint32_t scratchBytes)
{
  if (!window || !scratch || scratchBytes < 16384) return -22;
  *window = {};
  if (!native_ufs_store_ready()) return -11;
  // Picasso V13.0.5.0.SGICNXM partition.xml + rawprogram4.xml + gpt_main4.bin
  // agree: modem is on general LU4. Discover its bounds from the live GPT,
  // not the flash-package capacity placeholders or a hardcoded partition LBA.
  constexpr uint8_t lun = 4;
  uint64_t capacity = 0;
  int error = hostFirmwareGeometry(lun, &capacity);
  IOLog("PICASSO_ADSP firmware_lun=%u stage=geometry error=%d blocks=%llu\n",
        lun, error, static_cast<unsigned long long>(capacity));
  if (error) return error;
  error = firmwareGpt(lun, capacity, static_cast<uint8_t *>(scratch), window);
  IOLog("PICASSO_ADSP firmware_lun=%u stage=gpt error=%d first=%llx last=%llx\n",
        lun, error, static_cast<unsigned long long>(window->first), static_cast<unsigned long long>(window->last));
  return error;
}

extern "C" int native_ufs_modem_read(const UfsFirmwareWindow *window, uint64_t offset,
                                     void *out, uint32_t bytes, void *bounce)
{
  if (!native_ufs_store_ready()) return -11;
  if (!window || window->lun != 4 || !window->first || window->last < window->first ||
      window->last > UINT32_MAX) return -22;
  uint64_t size = (window->last - window->first + 1) * 4096ULL;
  if (!out || !bounce || bytes > 65536 || offset > size || bytes > size - offset) return -22;
  // Also reject partially overlapping buffers, not just equal pointers.
  uintptr_t dstAddress = reinterpret_cast<uintptr_t>(out), bounceAddress = reinterpret_cast<uintptr_t>(bounce);
  if (bytes && (dstAddress >= bounceAddress ? dstAddress - bounceAddress < 4096 : bounceAddress - dstAddress < bytes)) return -22;
  auto *dst = static_cast<uint8_t *>(out);
  while (bytes) {
    uint32_t within = static_cast<uint32_t>(offset & 4095);
    uint32_t take = bytes < 4096 - within ? bytes : 4096 - within;
    if (hostRead4k(static_cast<uint8_t>(window->lun), window->first + offset / 4096, 1, bounce) != kIOReturnSuccess) return -5;
    memcpy(dst, static_cast<uint8_t *>(bounce) + within, take);
    dst += take; bytes -= take; offset += take;
  }
  return 0;
}
