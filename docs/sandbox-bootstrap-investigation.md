# Original Sandbox dependency during launchd startup

## Device evidence (2026-10-29)

The user confirms that `picasso-windows-20261029-213238-171.log` comes from
[Compose 40879608398](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40879608398),
head `a6fe178246c90b9096fa6e217a235d4b93a5bc63`, and that the phone rebooted
automatically. That exact build completed successfully. The capture has
129,792 bytes / 1,760 lines, SHA-256
`22e8b8479418443225987d4348d89ebc55881eae820e926c372ec142279468fe`.
It still starts mid-line; the user's actual boot image was not independently
hashed.

The previous timer and cache boundaries now pass on the device:

- Timer IRQ counts increase from 0 to 1, 3, 5 and 8; the first USB yield returns.
- All 53 cache blobs reach native4k `ready=1 errno=0`. Requested, authenticated
  and derived byte totals agree at 3,294,543,872 bytes.
- All 53 owner-carrier decisions accept the expected OS/SystemOS pair.
- Map setup returns zero; VM mapping of 54 file records / 62 mappings returns
  `kr=0`, and the shared-region syscall returns zero.

The later failure and reset are explicit:

```text
PICASSO_PID1 abort begin ... namespace=7 code=1 ...
PICASSO_PID1 critical_exit begin ... rv=6 namespace=7 code=1 ...
initproc failed to start -- exit reason namespace 7 subcode 0x1 description:
_container_init(): sandbox_get_container_expected() failed: %d
=== PICASSO CDC PANIC FLUSH COMPLETE: FASTBOOT REASON ARMED ===
PICASSO CDC: PSHOLD reset begin ...
```

This establishes a PID 1 initialization failure followed by panic/reset. The
literal `%d` does not disclose the failing syscall's errno. Missing core-file
directories produce secondary coredump errors after PID 1 has already exited.
The USB session connected at 21:33:25.514 and detected disconnection at
21:34:59.592 (Asia/Shanghai); reset attribution comes from the kernel messages.

## Missing original policy

The downloaded artifact `picasso-bootimg-62` (artifact ID 12900217136) contains
`kernel/kernel.json` with 18 components, excluding both
`com.apple.security.sandbox` and `com.apple.kext.AppleMatch`. Its
`candidate_sha256` matches the downloaded `kernel.macho`:
`2caa19795ab737ad34c9ca706b21099b244d12b70be76f8ff12f2fea5a67ec22`.
This confirms the omission in an actual build, as well as in board.toml.

The fixed XNU's `security/mac_base.c::__mac_syscall` starts with `ENOPOLICY`
and dispatches only to a matching registered policy's `mpo_policy_syscall`.
UniXNU's pinned `check-owner-cache-carrier-checkpoints.py` records the same
container-init fatal description with an observed `Sandbox` request returning
`ENOPOLICY=103`. Its subsequent `check-original-launchd-main-checkpoints.py`
includes the original Sandbox and AppleMatch, preserves original executable
bytes, observes the request succeed and reaches original launchd LC_MAIN.
Those are upstream reference checks, not new picasso measurements of errno.

## Correction

The board now requests `com.apple.security.sandbox` as a prelink root. The
original metadata pulls in AppleMatch automatically. Original security policy
code supplies the container query; no syscall success stub or policy bypass is
added. The original trust inputs, AMFI initializer and owner authentication
remain unchanged.

Sandbox declares `AppleSecurityExtension=true`. The existing XNU bootstrap
loads such Apple extensions before `mac_late`, and OSKext starts dependencies,
constructors and the normal kmod entry. No extra guessed early initializer or
MAC policy capacity change is needed for this dependency.
The actual original Sandbox start path was also traced through its data
function pointer to the original `_mac_policy_register` import.

Compose also checks the actual prelinked kernel before packaging: its hash
must match the prelink report, both components must be selected, and each
reported module entry must lie in its own executable extent. This catches an
omitted dependency even if the source kernel itself compiled successfully.
It does not establish runtime policy registration.

## Validation and next device boundary

The local 21H461 IPSW's j120ap Erase KernelCache was extracted and decompressed.
Its SHA-256 is `f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7`,
matching the original input in the downloaded build report. Using the fixed
NativePrelink `2b6ffb50117eee820b98aca9bd51b5c7dfb2970e` dependency traversal:
the old closure matches all 18 actual components; adding Sandbox produces 20,
adding exactly Sandbox and AppleMatch. Both normal kmod entries are nonzero
and inside their respective original executable extents. After the unchanged
slide they are `0xfffffff0059f3384` and `0xfffffff00576bdfc` respectively.

Both modules fit the existing file-backed coalesced segment ranges. Rebuilding
the metadata with the pinned serializer predicts 12,288 additional bytes
(54,071,296 bytes total, with the same native input); the 55 MiB payload need
not be enlarged. This is a layout calculation, not a newly linked image.
The capacity calculation is retained in
`out/sandbox-closure-20261029/capacity-report.json`.

Workflow YAML and embedded Python parse successfully. The new artifact check
rejects the real old kernel for the expected missing-Sandbox reason. Reports
and retained inputs are in `out/sandbox-closure-20261029/closure-report.json`,
`out/sandbox-bootstrap-20261029/` and `out/sandbox-artifact-40879608398/`.
No files were deleted; the original image and extracted intermediates remain.

This correction changes packaging configuration, not XNU or USB source.
The previous source regressions remain applicable; there is no new SDK build
or device result yet. The next image must pass container initialization and
continue launchd startup. Writable Data, later boot services and display are
still separate unverified stages; they are not enabled to mask this failure.
