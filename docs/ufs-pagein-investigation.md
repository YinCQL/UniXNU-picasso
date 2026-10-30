# UFS read timeout after early boot (2026-10-30)

Build78 allocates the requested 768 MiB and reaches early userspace. The first
captured storage failure is a UFS READ timeout, followed by failed APFS reads,
SIGBUS exits and watchdog reboot. This differs from build77's Jetsam exits.

## Image and capture

The user confirms the latest compiled image and automatic reboot. Capture
`picasso-windows-20261030-082823-261.log` is 581,040 bytes. Its read-only snapshot
is preserved as `out/ufs-pagein-20261030/capture.log`, SHA-256
`e893a4c6453fff358971dd434d231a93d67fda2494882636f25fa2b9a2310620`.
The capture program retained its file handle; it was not stopped.

Kernel UUID `4E43D6F4-77D3-3493-B3F1-741B3BAE030A` matches downloaded
[Compose build78](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40978531030),
successful at `f9fc2a108192df7d3a57d050e6f563731de49fe8`.
Independent Android/FAT checks verified the embedded kernel, Data, trust cache,
ADT and loader configuration. The FAT is 60 MiB with 55,278,405 content bytes.

| Artifact | SHA-256 |
| --- | --- |
| kernel.macho | `0d6fce4974cbcc07457d0cc56af248c8bd46251c42620ec682f016810955248f` |
| boot.img | `737d3bea09c5c41e8e03c36180794c109f410b9369b7ecf5b62d895185158239` |

Line 172 reports `PICASSO_MEMORY boot phys_base=c007c000 bytes=30000000
actual=30000000 top=c4540000`. All three union paths report
`error=0 source_match=1`, Cache reaches stage6 and writable Data reaches stage4.
There are no namespace1 Jetsam exits and no `PICASSO_IOKIT` lines.

Line 1606 is the first failed UFS READ:

```text
NATIVE_UFS_READ end result=e00002d6 lba=2d6460 blocks=32 command=128187 polls=51 ocs=f scsi=ffff protocol=0
```

`e00002d6` is `kIOReturnTimeout`; READ uses a 1,000,000 us deadline, unlike
the 200,000 us NOP/Query deadline. Subsequent reads return EINVAL, APFS reports
buffer/hash-record/decompression/pagein errors, and SpringBoard, mobiletimerd
and mediaserverd exit with `exit_signal=10 namespace=2 code=a` (SIGBUS).
The final panic says `no checkins from watchdogd since boot (600 seconds ago)`.
It is not the preceding capture's 90-second loss after two checkins.

RAM Data remains in the panic's sampled allocation stack, but that does not
prove a leak caused this failure. No filesystem, signature or owner check is
bypassed to accommodate unreadable data.

## Transport defects and repair

The old poll samples DB/IS/OCS, then checks time. If execution is delayed
between those operations and the controller completes, the old code can time
out using stale hardware state and permanently quarantine a completed command.
The new deterministic fixture reproduces this with the unchanged production
implementation: a READ finishes during the clock callback at the deadline,
yet the old implementation returns timeout. The capture's 51 polls do **not**
prove this race occurred on the phone; a real hardware timeout remains possible.

After detecting expiry, the poll now performs one final hardware sample,
without a delay or a new deadline. Fatal status still wins; completion still
requires DB clear and non-invalid OCS, followed by the existing response and
transfer-length checks. Busy/invalid OCS still times out and quarantines DMA.
No doorbell re-ring, list restart, cancel, retry or controller reset is added.

The second reproduced defect is `hostRead4k` returning `BadArgument` when the
host is no longer ready. This explains the misleading EINVAL cascade after
the first timeout. Valid requests now return `NotReady`, with readiness and
poison state checked while holding the host lock. Write and sync entry points
also recheck after acquiring the lock, before touching command/data memory.
Invalid arguments retain their original error; quarantined buffers stay intact.

`RootTransportDiagnostics.h` records the first failure even with
`native-ufs-diagnostics=0`. `NATIVE_UFS_FAILURE` includes the command, observed
DB/IS/OCS, deadline samples, and fresh mapped HC/UIC/SMMU status. The sample mask
identifies which register spaces were actually read. Start/last/deadline values
are absolute clock ticks, not microseconds; `last` is the last deadline check.
`submitted=0` marks a rejected preflight, whose command counter can still name
the preceding command. There is only one failure report per boot; verbose mode
may refresh cached registers without emitting another report. Sampling does
not clear faults or touch DMA, and there is no per-success diagnostic dump.

## Verification and next capture

Local evidence is under `out/ufs-pagein-20261030/`:

- `baseline/results.json`: old code fails both deadline-completion and
  quarantined-read checks, as expected.
- `host-v1/results.json`: 12 successful MSVC `/std:c++17 /EHsc /W4 /WX /Od`
  runs, with and without ASan. RootCommands additionally runs with traces on
  and off. Cases include late completion, fatal/completion overlap, invalid
  residual, both timeout forms, unchanged output/DMA after quarantine, state
  changes while acquiring a lock, all eight mapped-register combinations and
  bounded reporting. Formatter tests cover every truncation boundary and the
  maximum-width media report; existing pause/NOP/Query/TUR/queue tests pass.
- `arm64/results.json`: ARM64 freestanding command code compiles with actual
  extracted cache/MMIO/pause primitives; the shared failure helper compiles
  for ARM64/iOS17 with format checking and external API declarations.
- `ci78-packaging.json`: independent checks of the preceding successful image.

The device driver directory is linked into the assembled tree; its local
headers are included by `NativeUfs.cpp` and `RootCommands.h`. The board uses
`build-arm-driver.py NativeUfs`, producing `build/native-ufs/libNativeUfs.a`.
No separate header allowlist or new archive is needed.

These tests do not execute XNU scheduling, real UFS/SMMU hardware or ARM64 code.
No new full SDK build or phone result is available for this repair. The next
Compose image must first pass CI, then demonstrate continued disk reads and
watchdog checkins. If it fails, preserve the first `NATIVE_UFS_FAILURE` block
and subsequent READ result to distinguish busy hardware, fatal faults and
response errors. CPU/GOP display remains unimplemented.
