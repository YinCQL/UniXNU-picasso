# NativeWlan (picasso, Qualcomm SM7250 "lito")

Platform and transport foundation for the Redmi K30 5G ("picasso", SM7250) WLAN part. **This is not a working
Wi-Fi driver and does not claim to be one.** XNU has no 802.11 stack for Qualcomm CNSS/WCN hardware, and Apple's
IO80211 expects Apple/Broadcom parts. This tree contains the board facts, the firmware containers and the staged
probe that stop at the real blocker. Nothing here has run on hardware; the pure layers are host-tested and the
kernel-side files are syntax-checked against the tree's recorded XNU arm64 flags.

## Files

| File | Contents |
| --- | --- |
| `WlanBoard.h` / `WlanBoard.cpp` | Every picasso WLAN address, IRQ, SMMU stream, MSA region, regulator rail and SMP2P fact with its DTS source; regulator/voltage math, SMMU tuple decoding, MSA page math and transport classification. No IOKit. |
| `WlanFirmware.h` / `WlanFirmware.c` | Firmware inventory with observed sizes/SHA-256, ELF32/MBN parser (single file and split `.mdt`/`.b00...`, SHA-1 hash table), board-data container parser and file naming, WLFW/QMI handshake state machine, staged probe orchestration. Pure C. |
| `WlanProbe.cpp` | `sheng-wlan=<n>` probe: read-only icnss identity, SMMU/MSA plan, firmware parse + handshake plan, transport conclusion, final "no Apple 802.11 stack" stop. Publishes `native_wlan_*`. |
| `Test/` | Host tests and `make test`; optional read-only cross-check against a real Qualcomm MDT/ELF set. |

All new files are BSD-2-Clause-Patent. No existing file was modified. No vendor blob is checked in.

## Transport determination: integrated WCSS/SNOC, not PCIe, not SDIO

| Evidence | Observation |
| --- | --- |
| `picasso.dts` | There is **no** `pci@`/`pcie` node and no `pcie` compatible anywhere. The only WLAN platform node is `icnss: qcom,icnss@18800000` (`picasso.dts:7300-7323`). |
| `picasso.dts` | `sdhci@7c4000` (`:4907`) and `sdhci@8804000` (`:4949`) exist, but neither has a WLAN/SDIO child or WLAN supply. The only WLAN-adjacent SDIO/WCN node is Bluetooth (`qca,wcn3990`, `:621-635`), not WLAN. |
| `picasso.dts` | icnss has an 8 MiB `membase`, 12 level-high SPIs, an apps-SMMU stream (`0xc0`), an IPA IOVA window and a 2 MiB MSA fixed region — the integrated WCSS register/CE model. `reg-names = "membase", "smmu_iova_ipa"` (`:7302-7303`). |
| `picasso.dts` | The WLAN SMP2P entry (`qcom,smp2p-wlan-1-in`, entry-name `wlan`) sits under `qcom,smp2p-mpss` with `remote-pid = <1>` (`:10878-10917`), which indicates the WLFW service is reached over the modem subsystem. |
| stock kernel `4.19.113-perf-ga1b60ef832fb` | The icnss compatible table contains only `qcom,icnss`; there is no `qcom,cnss-*` PCIe compatible. It contains `drivers/soc/qcom/icnss_qmi.c`, `wlfw_*_send_sync_msg`, `icnss_ce_request_irq`/`icnss_get_ce_id`, and `wcss_msa0`. |
| V12/V13 `qca_cld3_wlan.ko` | SNOC host interface: `hif_snoc_*`, `pld_snoc_probe`, `"Fail to register snoc driver"`, `"Required callback functions are missing"`; **27** `hif_snoc` strings versus 3 `hif_pci`. BMI downloader: `bmi_download_firmware`, `athwlan.bin`. |
| V13 `/build.prop` | `ro.board.platform=lito`, `ro.soc.model=SM7250`, `ro.hardware.wlan.chip=39xx`, `ro.hardware.wlan.dbs=1`. |

