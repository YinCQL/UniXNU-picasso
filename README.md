# UniXNU on Redmi K30 5G (picasso, SM7250)

> **Status: Stage 2 complete.** UFS physical root filesystem, Adreno 620 GPU Metal stack,
> Qualcomm QUPv3 GENI UART, dual-output framebuffer console mirroring, and no-teardown
> USB panic extraction are implemented and packaged.

## Progress

- **UEFI firmware: working.** Boots to the UniXNU boot menu. UFS Mass Storage, GOP
  framebuffer, button input and stable boot all work (`Mu-picasso-unixnu-0.img`).
- **XNU kernel: implemented and prelinked.** Compiled monolithic 64-bit ARM Mach-O kernel
  prelinked with 16 Apple kernel extensions (APFS, AppleMobileFileIntegrity, AppleImage4,
  IOStorageFamily) and custom platform drivers.
- **Storage: Route B physical UFS 2.1 root.** Native UFS block driver (`NativeUfs`)
  interfacing with SM7250 UFS host controller, mapped onto LUN 0 `userdata` partition
  (starting at LBA `0x200000`). Pre-formatted APFS rootfs and SystemOS volumes generated.
- **Graphics: Adreno 620 Metal stack.** Kernel driver (`NativeAdreno`) manages CX/GX power
  domains, GMU firmware, SMMUv2 address space, and PM4 ringbuffers. Userland Metal runtime
  bundle (`NativeAdrenoMetal.bundle`) paired with `adrenoprobed` and signed into static
  Apple Device Tree TrustCache.
- **Console & Non-invasive Debugging:**
  - Dual-output mirroring: `pe_kprintf.c` renders every character to LCD via `vcputc(c)`
    simultaneously with hardware GENI UART (`0x988000`).
  - Warm Boot PRAM panic log: 1 MiB physical DRAM reservation at `0x8FE00000` (`/pram`).
  - USB extraction tool: `scripts/dump-picasso-panic.py` parses Apple embedded panic headers
    (`0x46554E4B`) across warm reboots over USB Fastboot, ADB, or EDL.

## Quick Start

### 1. Build Payload and Kernel

```sh
# From the UniXNU root
./unixnu defconfig picasso_platform_defconfig -D devices/picasso
./unixnu fetch
./unixnu build kernel payload
```

### 2. Flash and Boot

```sh
# 1. Flash UFS root filesystem to userdata partition
fastboot flash userdata out/picasso/userdata/userdata-root.img

# 2. Boot composite payload
fastboot boot out/picasso/payload/payload.img
```

### 3. Non-invasive Crash / dmesg Extraction

If the kernel halts or reboots into fastboot:

```sh
python3 devices/picasso/scripts/dump-picasso-panic.py --fastboot
```

Or when booted into recovery/TWRP:

```sh
python3 devices/picasso/scripts/dump-picasso-panic.py --adb
```

## Repository Structure

- `board.toml`: Board definition, UFS GPT partition mapping, memory geometry, and boot arguments.
- `configs/picasso_platform_defconfig`: Kernel configuration enabling NativeUfs and NativeAdreno.
- `scripts/make-picasso-adt.py`: Apple Device Tree generator with GICv3, UART, PRAM, UFS, and Adreno nodes.
- `scripts/build-arm-driver.py`: Driver compilation recipe matching XNU kernel ABI.
- `scripts/dump-picasso-panic.py`: Host-side panic buffer and dmesg parser over USB.

## Reference

- UniXNU: https://github.com/UniXNU/UniXNU
