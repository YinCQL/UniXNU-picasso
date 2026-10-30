# UFS root bring-up investigation (2026-10-29)

Build78 now reaches early userspace with 768 MiB, then fails a UFS READ.
See the [2026-10-30 read-timeout investigation](ufs-pagein-investigation.md)
for the current capture, deadline fix and validation boundary.

The later `25e314d` device boot completed delayed UFS reads, mounted OS and
SystemOS, and reached early launchd before automatically rebooting. See the
[launchd and reboot investigation](launchd-reboot-investigation.md) for that
log's identity, the passed stages and the remaining uncertainty. The earlier
captures below retain their original evidence boundaries.

## Initial device evidence: NOP not yet observed

The user identified `picasso-windows-20261029-045550-937.log` as a capture from
Compose run [40760493056](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40760493056),
whose successful build has head `9075c4cafc258cbdf1304f444db2b600d54382c4`.
The capture is 13,818 bytes, SHA-256
`432a474f97fb83c935fc8499208c1a8bfadb96080c4c5e3eb20f6b0de784f210`.
This run association comes from the user; the boot image itself was not rehashed.

The log starts midway through a USB event line. Its last UFS stages are:

```text
NATIVE_UFS_BRINGUP dma ready phys=c47e9000 bytes=22000
NATIVE_UFS_BRINGUP queue stopped
NATIVE_UFS_BRINGUP smmu ready slot=50 bank=60
NATIVE_UFS_BRINGUP queue started
```

XNU then waits for OS UUID `04769D26-2CA0-4825-B443-E2E5F88300F2`; USB poller
heartbeats continue. There is no `nop ok`, `root bring-up failed: NOP OUT`,
window publication or owner-cache preparation in this capture. The earlier
`ufshc / root_provider=0 / rejected` message is the expected observational
personality rejection; it does not show rejection of the root personality.

`hostBringUp()` calls `hostNop()` immediately after `queue started`. Its normal
command deadline is 200,000 microseconds. The missing success/failure messages
identify the next boundary to instrument, but do not identify which operation
blocked or prove that the polling kick caused this device observation.

## Follow-up device evidence: NOP succeeds, power query is next

The user identified `picasso-windows-20261029-054016-015.log` as a capture from
Compose run [40772022992](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40772022992),
whose successful build has head `47ddbbab79b869bd5200dd071bc78209f34585f2`.
This capture is 12,732 bytes / 220 lines, SHA-256
`7e4b324561329f230e8806f34abb30c73d60ca5f1cdae75974d79300c25fa192`.
The image association comes from the user; its boot image was not rehashed.
The user also confirmed that the disconnect at 05:42:53 was a manual unplug.

The captured NOP now completes and passes response validation:

```text
NATIVE_UFS_SUBMIT poll=end result=0 polls=1 ocs=0 db=0 is=0
NATIVE_UFS_BRINGUP nop end result=00000000 command=1 polls=1 ocs=0 db_seen=0 is_seen=0 protocol=0 response=20000001
NATIVE_UFS_BRINGUP nop ok
```

The OS UUID wait remains. There is no subsequent `power=`, `lun ready`, root
window publication or root mount, and no bring-up failure message. Immediately
after NOP, `hostBringUp()` calls `hostQueryRead(0x03, 0x02, &power)` to read
`bCurrentPowerMode`. That query is the next unobserved completion boundary;
silence alone does not identify an instruction that blocked.

This NOP completed on its first poll, before calling the poll clock or delay.
It therefore does not validate the waiting path needed by a slower command.
In the pinned XNU, `IODelay(2)` can either enter a scheduler wait or call
`machine_delay_until()`, which executes WFE before checking the clock. This is
a reason to capture the query's first clock/delay boundaries, not proof that
either path caused the device observation. The 200 ms command deadline only
gets checked when the polling code is running.

This log still starts midway through a line. EP3 event and heartbeat traces
are disabled in this build, so their absence cannot establish a USB failure.

## Latest device evidence: power query succeeds, LUN readiness is next