Conclusion: the WLAN part is the integrated WCSS/SNOC path with copy engines, QMI/WLFW control and an IPA/WDI data
path. A PCIe link-training or SDIO-enumeration stage would be pure fiction on this board; stage 4 exists only to
record that conclusion.

## Board facts (all in `WlanBoard.h` with line sources)

- icnss `membase` `0x18800000` + `0x800000`, IPA IOVA window `0xb0000000` + `0x10000` (`picasso.dts:7302-7303`).
- apps-SMMU `0x15000000` + `0x100000`; `iommus = <0x18 0xc0 0x01>` → stream id `0xc0`, SMR mask `0x1`. IPA data
  stream `0x521` (`:7284-7291`). IOVA pool `0xa0000000` + `0x10000000`, fastmap (`:7307-7309`).
- MSA fixed region `0x8ba00000` + `0x200000` (phandle `0x99` = `pil_wlan_fw_mem`, `:450-454`; despite the label,
  icnss consumes it only as `qcom,wlan-msa-fixed-region`, so it is the MSA pool, not an AP-side PIL image region). The adjacent
  `modem_wlan_region@8c000000` (`+0xf800000`, `:485-489`) is the modem PIL region used by `pil_modem`, not a
  separate WLAN image.
- 12 interrupts, raw GIC SPIs: `0x84, 0x51, 0x19f, 0x1a0, 0x1a1, 0x56, 0x5e, 0x60, 0x52, 0x77, 0x78, 0x57`
  (`:7305`); all `IRQ_TYPE_LEVEL_HIGH`. The DT gives no interrupt names.
- Regulators (`:7310-7317` with the RPMH nodes):
  `vdd-cx-mx` = pm8150_l1 352000–952000 µV (no override), `vdd-1.8-xo` = pm8150_l14 1616000–1984000 µV,
  `vdd-1.3-rfa` = pm8150a_l2 1144000–1328000 µV, `vdd-3.3-ch0/ch1` = pm8150a_l10/l11 3000000–3312000 µV with the
  same range in `qcom,vdd-3.3-ch*-config`.
- Clocks/interconnect: the icnss DT node has **no** `clocks`, `clock-names`, `interconnects` or `power-domains`
  property. The vendor driver's `icnss_clk_init`/`"Clock %s being enabled"` strings show that a clock list existed
  in its DT fragment, but it is not resolvable from this dump. The probe therefore performs no GCC write rather
  than gating a live subsystem on a guess.
- SMP2P: entry `wlan`, remote pid 1; interrupt indices 0 = `qcom,smp2p-force-fatal-error`, 1 =
  `qcom,smp2p-early-crash-ind` (`:7320-7323`).
- IPA: `qcom,ipa@1e00000`, `ipa-base` + `gsi-base`, HW version `0x12`, `qcom,ipa-wdi2`, `qcom,ipa-wdi2_over_gsi`
  (`:7231-7248`); additional mapping `0x1ea0000` + `0x80000` for `ipa_smmu_wlan`.

## Firmware inventory

Present in the extracted artifacts (sizes/SHA-256 measured read-only; files are **not** in this repo):

| Artifact | Where | Size | SHA-256 |
| --- | --- | ---: | --- |
| `WCNSS_qcom_cfg.ini` (real) | V12 `vendor.img:/etc/wifi` | 16038 | `1741faf3…aea7` |
| `WCNSS_qcom_cfg.ini` (real) | V13 `super.raw:/etc/wifi` | 16744 | `fce09190…eccf` |
| `wigig.ini` | V12+V13 `/firmware/wigig` | 6650 | `7a416a31…0222` |
| `qca_cld3_wlan.ko` | V12 `vendor.img:/lib/modules` | 9998584 | `c6fd999f…e680` |
| `qca_cld3_wlan.ko` | V13 `super.raw:/lib/modules` | 10426184 | `31d1f1af…580f` |
| `cnss-daemon` | V13 `super.raw:/bin` | 95152 | `efce32d0…0bab` |

