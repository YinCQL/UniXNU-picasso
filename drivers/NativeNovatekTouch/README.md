# NativeNovatekTouch (picasso, NT36672C)

Touch driver for the Redmi K30 5G ("picasso", Qualcomm SM7250) Novatek NT36672C controller on GENI SPI SE7. This is a
new driver; nothing here has run on the device yet. The pure layers are host-tested, the board/probe layer has only
been syntax-checked against the XNU headers.

## Files

| File | Contents |
| --- | --- |
| `TouchBoard.h` | Every picasso address, clock, pin and panel constant with its source; pure clock-divider helpers; core-to-HID coordinate mapping; the 18x36 compile-time check. |
| `NvtProtocol.c/.h` | NT36xxx SPI framing, trim-id identification, firmware partition parser, bootloader download, THP frame access. |
| `NvtTouchCore.c/.h` | 18x36 baseline, delta, peak labelling, blob merge, palm rejection, centroid, edge compensation and tracking. No allocation; scratch lives in the caller's `struct nvt_touch_core`. |
| `TouchPublish.h` | The seqlock publication helper used by the probe and the host test. |
| `TouchProbe.cpp` | Staged `sheng-touch=<n>` probe and frame loop. Provides `native_novatek_touch_probe()` and the HID publication globals/sink. |
| `GeniSpi.c/.h` | Vendored from UniXNU/NativeGeniSpi (BSD-2-Clause-Patent, SPDX headers unchanged). |
| `Test/` | Host tests and `make test`. |

`NvtProtocol` and `NvtTouchCore` are Apache-2.0 (adapted from ShengNovatekTouch); the new files are
BSD-2-Clause-Patent. See `NOTICE`.

## Host tests

```
$ cd drivers/NativeNovatekTouch/Test && make test
cc -I.. -std=c11 -O2 -Wall -Wextra -Werror -o nvt_tests main.c ../NvtProtocol.c ../NvtTouchCore.c -lm
./nvt_tests
PASS: NT36672C board/clock/HID helpers, publication seqlock, SPI framing, trim-id, firmware parse and model download, frame header/matrix decode, two-blob tracking, thresholds, palm and edge rules; synthetic data, no hardware
```

The fake bus models the NT36xxx page-select framing (0x`ff`, page bytes, offset with bit 7 for writes, no dummy byte
on reads) and enough of the register file to check identification, the firmware parser, the full download handshake,
frame-header validation and matrix decode. The core tests feed synthetic 18x36 frames with two Gaussian blobs,
threshold and palm cases, sub-pixel movement and tracking continuity.

## Probe

`sheng-touch=<n>` selects the last stage:

| Stage | Work |
| --- | --- |
| 1 | Map GCC/TLMM, read the QUP branch states, GPLL0 mode and SE7 DFS levels; read the GENI identity if the clocks already run. |
| 2 | Vote GPLL0 and the wrapper/SE branches, wait for them, configure SPI pins 6..9 plus reset GPIO4 and IRQ GPIO5, read the GENI identity. |
| 3 | Reset pulse, GENI SPI init, DFS clock choice, chip identification (SPI mode sweep; `sheng-touch-mode` forces one). |
| 4 | Parse and download the firmware, read FWINFO, then peek/read frames and publish contacts. |

Options: `sheng-touch-poll=1` (frame-counter polling instead of the GPIO IRQ), `sheng-touch-hz` (default and maximum
5 MHz, the DT value), `sheng-touch-legacy-start=1` (send the NT36532E idle/doze commands, unconfirmed on this chip).
Stage 1/2/3 exit after their report; stage 4 loops.

The probe publishes through the existing globals (`native_touch_seq/count/xs/ys/ids`, 12 slots) and calls the sink
after every published frame, including an empty frame for a lift. `xs` is the 1080-pixel short axis mapped to
0..30480, `ys` the 2400-pixel long axis mapped to 0..20320, which is what `NativeHidInput.cpp` already normalises.
`TB_HID_MIRROR_X/Y` are the orientation switches to try if the panel is mounted rotated.

## What is confident, and what is a TODO

