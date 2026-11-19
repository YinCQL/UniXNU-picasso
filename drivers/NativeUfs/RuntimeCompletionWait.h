// SPDX-License-Identifier: BSD-2-Clause-Patent
// Generation is sampled before the doorbell. Register the wait before checking
// it again so a completion between registration and thread_block stays visible.
template<class Port>
static void ufsRuntimeWaitCompletion (Port &port, uint32_t seen, uint64_t started, uint64_t deadline)
{
  const uint64_t now = port.now ();
  if (!port.interruptMode () || !port.canBlock () || now - started < port.spinTicks () || now >= deadline) {
    port.pause (); return;
  }
  const uint64_t remainingUs = port.nanoseconds (deadline - now) / 1000;
  const uint32_t us = remainingUs > 1000 ? 1000 : static_cast<uint32_t> (remainingUs);
  if (!us) { port.relax (); return; }
  if (!port.arm (us)) return;
  if (port.generation () != seen) { port.cancel (); return; }
  port.block ();
}
