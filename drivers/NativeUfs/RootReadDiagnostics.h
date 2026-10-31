// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Bounded metadata only. The caller prints after releasing the window lock.
// No block contents, transport operations or changes to verification decisions.
static inline bool ufsClaimFailureReport (uint32_t *counter)
{
  uint32_t value = __atomic_load_n (counter, __ATOMIC_RELAXED);
  while (value < 4) {
    if (__atomic_compare_exchange_n (counter, &value, value + 1, false,
                                    __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return true;
  }
  return false;
}

struct UfsRecordFailure {
  const char *reason = nullptr;
  uint64_t record = 0, bytes = 0, expectedBytes = 0;
  bool expectedValid = false, actualValid = false;
  uint8_t expected[32] = {}, actual[32] = {};
};

static inline bool ufsRejectRecord (UfsRecordFailure *failure, const char *reason,
                                    uint64_t record, size_t bytes, size_t expectedBytes,
                                    const uint8_t *expected = nullptr, const uint8_t *actual = nullptr)
{
  if (failure) {
    *failure = {};
    failure->reason = reason;
    failure->record = record;
    failure->bytes = bytes;
    failure->expectedBytes = expectedBytes;
    failure->expectedValid = expected != nullptr;
    failure->actualValid = actual != nullptr;
    if (expected) memcpy (failure->expected, expected, sizeof (failure->expected));
    if (actual) memcpy (failure->actual, actual, sizeof (failure->actual));
  }
  return false;
}

static inline int formatUfsRecordFailure (char *out, size_t size, const char *window,
                                         const UfsRecordFailure &failure)
{
  return snprintf (out, size,
    "NATIVE_UFS_VERIFY read_failed window=%.31s record=%llu bytes=%llu expected_bytes=%llu reason=%.24s\n",
    window, static_cast<unsigned long long> (failure.record),
    static_cast<unsigned long long> (failure.bytes),
    static_cast<unsigned long long> (failure.expectedBytes), failure.reason);
}

static inline int formatUfsRecordDigest (char *out, size_t size, const char *window,
                                        const UfsRecordFailure &failure, bool expected)
{
  const uint8_t *digest = expected ? failure.expected : failure.actual;
  char hex[65] = "unavailable";
  if (expected ? failure.expectedValid : failure.actualValid) {
    static const char alphabet[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
      hex[i * 2] = alphabet[digest[i] >> 4];
      hex[i * 2 + 1] = alphabet[digest[i] & 15];
    }
    hex[64] = 0;
  }
  return snprintf (out, size, "NATIVE_UFS_VERIFY digest window=%.31s record=%llu %s=%s\n",
                   window, static_cast<unsigned long long> (failure.record),
                   expected ? "expected_sha256" : "actual_sha256", hex);
}
