# UniXNU on Redmi K30 5G (picasso, SM7250)

Experimental iPhone X / iOS 16.7.16 (20H392) userspace on XNU 10063 / Darwin 23,
with the retained native kext backend. This is a development port.

Status below reflects hardware checks on `feat/fix-v3` through 2026-11-18.

## Progress

- SpringBoard and apps run on the device, with touch, buttons, brightness,
  NativeGL rendering and speaker audio. Boot audio self-tests are disabled by default.
- Authenticated OS/SystemOS and unencrypted persistent 20 GiB Data are working.
  [NativeUfs](drivers/NativeUfs/README.md) now uses a four-slot runtime queue
  with three usable command slots, real interrupt completion, an 8 MiB paged
  verification-table cache and direct DMA into driver-owned authentication
  snapshots. Data and NVRAM restoration passed the hardware checks.
- Bluetooth can enable, discover and connect to Redmi Buds 6; playback of an
  existing Voice Memos recording and the sound settings were verified.
  Firmware handshakes can still time out (`-60`), briefly leaving the Bluetooth
  page waiting before it becomes usable. Long-term connection stability remains
  under validation.
- About opens normally. Device-name edits save, and the Bluetooth footer picks
  up the new name. Name persistence across reboots and the name advertised to
  another phone have not been separately verified.
- App-switcher snapshot resampling and persistence fixes passed repeated Photos
  and Settings checks without the previously observed black edge or white cards.
- Battery percentage, charging status and USB CDC-ACM logging work. Charging is
  source-limited: computer USB stays at 500 mA; a tested USB-A adapter reaches
  about 1.6 A at 5.1 V. The 5 V / 3 A policy is a ceiling, not a guaranteed rate.
- Long-press power opens the shutdown UI, and shutdown backlight delay is fixed.

## Recent driver updates

- The ShengUfs port keeps Picasso's SM7250 register/SMMU configuration and
  Darwin 23 interfaces. It adds bounded retries and DMA quarantine, strict
  LUN4 firmware reads, SSD/Internal media properties, read-only link diagnostics
  and completion/persistence statistics. The
  [NativeUfs README](drivers/NativeUfs/README.md) records the upstream revisions,
  implemented features and the remaining port decisions.
- Supporting ArmCpu changes restore Darwin multi-instruction assembly,
  serialize SPI registration and limit SGI self-tests to the owned interrupt.
  Picasso's CPU topology, interrupt routing and frequency policy are retained.
- PICASSO_ADSP sensor-registry startup now replies in the same GLINK processing
  round, uses bounded 1 ms bursts after real FastRPC traffic and limits routine
  synchronous logging. The `/mnt/vendor/persist/sensors/registry` requests use
  the RAM-backed FastRPC/apps_std namespace. For the same 1,283 requests,
  device timing fell from 9.982 s after phase one to 4.043 s after phase two
  (59.5%, one boot per version). This measures registry request handling;
  whole-device boot-time savings have not been measured. Both versions reached
  the desktop and passed speaker playback checks. Path indexing and a registry
  cache across reboots are not implemented.

## Remaining limitations

- Suspend/wake, standby consumption, battery learning and complete shutdown
  still need validation and further work.
- Full Wi-Fi and sensor functionality remain unqualified. NativeGL support
  does not establish complete Metal support or sustained 60 fps.
- Data is unencrypted developer storage. KeyStore compatibility placeholders
  do not provide a real keybag, passcode protection or hardware-backed encryption;
  hardware AES and FairPlay/DRM support remain incomplete.
- The latest UFS/ADSP images reached SpringBoard, restored Data and played
  speaker audio; the user reported no grey screen or automatic restart during
  those checks. Controlled throughput/power comparisons and long-term stability
  have not been established.

USB logs: run `capture-logs.cmd` on Windows, or
`scripts/capture-picasso-usb-console.py` on macOS.

## Build

- Boot firmware: [UEFI for picasso](https://github.com/YinCQL/uefi_picasso).
- Board firmware: [firmware/README.md](firmware/README.md). Small touch, GPU
  and Bluetooth inputs are bundled by category and pinned to the supplied
  `picasso_images_V13.0.5.0.SGICNXM` factory images. Preparation verifies every
  file without a firmware download. ADSP retains its UFS/PAS runtime path.
- Kernel, media payload and boot image: `board.toml` `[payload].size_mib`
  sets the actual FAT capacity for local and CI builds (currently 96 MiB).
  Contents must fit its 95% budget and the resulting boot image must fit the
  128 MiB boot partition. To build, run
  [Compose Boot Image (FD + kernel + payload)](.github/workflows/compose-bootimg.yml)
  manually on the selected branch. It builds the audio HAL, NativeGL and runtime
  UI with real Apple SDK inputs, then verifies signatures, Data identity and the
  payload. Recent hardware checks used local WSL builds with the same SDK and
  payload verification, followed by boot-image checks and boot-only flashing.
- Existing persistent Data requires its exact saved seed. Compose reuses a
  compatible retained artifact, or accepts an explicit `seed_artifact_id`.
  Changing payload capacity preserves seed compatibility; the retained original
  recipe is accepted only for the audited capacity migration, with all seed
  hashes, signed-library checks and Data UUID verification retained.
  If no seed is available, it stops unless `create_data_seed` is explicitly
  enabled for a new installation. A new seed cannot restore an existing Data overlay.
- Build success does not establish device stability or complete hardware support.

## Credits

- [silenzio111](https://github.com/silenzio111)
- [huydre](https://github.com/huydre)

## Reference

- [UniXNU](https://github.com/UniXNU/UniXNU)