The user identified `picasso-windows-20261029-061253-386.log` as a capture from
successful Compose run [40776894540](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40776894540),
head `c9c8ffaa1b1a5738c1feccf96f053b954c138123`. The capture is 14,132 bytes /
252 lines, SHA-256
`b96b1955a8a6c691119130e913d013349be4cea2761e034a6d1c6adbd6f75788`.
The image/run association is user-confirmed, not an independently checked boot
image hash. This capture still lacks the early boot prefix.

Both NOP and power query complete in one poll. The query passes response
validation (`response=36000001`, `protocol=0`) and returns `power=11`. That
value skips the wake branch, leaving the first TEST UNIT READY (TUR) command
as the next unobserved completion. The log has no `lun ready`, publication or
root mount evidence; the OS UUID wait remains.

Across these captures, commands with detailed tracing complete before their
first delay, and the following quiet command has no captured result. This
supports removing the transfer poll's dependency on scheduler/WFE wakeups;
it does not prove that the first TUR is blocked in `IODelay` or exclude other
transport/timing problems. In particular, the successful first polls have not
exercised the command wait path on the device.

## Configuration regression context

The user reports that the older version, before enabling
`XNU_OWNER_CACHE_MOUNT`, `XNU_OWNER_CACHE_STARTUP` and
`XNU_OWNER_BOOT_MANIFEST`, could continue booting. The user clarified that this
was a comparison with an older version, not the same source with only three
options changed. Commit `fb0b63d` enabled those options and added a NativeUfs
volume query implementation; `e8a5fda` subsequently changed those interfaces.
This history identifies a regression interval, not an isolated option test.

In the replayed pinned source, the cache mount is called by
`load_init_program()` in `kern_exec.c`, after root mounting. Successful cache
preparation then publishes the boot manifest and updates dyld flags. The
manifest function is defined in `IOKitBSDInit.cpp`, but its caller is in cache
preparation; its source file location does not make it an early root-discovery
call. These direct calls do not explain a stop before root media publication.
Build/link layout or timing effects remain possible, as do other changes in
the regression interval. A controlled comparison would need identical source
and inputs, with only a valid owner-cache configuration changed; it has not
yet been performed. No authorization check is disabled by this instrumentation.

## Correction

`HardwareTransferPort::beginPoll()` previously restarted UTRLRSR and rewrote the
doorbell every 16,384 polls, before checking completion, fatal status or expiry.
That could submit a slot again while the controller still owned its DMA buffer,
or race a completion that had just arrived. Its `kicks` and `reenables` counters
were also incremented without initialization.

Polling now only observes the initially submitted slot. The periodic writes and
unused counters are removed. Timeout/fatal outcomes still quarantine the DMA
allocation and reject later commands; this change adds no cancel, reset or retry.

Bring-up enables the existing command trace separately while its NOP and its
single power query are executing, even when `native-ufs-diagnostics=0`.
Markers cover clearing the command area, cache publication, deadline
construction, initial doorbell submission, the first poll's clock/delay calls,
response processing and the final result. Summaries use captured diagnostics;
the temporary trace flag does not enable the global diagnostic register dump.
Detailed command traces remain disabled for ordinary SCSI and block reads.
Bounded bring-up and read summaries described below make the later stages
observable. Diagnostic output can affect timing and is not itself proof of
completion.

The query summary distinguishes a failure before command submission from an
actual query result, so it cannot present the preceding NOP's completion as the
query's. Two old trace-only register reads after interrupt acknowledgement and
doorbell submission are removed; values still come from the existing preflight
and first poll. This also changes NOP trace timing and MMIO counts, although
the ordered writes and completion checks remain in place.

The transfer poll now calls `ufsPollPause()` instead of `IODelay(2)`. The shared
helper executes exactly 64 CPU hints, using the ARM `yield` instruction in the
driver. It never calls WFE, a scheduler wait or a clock itself. This bounds each
pacing batch without requiring a wakeup; it is not a calibrated 2 microsecond
delay. The existing outer monotonic deadline, completion/fatal priority,
single submission and DMA quarantine remain unchanged. A frozen command clock
can still prevent the outer timeout; this change does not claim to repair it.

