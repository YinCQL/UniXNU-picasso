# Launchd and automatic reboot investigation (2026-10-29)

The newer a6fe178 capture confirms timer IRQ delivery, complete cache
authentication and successful shared-region mapping, followed by an explicit
Sandbox/container initialization panic. See
[the Sandbox dependency investigation](sandbox-bootstrap-investigation.md)
for the current failure and packaging correction. The records below describe
the earlier captures and timer/preflight changes.

## Latest device evidence: 5cd9921

The user confirms that `picasso-windows-20261029-194221-283.log` came from
[Compose 40865543559](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40865543559),
head `5cd9921acb33e6713ebf00aa3ea958287d99bf89`, and that the phone rebooted
automatically. GitHub reports that exact build as successful. The log is
29,765 bytes / 468 lines, SHA-256
`3063dff09a485ea6696626309535c1adecf18027a308646d8240df28367147cf`.
The boot image was not independently hashed; the capture still starts mid-line.

UFS reads, OS and SystemOS mounts, manifest queries and owner-cache preparation
complete. `done stage=6 error=0 mount_error=0 cache_authorized=1 root_recheck=1`
precedes execution of `/sbin/launchd`. PID 1 thread 208 successfully opens
`dyld_shared_cache_arm64` and `.01` through `.17`. The last record is open
sample 19 returning `errno=0 fd=18`. There is no map entry, carrier decision,
abort, critical-exit or panic record. Successful open is not proof of successful
file reads, signature registration or mapping.

The four watchdog reports are at monotonic times 85.464, 86.589, 87.041 and
87.408 seconds, with gaps about 1.125, 0.452 and 0.367 seconds. These consume
the four initial log slots; later gaps below one second are silent. Missing
further reports do not establish that petting stopped. The host connected at
19:43:02.730 and reported Win32 22 / disconnect at 19:43:51.700; file last-write
time was 19:43:44. Host timestamps do not identify the reset source.

## Tutorial and reference audit

The audit follows UniXNU's `docs/zh/porting/platform.md`: trace each board
property through the generated ADT and consumer, then verify timer IRQ delivery
on the boot CPU. The security guide requires validating each original 16 KiB
code-signature hash before publishing the derived read-only 4 KiB hash table.
Both requirements apply to this port.

Reference checkouts were read at these local revisions; their device results
are not picasso results:

| Checkout | Revision | Relevant comparison |
| --- | --- | --- |
| UniXNU | `759eb61b6b8d475de5b3d1b39e1de1f5294e8d9a` | Platform/security tutorials and native4k implementation; actual build pin remains `75fba878a97af05ac990fe0310095f40c5681ceb` |
| UniXNU-sheng | `08cde20edc3254ed6f7df8d0f5384fb887786e8f` | Same 21H461 OS family; shared signing implementation belongs to UniXNU |
| UniXNU-sirius | `10f440b6b2370aed7cb0e8301b39b9817bc371ad` | Darwin 23 / 4 KiB; documented lengthy cache verification before mapping |
| UniXNU-hummer | `1322c2adf7ebf4ca0513b49a247352c8f0c5183c` | `build-hummer-kboot-probe.py` records native4k prepare entry and result |
| UniXNU-guacamole | `6269d315dbf43c4f7334177721f5c18e9b633f97` | Explicit phone virtual timer INTID 19, QEMU 27; different SoC/OS |
| UniXNU-OnePlus-Ace5 | `aaa100d19f6fb14e5897a7e496edf7838d4d4e9f` | USB background worker reference; different SoC/Darwin 22 |
| Repo/ios-picasso | `41bb6db2d6e3c5af585b0cb2c7332d6f64ef86f8` | Same SM7250 timer wiring; Darwin 22, 16 KiB/HFSX and different USB transport |

The dyld audit script references Apple dyld commit
`a571176e8e00c47e95b95e3156820ebec0cbd5e6` (dyld-1165.3). Its cache preflight
performs fstat, a 16 KiB pread, `F_ADDFILESIGS_RETURN`, a small executable mmap
and a comparison before shared-region mapping. This narrows useful diagnostic
boundaries, but the reference version has not been matched to the phone's
dyld binary.

