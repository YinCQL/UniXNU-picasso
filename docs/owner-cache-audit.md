# Owner cache integration audit

The transport issue recorded below was subsequently investigated in the
[UFS NOP investigation](ufs-nop-investigation.md). A later `25e314d` device log
shows OS/SystemOS mounts and owner-cache preparation succeeding before an
automatic reboot during early launchd. See the
[launchd and reboot investigation](launchd-reboot-investigation.md); actual
shared-cache mapping and stable userspace remain unverified. The audit below
describes its original source and validation snapshot.

## Audited inputs

- Local starting commit: `8e57db66d178ecb3a1681dc47154c85281bb4522` on `feat/ufs-stage6`.
- UniXNU pin: `75fba878a97af05ac990fe0310095f40c5681ceb`.
- XNU pin: `d8b80295118ef25ac3a784134bcf95cd8e88109f`.
- NativePrelink pin: `2b6ffb50117eee820b98aca9bd51b5c7dfb2970e` (two prelink patches).

## Confirmed integration defect and correction

The starting commit enabled OWNER_CACHE_CARRIER, but also created a late 333-line
copy of offline-owner-systemos-mount.patch with duplicated IOLog calls. Replaying
all preceding patches on the pinned XNU failed at kern_exec.c:183: that code had
already been changed by the real upstream mount patch. A stray shell-context line
also followed the added patch block. This was not a device-runtime observation.

The correction modifies the existing upstream mount/carrier patches instead of
adding a second implementation. Mount stages, the mount return code, final error,
and carrier authorization are logged with newline-terminated printf calls.
Mount targets, UUID authentication, read-only checks, fatal error handling and
USB/transport code are unchanged. The carrier selector remains enabled.

## What the carrier selector does and does not prove

The pinned baseline passes XNU_OWNER_CACHE_CARRIER to vm_unix.o. The non-CSR
path compares the cache vnode mount to the root mount and to the mount resolved
via /private/preboot/Cryptexes. This is a mount-identity check, not simply a
pathname-prefix check. The owner-carrier helper provides a separately authenticated
non-union SystemOS route, including exact volume devices and the direct parent
directory. Authorization success does not by itself prove the later mappings succeeded.

A first shared_region_check_np reporting an empty shared region can precede a
normal cache mapping attempt. It is not an EPERM observation. libignition output
also does not prove a shared cache was mapped. OWNER_CACHE_STARTUP explicitly
sets dyld flag bit 3 (the upstream no-ignition path), so absence of that banner
is not independently evidence of a regression.

IOFindBSDRoot prints Waiting on before waiting for the OS IOMedia UUID. If the
boot remains there and no root publication/mount evidence follows, it has not
reached the owner cache preparation or carrier decision. APFS module loading is
not an APFS root-mount result. Repeated UCD-clear traces prove neither completion
nor failure of the SCSI commands. No newer boot has been called known-good here.

## Separate unresolved transport issue

The existing kick is still present in HardwareTransferPort::beginPoll. Its kicks
and reenables counters are not initialized before increment; the callback can
also write the doorbell before pollUfsTransfer checks current completion, fatal
status or expiry. These are source-level defects/risks, not proof that the kick
caused the reported device hang (the user reports symptoms before that change).
No transport behavior or USB logging was changed in this audit.

## Validation and next device evidence

The regression script exports pinned Git trees into a new isolated directory,
applies the outer device patch and canonical USB override, then replays all 60
XNU/prelink patches twice using the baseline late-patch reversal rules. Both
passes succeed; the second pass leaves kern_exec.c, vm_unix.c and picasso_usb.c
byte-identical. Exactly one mount implementation and one carrier helper remain.

Run from the device repo (output must not exist):

```sh
python scripts/check-owner-cache-patches.py --upstream ../UniXNU --xnu ../XNU/xnu --prelink-patches <pinned-NativePrelink>/patches --output <new-audit-directory>
```

No macOS SDK compilation or hardware boot was performed. On the next boot,
distinguish: OS/SystemOS publication, actual root mount, NATIVE_OWNER_CACHE begin,
mount returned error=0, done error=0, NATIVE_OWNER_CARRIER accepted=1, and then
the shared-region mapping result. Do not collapse these into a single success claim.

Git history was not rewritten, committed or pushed. At audit time the tracking
ref still pointed to a872b6a, while a live ls-remote listed only main and
feat/verified-root-ci. Confirm the intended remote branch before publishing.
