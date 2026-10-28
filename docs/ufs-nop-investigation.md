# UFS root bring-up investigation (2026-10-29)

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
Normal SCSI commands and block reads remain quiet. Diagnostic output can affect
timing and is not itself proof of completion.

The query summary distinguishes a failure before command submission from an
actual query result, so it cannot present the preceding NOP's completion as the
query's. Two old trace-only register reads after interrupt acknowledgement and
doorbell submission are removed; values still come from the existing preflight
and first poll. This also changes NOP trace timing and MMIO counts, although
the ordered writes and completion checks remain in place.

On the next build, follow `NATIVE_UFS_BRINGUP power_query begin` through the
query and submit markers to `power_query end`. Distinguish the last completed
begin/end pair before changing transport behavior. Query completion should be
followed by the existing power/LUN/layout/publication stages; owner-cache
investigation still requires an actual root mount first. This instrumentation
does not change the query protocol, deadline, polling delay or quarantine policy.

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

The follow-up fixtures explicitly complete NOP first, then hold the power
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
