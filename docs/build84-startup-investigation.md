# Build84: microtime rollover and remaining startup dependencies

The user identifies `picasso-windows-20261030-145837-613.log` as the latest
image and reports a long Apple-logo stall followed by manual interruption.
The read-only, shared-access snapshot is
`out/build84-stall-20261030/capture.log`, 163,828 bytes, SHA256
`0334c922ccc8c2208d3c71cf7131021c7f5336a9db3b99ba4cccbe327ed73d39`.

[Compose run 41016174147](https://github.com/YinCQL/UniXNU-picasso/actions/runs/41016174147)
completed for `ec893674d52d7ce126d09e1182acd994a5567d57`. Its
`picasso-bootimg-84` artifact is retained under the evidence directory.
The kernel SHA256 is
`7907cb024cb2e9013299bdb6028492082cd6b987fa269936c3d46d2b00202c4d`.
This capture contains no kernel UUID, so device attribution uses the user's
confirmation and feature traces, rather than a captured UUID match.

## What this capture establishes

- No captured panic, PSHOLD reset or automatic reboot chain. The final line
  is a rejected mediaremoted state query, not an identified crash.
- No CommCenter process trace or induced exit appears. The actual build's
  read-only APFS policy check reports only `com.apple.CommCenter` disabled,
  root-owned policy files, and watchdogd enabled. Absence from this bounded
  capture alone is not a complete process inventory or watchdog checkin audit.
- Data is 512 MiB logically, and this capture contains no ENOSPC or lowdisk
  burst. Launchd completes early boot at about 153 seconds of uptime.
- At 299 seconds without a successful checkin, watchdogd induces exits of
  backboardd, audiomxd and mediaserverd. mediaserverd reports its startup vital
  task unfinished, with generic status -17932. A later audiomxd exit reports
  359 seconds without a checkin. There is no final panic listing each service's
  later checkin count, unlike build82; recovery must not be inferred here.
- CPU/GOP copies, console handoff and original queue retirement succeed.
  Four bounded pixel samples still contain the Apple-logo content. They do
  not imply that only four frames were submitted. SpringBoard's state queries
  with context zero succeed; unsupported key operations remain failures.
- Every sampled IOKit begin has an end, but mediaserverd's twelve single-match
  samples are exhausted by repeated camera-class lookups. These records do
  not show its later dependencies or the blocked startup stack.
- APFS repeatedly reports negative flush durations (-8981 and -10067 us) and
  very large accumulated statistics. These deserve arithmetic verification
  before changing timer IRQs, hardware frequency or watchdog policy.

## Tutorial and reference comparison

The local UniXNU documentation is at
`066c8d8315f83749d28bcae2faf3595718646be5`, sheng at
`08cde20edc3254ed6f7df8d0f5384fb887786e8f`, sirius at
`10f440b6b2370aed7cb0e8301b39b9817bc371ad`, and the same-device reference at
`41bb6db2d6e3c5af585b0cb2c7332d6f64ef86f8`.

| Area | Reference evidence | Result for this port |
| --- | --- | --- |
| Platform/timebase | [Platform guide](https://unixnu.com/zh/porting/platform/) requires following the timer frequency into its consumers. SM7250's `lito.dtsi` and this ADT specify 19.2 MHz. sheng's `porting.toml` audio findings independently document a 24 MHz CoreAudio constant needing adaptation to 19.2 MHz. | Inspect each conversion separately. The CoreAudio finding concerns audio IO cadence; it is not evidence that this startup failure is an audio-clock defect. A different defect in XNU microtime is reproduced below. |
| Migration | [Device-tree guide](https://unixnu.com/zh/porting/device-tree/#chosen-属性) describes `ephemeral-storage` as the temporary Data marker. [sirius c54ee19](https://github.com/57UU/UniXNU-sirius/commit/c54ee192c2b0a8a281964674e24a10ca12071f53) adds it after a 36-minute migration wait; sirius later reaches Setup near 358 seconds. | The actual build84 FAT-extracted ADT already has `/chosen/ephemeral-storage` as LE u32 1. Native `kern_mib.c` exposes it through `hw.ephemeral_storage`. Repeating that change or claiming that the marker is missing would be incorrect. The current log lacks a migration-plugin lifecycle trace. |
| Setup policy | [Skip Setup](https://unixnu.com/zh/guide/skip-setup/) combines migration, Setup preferences, job exclusions and watchdog exceptions. The same-device `tools/sweet-root-tweaks.py` also changes VirtualAudio and additional services on iOS 16.7.16 / 20H392 / HFS+. | These are separate mechanisms. Existing volatile Data and CommCenter-only policy stay selected; no Setup/activation completion marker, watchdog exception or broad job list is copied. |
| CPU display | [Display guide](https://unixnu.com/zh/porting/display/) requires real copy and swap completion before GPU presentation. sirius's `porting.toml` records the Setup welcome screen without a GPU, despite missing touch support. | Missing AGX, AppleCLCD or a default framebuffer surface cannot by itself establish the blocker with this working CPU path. Physical input and full UI rendering still need qualification. |
| Audio | sheng uses its SM8550 ADSP, GLINK, AudioReach graphs, codecs and a matching HAL plugin. The same-device reference removes VirtualAudio to address a different mediaserverd crash. | This SM7250 capture reports unfinished startup, not that reference's plugin crash. No native audio backend exists here, but missing camera/IOAccessoryManager matches are insufficient to identify the blocking substep. No empty-success audio service is introduced. |
| Diagnostics | [Debugging guide](https://unixnu.com/zh/guide/debugging/) separates platform-specific protocols and process evidence. sheng's `BootProcessProbe.c` follows wait chains; sirius adds bounded exec/exit timelines. | Existing sampled IOKit requests return. Preserve later distinct queries before proposing a blocked-thread sampler or copying another board's USB protocol. |

## Reproduced microtime defect

The real build84 `_absolutetime_to_microtime` divides the counter remainder
by `rtclock_usec_divisor`. `timebase_callback` sets that divisor to
`19200000 / 1000000`, which truncates to 19. Apple hardware's integral-MHz
counter makes this shortcut work; a 19.2 MHz counter does not.

For successive ticks around one second:

| Counter tick | Build84 result (us) | Exact result (us) |
| --- | ---: | ---: |
| 19,199,808 | 1,010,516 | 999,990 |
| 19,199,999 | 1,010,526 | 999,999 |
| 19,200,000 | 1,000,000 | 1,000,000 |
| 19,200,192 | 1,000,010 | 1,000,010 |

The last microsecond before the rollover therefore moves backwards by 10,526
us. This is a conversion error, not evidence of a hardware counter rollback.
The negative APFS durations are within that error envelope.

Read-only disassembly follows the real APFS `tx_flush` call at
`0xfffffff00575b994` through stub `0xfffffff005768ae8` and pointer
`0xfffffff005f25af0` to the native `_microuptime` at
`0xfffffff007967b58`. That wrapper calls `clock_get_system_microtime`, which
uses the defective conversion. APFS combines seconds and microseconds, then
subtracts its stored start value. Negative elapsed values can consequently
enter its statistics. The precise history producing every huge accumulated
value has not been reconstructed.

Unicorn executes the actual downloaded build84 ARM64 conversion, with explicit
timer-divisor inputs: 19.2 MHz reproduces the -10526 us rollover; 24 MHz gives
+1 us. The changed conversion is also cross-compiled as an isolated ARM64
function with a 19.2 MHz constant and executed: it returns the exact values in
the right column. This isolated check is not a full XNU/iPhoneOS SDK build.

The repair scales the subsecond remainder by `USEC_PER_SEC` and divides by
`rtclock_sec_divisor`. The remainder is less than the u32 second divisor,
so its u64 product cannot overflow. Seconds remain u64 and microseconds u32,
matching the original imported ABI. Raw `mach_absolute_time`, clock frequency,
timer INTID, deadline programming, calendar adjustment and watchdog policy
are unchanged. The existing RTClockData field/layout remains intact.

This proves and repairs a real timing defect, but does not establish that it
is the sole cause of the Apple-logo stall. The service watchdog's underlying
raw absolute-time deadline is not changed by this conversion repair.

## Later IOKit evidence

The first twelve existing reports per exact process name/operation are retained.
After they are exhausted, twelve more distinct bounded combinations of provider,
name, resource, client/detail and client type can be reported. Repeated equal
labels consume one supplemental slot. The signature table copies only the same
64-byte sanitized labels already logged; it retains no caller pointers.

One atomic try-lock protects each table and is released before logging. A busy
table loses a diagnostic sample instead of waiting. There is no allocation,
iterator consumption, RPC-result change or process supervision change. The
maximum is 5 names x 4 operations x 24 samples x 4 lines = 1920 lines for the
whole boot, shared across restarts. Supplemental samples start at 13.

## Verification and next device checkpoint

Evidence under `out/build84-stall-20261030` includes:

- `clock-audit.json`: original and actual build84 ABI/disassembly, APFS strings
  and callsites. `microtime-arm64.json`: actual build84 conversion replay,
  isolated changed ARM64 compilation/execution and APFS pointer binding.
- `microtime-host-v1/results.json`: 2,291 arbitrary-precision oracle vectors
  at eight frequencies, including sub-MHz and UINT32_MAX, UINT64_MAX tick
  inputs, output canaries, and all 19,200,001 ticks through a full second
  rollover. Ordinary and ASan checks pass using the actual extracted functions.
  `microtime-host-clang-v1/results.json` also passes the portable Clang compile
  path used by Compose, on this Windows host; it is not a native macOS run.
- `trace-host-v2/results.json`: nine ordinary and nine ASan cases, including
  exact-name filtering, long/bounded labels, late camera retry deduplication,
  copied signatures and concurrent maximum-budget enforcement.
- `replay-v2/report.json`: two complete ordered passes through 63 patches,
  retaining identical generated sources on the second pass. Comparing all
  5,181 generated XNU files finds changes only in rtclock.c and the marked
  IOKit diagnostic helper against the preceding replay.
- `source-review.json`: final scope, artifact attribution, retained guards,
  host/replayed-source equality, and pinned tutorial/reference source hashes.

Compose now checks the conversion against its actual patched XNU tree after
the kernel/payload build. At the close of the build84 investigation, this change
had no new Compose SDK build or device run; the later build85 result is below.
No CI dispatch or phone operation was performed during the investigation.

On the next image, require normalized microtime and absence of repeated
rollover-related negative APFS durations. Record whether the UI advances and
whether backboardd/media/audiomxd continue checking in. If the logo remains,
inspect supplemental `PICASSO_IOKIT` samples 13-24 and obtain the actual blocked
service stack or migration lifecycle; a generic startup health failure still
does not identify its substep. Preserve watchdog, Jetsam, APFS integrity, owner
and AMFI checks, and explicit failures for unsupported key operations.

## Build85: correct the Compose check's source directory

[Compose run 41082334132](https://github.com/YinCQL/UniXNU-picasso/actions/runs/41082334132),
build85 at `94df96fa084bc46235fe4f9a2f4ffe535cda33f5`, passes kernel/payload
compilation and the Data artifact guard, then fails in the new
microtime host check with `FileNotFoundError`. The check mistakenly used
`$UNIXNU_TREE/.cache/xnu-work`. That variable points to the preparation cache;
the kernel builder runs in `ctx.tree`, which is `ctx.work / 'tree'`.
The actual patched source is `out/picasso/tree/.cache/xnu-work`.

Compose now passes that build directory explicitly, consistent with the existing
Data, AES and display artifact checks. The arithmetic checks, kernel patch and
result upload remain unchanged. The earlier isolated Clang run verified the
conversion, but did not verify the workflow's two-directory layout; the earlier
scope audit checked the build script's relative path without following `ctx.tree`.

`out/ci-microtime-path-v1` retains the failed CI log, the preceding Memory and
`layout-check.json`. The layout check creates separate preparation and build
directories, places the actual replayed rtclock.c only in the build directory,
extracts the revised workflow arguments and runs the real checker with installed
Clang. Ordinary and ASan runs pass, and the result is copied to the workflow's
logs path. This is a Windows host/layout check; the corrected workflow still
requires a new Compose run for native macOS validation. No CI dispatch or
device operation was performed for this correction.
