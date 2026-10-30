# Writable Data for original launchd boot tasks

Data mounts writable and reaches early boot completion. The actual 768 MiB
loader allocation has been observed on later builds. Build81 exhausted APFS
transaction headroom in its 256 MiB logical Data volume; the next seed uses
512 MiB while retaining sparse allocation and the 4 MiB packed-seed limit.
See [build81 startup evidence](build81-startup-investigation.md). The new seed
still needs macOS formatting, SDK build and device validation.
The original mount and [watchdogd SIGTRAP](watchdogd-trap-investigation.md)
investigations below are retained as earlier evidence.

## Device evidence

The user identifies `picasso-windows-20261029-221748-215.log` as build
[40887829724](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40887829724),
commit `afc54fb7df892177803e0ea6422d848f70244ab2`, and confirms an automatic reboot.
GitHub reports that exact build as successful. The log is 159,715 bytes, SHA-256
`5f16fc4ce53548b6d0dbed5fa3dba78465a547454ac7ddd22e963372d22ae4d1`.
The user's individual boot.img hash has not been independently checked.

Original Sandbox initialization now passes. Launchd starts fsck, mount phases,
data-protection, usermanagerd, dirs_cleaner, init_featureflags and tzinit.
The first two mount phases report `data volume missing, but not required in env: 0`.
Writes under `/private/var` fail with `Read-only file system`.
Finally tzinit reports `Last resort force_system_dataset() failed`, exits with
status 30, and the kernel panics because this critical boot task exited. Panic
flush, Fastboot reason and PSHOLD reset follow. This is a captured panic/reset
chain, rather than an inference from USB disconnection.

The pinned upstream's `check-owner-boot-manifest-checkpoints.py` records the
same failure: tzinit's `symlink("/usr/share/zoneinfo.default",
"/var/db/timezone/zoneinfo")` returns EROFS. Its
`check-native-ram-data-checkpoints.py` records that call returning zero after a
typed, writable APFS RAM Data mount. The old md0 fixture is a negative example:
its APFS mount returned ENOENT. These are upstream results, not new Picasso runs.

## Change

The board now links the fixed NativeRamBlock provider, enables OWNER_DATA_MOUNT,
and supplies a packed APFS seed through DarwinBoot's `PackedRAMDisk` placeholder.
The user requested a **60 MiB FAT payload**, raised from 55 MiB. The existing 95%
content guard remains enabled. CI checks the actual FAT contents after building.
The committed FD maps the original boot ramdisk gap through `0xBC000000`, beyond
the historical ABL-base estimate of the new ramdisk end, `0xB8313000`. The fixed
loader copies all used payload files into its owned window before entering XNU.
This source/binary layout audit does not replace a 60 MiB device boot.

`prepare-picasso-ram-data.py` runs as root on the macOS build runner. It checks
the exact original OS DMG and launchd hashes, mounts the source read-only, creates
a new 512 MiB bare APFS image, and preserves the original `/private/var` contents,
UID/GID, modes and symlinks. It excludes `staged_system_apps`, adds needed empty
Data directories, and records source paths and hashes. It does not disable boot
jobs, skip Setup, alter authenticated system images or install fake user records.
The formatter's actual nonzero volume UUID is written into the generated
`out/picasso/.config` before compiling. Re-running defconfig afterwards would
erase that identity and is intentionally rejected by the kernel build.

The packed seed is capped at 4 MiB. NativeRamBlock's fixed `d4d22d9` implementation
uses sparse pages for logical disks up to 2 GiB; larger disks require a persistent
store and are excluded here. A 512 MiB logical size is not a 512 MiB allocation at
boot. Resident seed pages, new write pages and metadata consume the kernel's
owned memory window. Build77 used 256 MiB and reached page shortage; the
current 768 MiB allocation is confirmed; peak use and the larger seed's runtime
resident growth still need measurements.
**Data is volatile: guest changes are lost after reboot.** OS/SystemOS remain
read-only. No userdata layout changes or UFS persistence options are enabled.

NativeRamBlock uses IOBlockStorageDevice and SHA-256, with no NativeAES calls.
The build guard accepts the existing NativeUfs/original-storage route only with
prelinked crypto and the source corecrypto provider selected. NativeAES remains
off. The RAM object imports are checked against NativeUfs's sole storage binding
object, board slide and final original IOStorageFamily extents.

Before launchd, the existing owner Data function selects the actual APFS volume
by UUID, mounts it at `/private/var`, and verifies the selected device, APFS type
and writable mount flags. Failure still returns the original error and panics.
Added `NATIVE_OWNER_DATA` lines cover provider/UUID, selected device and logical
size, mount begin/return, lookup begin/return and final stage/error/readonly.
Success is `done stage=4 error=0 readonly=0`; the subsequent tzinit task must also
complete successfully. No new firmlink or `/Library` overlay is inferred from
the log; the seed report records those original paths for later analysis.

## Build and verification

Both existing manual GitHub workflows fetch OS, prepare Data, compile with the
generated UUID, and pass `--data-ramdisk out/picasso/ram-data/data.dbrdisk` to the
payload builder. No workflow is dispatched automatically by this change.
For a macOS local build, run the same steps after preparing sources and trust:

```sh
python3 ./unixnu defconfig picasso_defconfig
sudo "$(command -v python3)" scripts/prepare-picasso-ram-data.py \
  --tree "$(python3 ./unixnu --prepare)" \
  --source out/ipsw/21H461/044-76040-038.dmg \
  --output out/picasso/ram-data --config out/picasso/.config