Both `qca_cld3_wlan.ko` files contain ext4 holes (about 168–172 KiB around logical block 1140). The sizes and
hashes above are for the **logical** file with holes read as zeros; hashing only the allocated extents gives a
different, shorter result. This matters if the module is ever hashed for policy.

`/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini` and `wlan_mac.bin` are **symlinks**, not blobs. V12 points to
`/vendor/etc/wifi/WCNSS_qcom_cfg.ini` and `/data/vendor/mac_addr/wlan_mac.bin`; V13 points to
`/vendor/etc/wifi/WCNSS_qcom_cfg.ini` and `/mnt/vendor/persist/wlan/wlan_mac.bin`. The real config is the
16,038/16,744-byte ini above; the MAC/NV target lives outside the vendor image (`/data` or persist) and was not
backed up.

Referenced by the stock kernel / vendor module / cnss-daemon but **not present in the artifacts** (they live on the
modem/firmware partition mounted at `/vendor/firmware_mnt`, which is empty in `vendor.img`): `athwlan.bin`,
`athsetup.bin`, `otp.bin`, `epping.bin`, `utf.bin`, `fakeboar.bin`, `bdwlan.%03x.b%02x` / `bdwlan.bin`,
`regdb.bin`, `modem.mdt`. The conventional WCSS image name `wlanmdsp.mbn` is **not confirmed** by either this DT
(no wlan PIL node) or the stock Image strings; treat it as expected-but-unverified.

The real WLAN config in V13 `/vendor/etc/wifi/WCNSS_qcom_cfg.ini` is 16,744 bytes (477 non-empty lines) and sets
the usual qcacld policy (`gEnableImps`, `gEnableBmps`, `BandCapability=0`, default `Intf0MacAddress`).

Important: `/tmp/picasso-vendor/blobs/candidate1..7.bin` are **Novatek touch firmware arrays** (they begin with the
Novatek wrapper magic `0x425a6666` and sit next to `.NVT` in the stock kernel). They are not WLAN board data and
must not be used as `bdwlan`.

## Containers and what the probe can validate

- **MBN / PIL.** `WlanFirmware.c` parses Qualcomm ELF32 (`EM_ARM` 40 or `EM_HEXAGON` 164), all program headers,
  single-file images and the split `.mdt` + one part per program header form. A signed image's header 0 is the
  hash segment (`PT_LOAD`, `p_filesz == 0`, `p_memsz == table size`); the table starts after the program headers
  and holds little-endian `{u32 length, 20-byte SHA-1}` entries (a trailing self-hash entry, if present, is
  accepted but not recomputed). The parser checks extents, overlap, hash-table shape and segment lengths; SHA-1
  digests are verifiable for single-file and split images. The probe refuses a malformed staged image.
- **Real-file proof.** `/firmware/ipa_fws.mdt` + `ipa_fws.b00..b04` + `ipa_fws.elf` from `vendor.img` is a real
  Qualcomm image (`EM_HEXAGON`, 5 segments, 21,676 payload bytes). The host test parses the `.mdt` header, checks
  every split part against the assembled `.elf` at each `p_offset`, and confirms the image is unsigned. This does
  not make the parser correct for every MBN, but it is a real container, not only synthetic bytes.
- **Board data (`bdwlan`).** Two outer forms are observed in Qualcomm trees: an opaque raw blob and an ELF board
  file (ath11k sniffs `0x7f 'E' 'L' 'F'` for QCA6390). The internal BDF layout is NDA-encumbered, so
  `wlan_bdf_parse()` validates only the outer container (bounded, word-oriented raw blob or a valid ELF). The
  4-byte raw-size rule is a conservative assumption; relax it if a real board file proves otherwise. File names
  follow the cnss-daemon fragments: `bdwlan.bin`, `bdwlan.b%02x`, `bdwlan.%03x`, `bdwlan.%03x.b%02x`.
  No real `bdwlan` file exists in the artifacts, so that parser has only synthetic coverage and is explicitly
  marked as container-only.

