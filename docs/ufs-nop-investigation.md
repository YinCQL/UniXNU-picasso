# UFS NOP startup investigation (2026-10-29)

## Device evidence

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

## Correction

`HardwareTransferPort::beginPoll()` previously restarted UTRLRSR and rewrote the
doorbell every 16,384 polls, before checking completion, fatal status or expiry.
That could submit a slot again while the controller still owned its DMA buffer,
or race a completion that had just arrived. Its `kicks` and `reenables` counters
were also incremented without initialization.

Polling now only observes the initially submitted slot. The periodic writes and
unused counters are removed. Timeout/fatal outcomes still quarantine the DMA
allocation and reject later commands; this change adds no cancel, reset or retry.

Bring-up enables the existing command trace only while its NOP is executing,
even when `native-ufs-diagnostics=0`. Markers cover clearing the command area,
cache publication, deadline construction, initial doorbell submission, the first
poll's clock/delay calls, and the final result. The final summary uses captured
diagnostics rather than adding status-register reads. Normal block reads remain
quiet. Diagnostic output can affect timing and is not itself proof of completion.

On the next build, distinguish the last completed begin/end pair before changing
any further transport behavior. A successful NOP should be followed by the
existing power/LUN/layout/publication stages; owner-cache investigation still
requires an actual root mount first.

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
