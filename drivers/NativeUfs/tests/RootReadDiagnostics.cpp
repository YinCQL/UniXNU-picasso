// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#include "../RootReadDiagnostics.h"

struct Shared { uint32_t reports = 0, accepted = 0; };
#ifdef _WIN32
static DWORD WINAPI worker (void *context)
#else
static void *worker (void *context)
#endif
{
  auto *shared = static_cast<Shared *> (context);
  for (unsigned j = 0; j < 1000; ++j)
    if (ufsClaimFailureReport (&shared->reports))
      __atomic_fetch_add (&shared->accepted, 1, __ATOMIC_RELAXED);
  return 0;
}

int main ()
{
  Shared shared;
#ifdef _WIN32
  HANDLE threads[16];
  for (auto &thread : threads) {
    thread = CreateThread (nullptr, 0, worker, &shared, 0, nullptr);
    assert (thread);
  }
  assert (WaitForMultipleObjects (16, threads, TRUE, INFINITE) == WAIT_OBJECT_0);
  for (auto thread : threads) CloseHandle (thread);
#else
  pthread_t threads[16];
  for (auto &thread : threads) assert (!pthread_create (&thread, nullptr, worker, &shared));
  for (auto thread : threads) assert (!pthread_join (thread, nullptr));
#endif
  assert (shared.reports == 4 && shared.accepted == 4);
  shared.reports = UINT32_MAX;
  assert (!ufsClaimFailureReport (&shared.reports) && shared.reports == UINT32_MAX);
  puts ("PASS concurrent diagnostic budget saturates at four without wrapping");

  UfsRecordFailure failure;
  uint8_t expected[32], actual[32];
  for (unsigned i = 0; i < 32; ++i) { expected[i] = uint8_t(i); actual[i] = uint8_t(255 - i); }
  assert (!ufsRejectRecord (&failure, "hash_mismatch", UINT64_MAX, SIZE_MAX, SIZE_MAX, expected, actual));
  assert (failure.expectedValid && failure.actualValid);
  memset (expected, 0, sizeof (expected));
  assert (failure.expected[31] == 31); // Snapshot outlives the source buffer.
  const char *label = "0123456789012345678901234567890123456789";
  char line[256];
  int length = formatUfsRecordFailure (line, sizeof (line), label, failure);
  assert (length > 0 && length < 256 && size_t(length) == strlen (line) && line[length-1] == '\n');
  for (unsigned which = 0; which < 2; ++which) {
    bool wantExpected = which == 0;
    length = formatUfsRecordDigest (line, sizeof (line), label, failure, wantExpected);
    assert (length > 0 && length < 256 && size_t(length) == strlen (line) && line[length-1] == '\n');
    assert (strstr (line, wantExpected ? "0001020304050607" : "fffefdfcfbfaf9f8"));
  }
  char tiny[2] = {};
  assert (formatUfsRecordFailure (tiny, sizeof (tiny), label, failure) > 2 && tiny[1] == 0);
  assert (formatUfsRecordDigest (tiny, sizeof (tiny), label, failure, true) > 2 && tiny[1] == 0);
  puts ("PASS full-width fields, independent hash snapshots and bounded formatting");

  assert (!ufsRejectRecord (&failure, "sha256_failed", 0, 131072, 131072, expected));
  assert (failure.expectedValid && !failure.actualValid);
  formatUfsRecordDigest (line, sizeof (line), "SystemOS", failure, false);
  assert (strstr (line, "actual_sha256=unavailable"));
  assert (!ufsRejectRecord (&failure, "invalid_input", 0, 0, 0));
  assert (!failure.expectedValid && !failure.actualValid);
  assert (!ufsRejectRecord (nullptr, "invalid_input", 0, 0, 0));
  puts ("PASS unavailable hashes are explicit; rejection remains false with or without diagnostics");
}