## The load handshake the stock kernel reveals

The stock kernel is the QMI variant of icnss (`drivers/soc/qcom/icnss_qmi.c`), not the older PIL-only driver. Its
strings give the following host-side sequence; this is evidence, not a captured trace, and the exact order of the
prelude messages must be confirmed against a boot log:

1. WLFW service discovery over SMP2P (`wlfw_new_server`, `"WLFW server arrive: node %u port %u"`,
   `"WLFW server registration failed"`). The `wlan` SMP2P entry is under the **modem** SMP2P block, so the service
   is hosted by MPSS, not by an independent AP-side wlan remoteproc.
2. Capability request/response — the response logs `chip_id`, `chip_family`, `board_id`, `soc_id`, `fw_version`,
   `fw_build_timestamp`, `fw_build_id`.
3. Config request, indication-register, INI (`wlfw_wlan_cfg_send_sync_msg`, `wlfw_ind_register_send_sync_msg`,
   `wlfw_ini_send_sync_msg`), plus an optional dynamic-feature mask.
4. `"Received FW Ready Indication"`.
5. MSA mem-info request (`wlfw_msa_mem_info_send_sync_msg`) with the translated fixed-region PA/size; the kernel
   logs `"MSA pa: %pa ... MSA Memory Size: 0x%x"`.
6. `wlfw_msa_ready_send_sync_msg`, then `"Received MSA Ready Indication"`.
7. `wlfw_wlan_mode_send_sync_msg` (mode on) and the 802.11 firmware handover.
8. Optional/auxiliary: `wlfw_rejuvenate_ack_send_sync_msg`, `wlfw_send_modem_shutdown_msg`,
   `wlfw_athdiag_read/write_send_sync_msg`.

The vendor qcacld module then brings up the MAC image over SNOC copy engines: `hif_snoc_*`, `bmi_download_firmware`,
`"Loading firmware file %s"`, `athwlan.bin` / `athsetup.bin` / `otp.bin` / `fakeboar.bin`. qcacld's BMI naming and
the cnss-daemon board-data names match the public qcacld-3.0 conventions.

What this means for the AP side: the WLAN subsystem image is **not** staged by a wlan PIL node in this DT. The
only PIL container near the WLAN is the modem's (`modem.mdt` into `modem_wlan_mem`), and the WLFW service lives on
that subsystem. Loading a standalone `wlanmdsp.mbn` from XNU would first require either a secure-monitor/PIL call
that this tree does not have, or a confirmed modem-side handoff. The probe therefore validates the MBN and
publishes the QMI plan, but the `load_image` callback deliberately returns `ENOTSUP` instead of pretending.

## Probe: `sheng-wlan=<n>`

| Stage | Work | Result on hardware (expected) |
| --- | --- | --- |
| 1 | Map the 8 MiB icnss block read-only, snapshot the first eight registers, validate the five regulator rails against the DT. No clock/interconnect writes (none are resolvable). | Read-only facts; no PMIC backend. |
| 2 | Map apps-SMMU, read IDR0/1/2, find the SMR entry for stream `0xc0`, compute the 512-page MSA plan, touch MSA page 0. No SMR/S2CR writes. | Read-only plan; slot may be absent before firmware boot. |
| 3 | Take a caller-supplied MBN (weak symbols or `native_wlan_set_firmware()`), parse and hash-check it, publish segments/payload, run the WLFW handshake plan. | Stops at `load_image` with `ENOTSUP`; publishes `native_wlan_handshake_*`. |
| 4 | Publish the transport conclusion (integrated SNOC/CE). | No PCIe/SDIO attach; explicitly `attach=none`. |
| 5 | Stop with the honest blocker: no Apple 802.11 stack / mac80211-style data path. | Returns `WLAN_ERR_NOTSUP`. |

