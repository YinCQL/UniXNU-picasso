# Launchd and automatic reboot investigation (2026-10-29)

## Device evidence

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

## Watchdog servicing correction

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

## Launchd diagnostics

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

## Host validation

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

## Remaining device evidence

The next capture should distinguish a missing open result, an actual cache
path error, a map/setup rejection, an unfinished VM map, PID 1 termination,
and a watchdog service gap. A successful cache mount is only preparation;
actual mapping and sustained userspace progress remain unverified.

Panic capture is best-effort: it requires a live/configured USB controller and
successful try-locks. The absence of panic text does not exclude a panic/reset.
A natural APSS watchdog expiration may produce no software reset marker.
These changes do not write a persistent reset record or phone partitions.
