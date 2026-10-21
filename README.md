# UniXNU on Redmi K30 5G (picasso, SM7250)

> **Status: early bring-up.** The UniXNU boot menu is reached on hardware. The XNU
> kernel does not boot yet.

## Progress

- **UEFI firmware: working.** Boots to the UniXNU menu. UFS Mass Storage, GOP
  framebuffer, button input and stable boot all work. Built via the UniXNU build
  driver; produces a flashable `Mu-picasso-unixnu-0.img`.
- **XNU kernel: not booting.** Build workflow, first-console config and the SM7250
  ADT generator are in place. The SM7250 kernel patch set
  (`patches/unixnu-source-kernel.patch`) is still a placeholder and is the blocker.

## Build

- Firmware: `Build UniXNU UEFI for picasso` workflow -> `fastboot boot Mu-picasso-unixnu-0.img`.
- Kernel + payload: `picasso XNU kernel` workflow (macos-latest).

## TODO

1. Write the SM7250 kernel patch set (the blocker).
2. Bring up the kernel in stages: bootstrap -> first-console -> UFS / display / Adreno.
3. Fill in the real `userdata` GPT geometry in `board.toml`.
4. Confirm the UniXNU boot menu replacing the stock Mass Storage / Power Off / Exit entries.
5. Wire the UART for the `UNIXNU:` / `DARWINBOOT:` / XNU serial log.
6. Cross-check ADT values (DRAM size, CPU MPIDR) on hardware.
7. Set picasso-specific device identity (currently pinned to iPad7,1).

## Reference

- UniXNU: https://github.com/UniXNU/UniXNU
