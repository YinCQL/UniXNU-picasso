# Missing main CADisplay after build79

Build80 has now passed the full SDK display build and shows real CPU copies and
retirement on the phone. The user sees an Apple logo and manually disconnects
the capture. See [the keystore-state investigation](keystore-state-investigation.md)
for this newer evidence and the current repair. The build79 account below is
the earlier checkpoint.

The user supplied `picasso-windows-20261030-091846-628.log` and confirmed that
the latest image rebooted automatically. The shared-read snapshot is preserved
at `out/display-gop-20261030/capture.log` (129,349 bytes, SHA-256
`1d887e6ec72e6c4c12845a3b93fd431f4027b1b8a554c6bb1e27af543eb9516b`).

## Image and first failure

[Compose build79](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40982944048)
succeeded at `d202f7cd42f81945c8e12013287189c1f7665241`.
Artifact `12946460842` was downloaded and independently checked, including
Android header, gzip BootShim/FD, appended DTB, FAT chains and embedded
kernel/Data/trust/ADT/loader bytes. The kernel UUID
`28AB6AB2-28B5-3F4E-AD66-E5F09A2E21BB` matches the panic.
`ci79-packaging.json` records the checks and hashes.

| Item | SHA-256 |
| --- | --- |
| kernel.macho | `bb450d52d24a5777ffba42b84ee0b52b7047621ce0f7a8ae09cff1a4eba8c5cb` |
| boot.img | `cbe7b767b2662560a1cf9bac95fd798f8040c034ab7bb93cf83addfeb00af351` |
| FD | `037e28ec5d69fa24d472d518798c5397a8b950c3585814e1b23538742ac7c946` |

This capture contains no `NATIVE_UFS_FAILURE` or failed UFS READ, no SIGBUS
service exit and no namespace-1 Jetsam exit. The previous UFS failure chain
did not recur during this boot; long-term UFS reliability remains unqualified.
The requested 768 MiB is still actually allocated.

The first selected service exit, line 1624, is SpringBoard SIGTRAP. Its
captured breakpoint message is `failed to find the main CADisplay`, repeated
on relaunch. The `/usr/lib/libobjc-trampolines.dylib` field identifies the first
dyld image, not a newly missing library or the location of this exception.

The final panic is `no successful checkins from mediaserverd (2 induced
crashes)`. In 451 seconds backboardd checked in 17 times and had just checked
in; logd, thermalmonitord, runningboardd and configd also had recent checkins.
mediaserverd never checked in successfully. Its captured unhealthy-task
description is truncated before the task name, so the log does not establish
that display is its sole blocker. This is not build78's 600-second failure to
receive any watchdogd checkin.

## Display repair

The board now includes the original IOMobileGraphicsFamily and its declared
dependency closure (nine added components, 2,217,504 bytes of non-LINKEDIT VM
extents). These include the original media interfaces needed by the display
family; adding them does not implement Qualcomm audio hardware. Previous FAT
content is 55,278,405 bytes in 60 MiB. Final coalescing and actual FAT size remain
subject to the existing CI budget check.

`build-picasso-display.py` uses the actual IOPlatformExpert compile record and
the exact original kernelcache, with the existing `-0x1000000` slide. It checks
the original 358-slot Legacy vtable, nine abstract slots, constructor chain,
dimension getters, completion/refresh callsites and IOSurface getter code.
The object audit checks every inherited/native slot and all original-class
imports. Late registration removes only the initializer section; executable
bytes are compared. The new CI artifact guard verifies the final linked vtable,
aliases, superclass, archive members and current source hashes.

The existing fixed-pin early and post-IOBSD calls initialize the class and
start a real IOPlatformDevice backed by the retained GOP memory, with a
`display-subsystem` node under `/arm-io`. The driver inherits the original
UserClient. Its primary-display status follows the real mapping, including
the current loader's text-console mode. The adapter releases the XNU text
console before a copy; it does not change CDC or watchdog ownership.

The FD was decompressed read-only. Its compiled memory tables contain 46
matching `Display Reserved` records for `0xA0000000 + 0x02300000`. The SimpleFb
source selects 1080x2400, 32-bit BGRX and width-based stride; runtime admission
still checks PE values rather than assuming those dimensions. The range must
not overlap kernel-owned memory, which begins at `0xC007C000` in this capture.
Rotation, offset, unsupported pixel formats and invalid extents are refused.

The CPU adaptation copies completed linear BGRA surfaces or fills an explicitly
blank request. Compressed `&BGA` cannot be interpreted as plain rows on this
port. No GPU presenter or early retirement is linked. Original queue ownership
moves to current only after the copy and store barrier, then the original
completion releases the frame and wakes waiters. UI scale is two pixels per
point; failed startup rolls back only properties added by this adapter.
The 60 Hz timer sends software refresh notifications, not measured panel
vblank. Seven unknown backend leaves retain failure guards.

## Verification and next boot

`scripts/check-picasso-display.py` runs ordinary and ASan MSVC host fixtures
against production helpers: text/graphics handoff, reserved-range boundaries,
overflow, rotation/offset/format refusals, padded/unaligned input, visible-only
copies, no writes on rejected bounds, blank frames, and one/two-item queue
ownership with malformed-tail/current-request refusals. It also compiles the
copy helpers and store barrier for ARM64/iOS17 and rejects wrong original-image
or slide inputs. Evidence is under `out/display-gop-20261030/host-v2` and the
final verification directory recorded in `Memory.md`. The initial host-v1
failure was fixture compatibility with MSVC builtins and a shadowed fixture
member; production source was not changed for those errors.

These are host/helper and static original-image checks. The full new driver
has not yet passed the real SDK build or executed on the phone. No CI dispatch
or device operation was performed in this repair. Existing XNU patches and
pins are unchanged, so their full replay was not repeated.

Next capture should show `PICASSO_DISPLAY` stage 1 metaclass success, stage 2
IOSurface found, a valid mapping, and stage 3 `started=1`. Event 1 observes
registration; events 3/4 are client entry/result; event 6 is swap validation;
event 7 is actual transfer; event 8 releases the console; event 9 retires the
frame; event 10 records copy ticks/bytes; event 11 records software refresh.
Registration alone is not a displayed frame. Check SpringBoard's main-display
exception, actual copies and mediaserverd health separately. Audio, display
power/backlight, input and persistent Data remain unfinished.