Interrupts and preemption stay enabled as before. A slow or stuck command can
consume more CPU while it polls, including the longer read/write/sync deadlines
which share this transport. Queue-takeover and other existing waits are outside
this narrowly scoped change.

The next image includes these bounded checkpoints together, so later failures
do not require adding a new log for each command:

- Host bring-up entry/result, NOP, power query, and TUR entry plus at most 20
  attempt results. TUR keeps its detailed trace off during submission/polling.
- The first eight block-read start/result pairs (LBA, block count and result),
  then only command failures. This does not print every APFS read.
- GPT header, entries and root manifest reads, parsed layout, and userdata
  range. A layout failure also reports the parser's reason.
- Per-window preparation, policy/signature/hash/protection boundaries,
  identity validation, publisher scheduling, start and volume-ready waits.
  SystemOS must still become ready before OS is published.
- Existing `NATIVE_OWNER_TRUST`, `NATIVE_OWNER_CACHE` stage/result and
  `NATIVE_OWNER_CARRIER` logs remain available after root discovery/mounting.

Compare the last begin/end pair and return code. A published IOMedia or matching
UUID alone is not proof of root mounting or shared-cache success. This change
does not disable any owner-cache option or authorization check.

## Host validation

The unmodified production `RootCommands` fixture reproduced the fault:
`Assertion failed: released`, at the repeated doorbell write. The corrected
fixture additionally changes hardware completion, fatal status and expiry
immediately before poll 16,384. It asserts exactly one doorbell write, one DMA
publication, no request-list restart, and continued quarantine on failure.
Both DB=0 with invalid OCS and a doorbell that remains busy are covered.

`TransferPoll.cpp` also needed its missing `beginPoll` fixture method to match
the production port interface. Existing immediate/delayed/fatal completion
cases are retained.

Both fixtures compile and pass with MSVC 19.44.35228 using
`/std:c++17 /EHsc /W4 /WX /Od`, and pass again with AddressSanitizer enabled
(`/fsanitize=address /Zi /link /INCREMENTAL:NO`). Test outputs and the compiler
details are kept locally under `out/ufs-poll-regression-20261029/`.
UBSan was not run. These are host checks of production command logic, not an
Apple SDK build or device verification of the repair.

The `c9c8ffa` fixtures explicitly completed NOP first, then held the power
query busy for three calls to the production `IODelay(2)` adapter before
completion. They also cover query preflight busy/fatal rejection, delayed
fatal status, invalid response echo, DB=0 with invalid OCS, and a persistently
busy doorbell. Assertions check one submission per command, DMA ownership,
unchanged output on failure and quarantine of subsequent commands.

These fixtures pass MSVC `/std:c++17 /EHsc /W4 /WX /Od` with traces disabled
and enabled, and both variants also pass AddressSanitizer. Enabling traces
evaluates and formats their arguments without depending on exact log text.
Results and source hashes are in `out/ufs-power-query-20261029/results.json`.
The fixture supplies the clock/delay primitives; it does not exercise XNU's
scheduler or WFE, or prove that this device's query can complete.

For the bounded-pause change, the production fixtures use a separate CPU-hint
callback and reject legacy delay calls during command polling. They complete
NOP and Query immediately, then require five pause batches for TUR, also
covering fatal status, simultaneous completion/fatal, both timeout forms and
subsequent quarantine. The simulated time per hint batch is arbitrary; it is
not a device delay calibration. A direct `HardwareTransferPort::delay()` check
confirms no clock, legacy-delay, MMIO or DMA callbacks, and the standalone
helper compiles without declaring any clock or scheduler API.

Ten host runs pass: PollPause, TransferPoll and RootPublication in normal/ASan
variants, plus RootCommands with traces off/on in normal/ASan variants.
The production RootLayout header also passes trace-off/on MSVC syntax checks;
those checks only declare transport and crypto types and do not link or execute
crypto. Updated Python runners parse and extract the production CPU hint.
Results and tested source hashes are in
`out/ufs-poll-pause-20261029/results.json`.
No ARM64 compilation/disassembly, QEMU execution, UBSan, complete OpenSSL layout
test or SDK/device validation of this change has been performed.
