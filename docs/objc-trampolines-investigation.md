# Standalone Objective-C library missing from the root view

Update: [build 77](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40974341838)
at `726e4b6b1620df9e605365126f0db5edc4601998` passes the Apple SDK build,
linked artifact checks and boot-image composition. Its latest device log
`075201-502` matches the downloaded kernel UUID: all three union paths pass,
and the missing-library SIGABRTs are absent. The new failure is
[VM-page shortage followed by watchdogd timeout](memory-pressure-investigation.md).
The build76 evidence below describes the original failure.

Build 76 / [40970250344](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40970250344)
at `342b707f24e7178eec0a58c7e35e979ff700bd9a` completed successfully. Its
downloaded artifact is `12939683361` / `picasso-bootimg-76`. The kernel UUID
`50C46030-7091-3604-AF5B-000DD580429B` matches
`picasso-windows-20261030-071203-916.log`; the user confirms an automatic reboot.

The new exit diagnostics report four SIGABRTs, from SpringBoard twice,
mobiletimerd and powerd, with reason namespace 8/code 1:

```text
couldn't dlopen libobjc-trampolines.dylib:
dlopen(/usr/lib/libobjc-trampolines.dylib, 0x0106): ... no such file, not in
```

The description is capped at 160 bytes. Backboardd, audiomxd and CommCenter
are killed by watchdogd after failed ping delivery; mediaserverd reports an
unhealthy vital task. The eventual panic reports 376 seconds without a
successful backboardd checkin. No `PICASSO_IOKIT` lines or blocked backboardd
stack appear. The missing library explains the named SIGABRTs; it does not
yet establish every cause of the service stalls or reboot.

AES checks and original watchdog client opening still pass. The downloaded
kernel contains IOSurface and its executable module entry; this log does not
show a successful IOSurface lookup/open by the traced processes.

## Compose the original volumes at their expected paths

Read-only inspection of the original 21H461 images finds the 68,080-byte
`usr/lib/libobjc-trampolines.dylib` in SystemOS, absent at that path in OS.
SystemOS was mounted at `/private/preboot`; `DYLD_SHARED_CACHE_DIR` exposed
the cache but did not expose standalone `/usr/lib` files. The OS's existing
`System/Cryptexes/OS` link points to `../../private/preboot/Cryptexes/OS`, not
the prior carrier's root.

The fixed upstream already provides authenticated native union composition.
Its `check-owner-cryptex-fallback-checkpoints.py` records an unsuccessful
fallback-environment experiment with this same error, while
`check-owner-union-checkpoints.py` records the original library's header
passing signature validation under union. Those are upstream historical
observations, not tests rerun on Picasso here.

The defconfig now selects `OWNER_UNION_SYSTEMOS` and disables the additional
non-union `OWNER_CACHE_CARRIER` allowance. SystemOS mounts read-only over `/`;
native union lookup falls back to OS for lower-only files. Owner checks still
require the selected UUID/device, read-only APFS, verified root media and the
actual union root graph. Boot-manifest publication and code-signature checks
remain intact. The original image bytes and separate RAM Data are unchanged.

Before starting launchd, a new preflight uses ordinary vnode lookup to check:

- `/usr/lib/libobjc-trampolines.dylib` belongs to the mounted SystemOS.
- `/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64` belongs to SystemOS.
- `/sbin/launchd` belongs to the original lower OS mount.

Each must resolve to a regular file on the expected mount or startup fails
with its real error. These checks establish path/source identity, not that
file content has been read, decompressed or accepted by AMFI. Original callers
perform those checks while opening and mapping the files.

## Preserve root vnode ownership

In the pinned XNU, `set_rootvnode()` gives `kernproc->fd_cdir` a borrowed alias
of the one root reference. `checkdirs_callback()` previously transferred it
as an independently owned cwd reference; `checkdirs()` then transferred the
root reference again. A mount over `/` left the old root one usecount short.

The callback now skips that reference transfer only for kernproc's alias of
the current root. Its provisional new-vnode reference is released normally;
`checkdirs()` performs the single root transfer. Ordinary cwd/rdir references,
including a kernel cwd outside the root, keep their original behavior.

The ownership issue is also documented by the
[asteroids reference fix](https://github.com/rootstockos/UniXNU/blob/0d215db/patches/xnu/native-union-root-checkdirs.patch).
This adaptation keeps the ordinary kernel cwd/rdir path instead of returning
early for every kernproc callback. It changes no filesystem authentication.

## Validation and next checkpoint

Evidence resides in `out/objc-trampolines-20261030/`:

- `ci76-packaging-verified.json` independently verifies the Android boot header,
  FAT chains and embedded kernel/Data/trust/ADT/loader bytes. Kernel SHA-256 is
  `a16402ef079f322f7745b3188c171cf57d40b32b5b9aefc1047e7c15373d260c`;
  boot SHA-256 is `1aa89eb7543c1e5986ef779a1abca7356a37501b87fab8b04d37565402471e06`.
- `original-path-evidence.json` records original archive metadata. SystemOS
  SHA-256 is `b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f`.
  Local 7-Zip cannot decode this file's ZBM resource fork, so no successful
  local library extraction or library-byte hash is claimed.
- `replay-v1/report.json`: both passes check 62 patches; all 14 tracked generated
  sources are identical between passes. Incremental reversal removes the new
  union patch before the owner mount patch is reconsidered.
- `host-v3/results.json`: the actual extracted functions pass 8 reference and
  23 mount/preflight cases, both ordinary and ASan. Tests cover shutdown ref
  balance, non-root kernel cwd, rdir, ref failures, mount/lookup/copyout errors,
  wrong source/type/device/graph, readonly/owner refusal and manifest failure.
  The historical callback reproduces the reference undercount. Earlier fixture
  builds had naming errors; only the fixture declarations were corrected.

The local tests use host VFS/owner/copyout stand-ins. Build 77 now passes Apple
SDK compilation; device execution of the union change remains pending. The
next device log should show `composition union=1`,
three `NATIVE_OWNER_UNION ... error=0 source_match=1` lines, normal Data/cache
startup, and whether the missing-library abort disappears. Continue from the
actual remaining service exits/checkins; do not infer a usable display.

At the user's request, `build-kernel.yml` is removed. The Compose workflow
continues to build the kernel, payload and boot image, with its existing
artifact checks. It also retains the two affected XNU compilation records.
