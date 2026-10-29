# UniXNU on Redmi K30 5G (picasso, SM7250)

> **Status: original early userspace runs; service startup is still incomplete.**
> UFS, authenticated OS/SystemOS, the dyld shared cache, volatile Data and AES
> startup pass on the device. The latest tested image reboots after a userspace
> watchdog timeout. The current SystemOS union change addresses an observed
> missing-library abort and still needs a new build and device test; see
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

## Tooling merged from the silenzio111 fork

`silenzio111/UniXNU-picasso` forked the same bring-up baseline (`189c01e`).
Its host-side build tooling and drivers are merged here:

| File | Purpose |
| --- | --- |
| `scripts/build-arm-driver.py` | Builds an archive from the kernel's own `IOPlatformExpert.cpo.json` compile record: ArmCpu, ArmDTPlatform, NativeRamBlock, NativeUfs, NativeAdreno. Replaces the copy that only existed inside the device patch set. |
| `scripts/prepare-xnu-source.py` | Restores the omitted DriverKit / TrustCache headers, with `scripts/headers-cache/` as a seed so a rate-limited CI egress IP does not fail the fetch. A real repository file instead of a patched-in script. |
| `drivers/ArmCpu/` | Upstream ArmCpu `1951cc84` plus a `/arm-io/intc/virtual-timer-intid` override (validated little-endian u32 PPI, 16..31; absent keeps upstream's 27) and the SPI attach helper. |
| `scripts/make-picasso-adt.py` | Adds `/arm-io/serial` (GENI UART 0x988000), `/pram` panic-log region, absolute `reg` values under a DRAM `ranges`, UFS and UFS-PHY `reg`s, and the Adreno/GMU/SMMU nodes. Each new node is inert unless the board asks for it. |
| `scripts/dump-picasso-panic.py` | Host-side reader for the `0x46554E4B` panic header over fastboot / adb. |
| `scripts/derive-a620-hwinit.py` | Derives the A620 hw_init register table from the pinned a6xx sources; needs `.cache/adreno-source`, so it is not part of the CI build. |
| `patches/unixnu-source-kernel.patch` | Rebuilt on the new pin: guards the `native_legacy_display_probe` call with `XNU_NATIVE_LEGACY_DISPLAY` (instead of the earlier no-op stub, which would collide with the archive's definition once legacy display is enabled) and adds the `XNU_CRYPTO_BOOTSTRAP="source"` prelink bypass. |

## Reference

- UniXNU: https://github.com/UniXNU/UniXNU