UniXNU performance commit `477e530a8689af4a09eacb4eb9bdb878c737d8b6` already
changed native4k reads to 1 MiB and uses `IO_NOCACHE` for files above 32 MiB.
The pinned picasso sources include that change. Its published 28.9-second
read / 11.0-second hash measurements belong to TB128FU, not picasso. There is
no justification to skip signatures or repeat this already-applied optimization.

## Platform correction

Picasso's captured Linux DT identifies `Qualcomm Technologies, Inc. 7250
picasso` / `qcom,lito`. The `/soc/timer` (`arm,armv8-timer`) node's third interrupt tuple is
`<1 3 0xff08>`: virtual timer, GIC PPI 3, absolute INTID **19**. The device's
ADT generator omitted `virtual-timer-intid`, so the local ArmCpu consumer
selected its default **27**. The configured payload path uses that generator
and has no timer override.

The local source is `D:/App/picasso/devicetree/base/soc/timer/interrupts`
(48 bytes, SHA-256
`8b51da2f351280f3b17a3f22b2b1d675c1392eb7fdbde28d9392e95d3da6c816`).
Its four big-endian tuples are `<1 1 0xff08>`, `<1 2 0xff08>`,
`<1 3 0xff08>`, `<1 0 0xff08>`; Android's architectural timer enumeration
identifies index 2 as virtual. This is a device fact, not an INTID copied
from another phone.

`make-picasso-adt.py` now emits `/arm-io/intc/virtual-timer-intid` as four bytes
`13 00 00 00`. ArmCpu already validates this ABI and uses the selected PPI for
both redistributor enables and IRQ dispatch to `rtclock_intr`. A thread-context
startup message reports the selection. This repairs a board-input mismatch;
hardware IRQ delivery and its relationship to the reboot still need a new boot.

The fixed XNU's `thread_yield_internal(0)` also has a scheduling hazard: it
depresses priority to zero without arming a priority-restoration timer. It only
restores priority after the thread is scheduled again. On a busy single CPU,
that is not a bounded pause. Timed restoration itself depends on working timer
IRQs, which is why the ADT correction and USB scheduling correction are paired.

USB now calls `thread_yield_internal(1)` after the existing bounded instruction
spin, keeping the 15-second watchdog and the requirement that a normal poll
complete before petting. The first yield has one begin/end pair. Existing pet
records include `timer_irqs` and `timer_irq_valid`; the optional ArmCpu symbol
is read only after a null check. This is a RAM counter snapshot in thread
context, not MMIO or logging inside an IRQ. Missing or zero IRQ counts never
gate petting. Poll/pet/yield diagnostics have a combined 16-line limit, plus
the pre-existing poller startup line. Bulk EP3 and heartbeat remain opt-in.

## Preflight diagnostics in this correction

PID 1 gets bounded entry/result pairs for pread, fstat, file-backed mmap and
`F_ADDFILESIGS_RETURN`, at most 64 calls per class. The fcntl trace records the
signature blob's offset and length immediately before registration. These
records fill the gap between the already-observed opens and shared-region map.
They use existing syscall arguments and kernel values, not extra user-memory
reads. Return wrappers evaluate the original expression, including cleanup,
once before logging and returning its original error.

Native4k preparation logs at most 64 entry/result pairs. Files larger than
32 MiB can additionally report before the first read, after the first chunk
and at halfway, at most three observations per file and 96 across the boot.
The final record distinguishes requested read bytes (`req`), bytes whose
original 16 KiB hash matched (`auth`), bytes with derived 4 KiB hashes
(`derived`), and successful read-only publication (`ready=1`). Derived bytes
alone do not imply publication. Error, residual, stage and thread/sample
identify failures without bypassing any original checks.

`read_ticks` uses the original read interval; `hash_ticks` covers completed
chunks and excludes an unfinished/failed chunk, with diagnostic/update and
scheduling time included. Both are architectural absolute-time ticks, not
milliseconds or pure CPU execution time.
No cooperative yield is added to the signature loop, and its authenticated
1 MiB / uncached read behavior remains intact. A limit suppresses subsequent
records, so a missing late record is not proof of a stall.

## Host checks for this correction

`scripts/check-picasso-adt.py` generates and decodes minimal, configured and
Data-enabled trees. It verifies property placement, little-endian u32 19,
GIC layout and the 19.2 MHz timebase. Compose runs it before building.
Independent old/new comparisons found only the new property, excluding the
random seed, and the old generator fails the new check. The actual ADT bytes
also passed the production `ArmCpuTimer.h` decoder/mask in MSVC ordinary and
ASan builds, including absent/malformed/endian/unaligned/boundary cases.
Reports: `out/picasso-timer-adt-final-20261029/results.json` and
`out/picasso-timer-independent-20261029/results.json`.

The updated USB production-helper fixture passes 17 groups in MSVC ordinary
and ASan builds. It checks pet-before-yield order, first-yield pairing, panic
after yielding, null/64-bit IRQ counters and finite output budgets, in addition
to the earlier lock/control/clock regressions. Report:
`out/usb-watchdog-finite-yield-20261029/results.json`. These mocks do not
validate Mach-O weak-symbol binding, IRQ timing or actual scheduler behavior.

The full pinned patch chain passes two rounds: 61 patches applied in round 1,
all 61 checked and five late patches reapplied in round 2. All ten monitored
source files are byte-identical across rounds, including the four new preflight
files. Report: `out/timer-preflight-replay-20261029/report.json`. The generated
USB source matches its tested fixture, and the four generated preflight files
match the reviewed diagnostic source. These checks do not replace an Apple SDK
build or a new boot with this correction.

The extracted native4k function passes MSVC `/W4 /WX` host checks for success,
partial tail blocks, allocation/read/short-read/hash/protection failures,
cleanup and publication order. A large file emits at most three progress
records; other PIDs and exhausted tickets stay silent. Sixteen concurrent
threads obtain exactly 64 unique tickets. I/O, VM and digest operations are
deterministic stubs, not real kernel cryptographic or hardware validation.
Removing diagnostics and restoring return wrappers reproduces the four
baseline files' original statements. Report:
`out/launchd-preflight-20261029/selfcheck-results.json`.

## Earlier device evidence and 5cd9921 changes

The following records describe the preceding 25e314d capture and the changes
delivered in 5cd9921; they do not claim results for the current correction.

### Earlier device evidence

The user identifies `picasso-windows-20261029-065140-160.log` with successful
Compose run [40781483234](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40781483234),
head `25e314dc3fb000eb7c8ea5fc43b8720265d4ffe9`. The capture is 26,132 bytes,
411 lines, SHA-256
`3b0e348f143c43855398474e7ef4ad22dea2d35b91a9f806595af0f19b9994fc`.
The image association is user-confirmed; the boot image was not independently
hashed. The log starts mid-line and still lacks the early boot prefix.

This boot passes the earlier root wait. TUR completes after 12 polls, and GPT,
manifest and subsequent 128 KiB reads complete after multiple polls. Both
verified windows pass signature, table hash and protection checks. OS mounts
as the BSD root, SystemOS mounts read-only at `/private/preboot`, and the
owner-cache preparation ends with `stage=6 error=0 cache_authorized=1`.
Both manifest SHA384 queries succeed. These are device results for this boot,
not completion of all storage functionality or long-term stability testing.

The last two lines come from PID 1's `shared_region_check_np`. In the pinned
XNU, an empty shared region returns `KERN_INVALID_ADDRESS`, which this syscall
translates to `ENOMEM`. It does not panic and does not by itself prove memory
exhaustion. Dyld can subsequently open and map the cache. These lines prove
early PID 1 user execution, not that launchd reached `main` or mapped its cache.

There is no carrier decision, map completion, PID 1 exit reason or panic marker
in this capture. The carrier runs only after map setup obtains and checks a
file whose mount differs from the root mount. Its absence is not a rejection.

The user confirms that the phone automatically rebooted a few seconds later.
The host connected at 06:52:51.227 and detected disconnection at 06:53:38.053
(Asia/Shanghai); the raw file's last-write time is 06:53:19. This coarse gap
is not a device reset timestamp or proof of a 15-second watchdog expiration.

### Watchdog servicing correction in 5cd9921

The log confirms the APSS watchdog was armed with a configured 15-second
timeout. Previously the background USB thread serviced it only every 1024
passes. Each pass includes `picasso_usb_busy_wait(100)`, which actually executes
409,600 ARM `yield` instructions. Consequently each pet interval included
419,430,400 hints, USB work and scheduler waits. Neither the function argument
nor its old microsecond name establishes the wall-clock interval.

The background thread now services the watchdog after each completed normal
USB poll. A failed poll try-lock is not counted as completed work, and panic
mode does not receive background pets. Console-driven polling does not pet
the watchdog. Register settling, the 15-second timeout, the scheduling yield
and the existing panic/reset paths retain their behavior.

This removes the large iteration multiplier from the service interval. It
does not prove that the watchdog caused this reboot: a single USB operation,
MMIO access or scheduling stall can still prevent the thread from returning.
Servicing more often also adds watchdog MMIO writes. Device timing remains
to be measured.

The first background poll's begin/end, the first four pets, and at most eight
later gaps of at least one second are logged: at most 14 records in total,
even with `picasso_usb_trace=0`. Service timestamps are observations
from the existing monotonic clock, not prerequisites for petting. Frozen or
incorrect clock readings cannot be used to establish elapsed physical time.
The EP3 event and periodic heartbeat logs remain disabled by default.

### Launchd diagnostics in 5cd9921

Development/debug diagnostics cover PID 1's early file opens, already-copied
cache paths, shared-region map entry and return, map setup, actual VM mapping,
abort reasons and critical exit. Each class has a finite budget. Pair begin/end
records before interpreting the last line as a completed operation.

Path records use the existing kernel name buffer after successful pathname
copying. They do not perform an additional user-memory read, change pathname
resolution or establish that opening the file succeeded. Open results provide
the separate errno/file-descriptor boundary. Map diagnostics retain the
original carrier, signature and mount-identity checks and original errors.

The diagnostics are added as the final device XNU patch and reversed first
before an incremental replay. They do not enable the unrelated boot-progress
feature or change the three owner-cache selectors.

The budgets are 64 open calls (begin/end), 64 cache-path records, 16 calls
for each map/setup/VM-map boundary, 64 setup-stage arrivals plus a limit notice,
eight abort calls and eight critical-exit entries. Records include a thread
identifier. Sample counters are independent between classes; in particular a
path sample number is not an open-call ID. The stage's file index is
`4294967295` before the file loop starts. Reaching a log limit means subsequent
operations may continue silently.

### Host validation of 5cd9921

The watchdog fixture extracts the actual helpers and background thread from
the canonical device patch. MSVC `/W4 /WX` builds and runs all 13 regression
groups in both ordinary and AddressSanitizer variants. It covers immediate
service on each completed pass, lock-busy recovery, panic and pending control
commands, console re-entry, frozen/backward time, and the logging budget.
Results are retained in `out/usb-watchdog-service-tracked-20261029/results.json`.
The fixture simulates hardware, clock and scheduling; it does not measure
device latency.

```sh
python scripts/check-picasso-usb-watchdog.py --output out/usb-watchdog-service-NEW
```

The full fixed-pin chain replays 61 patches successfully, then checks all 61
again and reapplies five late patches. All six monitored generated source
files are byte-identical after the second pass. The generated USB and four
PID 1 source hashes match the independently tested/reviewed source files.
Results are in `out/launchd-watchdog-replay-20261029/report.json`.

```sh
python scripts/check-owner-cache-patches.py --upstream ../UniXNU --xnu ../XNU/xnu --prelink-patches ../ufs-carrier-audit-20261029/prelink-patches --output out/launchd-watchdog-replay-NEW
```

A separate local MSVC helper check exercised the production ticket/path
functions: 24 concurrent threads obtained exactly 64 unique tickets, and the
path filter accepted expected cache names while rejecting unrelated paths,
unsafe characters and other processes/operations. A source comparison of
seven instrumented functions found identical original control flow after
removing diagnostic statements/macros. These checks and independent source
review are not a complete kernel compile. No SDK build, ARM64 execution or
device test of this correction was performed.

### Evidence requested after 5cd9921

The next capture should distinguish a missing open result, an actual cache
path error, a map/setup rejection, an unfinished VM map, PID 1 termination,
and a watchdog service gap. A successful cache mount is only preparation;
actual mapping and sustained userspace progress remain unverified.

Panic capture is best-effort: it requires a live/configured USB controller and
successful try-locks. The absence of panic text does not exclude a panic/reset.
A natural APSS watchdog expiration may produce no software reset marker.
These changes do not write a persistent reset record or phone partitions.
