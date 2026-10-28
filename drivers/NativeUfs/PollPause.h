// SPDX-License-Identifier: BSD-2-Clause-Patent
// A bounded polling hint, not a calibrated microsecond delay. The command's
// existing monotonic deadline remains responsible for its timeout.
// A stopped command clock is not repaired by bounding this hint batch.
#ifndef NATIVE_UFS_POLL_PAUSE_H
#define NATIVE_UFS_POLL_PAUSE_H

static constexpr unsigned kUfsPollPauseHintCount = 64;

template <class Hint>
static inline void ufsPollPause (Hint hint)
{
  for (unsigned count = 0; count < kUfsPollPauseHintCount; ++count) {
    hint ();
  }
}

#endif