Confident (public NT36xxx driver and this board's DT):

- SPI framing: page select `0xff, addr[22:15], addr[14:7]`, then the low 7 address bits (bit 7 set for writes) and
  data; reads have no dummy byte.
- Trim id read at `0x1F64E` (six id bytes at +1), NT36672C entry `{0x0c, x, x, 0x72, 0x66, 0x03}` with mask
  `{1,0,0,1,1,1}`, selecting the NT36675 map, `hw_crc = 2`.
- Map: event buffer `0x22D00`, boot-ready `0x3F10D`, software reset `0x3F0FE`, fast-read `0x3F310`, ILM/DLM
  descriptor/length/checksum and BLD-CRC registers.
- Download: `0x69` reset, ~35 ms, clear fast-read, write all non-empty partitions (`size + 1` bytes), program
  descriptors/lengths/checksums, set BLD CRC enable bit 7, clear the reset event, host command `0xae`, boot-ready 1,
  10 ms, poll byte 1 of the reset event for `0xa0..0xaf`.
- Firmware header: image end at +0, ILM/DLM descriptors at 0/12, golden checksums at 0x18/0x1C (the `hw_crc=2` case),
  info sections at 0x30, overlays after the DLM, `NVT`/`MOD` 4 KiB marker, `FW_VER + FW_VER_BAR == 0xff`. The
  stock-kernel objects carry a 0x2000-byte trailer after the image; `nvt_fw_parse_stock()` handles that and a
  marker-anchored carve that starts 0x2000 early.
- Board addresses: SE `0x984000`, wrapper `0x9c0000`, SMMU SID `0x23`, GCC `0x100000`, clocks se7 `0x4b`/`0x57`/
  `0x58`, pins GPIO4 reset / GPIO5 IRQ / GPIO6-9 QUP, panel 1080x2400, 18x36.
- SM7250 GCC register offsets for the branch/vote/DFS registers (proposal-stage `gcc-sm7250` driver), GPLL0 600 MHz
  main / 300 MHz even.

TODO / unverified (all marked in source):

- The classic THP 32-byte header field offsets (counter +2, integrity +4/+12/+8/+16, type +24) and the matrix
  row/column order. The vendor THP test protocol is not public; the layout is the NT36532E 32-byte sub-header plus
  an 18x36 little-endian matrix. This must be checked against a captured frame. The DT's four key channels are not
  consumed either; if the classic frame appends key data or a checksum, the frame length will need extending.
- The numeric TLMM function selector for `qup11` is not in the DTS; 1 is assumed.
- Reset pulse and IRQ edge timing/polarity.
- Signal thresholds, startup frame count, palm node count and the 120 Hz filter rate (NT36532E values, partly
  rescaled for 648 nodes).
- GPLL0 mode register offset (0x0 assumed), and the `hw_crc = 2` "3 bytes" meaning.
- Legacy start commands (0x19 / doze delay 0x113718 / 0x01) are only sent on request.
- No DMA/SMMU path: GENI FIFO mode only. If the SE reports its FIFO interface disabled, the probe stops with
  `GENI_ERR_NOTSUP`; the integrator needs the SE DMA path with the apps-SMMU bypass stream 0x23 (constants are in
  `TouchBoard.h`).
- Nothing has been run on hardware. No hardware success is claimed.

## Firmware

`/tmp/picasso-vendor/vendor.img` (ext4, volume `vendor`) was read with a small read-only ext4 reader. It contains
`/firmware/NT36xxx_MP_Setting_Criteria_592D.csv` (37,928 bytes, SHA-256 `5a28c31f…cf65`), `5931.csv` (37,360 bytes,
SHA-256 `f15e16a9…9f16`) and `5933.csv` (34,888 bytes, SHA-256 `9ad693f8…eb22`), which confirm the 18x36 / 18+36
channel configuration, but it does **not** contain the firmware blobs. `/firmware_mnt` is empty and `/odm` is a
separate mount.

The blobs are embedded in the phone's stock kernel: `/tmp/picasso-vendor/stock-kernel.img` (50,456,588 bytes). The
seven kallsyms symbols `_fw_<name>_bin_bin` point at the **bare image headers**, and each object is 0x22000 bytes: a
0x20000-byte image followed by a 0x2000-byte vendor trailer (the next symbol/`_fw_end` is exactly +0x22000). The seven
139,264-byte candidates at `/tmp/picasso-vendor/blobs/candidate1.bin` .. `candidate7.bin` were carved backward from
each image's `NVT`/`MOD` end marker, so each is shifted **0x2000 early**: `candidate[0..0x2000)` is the *previous*
object's trailer and `candidate[0x2000..0x22000)` is the bare image. The true array starts at `candidate + 0x2000`;
its own trailer is the next candidate's first 0x2000 bytes (for candidate7, the bytes at image end in the kernel). The
earlier "slot start may be wrong" concern is confirmed: the slot starts are 0x2000 early. Symbol addresses
(vmlinux-to-elf base): fw01 `0xffffff8009f30410`, mp01 `0xffffff8009f52410`, fw02 `0xffffff8009f74410`, mp02
`0xffffff8009f96410`, fw03 `0xffffff8009fb8410`, mp03 `0xffffff8009fda410`, ts_fw `0xffffff8009ffc410`.

Bare image = `candidate + 0x2000` in every candidate. Names and decoded headers:

| candidate | kernel file name | role | fw_ver/bar | ILM (bin 0x100) | DLM (bin 0x18100) | golden CRC ILM / DLM |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | `novatek_nt36672c_g7a_fw01.bin` | firmware | 0x14 / 0xeb | 0x13a77 | 0x4197 | 0x58e87d07 / 0x8b987915 |
| 2 | `novatek_nt36672c_g7a_mp01.bin` | MP test | 0x14 / 0xeb | 0x1398f | 0x4197 | 0x8ffa31e2 / 0x534ff590 |
| 3 | `novatek_nt36672c_g7a_fw02.bin` | firmware | 0x07 / 0xf8 | 0x13a97 | 0x4197 | 0xc0a0f3ab / 0x41e3fa54 |
| 4 | `novatek_nt36672c_g7a_mp02.bin` | MP test | 0x07 / 0xf8 | 0x13653 | 0x4197 | 0x6ecf6721 / 0x993476d1 |
| 5 | `novatek_nt36672c_g7a_fw03.bin` | firmware | 0x0a / 0xf5 | 0x13a4f | 0x4197 | 0x547123e5 / 0x0a934c49 |
| 6 | `novatek_nt36672c_g7a_mp03.bin` | MP test | 0x0a / 0xf5 | 0x13707 | 0x4197 | 0xf546ae03 / 0xd244c0cc |
| 7 | `novatek_ts_fw.bin` | unknown (alternate/base?) | 0x01 / 0xfe | 0x1049f | 0x44df | 0x355e7ceb / 0x69f0842a |

All seven parse to: header end 0x100, 13 0x10-byte info slots at 0x30..0xF0 (the last is the Header), no cascade
header, no overlays, 15 partitions, DLM SRAM 0x27140, Header partition (bin 0, SRAM 0x27040, size 0xff), `write_size`
0x20000. The three `fw0N`/`mp0N` pairs share a version but have different ILM images; the MP ones are factory-test
firmware. `ts_fw` has a distinct version, ILM and DLM. For normal boot start with `fw01`, then try `fw02`/`fw03`; the
vendor driver chooses among them by TP/display/glass vendor, which this driver does not implement.

Trailer layout (the 0x2000 block **after** each image; opaque, never written to the controller):

```
+0x0000  u32 magic 0x425a6666 in all seven trailers
+0x0004  f32 200.0 (group A: fw01/mp01/fw02/mp02/ts_fw) or 171.2 (group B: fw03/mp03)
+0x0008  f32 17.5 or 6.227; +0x000c f32 5.71875
+0x001c  u32 10 (A) or 5 (B)
+0x0030  four u32: 0x922400 (A) or 0x922000 (B), 0, 0x18b5020, 0x5027b90
+0x0040  nine-byte records, then small tables at +0x0100/+0x01c0; remainder zero
```

Trailer SHA-256: group A `b2d9022716a9de9f…`, group B `6f5836611cfc55c3…`, ts_fw `e91047ae2ff4af45…`. The 0x2000 block
before candidate1 is not a trailer: it is data preceding the first object and begins with 0x19 fill.

Parsing/staging:

- `nvt_fw_parse_stock()` accepts a bare image directly, the true 0x22000 object (image + its own trailer) directly, the
  shifted 0x22000 carve via the known +0x2000 offset, or (fallback) a header found in the first 32 KiB. It never copies
  and reports the skipped leading bytes as `fw.source_offset`.
- Cleanest staging: pass `candidateN + 0x2000` with size 0x20000 (the bare image), or extract the true object
  (image + its own trailer). Passing the unmodified candidate also works and reports `skip=8192`.
- Supply it through the weak `native_novatek_touch_firmware[]`/`_size` symbols or `native_novatek_touch_set_firmware()`.
  Do **not** copy a blob into this repository; reference `/tmp` or stage it from the odm/userland path.
- Probe expectation for `fw01`: shifted carve `in=139264 skip=8192 size=131072 write=131072 ...`; bare image
  `in=131072 skip=0 size=131072 ...`; true object `in=139264 skip=0 size=139264 write=131072 ...`. In all cases the
  tail is `ver=14/eb parts=15 ilm=80503@100 dlm=16791@18100` (`fw02` ILM 80535, `fw03` ILM 80463, `ts_fw` ILM
  66719/DLM 17631).

Write-path sanity check (all seven): every partition's `bin_addr + size` is <= 0x1c5e0, inside the 0x20000 image. The
parser's 15 partitions include 12 zero-size info sections and the Header; `write_firmware()` skips size 0, so one
download attempt performs four SRAM writes: ILM in two <= 63 KiB chunks (e.g. 0x13a78 bytes), DLM in one
(0x4198/0x44e0), and the 0x100-byte Header at SRAM 0x27040. The code writes `size + 1` bytes per non-empty partition,
matching the vendor `nvt_write_firmware()` and remaining within the image.

CRC note: for NT36672C (`hw_crc = 2`) the words at 0x18/0x1C are the boot-ROM golden checksums and the vendor parser
reads them verbatim; they are not derived by the host. `crc_calc` is the NT36532E word-CRC diagnostic and is expected
to differ. Do not reject an image because `crc != crc_calc`.

SHA-256 (`candidateN` = the shifted 139,264-byte carve; `payloadN` = `candidateN[0x2000:]` = the bare 131,072-byte
image):

```
candidate1 6d709c87e719e13410d4262aadf1d0d7c7e0e00b2bc76b73bc45ea71a6c1f519  payload1 f62db7d12c372834e3d0d1ea95665a338a1c64ec955d45e4b91f921ba0690ce2
candidate2 327793ee80874822a05375f8c91051836a84c5c64c7e64e30936cc0ed6ff8e3b  payload2 a4fd4c21193216e8a4f74356aea23624b9de1fac9d6041e9dbe6e83e0a7e01ae
candidate3 bf03fa249b1a5088ae74a25877e1a749343787f8e8f197eff63e6556fe2896e2  payload3 97ac7e65abdcf206d8a6719a5bce0ace2d82382e67583128dcd9444b1bb7a954
candidate4 6b535713d964c338a3fb01a3bd9203d1e5b1d33436b35258c43e98d8f90442f0  payload4 40d4c7bca594a73268cf884e1c33e2390d1828decdf6ad76c8a392ac3b078374
candidate5 f6bc135ee00f92115b6c76a815f5758e8fd6adf28bc2c3f7d095dffa87a36817  payload5 b6a0a94837533fb6d6923c43295a7392ec859fb56d3da70ded38132e96d6cb62
candidate6 53e7970109428a0a3efd0208125885ff11d208915e091ff1c479511e2af2657e  payload6 dec8bc4a13b68763de77cb8cf67c54b71bc1045c01798e82d696739157c6c88c
candidate7 8299566dea99f4fd3a62f68263a5cebbe2f302b0a218ab6fbe69df6cf0e58be7  payload7 07c3f7318b2ecbc6e5313b482895d7b7a04f68144f8e8bc1ae1b1fcb6b17f3e9
```

## What the main session must wire up

1. Build entry: compile `GeniSpi.c`, `NvtProtocol.c`, `NvtTouchCore.c` (C, `gnu11`) and `TouchProbe.cpp` (C++, the
   XNU kext flags) for arm64 into the kernel link, with `drivers/NativeNovatekTouch` on the include path. No existing
   file was modified.
2. Boot monitor: call `native_novatek_touch_probe()` from the existing boot-monitor sequence (the same place
   `native_hid_input_probe()` is called, or later once userland owns the panel). It is inert without
   `sheng-touch=<n>`.
3. Firmware: link the chosen blob (the bare 0x20000 image is cleanest; the true 0x22000 object or the shifted carve
   also parse) as the two weak symbols above or call the setter before the probe. No blob is checked into this
   repository.
4. Symbols: `TouchProbe.cpp` defines the HID publication globals, a strong `native_touch_set_sink` and
   `native_novatek_touch_probe`. `package/HidInputProbe.cpp` currently also defines `native_touch_set_sink`
   strongly; exactly one strong definition may remain. The weak stubs in `PicassoBootProgressStubs.c` are overridden
   as intended.
5. IRQ: the probe registers a frame handler on TLMM GPIO5 and calls the weak `armcpu_spi_attach(TB_TLMM_SUMMARY_INTID,
   false, ...)` (INTID 240). If that helper is not linked, use `sheng-touch-poll=1`; the GIC SPI for the SE itself
   (354) is in `TouchBoard.h` but unused by the GPIO-driven loop.
6. Diagnostics: read `native_novatek_touch_stage/step/error`, `native_novatek_touch_frames/valid/contacts`,
   `native_novatek_touch_dfshz[8]`, `native_novatek_touch_id[6]` and `native_novatek_touch_irq_stats[3]` from the
   boot report. The probe prints `PICASSO_NVT ...` lines.

## Verification status

- Host tests: built and passed on macOS with the system `cc` (command and output above).
- `TouchProbe.cpp`: `clang -fsyntax-only` with the tree's recorded XNU arm64 kernel arguments, no errors.
- Kernel build, link, boot and hardware: not run. The driver is off by default.
