> Integration note (PR18, 2026-11-06): this is the contributor's original
> hardware report, not validation of the merged branch. The reviewed merge keeps
> the existing display geometry, brightness fixes and 20 GiB persistent Data
> identity. IPC census and frame timing diagnostics described below are not
> included. Bluetooth job enablement is migrated before launchd; radio hardware
> remains unimplemented. KeyStore compatibility stubs do not implement encryption.

# iOS16 / iPhone10,3 migration status (2026-11-06 02:00 CST)

Workspace: `worktrees/ios16-picasso`, branch `stage12/picasso-ios16` (from
`origin/feat/ios16-gpu` @ `65529dd`). Temporary files and device logs live in
`temp/`; build outputs stay in `out/`.

## Image built tonight

The iPhone 16.7.16 (20H392) userdata image is built from the pinned IPSW
(`out/ipsw/20H392` extraction) and contains the unchanged OS and SystemOS
windows:

- `out/picasso/userdata/userdata-root.img` — 8,380,219,392 bytes
- manifest sha256 `6d7ac29a...`; OS sha256 `6e87bfca...`; SystemOS sha256
  `9cd738f3...`
- flash: `fastboot flash userdata out/picasso/userdata/userdata-root.img`
  (partition capacity 50,981,744,640 bytes; the image fits)

Boot images:

- `out/picasso/boot/boot-latest.img` — 60 MiB payload, 65,105,920 bytes,
  kernel sha256 `13d15066d430e933d9422330815292583e991f3aec3e80b309f931b2f77fa33f`
- `out/ios16-gpu1/picasso/boot/ios16-gpu1.img` — 80 MiB experiment payload
  with the iOS16 GPU addon, 86,077,440 bytes; payload sha256 `32cde9b0...`
  for the addon, 960-byte kernel cmdline
- checks: `make-picasso-adt.py` produces a 15,004-byte ADT with
  `display-scale=3`, `display-height=2400` and artwork subtype 2400;
  `check-picasso-display-policy.py` passes.

## Commits made tonight

1. `df7a484 display: match the 360x800 drawable to the 1080x2400 panel`
   — adds `display-height=2400`, artwork subtype 2400.
2. `65fe3d8 hid: retry undelivered touch reports and qualify the dispatch table`
   — iPadOS `94e1c8c` retry and `cd3c8e9` vtable/class guard; kernel rebuilt.
3. `6f4f58d gpu: advertise the owner Metal plugin only when its addon is present`
   — restores `MetalPluginName`/`MetalPluginClassName` behind `sheng-gpu-addon=1`.
4. `2686825 usb: drop the DWC3 pull-up before a warm reset`
   — ports the iPadOS `2906fa3` fix so CDC `reboot-fastboot` gives the host a
   clean disconnect and the boot loader's fastboot gadget re-enumerates.
5. `a7ecd94 board: keep com.apple.bluetoothd enabled for the audiomxd touch path`
   — removes it from the offline DENY list (iPadOS `b41bce2`/`ffc849f`).
6. `157ae5a keystore: absorb the iPadOS selector surface and descriptor fix`
   — ports `f29f391`: bookkeeping selectors 4/5/9/12/13/26/27/55/101/102,
   keybag handle 0 for 14, empty configuration for 31, any context for
   7/17/31/35, and `prepare`/`complete` around the out-of-line output
   descriptor. Host check passes.

Final kernel with all changes:
sha256 `c49616bc62c65814fd083aa63c1366dd95f771b5f51747840b49fbfc4cf383d4`.
Final boot images: `out/picasso/boot/boot-latest.img` (60 MiB) and
`out/ios16-gpu3/picasso/boot/ios16-gpu3.img` (80 MiB, addon
`32cde9b0...`, 960-byte cmdline).

## iOS16 GPU addon (experiment)

`temp/gpu-ios16/` contains a native arm64/iOS16 build made from the current
NativeMetal sources with the iOS16 ABI fixes:

- `NativeAdrenoMetal.bundle` — `MinimumOSVersion=16.0`, plugin/Turnip/daemon
  all `LC_BUILD_VERSION minos 16.0` and ad-hoc 4 KiB signed.
- `ngpu-addon-ios16.bin` — 12,133,208 bytes, NGPUADD2, `--publish` and
  clients `backboardd,SpringBoard`.
- `gpu-trust.bundle` — 67,888 bytes, two original 20H392 modules plus one
  owner module with four CDHashes (SB dylib, plugin, Turnip, `adrenoprobed`).
- `BUILD.md` records the exact source edits, commands, CDHashes and the
  known limitation: the existing bridge static archive was built for iOS 17
  and only the final Mach-O was rewritten to minos 16.

This is a build artifact only; it has not been device-qualified. The kernel
advertises the plugin only when the payload carries `sheng-gpu-addon=1`, so
the normal 60 MiB image keeps the old CPU-only behavior.

## Device state (2026-11-06 06:30 CST)

The phone got a responsive fastboot back at 02:03 CST: a hub-port VBUS cycle
followed by ~75 s of settling returns the boot loader's protocol, and small
`fastboot flash`/`getvar` commands then work. The raw 8.4 GiB userdata image
cannot be flashed with fastboot 34/37: the size check reaches
`copy_avb_footer()`, which reads the whole image into memory and dies with
`Failed reading from userdata`. Two independent fixes are prepared:
`out/picasso/userdata/userdata-root.sparse.img` (Android-sparse, 7.3 GiB,
2,045,952 blocks) takes fastboot's sparse path and skips the AVB footer copy,
and `temp/pt28/platform-tools/fastboot` (28.0.1) predates `copy_avb_footer`
entirely. The watcher tries the raw image with fastboot 28 first, then the
sparse image.

A `fastboot boot temp/tiny-boot.img` (2.1 MiB, no payload) then booted the UEFI
firmware successfully, but that image has no payload in its ramdisk, so the
firmware sat in its boot menu without exposing USB. A VBUS cycle does not reset
a running UEFI session; the phone now needs a physical power-button press
(long press) before fastboot is reachable again. Do not use the payload-less
image as a diagnostic again.

`temp/overnight-control.py` is the detached watcher. When a responsive
fastboot appears it will flash the sparse userdata once, then alternately boot
`out/picasso/boot/boot-latest.img` and `out/ios16-gpu2/picasso/boot/ios16-gpu2.img`
(the USB-reset-fix kernel, HID retry, 2400 ADT and GPU addon), capture CDC,
send `status`/`snapshot` and leave the working image running.

## Next steps on a responsive device

1. `fastboot flash userdata out/picasso/userdata/userdata-root.img`
2. `fastboot boot out/picasso/boot/boot-latest.img`
3. capture `/dev/cu.usbmodem*` and confirm stage ①/②: AMFI rejects zero,
   `bsd_init` -> launchd -> SpringBoard lock screen.
4. If the baseline reaches CDC, test `picasso-cdc-command.py --command
   reboot-fastboot`, then repeat with `ios16-gpu1.img` and look for
   `PICASSO_GPU cp: metal ... plugin=declared`, `PICASSO_GPU open: plugin`,
   `submissions` growth and `PICASSO_DISPLAY_STATE ... copies=0`/`flips` growth.
