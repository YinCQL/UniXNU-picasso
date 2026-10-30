# Userspace watchdog after AES startup

For build80's Apple logo and the expanded bounded service descriptions, see
[the keystore-state investigation](keystore-state-investigation.md).

Build 76's service-exit diagnostics identify a missing standalone Objective-C
library. See [the SystemOS union investigation](objc-trampolines-investigation.md)
for the current change and validation limits. The build-75 work below is historical.

Build 75 / [40964749987](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40964749987)
at `59eaf23dc10066a5a0f0b799764c591451beb950` completed successfully.
The downloaded artifact is `12938756852` / `picasso-bootimg-75`.
Its kernel SHA-256 is
`fd8ec87e930f98dbca59f0188e3bf7dbab8de794d6975f570f5a1b1eec6ea566`;
UUID `CAE1ABB9-D122-3630-8796-04EE72DC12EF` matches the supplied
`picasso-windows-20261030-063347-779.log`. The user confirms the latest image
and an automatic phone reboot. The boot header, FAT allocation chains and
kernel/Data/trust/ADT/loader bytes were independently checked.

The device passes AES known-answer and interface tests (`kat=1 interface=1`)
and publishes IOAESAccelerator. Early boot completes, the original watchdog
client opens, and the prior `defaults` busy timeout is absent. The new panic
is `userspace watchdog timeout: no successful checkins from backboardd`.
The report records 382 seconds without a successful backboardd checkin and
two induced crashes. audiomxd and CommCenter also have no successful checkins;
mediaserverd has one older checkin. logd, thermalmonitord, runningboardd and
configd have recent checkins. Panic flush and PSHOLD reset follow. This is
the original userspace supervision policy acting on unhealthy services.

The log does not provide backboardd's blocked stack or the causes of the
SpringBoard, mobiletimerd and powerd crashes. It does not establish that
display is the sole cause, or that adding one dependency fixes the reboot.

## Original graphics-memory dependency

The delivered prelink closure lacks `com.apple.iokit.IOSurface`. Its original
21H461 personality publishes `IOSurfaceRoot` on `IOResources` with
`IOResourceMatch=IOBSD`, without a board-specific hardware provider.
board.toml now retains this original component. Replaying the dependency
closure from the pinned original metadata adds only IOSurface: 20 components
become 21. The component contributes 189,185 bytes of non-LINKEDIT contents;
coalesced segment geometry and final packaging remain the builder's concern.
Both workflows require the component and its executable module entrypoint.

This supplies the original surface-memory service needed by graphics/media
clients. It is not a framebuffer, GPU, backlight or audio driver. The CPU/GOP
display path still needs its actual provider and ABI integration. The public
display and GopFramebuffer guides, sheng and sirius display integration, and
the actual pin's NativeMobileDisplay revision
`8666a9950c9699b368931d18cac92c0fdb2ce9c0` were inspected. Their newer component
layouts, display registers, hardware-key stand-ins and GPU paths are not
copied into this change.

## Bounded service diagnostics

The existing final XNU diagnostics patch now reports selected service exits
before ordinary corpse handling, which previously printed only the process
name when corpse resources were exhausted. There are four reports per exact
process name, shared across restarts: backboardd, SpringBoard, mediaserverd,
audiomxd, CommCenter, powerd and mobiletimerd. Each report includes exit status,
signal, reason namespace/code and at most 160 sanitized description bytes.

A SIGTRAP exit may use the existing saved breakpoint registers, main header,
cache slide and bounded frame reads only when process lifetime, task and
exiting thread match. Missing or unrelated snapshots are explicitly labeled;
SIGKILL from watchdogd is not treated as a breakpoint. The critical-process
report budget remains separate. The original breakpoint capture hook is
unchanged; this does not capture blocked threads or every exception type.

backboardd and SpringBoard also receive 12 begin/end pairs per operation and
exact name, shared across restarts:

| `PICASSO_IOKIT op` | Observation |
| --- | --- |
| 0 | Single-service lookup and its actual result |
| 1 | Matching iterator creation; `object=1` does not mean it contains a service |
| 2 | Notification registration; `object=1` does not prove matching delivery |
| 3 | Service open, including distinct RPC and UserClient result codes |

The begin line copies only bounded matching keys/class names already in
kernel memory. Non-string matching values are labeled; no dictionary is
serialized, iterator consumed or result changed. The output is capped at
192 IOKit lines plus 196 service-exit lines. No timeout, checkin, critical-exit
policy or Sandbox permission changes accompany these diagnostics.

## Verification and next device checkpoint

Evidence is retained under `out/backboard-watchdog-20261030/`:

- `replay-v2/report.json`: both passes check all 61 patches; 12 generated
  sources are identical across passes. Original mount/carrier wiring remains.
- `fault-v2/results.json`: 64 ordinary and 64 ASan host cases pass, including
  exact-name filtering, independent limits, normal exit, induced SIGKILL,
  snapshot identity, bounded descriptions and the existing copy-error cases.
  The opaque BSD task_t compile negative still fails as intended. The first
  fixture attempt used a global name that shadowed a production local; only
  the fixture name changed to resolve the MSVC warning.
- `trace-v2/results.json`: ordinary and ASan each pass seven scenarios, including
  32 concurrent callers, exact per-name/operation limits, malformed/long text,
  filtered processes, separate RPC/client failures and bounded source reads.
- `source-review.json`: removing marked IOKit diagnostics reproduces the prior
  nonblank source lines exactly; exit control flow outside the helper, the
  ARM64 capture hook, other outer-patch sections and canonical USB patch match
  their prior inputs. Board changes only add IOSurface. Python/TOML/YAML pass.
- `iosurface-closure.json`: original IOBSD personality, dependency closure and
  component extents. This is metadata analysis, not new SDK linking.

The host tests use process/IOKit stand-ins. New Apple SDK compilation and
device execution remain pending. On the next log, inspect IOSurfaceRoot
lookups/opens, display-related matching requests and the selected exit reasons.
Continue implementing the real missing providers from that evidence; a
successful service lookup alone is not a display or successful checkin.
