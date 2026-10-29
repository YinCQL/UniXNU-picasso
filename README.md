# UniXNU on Redmi K30 5G (picasso, SM7250)

> **Status: original early userspace runs; service startup is still incomplete.**
> UFS, authenticated OS/SystemOS, the dyld shared cache, volatile Data and AES
> startup pass on the device. The latest tested image reboots after a userspace
> watchdog timeout. The current SystemOS union change addresses an observed
> missing-library abort and passes [build 77](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40974341838);
> device validation is pending. CPU/GOP display is next; see
> [the investigation](docs/objc-trampolines-investigation.md).
>
> UEFI firmware: https://github.com/YinCQL/uefi_picasso

## Progress

- **UEFI firmware: working.** Boots to the UniXNU menu. UFS Mass Storage, GOP
  framebuffer, button input and stable boot all work. Built via the UniXNU build
  driver; produces a flashable `Mu-picasso-unixnu-0.img`.
- **XNU kernel: reaches original launchd and system services.** NativeUfs
  publishes the authenticated OS and SystemOS volumes; APFS mounts them and
  dyld maps the shared cache. A separate writable Data volume runs in RAM.
  Service failures and the watchdog reboot remain under investigation; a
  usable SpringBoard display and persistent Data are not established.
- **USB kernel console: working.** The kernel exposes a CDC-ACM device named
  `picasso XNU USB console` and streams the buffered/live console over DWC3.
  Capture it on macOS with `scripts/capture-picasso-usb-console.py`, or on Windows
  by double-clicking `capture-logs.cmd` for automatic device detection and saving; see
  `docs/usb-console-bringup.md` for hardware results and safeguards.


## Build

- Firmware: `Build UniXNU UEFI for picasso` workflow -> `fastboot boot Mu-picasso-unixnu-0.img`.
- Kernel + payload + boot image: `Compose Boot Image (FD + kernel + payload)`
  workflow (macos-latest).

## Credits

Thanks to [silenzio111](https://github.com/silenzio111) for build/driver tooling,
[#1](https://github.com/YinCQL/UniXNU-picasso/pull/1) USB CDC console,
[#2](https://github.com/YinCQL/UniXNU-picasso/pull/2) verified UFS/APFS root, and
[#3](https://github.com/YinCQL/UniXNU-picasso/pull/3) CDC fixes, command tools and hardware tests.

## Reference

- UniXNU: https://github.com/UniXNU/UniXNU
