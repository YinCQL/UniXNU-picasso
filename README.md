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

## Reference

- UniXNU: https://github.com/UniXNU/UniXNU
