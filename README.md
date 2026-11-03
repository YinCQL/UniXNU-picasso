# UniXNU on Redmi K30 5G (picasso, SM7250)

UniXNU brings the XNU kernel and Apple userland to the Redmi K30 5G.
Development has reached **Adreno 620 GPU integration**. CPU/GOP presentation,
authenticated UFS root volumes, sparse writable Data and Novatek touch are
implemented. GPU rendering and scanout handoff have reported device results
with the external Metal/Turnip addon used in [PR #13](https://github.com/YinCQL/UniXNU-picasso/pull/13).
The default build does not include that addon; GPU integration, startup time
and sustained UI stability remain active work.

The authoritative progress record is [porting.toml](porting.toml). It covers
all 13 porting areas, their source references, required inputs, limits and next
actions. Follow the [UniXNU record guide](https://unixnu.com/porting/records)
when changing an implementation or its configuration.

## Target and progress

| Area | Current scope |
| --- | --- |
| OS and kernel | `main` selects iPad7,1 / iPadOS 17.7.11 (`21H461`) with a Darwin 23 backend. `feat/ios16-gpu` is a separate iOS 16 target. |
| Firmware | Mu-Silicium UEFI, PSCI, GOP, boot menu and Android boot-image assembly. Firmware source: [uefi_picasso](https://github.com/YinCQL/uefi_picasso), branch `UniXNU`. |
| CPU and memory | Eight cores with 6 A55/E and 2 A76/P topology. The FD publishes the 6 GiB DRAM map; DarwinBoot clamps the requested 5 GiB contiguous window to available memory. Reserved regions remain excluded. |
| Storage | Authenticated OS/SystemOS over NativeUfs, APFS root and sparse RAM Data. `sheng-data-persist=2` requests a UFS overlay; reboot persistence still needs a write/reboot/readback result. |
| Display and input | Original Legacy framebuffer ABI, CPU fallback, GOP handoff and NT36672C touch over GENI SPI. Panel initialization and timing remain firmware-owned. |
| GPU | A620 platform/GMU/CP, firmware validation, VM/submission/fences and presenter integration. PR #13 reports rendering through Metal translation and Turnip; matching external firmware, addon and trust inputs are required to reproduce it. |
| Network, audio and power | WLAN platform/protocol foundations exist; association and packet transfer are unfinished. Audio and full battery/suspend/resume support remain planned. |

Historical PR #13 measurements describe about 4.95 GiB managed memory and GPU
scanout with CPU copies frozen after warmup. They refer to experimental images
and external plugin builds, and do not certify every image built from `main`.
The record keeps these conditions beside the implementation evidence.

## Build

Use a macOS host with Xcode's iPhoneOS SDK, Python 3.12+, LLVM/lld, CMake,
Ninja, mtools and dtc. [board.toml](board.toml) declares the IPSW, prelink ABI,
archive order, firmware paths and payload inputs. [upstream.rev](upstream.rev)
pins the legacy upstream tree; this repository has not migrated to format 2.

```sh
python3 ./unixnu --prepare
python3 ./unixnu build kernel prelink
```

The complete `Compose Boot Image (FD + kernel + payload)` workflow fetches the
IPSW, prepares signed root inputs, trust caches and the Data seed, then builds
and packages the image. Its FD input is [uefi/SILICIUM_UEFI.fd](uefi/SILICIUM_UEFI.fd);
see [uefi/README.md](uefi/README.md) for firmware build and replacement steps.
Download the workflow artifact and use `fastboot boot boot.img` for a temporary
device test.

GPU addon builds additionally require matching A620 firmware, the published
Metal/Turnip addon and its owner trust bundle. The default Compose workflow
does not supply these. `scripts/build-wf-payload.sh` is an experimental helper
that reuses an existing kernel and has developer-specific paths; review its
inputs before use.

The board's default kernel command line is **968 bytes**, below the payload
builder's 1000-byte limit. Additional experimental arguments consume the same
budget; count the final joined command line after applying overrides.

## Diagnostics and record checks

On Windows, run `capture-logs.cmd` to detect the USB CDC console and save logs.
On macOS, use `scripts/capture-picasso-usb-console.py`. Record the device,
firmware, image hashes and source commit with each test. The selected USB
watchdog and recovery arguments remain in the default command line.

From a current UniXNU checkout, validate the record and committed changes:

```sh
python3 scripts/porting-record.py check --device ../UniXNU-picasso
python3 scripts/porting-record.py check --device ../UniXNU-picasso --base origin/main
```

Run the second command on a proposed branch before merging. This repository's
`Porting record` CI check applies the same ownership and evidence rules.

## Credits

- [silenzio111](https://github.com/silenzio111)

## References

- [UniXNU](https://github.com/UniXNU/UniXNU)
- [Porting and driver adaptation](https://unixnu.com/porting/drivers)
- [GPU adaptation](https://unixnu.com/porting/gpu)
