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
added nine commits. Its build-side tooling and drivers are merged here. Its
board and payload values are not adopted wholesale: they describe the stage-6
UFS-root + Adreno target that has not been reached here, and two of them
(`size_mib = 192` and `dram_size = 0x180000000`) do not match what the DT and
the ABL window on this device actually allow. Their CI history is not evidence
against the code: those runs failed on an early repository configuration, and
the branch was built locally on macOS.

Merged (host-side tooling and drivers):

| File | Purpose |
| --- | --- |
| `scripts/build-arm-driver.py` | Builds an archive from the kernel's own `IOPlatformExpert.cpo.json` compile record: ArmCpu, ArmDTPlatform, NativeRamBlock, NativeUfs, NativeAdreno. Replaces the copy that only existed inside the device patch set. |
| `scripts/prepare-xnu-source.py` | Restores the omitted DriverKit / TrustCache headers, with `scripts/headers-cache/` as a seed so a rate-limited CI egress IP does not fail the fetch. A real repository file instead of a patched-in script. |
| `drivers/ArmCpu/` | Upstream ArmCpu `1951cc84` plus a `/arm-io/intc/virtual-timer-intid` override (validated little-endian u32 PPI, 16..31; absent keeps upstream's 27) and the SPI attach helper. |
| `scripts/make-picasso-adt.py` | Adds `/arm-io/serial` (GENI UART 0x988000), `/pram` panic-log region, absolute `reg` values under a DRAM `ranges`, UFS and UFS-PHY `reg`s, and the Adreno/GMU/SMMU nodes. Each new node is inert unless the board asks for it. |
| `scripts/dump-picasso-panic.py` | Host-side reader for the `0x46554E4B` panic header over fastboot / adb. |
| `scripts/derive-a620-hwinit.py` | Derives the A620 hw_init register table from the pinned a6xx sources; needs `.cache/adreno-source`, so it is not part of the CI build. |
| `patches/unixnu-source-kernel.patch` | Rebuilt on the new pin: guards the `native_legacy_display_probe` call with `XNU_NATIVE_LEGACY_DISPLAY` (instead of the earlier no-op stub, which would collide with the archive's definition once legacy display is enabled) and adds the `XNU_CRYPTO_BOOTSTRAP="source"` prelink bypass. |

Board values kept as they are here, with the reason:

- `[payload] size_mib = 54`. The ABL buffer holding kernel + ramdisk ends near
  `0xB7E00000`; the device DT reports `linux,initrd-start = 0xB5513000` and
  `initrd-end = 0xB7DFE66D`, so the window is about 57 MiB and 192 cannot load.
- `[payload] dram_size` stays `0x80000000`. DarwinBoot does not read
  `/chosen/dram-size` at all (it reads only `darwinboot-memory-size`), and no
  XNU patch in the tree consumes it either; sheng (>= 8 GiB) and guacamole
  (>= 6 GiB) both carry far smaller values, so this is not a total-memory field
  and raising it to the DT's 6 GiB is not a fix.
- `[payload.adt] chip_id = "7250"`, `owned_memory_size = 0x100000000`,
  `unique_chip_id = "required"`. `make-picasso-adt.py` requires `--chip-id`
  together with `--unique-chip-id`, so dropping `chip_id` breaks the payload
  step for this defconfig.
- `[userdata]` geometry and `kernelcache_sha256` stay unset: the local device
  data (`picasso/devicetree/base/firmware/android/fstab`, `xbl.img`) has no
  usable GPT entry for `userdata`, and a wrong kernelcache hash makes the
  prelink step refuse the cache outright.
- `configs/picasso_platform_defconfig` is kept as the stage-6 candidate rather
  than a draft. It is the config the fork actually built with
  (`CONFIG_XNU_CRYPTO_BOOTSTRAP="source"`, `NATIVE_UFS`, `NATIVE_ADRENO`);
  switching to it also needs the matching `[kernel] archives` entries
  (`build-libpthread-kernel.py`, `NativeUfs`, `NativeAdreno`).

## Reference

- UniXNU: https://github.com/UniXNU/UniXNU