Boot arguments: `sheng-wlan=1..5` selects the last stage; without it the probe is inert. Every stage is synchronous
and uses only bounded MMIO reads — there are no `IOSleep`/`IOPause` loops, no firmware-upload wait, and no thread.
The entry point `native_wlan_probe()` depends only on IOKit and its own helpers; a boot monitor should declare it
`weak_import` so the image links whether or not `NativeWlan` is included.

Published globals (C linkage): `native_wlan_stage_req/stage/step`, `native_wlan_error`,
`native_wlan_transport/attach`, `native_wlan_regs[8]`, `native_wlan_rails`, `native_wlan_smmu_idr[3]`,
`native_wlan_smmu_slot/smr/s2cr`, `native_wlan_sid`, `native_wlan_msa_base/size/pages/word`,
`native_wlan_fw_present/segments/hash/payload`, `native_wlan_handshake_step/error`. Log lines start with
`PICASSO_WLAN`.

## Host tests

```
$ cd drivers/NativeWlan/Test && make test
cc -I.. -std=c11 -O2 -Wall -Wextra -Werror -c main.c -o main.o
cc -I.. -std=c11 -O2 -Wall -Wextra -Werror -c ../WlanFirmware.c -o WlanFirmware.o
c++ -I.. -std=c++17 -O2 -Wall -Wextra -Werror -c ../WlanBoard.cpp -o WlanBoard.o
c++ -o wlan_tests main.o WlanFirmware.o WlanBoard.o
./wlan_tests
SKIP real MBN: pass a firmware base path as argv[1] (e.g. /tmp/wlan-probe/real/ipa_fws)
PASS: WLAN board math, IRQ/SMMU/MSA facts, transport classification, firmware inventory, ELF/MBN parse+hash+split,
BDF container/names, WLFW handshake and staged probe; synthetic data, no hardware
```

Real-file cross-check (read-only; the extracted fixture is outside the repo):

```
$ make test REAL=/tmp/wlan-probe/real/ipa_fws
./wlan_tests /tmp/wlan-probe/real/ipa_fws
PASS real MBN /tmp/wlan-probe/real/ipa_fws: segments=5 machine=164 payload=21676 bytes; split .b00.. map onto the
.elf image
PASS: ... (same synthetic suite)
```

What the tests prove: SHA-1 known answers; ELF32/MBN header, segment, overlap and hash-table validation; hash
mismatch/truncation/malformed-class/machine/overlap rejection; split `.mdt` assembly and per-part mapping; BDF
raw/ELF/naming rules including overflow; the five regulator rails' targets, ranges and clamping; SMMU tuple and SMR
encoding; MSA page arithmetic; transport classification; the complete WLFW request/indication order and failure at
each step; staged-probe gating and stop-on-error. They do **not** prove anything about the hardware.

The kernel-side files were syntax-checked with `clang -fsyntax-only` using the tree's recorded XNU arm64 compile
record (the same recipe `scripts/build-arm-driver.py` uses): `WlanFirmware.c`, `WlanBoard.cpp` and `WlanProbe.cpp`
produce no errors and no warnings attributable to the new files. No kernel image was built and no device command
was run.

## Integration steps for the main session

1. **Build.** Add `WlanFirmware.c` (C, `gnu11`), `WlanBoard.cpp` and `WlanProbe.cpp` (C++, the XNU kext flags) to
   the kernel link with `drivers/NativeWlan` on the include path. `WlanFirmware.c` and `WlanBoard.cpp` also build
   with the host compiler; `WlanProbe.cpp` is kernel-only. No existing file was changed.
2. **Entry point.** Call `native_wlan_probe()` from the boot-monitor sequence, ideally declared `weak_import`; it
   is inert without `sheng-wlan=<n>`. Publish/read the `native_wlan_*` globals from the boot report.
3. **Firmware staging.** Obtain `athwlan.bin` (and any `bdwlan.*`/`wlan_mdsp` image once confirmed) from the
   device's firmware partition at runtime (`athwlan.bin`, and a `wlanmdsp`/board file only once confirmed). Link it
   as `native_wlan_firmware[]`/`native_wlan_firmware_size` or call
   `native_wlan_set_firmware()` before the probe. Do not copy blobs into this repository.
