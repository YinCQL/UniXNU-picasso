// SPDX-License-Identifier: BSD-2-Clause-Patent
// Single-slot completion policy. Port supplies ordered MMIO, invalidate-only
// descriptor reads, a monotonic deadline and bounded pacing. No resubmission.
enum class UfsTransferEnd { Completed, Fatal, Timeout };
struct UfsTransferPoll {
  uint32_t polls = 0, doorbellSeen = 0, interruptSeen = 0, ocs = 15;
  uint32_t doorbellLast = 0, interruptLast = 0;
  bool deadlineSample = false;
};
static constexpr uint32_t kUfsFatalStatus = 0x71880U;

template <class Port>
static UfsTransferEnd pollUfsTransfer(Port &port, UfsTransferPoll &state)
{
  for (;;) {
    ++state.polls;
    port.beginPoll(state.polls);
    uint32_t doorbell = port.doorbell();
    state.doorbellLast = doorbell;
    state.doorbellSeen |= doorbell;
    state.interruptLast = port.interrupt();
    state.interruptSeen |= state.interruptLast;
    if (state.interruptSeen & kUfsFatalStatus) return UfsTransferEnd::Fatal;
    if (!(doorbell & 1)) {
      state.ocs = port.completedOcs();
      // A zero doorbell alone does not prove that this command was accepted.
      // Also allow a completion so fast that software never observes DB=1.
      if (state.ocs != 15) return UfsTransferEnd::Completed;
    }
    if (state.deadlineSample) return UfsTransferEnd::Timeout;
    if (port.expired()) {
      // The thread may have been preempted after sampling hardware. Observe
      // it once more after detecting expiry before quarantining this slot.
      // No extra delay, extended deadline, resubmission or DMA publication.
      state.deadlineSample = true;
      continue;
    }
    port.delay();
  }
}
