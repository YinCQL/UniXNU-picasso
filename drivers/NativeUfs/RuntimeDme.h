// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only local DME_GET (UFSHCI UICCMD=1). The port serializes UIC,
// supplies mapped registers, a bounded clock and sleep, and never resets HC.
// UFSHCI IS: link lost, device/controller/system bus/crypto fatal.
static constexpr uint32_t kUfsDmeFatalStatus = 0x70880U;
enum class UfsDmeResult { Success, Invalid, Unavailable, Busy, DeviceError, Timeout, Fatal };
template<class Port>
static UfsDmeResult ufsDmeGet (Port &port, uint32_t attribute, uint32_t selector, uint32_t *value)
{
  if (!value) return UfsDmeResult::Invalid;
  *value = ~uint32_t (0);
  if (attribute > 0xffff || selector > 0xffff) return UfsDmeResult::Invalid;
  if (!port.canQuery ()) return UfsDmeResult::Unavailable;
  if (!port.enter ()) return UfsDmeResult::Busy;
  struct UicGuard { Port &p; ~UicGuard () { p.leave (); } } uic = {port};
  if (!port.usable ()) return UfsDmeResult::Unavailable;
  if (!port.submit ()) return UfsDmeResult::Busy;
  if (!port.usable () || !port.idle () || !(port.read (0x30) & (1U << 3))) {
    port.unsubmit (); return UfsDmeResult::Unavailable;
  }
  if (port.read (0x20) & kUfsDmeFatalStatus) { port.unsubmit (); return UfsDmeResult::Fatal; }
  const uint64_t deadline = port.deadline ();
  port.write (0x20, 1U << 10); // UCCS only; preserve transfer and error bits.
  port.write (0x94, (attribute << 16) | selector);
  port.write (0x98, 0);
  port.write (0x9c, 0);
  port.write (0x90, 1);
  port.unsubmit ();
  for (;;) {
    const uint32_t status = port.read (0x20);
    if (status & kUfsDmeFatalStatus) { port.poison (); return UfsDmeResult::Fatal; }
    if (status & (1U << 10)) {
      const uint32_t code = port.read (0x98) & 0xff;
      *value = code ? 0xffff0000U | code : port.read (0x9c);
      port.write (0x20, 1U << 10);
      return code ? UfsDmeResult::DeviceError : UfsDmeResult::Success;
    }
    if (port.now () >= deadline) {
      // Check once more after the clock read: scheduling may have delayed it.
      const uint32_t final = port.read (0x20);
      if (final & (kUfsDmeFatalStatus | (1U << 10))) continue;
      port.poison (); return UfsDmeResult::Timeout;
    }
    port.pause ();
  }
}
