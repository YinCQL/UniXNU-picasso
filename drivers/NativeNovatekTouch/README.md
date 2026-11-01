# NativeNovatekTouch (picasso, NT36672C)

Touch driver for the Redmi K30 5G (SM7250), using GENI SPI SE7 and the panel's 65-byte reports with ten 6-byte contact records. The 18x36 matrix helpers are retained for diagnostics; the running contact path does not use them. See `NOTICE` for source provenance and licenses.

## Build and firmware

The ios16 board builds this archive after bootstrap compile records exist. The Compose workflow downloads the pinned stock `fw01` image for the captured `g7a_37_02_0a` panel and supplies it through `NOVATEK_TOUCH_FW`. Local builds can use `scripts/fetch-picasso-touch-firmware.py --source PATH --output out/picasso/novatek/firmware.bin`. The builder checks the fixed SHA256 before compilation. Firmware bytes are distributed as a Release asset and are not stored in Git. Without firmware, diagnostic stages 1-3 remain available; stage 4 stops before touching the controller.

## Probe

The probe is inert unless `sheng-touch=<1..4>` is supplied. In ios16 it starts once the owned HID event driver is ready, using its own kernel thread. A physical contact stops virtual startup input; idle physical polls do not overwrite synthetic gestures. A shared writer lock protects both producers.

| Stage | Work |
| --- | --- |
| 1 | Read GCC branches, SE7 DFS levels, pins and the GENI identity if clocks are running. |
| 2 | Vote clocks and configure reset GPIO4, IRQ GPIO5 and SPI GPIO6-9. |
| 3 | Reset, initialize SPI, choose the clock and identify the controller. |
| 4 | Download supplied firmware, validate FWINFO and publish contact reports. |

`sheng-touch-hz` is capped at 5 MHz. `sheng-touch-mode` forces an SPI mode; `sheng-touch-poll=1` uses polling. GPIO IRQ SPI240 has a 20 ms fallback. Stage 4 publishes into the existing 12-slot HID snapshot and calls the shared sink after closing the sequence and releasing the writer lock. Physical and virtual producers share that lock; a busy writer drops the competing frame.

SPI reads send the low address byte plus a leading dummy byte, then return data starting at receive offset 2. Writes set address bit 7. GPIO4/5 use the EAST tile, GPIO6-9 the WEST tile. The board helpers map the 1080x2400 panel to HID ranges 0..30480 and 0..20320.

## Verification

`Test/main.c` covers board clocks, HID mapping, publication including busy writers and lift frames, SPI framing, trim IDs, firmware parsing and simulated download, plus diagnostic matrix processing and tracking. `make test` runs these host models. They do not validate hardware register timing.

The PR author reported a successful firmware download, IRQ-driven contacts and HID event-driver attachment on picasso. This ios16 board selects stage 4 with authenticated firmware. The integrated driver still needs a new complete kernel build and device run.

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

## Integration

The baseline build links the archive and the boot monitor calls the staged probe. `TouchProbe.cpp` supplies the contact globals; `PicassoBootProgressStubs.c` owns the writer lock and shared sink, including when the virtual HID producer is disabled. The HID archive registers its notification callback there. Inspect `PICASSO_NVT` logs and `native_novatek_touch_stage/step/error`, frame/contact counters and IRQ statistics during qualification.