python3 ./unixnu build kernel payload -j 3 \
  --trust-cache out/picasso/original-static-system.bundle \
  --data-ramdisk out/picasso/ram-data/data.dbrdisk
python3 scripts/check-picasso-data-artifacts.py --tree out/picasso/tree \
  --work out/picasso --output out/picasso/logs/data-artifacts.json
```

The seed output directory must be new. Move any prior output to a temporary
directory for retention before retrying; do not recursively delete it.
Reports include seed provenance, real UUID, logical/packed sizes and hashes.
The final checker verifies the generated header and kernel UUID, RAM provider
symbol and match category, exact storage imports, FAT-extracted kernel/Data/trust
bytes, loader initrd path, packed ADT mode and ephemeral-storage property.

Host checks passed: 41 seed format/config/copy cases, three generated ADTs,
eight build dependency gates, six UUID cases, and all 61 patches in two passes
(61 first applications, five incremental reapplications). The actual generated
Data function also passed 16 cases under MSVC and 16 under ASan, including wrong
UUID/provider, copyout/mount/lookup failure, wrong device/type and read-only mount.
Storage import gate tests reject uncovered imports, empty audits and wrong slide.
They do not run Apple's APFS formatter, Apple SDK compilation or a phone.
The next matching CI build and device log must establish that Data mounts and
tzinit completes; later launchd failures remain separate work.

## CI mount-state correction

[40898162574](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40898162574)
at `fb34d7d` passed the macOS host checks, then failed in source OS mount
validation, before creating Data or compiling XNU. The validator incorrectly
assumed a `diskutil info -plist` property named `VolumeReadOnly`; the old run did
not save the plist, so its exact fields/values cannot be recovered from this log.
The validator now checks `os.statvfs(mountpoint).f_flag & os.ST_RDONLY`, which
macOS derives from the actual filesystem's `MNT_RDONLY` flag. Source must be
read-only and Data must be writable; APFS and ownership checks remain required.
Each mount saves its diskutil information, actual VFS flags and expected state
in `ram-data/os-mount.json` or `data-mount.json`, even when a check fails.

The same run also failed artifact upload: its recursive logs pattern followed
an intentional `/Library` test symlink into a protected host directory. Both
workflows now keep those fixtures under `host-checks/`, outside artifact paths,
and copy only `results.json` into logs. Fixtures are retained, not deleted.
The updated host suite covers absent plist read-only fields, unrelated VFS bits,
writable source rejection, read-only Data rejection, metadata disagreement and
failure cleanup. A new macOS CI run is still needed to verify the full pipeline.
