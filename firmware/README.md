# Picasso board firmware

All inputs are pinned to the user-supplied
`picasso_images_V13.0.5.0.SGICNXM` factory package. This directory follows the
board-owned manifest/preparation approach in
[UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng/tree/c8f92b208068eeaa4cd2b0f592671f04ee70076e/firmware),
with Picasso's SM7250, Adreno 620 and WCN399x inputs.

| Directory | Inputs | Storage and use |
| --- | --- | --- |
| `novatek/` | NT36672C g7a cfg_0, 131,072 bytes | Tracked; generated into the existing touch firmware embedding path |
| `adreno620/` | GMU, two SQE files, ZAP metadata/segments and joined ELF, 134,804 bytes | Tracked; existing GPU embedding and PAS path |
| `bluetooth/` | `crbtfw32.tlv`, `crnv32.bin`, `crnv32u.bin`, 135,068 bytes | Tracked; staged into the existing media addon and read at `/private/var/ngpu/bluetooth-firmware` |
| `adsp/` | `NON-HLOS.bin` and `dspso.bin`, 246,067,200 bytes | Local, ignored factory images; manifests are tracked |

The twelve small binary files total 400,944 bytes (about 392 KiB). Each group
has a `manifest.json` containing exact file sizes, SHA-256 identities and
factory container provenance. The manifests also check the runtime GPU and
Bluetooth descriptor identities. No WCN7850 or SM8550 firmware is used.

## Build preparation

From the device repository, using Python 3.10 or newer:

```sh
python3 -B scripts/prepare-picasso-firmware.py --work out/picasso
export NOVATEK_TOUCH_FW="$PWD/out/picasso/novatek/firmware.bin"
export ADRENO620_FW_DIR="$PWD/out/picasso/gpu-firmware"
```

This verifies and stages touch, GPU and Bluetooth inputs from this checkout.
It performs no firmware download. Explicit local source/archive options remain
available; they must match the same pins. Existing outputs are checked before
reuse, and changed or incomplete inputs fail instead of being substituted.
Compose uses this entry point; the existing touch/GPU preparation scripts also
default to the bundled files.

## Reproduce extraction

Linux/WSL requires `debugfs` from `e2fsprogs` to extract GPU files from vendor.
For the supplied Windows package:

```sh
python3 -B scripts/extract-picasso-firmware.py \
  --stock-dir /mnt/d/App/picasso/picasso_images_V13.0.5.0.SGICNXM
```

The extractor verifies the complete selected factory containers, the pinned
vendor extent and every output before staging. It reads the original touch
bytes from boot, GPU files from sparse super/vendor and Bluetooth files from
BTFM's FAT16 filesystem. `--group` limits extraction and `--output` selects a
different destination. It does not run the factory flashing scripts.

For an optional ADSP inspection, after extracting the local large images:

```sh
python3 -B scripts/prepare-picasso-firmware.py --group adsp --work out/adsp-review
```

This checks FAT16, split pieces, Hexagon ELF layout and the existing PAS
carveout. It does not replace the runtime firmware loader: ADSP still reads
the phone's UFS LUN4 NON-HLOS partition through the existing PAS path.
`dspso.bin` is retained as a verified source image; it has no new runtime
loader. Full Wi-Fi firmware loading is not implemented by this layout.

## Qualification

```sh
python3 -B scripts/check-picasso-firmware-layout.py --output out/firmware-check
```

The previous GPU inputs carried the same MIUI version, vendor build date and
security patch level. GMU, both SQE files and ZAP executable segments match.
The factory ZAP metadata/signature differs, with a 2022-10-28 leaf certificate
instead of 2022-10-08. That difference establishes source identity, without
establishing a performance or functional improvement. On 2026-11-19 the factory
ZAP passed PAS authentication and reset. The resulting boot image restored
the existing Data and reached SpringBoard; the user confirmed normal touch
and speaker playback, with no grey screen or automatic restart during the
check. Long-term stability and controlled performance comparisons remain
unmeasured. The historical Bluetooth firmware-handshake timeout (`-60`) can
still occur; this layout does not claim to resolve it.

## Retained factory images and CI

Complete `NON-HLOS.bin`, `dspso.bin` and `BTFM.bin` from the same factory package
are retained in [picasso-factory-firmware-v13.0.5.0](https://github.com/YinCQL/UniXNU-picasso/releases/tag/picasso-factory-firmware-v13.0.5.0).
The attached `factory-firmware-manifest.json` fixes their sizes and SHA256 hashes.
Existing firmware and toolchain Releases remain available to other consumers.
Both build workflows stage small board firmware directly from this directory;
they do not fetch the old touch/GPU firmware Releases. ADSP runtime still uses
the existing UFS LUN4/PAS path.

Compose retains the original 20 GiB Data seed through the hash-pinned Release in
`configs/retained-data-seed.json`; it does not regenerate Data when Actions
artifacts expire. Linux requires `compose_run_id` for a successful Compose of
the same branch and commit, then imports its signed audio/GL/UI/Bluetooth media
and unchanged Data inputs. Both workflows build a 96 MiB combined payload to fit the larger Apple-clang
kernel and FAT reserve. Local builds retain their qualified 80 MiB payload;
the source board and Data recipe remain unchanged.
