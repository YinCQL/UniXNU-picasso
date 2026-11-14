# UniXNU on Redmi K30 5G (picasso, SM7250)

Experimental iPhone X / iOS 16.7.16 (20H392) userspace on XNU 10063 / Darwin 23,
with the retained native kext backend. This is a development port.

## Progress

- SpringBoard and apps run on the device, with touch, buttons, brightness,
  NativeGL rendering and speaker audio. Boot audio self-tests are disabled by default.
- Authenticated OS/SystemOS and unencrypted persistent 20 GiB Data are working.
- Battery percentage, charging status and USB CDC-ACM logging work. Charging is
  source-limited: computer USB stays at 500 mA; a tested USB-A adapter reaches
  about 1.6 A at 5.1 V. The 5 V / 3 A policy is a ceiling, not a guaranteed rate.
- Long-press power opens the shutdown UI, and shutdown backlight delay is fixed.
- Known issues: app-switcher card edge lines and intermittent white cards;
  suspend/wake, standby consumption and battery learning still need work.
  Other hardware support and complete shutdown remain under validation.

USB logs: run `capture-logs.cmd` on Windows, or
`scripts/capture-picasso-usb-console.py` on macOS.

## Build

- Firmware: [UEFI for picasso](https://github.com/YinCQL/uefi_picasso).
- Kernel, 80 MiB media payload and boot image: run
  `Compose Boot Image (FD + kernel + payload)` manually on the selected branch.
  It builds the audio HAL, NativeGL and runtime UI with real Apple SDK inputs,
  then verifies signatures, Data identity and the payload.
- Existing persistent Data requires its exact saved seed. Compose reuses a
  compatible retained artifact, or accepts an explicit `seed_artifact_id`.
  If no seed is available, it stops unless `create_data_seed` is explicitly
  enabled for a new installation. A new seed cannot restore an existing Data overlay.
- Build success does not establish device stability or complete hardware support.

## Credits

- [silenzio111](https://github.com/silenzio111)
- [huydre](https://github.com/huydre)

## Reference

- UniXNU: https://github.com/UniXNU/UniXNU
