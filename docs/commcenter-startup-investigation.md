# Build82: CommCenter startup and offline launch policy

For the newer build84 manual-interruption capture and microtime conversion
repair, see [the build84 investigation](build84-startup-investigation.md).

The latest capture is `picasso-windows-20261030-140559-341.log`. The user
reports an automatic device reboot. The preserved snapshot is
`out/build82-reboot-20261030/capture.log`, 161,695 bytes, SHA256
`d13c713b4bd5df92ec3b5acc7a177677009ad6291d1d388406fe0191dd93c431`.

GitHub Compose run 41011057374 completed for
`9fd9a7a1b65fe1fb7f6705135902e6745dbbd14b`; its downloaded build82 kernel
SHA256 is `f9228f37721263d161cecfc2dc868a742362745d171c5c94f57e32c5428f27a6`.
Kernel UUID `F6A4722A-9AF9-3B4C-8C39-10C903FE1897` matches the panic log.
The earlier build81 log ended with a manual disconnect; this capture contains
the complete panic, panic flush, Fastboot reason and PSHOLD reset chain.

## What the device establishes

The panic is `userspace watchdog timeout: no successful checkins from
CommCenter (2 induced crashes)`. Its reported context is an unresponsive
`com.apple.main-thread`; there are no CommCenter checkins in 417 seconds.
At that point backboardd has seven successful checkins, mediaserverd nine and
audiomxd two, each with its last checkin zero seconds ago. They failed some
earlier startup checks, then recovered. wifid's last checkin is 120 seconds
old and remains a possible later dependency to investigate.

The 512 MiB logical Data seed is selected and this shorter capture has no
ENOSPC/lowdisk burst. This does not establish indefinite Data or RAM capacity.
CPU frame copies and original queue retirement still succeed, with nonblack
Apple-logo samples. SpringBoard opens AppleKeyStore and makes successful
state queries with context zero. securityd's rejected keybag/key operations
remain explicit failures. The state service's key contracts are unchanged.

The panic stack belongs to watchdogd. It does not identify CommCenter's
blocked call, and the missing camera/audio matching records do not establish
that those drivers caused the final reboot. The zone report's suspected
allocation leak is also not a demonstrated cause of this panic.

## Tutorial and reference comparison

[Skip Setup](https://unixnu.com/zh/guide/skip-setup/#任务) documents selecting
unavailable jobs with a board-specific launch policy and names CommCenter.
Local tutorial source is `docs/zh/guide/skip-setup.md` in UniXNU at
`066c8d8315f83749d28bcae2faf3595718646be5`.

sheng (`08cde20edc3254ed6f7df8d0f5384fb887786e8f`) and sirius
(`10f440b6b2370aed7cb0e8301b39b9817bc371ad`) implement launchd overrides in
`package/BootWatchdogPolicy.c`, with separate watchdog-disable and job-list
options. Their broad lite/Setup policies are not selected here.

The [same-device reference](https://github.com/minhtritt1996/ios-picasso), locally
at `41bb6db2d6e3c5af585b0cb2c7332d6f64ef86f8`, records removing CommCenter to
avoid CoreTelephony waits; see `NOTES.md` and `tools/sweet-root-tweaks.py`.
That work uses iOS 16.7.16 / 20H392, HFS+ and additional supervision/Setup
changes. Its whole root-edit script is unsuitable for this authenticated
21H461/APFS configuration.

The original 21H461 `System/Library/xpc/launchd.plist` contains cellular and
noncellular CommCenter jobs with the same `com.apple.CommCenter` label and
the same executable. Thus the first extracted cellular plist was only one
variant; lack of a baseband node does not itself prevent the noncellular job.
Neither variant has a force-enable override. No captured stack proves a
specific baseband wait.

## Delivered source change

`board.toml` explicitly selects only `com.apple.CommCenter` in
`payload.offline_disabled_jobs`. The fresh packed volatile APFS Data seed adds
root-owned 0644 `db/com.apple.xpc.launchd/disabled.plist` and an empty
`disabled.migrated`, selecting the original modern override loader. Both
CommCenter variants resolve through their shared label. No native telephony
backend is selected for this port; its XPC services should be unavailable
instead of repeatedly starting an unqualified hardware stack.

The preparer refuses other labels, persistent/nonpacked Data, symlink or
unprotected ancestors, existing output objects and content/ownership errors.
It verifies original copied objects again after installation. OS/SystemOS
files, watchdogd, `_PanicOnCrash`, keys, activation and Setup preferences are
unchanged. The artifact guard remounts the actual raw seed read-only and
verifies both policy files against the board and seed report.

Bounded IOKit tracking adds the exact CommCenter name to the existing four
names. It keeps twelve reports per name/operation, at most 960 lines, and
does not alter matching, client results or the original supervision policy.
The provisional exiting-thread stack sampler was retained in ignored evidence
and removed from the delivered patch after the narrower launch-policy repair
became concrete.

## Verification and next device checkpoint

Evidence is retained under `out/build82-reboot-20261030`:

- `device-evidence.json` binds the downloaded kernel UUID to the capture.
- `original-launch-jobs.json` records both original jobs and watchdogd.
- `launchd-gate.json` runs the original 21H461 ARM64 override loader and job
  import gate under Unicorn. Root-owned policy skips CommCenter, rejects a
  non-root policy owner, and keeps watchdogd/mediaserverd enabled. Filesystem
  and libxpc APIs are stand-ins; this is not a device supervision result.
- `replay-v1/report.json` verifies the full ordered 62-patch chain twice.
- `trace-host-v1/results.json` passes seven ordinary and seven ASan cases,
  including concurrent per-name budgets and exact CommCenter filtering.
- `data-host-v2/results.json` passes 70 seed copy/format/mount cases and the
  new policy's scope, content, metadata, collisions and failure paths. Windows
  ownership is mocked; no new APFS formatting or SDK build ran locally.

The next Compose build must pass the actual APFS policy readback. On-device
acceptance requires CommCenter to stay unloaded, watchdogd and other services
to keep checking in, and the UI to advance. Whether the watchdog stops requiring
the excluded job and whether another UI dependency remains are device checks.
The Apple-logo stall is not yet declared solved. No CI dispatch or device
operation was performed for this source change.

## Build83 host fixture failure

Compose run [41015328143](https://github.com/YinCQL/UniXNU-picasso/actions/runs/41015328143)
failed at the packed Data host check for `2812f15`, before APFS preparation or
kernel compilation. The new policy fixture directories belonged to the ordinary
macOS runner, but the host check simulated root ownership only on Windows.
The production directory guard correctly rejected those non-root directories.

The host fixture now simulates policy ownership on every host. Only its ancestors
start as root; newly created files gain simulated root ownership through the
installer's chown calls. Skipping those calls still fails readback. POSIX hosts
retain real modes and the existing copy tests retain native ownership calls.
Production preparation, raw APFS readback, workflow privilege boundaries and the
CommCenter-only policy are unchanged.

Evidence is retained under `out/ci83-policy-fixtures-20261030`: 85 Windows host
cases pass with the retained DarwinBoot packer. The non-root POSIX fixture model
reproduces the original error at UID 501/GID 20 and passes all 37 policy cases
after the fix without invoking native root chown. Each of the three policy
ancestors is checked independently for type, owner, group and writable modes.
This model uses Windows file operations and simulated POSIX ownership/modes;
it does not establish native macOS or APFS acceptance. A new Compose build and
device verification remain pending; the Apple-logo stall is still unresolved.
