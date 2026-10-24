# UniXNU on Redmi K30 5G (picasso, SM7250)

> **Status: XNU boots from UEFI.** The UEFI firmware boots the UniXNU menu and the
> xnu entry starts the XNU kernel through DarwinBoot.
>
> Booting the kernel is all that works today: only the XNU kernel start is
> supported, and XNU still needs substantial fixes before it reaches a
> usable system.
>
> UEFI firmware: https://github.com/YinCQL/uefi_picasso

## Progress

- **UEFI firmware: working.** Boots to the UniXNU menu. UFS Mass Storage, GOP
  framebuffer, button input and stable boot all work. Built via the UniXNU build
  driver; produces a flashable `Mu-picasso-unixnu-0.img`.
- **XNU kernel: boots from UEFI.** Build workflow, first-console config and the
  SM7250 ADT generator are in place, and the SM7250 kernel patch set
  (`patches/unixnu-source-kernel.patch`) builds the kernel that DarwinBoot starts.

## Build

- Firmware: `Build UniXNU UEFI for picasso` workflow -> `fastboot boot Mu-picasso-unixnu-0.img`.
- Kernel + payload: `picasso XNU kernel` workflow (macos-latest).

## Merged work from the silenzio111 branch

`silenzio111/UniXNU-picasso` forked the same bring-up baseline (`189c01e`) and
added nine commits. Their build-side tooling is merged here; their board and
payload settings are not, because none of their six CI runs reached a green
build (the last one stops at `_corecrypto_kext_start` undefined, and the
"Stage 2 complete" claims in their README are not CI-verified).

Merged, host-side only (no effect unless a board selects them):

| File | Purpose |
| --- | --- |
| `scripts/build-arm-driver.py` | Builds an archive from the kernel's own `IOPlatformExpert.cpo.json` compile record: ArmCpu, ArmDTPlatform, NativeRamBlock, NativeUfs, NativeAdreno. Replaces the copy that only existed inside the device patch set. |
| `scripts/prepare-xnu-source.py` | Restores the omitted DriverKit / TrustCache headers with auth, plus a bundled `scripts/headers-cache/` so a rate-limited CI egress IP does not fail the fetch. This is now a real repository file instead of a patched-in script. |
| `drivers/ArmCpu/` | ArmCpu with a `/arm-io/intc/virtual-timer-intid` override (validated PPI 16..31, default 27) and the SPI attach helper, based on the upstream `1951cc84` submodule. |
| `scripts/make-picasso-adt.py` | Adds `/arm-io/serial` (GENI UART 0x988000), `/pram` panic-log region, absolute `reg` values under a DRAM `ranges`, UFS/UFS-PHY nodes and the Adreno/GMU/SMMU nodes, each behind the board's optional flags. |
| `scripts/dump-picasso-panic.py` | Host-side reader for the `0x46554E4B` panic header over fastboot / adb. |
| `patches/unixnu-source-kernel.patch` | Also guards the `native_legacy_display_probe` call with `XNU_NATIVE_LEGACY_DISPLAY`, and adds the `XNU_CRYPTO_BOOTSTRAP="source"` prelink bypass. |

Not merged (fork-only board state, unverified):

- `size_mib = 192`, `dram_size = 0x180000000`, `title`/`kernel_args` for the
  UFS-root + Adreno stage, and `chip_id` removed from `[payload.adt]`. The
  payload window is limited to about 57 MiB by the ABL buffer, so 192 MiB cannot
  load on this device.
- `[userdata]` geometry `0x200000`..`0x3A00000` and `kernelcache_sha256`.
- `unixnu.lock`: harmless but only records the fork's own fetch, and
  `unixnu fetch` rewrites it.
- `configs/picasso_platform_defconfig`: kept as the stage-6 draft, not the CI
  config.

## Reference

- UniXNU: https://github.com/UniXNU/UniXNU
