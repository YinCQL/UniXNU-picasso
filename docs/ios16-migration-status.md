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

## Device state

The phone is enumerated as fastboot (`52cc791d`, Google 18d1:d00d, 480 Mbps)
but its fastboot protocol stopped answering after an earlier `fastboot boot`
transfer into the 65 MiB image. USB re-enumeration, endpoint clear, and hub
port power cycles do not recover it; the phone needs a physical power cycle
(power button long press) before the next `fastboot` command.

`temp/overnight-control.py` is running as a detached watcher. When the phone
appears in a responsive fastboot it will:

1. flash `out/picasso/userdata/userdata-root.img` once;
2. alternately boot `boot-latest.img` and `ios16-gpu1.img`;
3. capture the CDC console, run `status` and `snapshot`, and leave the image
   running for inspection.

The CDC console control path (`picasso_usb=2`, `reboot-fastboot`, `status`,
`snapshot`) is retained in both images. `scripts/picasso-cdc-command.py`
remains the intended way to return a running XNU to fastboot without a button.

## Next steps on a responsive device

1. `fastboot flash userdata out/picasso/userdata/userdata-root.img`
2. `fastboot boot out/picasso/boot/boot-latest.img`
3. capture `/dev/cu.usbmodem*` and confirm stage ①/②: AMFI rejects zero,
   `bsd_init` -> launchd -> SpringBoard lock screen.
4. If the baseline reaches CDC, test `picasso-cdc-command.py --command
   reboot-fastboot`, then repeat with `ios16-gpu1.img` and look for
   `PICASSO_GPU cp: metal ... plugin=declared`, `PICASSO_GPU open: plugin`,
   `submissions` growth and `PICASSO_DISPLAY_STATE ... copies=0`/`flips` growth.