4. **Control plane (not implemented).** Implement a GLINK/QMI client for the WLFW service (the message sequence is
   in `WlanFirmware.c`) and decide who loads the WLAN subsystem image: an SCM/PIL call from XNU or a confirmed
   modem-side handoff. Until one of those exists, stage 3 cannot pass its `ENOTSUP` load callback.
5. **Data path (not implemented).** CE ring allocation and doorbells in the icnss `membase`, the apps-SMMU
   mapping for stream `0xc0`, IPA/WDI over GSI (`ipa_smmu_wlan`, stream `0x521`), then the WLAN MAC firmware's
   HTT/WMI (or equivalent) interface.
6. **Apple side (not implemented).** Replace the IO80211 assumption: either an IO80211-compatible shim or a new
   networking stack. There is no mac80211 in XNU.

## Honest prerequisites for real Wi-Fi on this board

- **No IO80211 driver for Qualcomm CNSS/WCN exists.** IO80211 is Apple's closed 802.11 framework and its drivers
  are written for Apple/Broadcom parts. A compatible replacement (or a new stack) is a prerequisite, not a port.
- **Full 802.11 stack:** MLME, scan, auth/assoc, rate control, block-ack/aggregation, power save, channel switch,
  DFS, and the regulatory domain. mac80211 + cfg80211 + a supplicant would have to be replaced or reimplemented.
- **Firmware/control path:** confirmed ownership and loading of the WLAN subsystem image, WLFW QMI over
  GLINK/SMP2P, CE/BMI upload of `athwlan.bin`/board data, and the HTT/WMI data interface.
- **Calibration/identity:** board data (`bdwlan`) with the correct chip/board id, OTP/MAC handling and `regdb.bin`.
  Transmitting without the calibrated board file and regulatory database is both non-functional and potentially
  illegal; do not key a transmitter from this probe.
- **Coexistence/power:** WCN3990 Bluetooth shares the subsystem (BT is `qca,wcn3990` over SLIM/UART on this board);
  coexistence, PM and thermal limits need an owner.
- **Driver model:** an XNU kext (or DriverKit equivalent) with DMA/SMMU, interrupts and a user/kernel interface for
  the network stack; the 12 level-high SPIs and the IPA/WDI path are only the beginning.

## References used for the container claims

- Qualcomm qcacld-3.0 board-data naming (`bdwlan%d.b%02x`, `utfbd%d.b%02x`, `bdwlan%d.bin`):
  <https://git.keyedlimepie.org/voidanix/vendor-qcom-opensource-wlan-qcacld-3.0/src/commit/332f4afc058caca900b8b37f1570d42e8d570738/core/bmi/src/i_bmi.h>
- ath11k ELF board files and the raw-vs-ELF container split:
  <https://github.com/xanmod/linux/commit/616c16a4b26a9f136cf8a1ad0b8c4d03a72d4328>
- ath11k `board-2.bin` container format (magic + TLV entries):
  <https://raw.githubusercontent.com/qualcomm/qca-swiss-army-knife/master/tools/scripts/ath11k/ath11k-bdencoder>
- Librephone note that the internal BDF format is NDA-only:
  <https://codeberg.org/tct123/librephone/commit/f592d0d381760c52eec801318a161e369f3aaba6?files=docs/wireless.md>
- XNU `NativeUfs/SmmuTakeover.h` for the QSMMU-v500 IDR/SMR/S2CR offsets reused in the read-only plan.

## Verification status

- Host tests: built and passed on macOS with the system `cc`/`c++`, both with and without the real MDT fixture.
- Kernel-side syntax: clean against the recorded XNU arm64 flags (no errors; no new-file warnings).
- Kernel build, link, boot, firmware load and any RF transmission: **not run**. No hardware success is claimed.
