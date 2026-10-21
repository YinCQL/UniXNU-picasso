# UniXNU on Redmi K30 5G (picasso, SM7250)

Porting [UniXNU](https://github.com/UniXNU/UniXNU) (the XNU / Darwin kernel) to the
Xiaomi Redmi K30 5G, codename **picasso**, Qualcomm **SM7250 / lito** (Snapdragon 765G).

This repository is the **device repository**: board description, kernel config, the
ADT generator and the build wrapper. The UEFI firmware is built from the picasso
Mu-Silicium platform package on the project's `UniXNU` firmware branch.

> **Status: early bring-up.** The UniXNU boot menu is reached on hardware. The XNU
> kernel does **not** boot yet — the SM7250 kernel adaptation patch set is still a
> placeholder. Expect breakage.

---

## Current progress

### UEFI firmware — working

The picasso Mu-Silicium UEFI port boots to the **UniXNU menu** on hardware.

What works:

| Feature | State |
|---|---|
| UEFI boots to the UniXNU menu | Working |
| UFS BlockIo / Mass Storage | Working (host PC sees all GPT partitions) |
| Framebuffer via GOP (SimpleFbDxe) | Working |
| Buttons as console input (power = Enter) | Working |
| Stable boot (no random shutdown) | Working |

The firmware is built through the UniXNU build driver, which links the UniXNU menu
app (`UniXNUPkg`) and the `DarwinBoot` loader into the firmware volume, then packs a
flashable `Mu-picasso-unixnu-0.img` (BootShim + FD + DTB).

### XNU kernel — not booting yet

The XNU kernel is the main remaining work. The macOS kernel build workflow is in
`.github/workflows/build-kernel.yml` (self-contained: the repo's `./unixnu` wrapper
clones the pinned upstream revision from `upstream.rev`, applies the kernel patch
set, and builds).

What is in place:

- `configs/picasso_defconfig`: first-console kernel (corecrypto, ArmDTPlatform,
  ArmCpu, arch timer, PAN, DT panic region).
- `configs/bootstrap_defconfig`: base XNU + prelinked candidate with no device
  archives (compile records only).
- `board.toml`: iPad7,1 iPadOS 17.7.11 selected as the kernel source
  (instruction-compatible with SM7250 / Cortex-A76).
- `scripts/make-picasso-adt.py`: generates the SM7250 Apple-format device tree
  (ADT). Pinned values: GICv3 distributor `0x17A00000`, redistributor `0x17A60000`,
  8 Cortex-A76 cores (Affinity1 `0x000..0x700`), timebase `19200000`, UFS `0x1d84000`,
  DRAM base `0x80000000`.

What is **missing** (the blocker):

- `patches/unixnu-source-kernel.patch` is a **placeholder**. The real SM7250 XNU
  adaptation patch set — SoC platform drivers, interrupt/timer, and the kernel-source
  changes — has not been written yet. Until it lands, the kernel build either fails
  or produces a kernel that does not match the SM7250 hardware.

---

## Repository layout

    UniXNU-picasso/
      board.toml                  # board description, IPSW, firmware/kernel/payload config
      upstream.rev                # pinned upstream UniXNU revision
      unixnu                      # repo-local wrapper: clone upstream, apply patch, build
      configs/
        bootstrap_defconfig       # minimal bootstrap kernel
        picasso_defconfig         # first-console kernel
      scripts/
        make-picasso-adt.py       # SM7250 ADT generator
      patches/
        unixnu-source-kernel.patch  # SM7250 kernel patch set (PLACEHOLDER)
      .github/workflows/
        build-kernel.yml          # macOS kernel + payload build

---

## Build

### UEFI firmware (flashable image)

Run the `Build UniXNU UEFI for picasso` workflow on the project's `UniXNU` firmware
branch. It uploads `Mu-picasso-unixnu-0.img`.

Flash (temporary, does not touch the installed boot partition):

    fastboot boot Mu-picasso-unixnu-0.img

### XNU kernel + payload (macOS only)

Run the `picasso XNU kernel` workflow here (GitHub Actions, macos-latest). It builds
the kernel and the FAT payload (kernel + ADT + `unixnu.toml`), and uploads
`kernel.macho`, `payload.img` and logs.

---

## What needs further work / testing

1. **SM7250 kernel patch set** (the blocker). Write `patches/unixnu-source-kernel.patch`:
   SoC platform drivers (interrupt, timer, UART), the XNU source changes, and the
   prelink roots for SM7250. Start from the first-console config and iterate against
   the serial log.

2. **Kernel bring-up order.** bootstrap (compile records) -> first-console (reach the
   video/serial console, expect a genuine missing-driver panic) -> then add UFS,
   AES, VirtIO, display and Adreno one stage at a time.

3. **UserData partition geometry.** `board.toml` `[userdata]` is a placeholder. Read
   the GPT `userdata` entry (first_lba / last_lba / block size) from the device and
   fill it in; the first real boot needs a userdata image.

4. **Boot-menu entries.** The stock Mass Storage / Power Off / Exit entries are not
   present in the UniXNU boot menu; it shows UniXNU first then `UEFI Misc Device`
   entries. Confirm whether this is expected (UniXNU replaces the stock BDS menu) or
   whether the stock actions need to be restored.

5. **Serial log channel.** The vendor serial driver is loaded but the UART test
   points are not populated. For kernel bring-up, wire the SM7250 UART (QUPV3 BLSP)
   to a USB-TTL adapter to capture the `UNIXNU:` / `DARWINBOOT:` / XNU boot log.

6. **ADT values on hardware.** `make-picasso-adt.py` uses pinned SM7250 values from
   the board DT; cross-check the DRAM size and CPU MPIDR values against the real
   device and update `board.toml` `[payload.adt]`.

7. **Device identity.** `board.toml` pins the iPad7,1 identity (chip_id 0x8011,
   board_id 12). Establish picasso-specific identity values once the kernel boots far
   enough to read them.

---

## Reference

- UniXNU: https://github.com/UniXNU/UniXNU
- picasso UEFI status: `Picasso-UEFI.md` in the firmware checkout
