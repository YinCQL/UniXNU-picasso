# Build81: startup dependencies and volatile Data headroom

For the newer build82 automatic reboot and its CommCenter launch-policy change,
see [the CommCenter investigation](commcenter-startup-investigation.md).

The user supplied `D:/App/picasso-bootimg-81.zip` after removing its GitHub run,
and identifies `picasso-windows-20261030-123539-981.log` as this image. The screen
shows the Apple logo without reaching a usable interface. The user manually
disconnected the device; the end of the capture is not a reset.

## Artifact and device evidence

The ZIP is retained and extracted under `out/build81-stall-20261030/build81`.
`ci81-packaging.json` independently checks Android/FAT chains, embedded FD/DTB,
kernel, trust, ADT and Data bytes. FAT content is 55,516,210 bytes in 60 MiB.
`sdk81-review.json` checks source blobs against `418820d9c4201434372a7919f9503d71b507d27f`,
SDK objects, both late initializers, archive members and the final 358-slot ABI.
The kernel hash is `68eef1dfe810e502fc483b5f3b37a56ee0a4cb846930ca754641368709a3b0b4`;
its UUID is `70B5BD11-BAC3-353D-8FCC-01196CEB3970`. The capture has no kernel UUID
line, so attribution uses the user's confirmation and new feature traces.

- AppleKeyStore publishes, opens and handshakes. SpringBoard's matching/open
  operations now return zero with an actual object. Selectors 7, 17 and 35
  have successful queries. This fixes the missing-service condition, without
  establishing that all keybag or startup requirements are fulfilled.
- CPU copies and original frame retirement succeed; no new display failure
  or old `failed to find the main CADisplay` appears. Four bounded samples have
  41,011 nonblack pixels. They do not prove that only four frames were submitted.
- mediaserverd twice reports `[1 = mediaserverd startup]` still running.
  backboardd/audiomxd/CommCenter also receive induced watchdog exits.
- The 256 MiB Data volume later sends lowdisk notifications and repeatedly
  returns ENOSPC. APFS requests 6,010 blocks for a DELETE transaction while
  only about 5,200–6,000 blocks remain; concurrent reservations also matter.
  Transaction headroom is insufficient before free space literally reaches
  zero. cfprefsd, mobileassetd, softwareupdateservicesd and maild are affected.
- mobiletimerd exits in namespace 1/code 4 before the ENOSPC burst. Increasing
  logical Data capacity does not add physical RAM. Jetsam remains enabled;
  later memory usage still requires measurement.

## What error -17932 means

The original 21H461 CoreMedia health callback formats
`%d vital tasks are still running: %s` when its vital-task count is nonzero.
At `0x1939af164` it writes `-0x460c` (-17932) to the health status and returns
false. Evidence is in `media-error-hits.json` and `media-health-evidence.json`.
This is the status for unfinished vital work, not a specific audio-driver error.
The visible task name covers the entire startup and does not identify its
blocked substep.

The induced media exits precede the lowdisk/ENOSPC burst. Data headroom is a
confirmed later problem, not proof of the first Apple-logo blocker. Missing
AGX/CLCD lookups alone do not establish a GPU requirement with working CPU
scanout. AKS failures also need caller/context attribution. No fake key results,
system-bag handles or assertions are introduced to hide rejected operations.

## Changes after build81

The next seed is 512 MiB of logical APFS Data. Its packed payload remains capped
at 4 MiB, sparse pages allocate on writes, and the 2 GiB maximum still excludes
the persistent-store path. The actual loader memory request stays 768 MiB and
FAT stays 60 MiB. No device partition is modified. A larger logical disk can
consume more resident RAM as it fills; it is not a complete memory-pressure
fix or a guarantee of indefinite runtime storage.

Service exit metadata is separate from 128-byte description chunks, retaining
up to 960 bytes. Every new description line fits within 255 bytes including its
prefix. Build81 showed that 160 bytes plus the old metadata prefix was still
truncated before the newline. IOKit metadata/matching/detail lines use the same
formatting bound.

IOKit tracing adds exact mediaserverd and audiomxd names to backboardd and
SpringBoard, with twelve operations per name/type across restarts. It never
consumes iterators, changes an RPC result or alters supervision. AKS logs now
identify the caller and only the integer state-query context. SpringBoard,
keybagd, securityd and other callers have independent 24-line budgets; the total
is at most 96 lines. Key-operation payloads are not logged.

## Validation and next capture

Evidence under `out/build81-stall-20261030/`:

- `replay-v1/report.json`: two passes through all 62 patches; only marked
  service-description and IOKit diagnostic blocks change in generated XNU.
- `fault-host-v1/results.json`: 72 ordinary and 72 ASan cases, description
  lengths, malformed items, identity checks and the compile-negative check.
- `trace-host-v2/results.json`: seven ordinary and seven ASan cases, four-name
  concurrent limits, long matching fields and bounded formatted lines.
- `state-host-v3/results.json`: actual state-service/logger host checks, input
  and output bounds, unsupported key operations, separate caller budgets and
  no logging of key-operation payloads.
- `data-host-v1/results.json`: 47 host seed/mount/configuration cases, including
  a 512 MiB logical zero image without a dense allocation.
- `source-review.json`: source, patch boundaries, provenance and build wiring.

These host checks do not format a new APFS filesystem or compile the new kernel
with the Apple SDK. Build81 passed SDK/link verification for the preceding
commit. The new source still requires a Compose SDK build and a device capture.
Check the new Data size, mediaserverd/audiomxd IOKit records, SpringBoard's AKS
caller/context records, and complete service descriptions. An unmatched begin
locates an in-flight RPC; a completed iterator does not prove it contains a
matching device.
