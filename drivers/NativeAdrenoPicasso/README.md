# NativeAdrenoPicasso

Adreno 620 (Qualcomm SM7250) kernel-side bring-up for the Redmi K30 5G
("picasso"). This directory adds platform inputs, firmware container
validation and a staged probe. It does not modify `drivers/NativeAdreno` and
does not contain vendor blobs.

The existing `drivers/NativeAdreno` driver is the A740 origin. This port is
scoped to what can be proven in this repository: the device-tree values, the
firmware formats, the probe state machine and its host tests. Stages 1 and 2
have run on the device (see [Hardware results](#hardware-results)); GMU cold
start, the GPU SMMU context, CP execution and compositing still need the
integration hooks and the userland stack listed below.

## Hardware results

On 2026-11-02, `sheng-gpu=2` on serial `52cc791d` (booted with
`fastboot boot`, never flashed):

```
PICASSO_GPU start requested=2 chip_id=06020001
PICASSO_GPU clocks: mem_clk has no recorded CBCR; hardware vote required
PICASSO_GPU clocks: cx_gdscr=f8222000 cx_status=80001fe0
PICASSO_GPU identity: GX is not powered; GPU register reads skipped
PICASSO_GPU identity: chip_id=06020001 (DT) catalog=06020100 cx=f8222000/80001fe0 gx=00222001/00088400
PICASSO_GPU smmu: idr0=4c017e05 idr1=20000005 idr2=00005511 scr0=00210406 gfsr=00000000 banks=5 s2=0
PICASSO_GPU smmu: attach hook absent; plan published, nothing programmed
PICASSO_GPU abort: stage=2 retained for inspection
PICASSO_GPU end stage=2 step=2 error=-21 requested=2 ticks=2 done=2
```

CX was already powered by the platform path (status `0x80001fe0`); GX stayed
off, so the probe skipped GPU register reads. The SMMU answered with five
context banks and no secure banks, matching the planned first-bring-up banks.
Stage 2 stops on the absent `smmu_attach` hook as designed; the boot continues
to SpringBoard.

The kernel build links this probe when `CONFIG_XNU_ADRENO620_PROBE=1`
(`board.toml` runs `build-arm-driver.py AdrenoPicasso`, and
`scripts/build-xnu-4k-baseline.sh` force-loads `libAdrenoPicasso.a`). The boot
monitor calls `native_adreno620_set_embedded_firmware()` and then
`native_adreno_probe()`; both are weak, so a build without this archive keeps
the old path. `Adreno620Hooks.cpp` supplies weak "not implemented" definitions
for the seven integration hooks because a static kernel link cannot leave them
undefined; each returns `ProbeHookAbsent` (-78), which the probe treats exactly
like a null import, and a real implementation linked from another archive
overrides it.

The vendor firmware is embedded at build time, never committed:
`scripts/build-adreno-picasso-firmware.py` reads `ADRENO620_FW_DIR`
(default `/tmp/picasso-gpu-fw`) and generates
`build/adreno-picasso/adreno620_embedded_firmware.c` (seven files, including
both SQE candidates). Missing files become empty arrays, so stage 3 reports
"blobs were not staged" rather than failing at link time.

## Files

| File | Role |
| --- | --- |
| `Adreno620Board.h/.cpp` | Every platform input with a DTS or upstream source: windows, interrupts, clock ids, GDSCs, OPP bins, interconnect, SMMU streams/banks, firmware names and load addresses, chip id. Pure parsing and clock math only. |
| `Adreno620Firmware.h/.c` | CRC32 and validated parsers for the `.mdt`/`.bNN` (ZAP), SQE microcode and GMU TLV containers. No allocation, no I/O, output unchanged on failure. |
| `Adreno620ProbeCore.h/.cpp` | Tick-driven stage machine. One tick runs at most one stage, never loops or sleeps, and reports a negative diagnostic code on the first failure. |
| `Adreno620Probe.h/.cpp` | Kernel adapter: `sheng-gpu=<n>` entry, real window mapping, CX power and clock branches, identity readback, SMMU ID readback, firmware checks, weak integration hooks, probe globals. |
| `Test/` | Host tests (`make test`) with synthetic containers and a fake transport, plus optional validation of the real vendor files. |

## Platform inputs derived from `/tmp/picasso.dts`

The line numbers refer to the decompiled stock tree. `parseReg` and
`parseIommus` mirror `NativeAdrenoBoard` so the same values can be read from
the live provider properties instead of the constants.

### MMIO windows

| Name | Base | Bytes | DTS |
| --- | --- | --- | --- |
| `kgsl_3d0_reg_memory` | `0x3d00000` | `0x40000` | `msm_gpu` reg[0], line 13514 |
| `cx_dbgc` | `0x3d61000` | `0x800` | `msm_gpu` reg[1] |
| `rscc` | `0x3de0000` | `0x1000` | `msm_gpu` reg[2] |
| `isense_cntl` | `0x3d8b000` | `0x2000` | `msm_gpu` reg[3] |
| `kgsl_gmu_reg` | `0x3d6a000` | `0x30000` | `gmu` reg[0], line 13917 |
| `kgsl_gmu_pdc_cfg` | `0xb290000` | `0x10000` | `gmu` reg[1] |
| `kgsl_gmu_pdc_seq` | `0xb490000` | `0x10000` | `gmu` reg[2] |
| `smmu_base` | `0x3da0000` | `0x10000` | `kgsl_smmu` reg[0], line 10672 |
| `smmu_tcu_base` | `0x3dc2000` | `0x20` | `kgsl_smmu` reg[1] |
| `gpucc` | `0x3d90000` | `0x9000` | `gpucc`, line 3479 |
| `gcc` | `0x100000` | `0x1f0000` | `gcc`, line 3427 |

### Interrupts

| Name | DTS cells | SPI | GIC INTID |
| --- | --- | --- | --- |
| `kgsl_3d0_irq` | `<0 0x12c 4>` (line 13516) | 300 | 332 |
| `kgsl_hfi_irq` | `<0 0x130 4>` (line 13919) | 304 | 336 |
| `kgsl_gmu_irq` | `<0 0x131 4>` | 305 | 337 |
| SMMU global | `<0 0x2a1 4>` | 673 | 705 |
| SMMU context banks | `<0 0x2a6 4>` … `<0 0x2ad 4>` | 678–685 | 710–717 |
| `gfx_0_tbu` / `gfx_1_tbu` | `<0 0x2af 4>` / `<0 0x2b0 4>` | 687 / 688 | 719 / 720 |

### Clocks (stock `qcom,gpucc-lito.h` and `qcom,gcc-lito.h`)

| Name | Controller | ID | CBCR offset used by the probe |
| --- | --- | --- | --- |
| `gpu_cc_ahb` | GPUCC | 0 | `0x1078` |
| `gmu_clk` (`GPU_CC_CX_GMU_CLK`) | GPUCC | 3 | `0x1098` |
| `cxo_clk` (`GPU_CC_CXO_CLK`) | GPUCC | 6 | `0x109c` |
| `smmu_vote` (`GPU_CC_HLOS1_VOTE_GPU_SMMU_CLK`) | GPUCC | 12 | `0x5000` |
| `mem_clk` (`GCC_DDRSS_GPU_AXI_CLK`) | GCC | 13 | none recorded |
| `mem_iface_clk` (`GCC_GPU_MEMNOC_GFX_CLK`) | GCC | 35 | `0x7100c` |
| `gcc_gpu_snoc_dvm_gfx` | GCC | 36 | `0x71018` |
| `gcc_gpu_cfg_ahb` | GCC | 31 | `0x71004` |

The CBCR offsets are the SM7250/lito ones already used by
`drivers/NativeAdreno/NativeAdrenoService.cpp`. `GCC_DDRSS_GPU_AXI_CLK` has no
recorded CBCR here, so the probe logs it as a clock-framework vote and does not
write a guessed register.

### Power, interconnect and fuses

| Input | Value | DTS |
| --- | --- | --- |
| `gpu_cx_gdsc` control / status | `0x3d9106c` / `0x3d91540`, timeout `0x1f4` (500 us) | line 10585 |
| `gpu_gx_gdsc` control / status | `0x3d9100c` / `0x3d91010` | line 10613 |
| CX parent | `pm8150_s1_level` (`VDD_CX_LEVEL`) | line 5036 |
| MX parent | `pm8150a_s4_level` (`VDD_MX_LEVEL`) | line 5520 |
| GX parent | `pm8150a_s6_level` (`VDD_GFX_LEVEL`) | line 5543 |
| GPU bus path | `<0x1a 0x200>`; 13 bandwidth cases `0 … 8,371,200` KB/s | line 13541 |
| GMU CNOC path | `<0x1a 0x2734>`; `0`, `100` KB/s | line 13924 |
| SMMU bus path | `<0x9b 0x200>`; `0`, `1000` KB/s | line 10687 |
| `speed_bin` fuse | `0x1ea`, bit 5, 8 bits | line 7349 |
| `gaming_bin` fuse | `0x212`, bit 0, 1 bit | line 7355 |
| `isense_slope` fuse | `0x45c8`, 4 bytes | line 7360 |

OPP bins (`qcom,gpu-pwrlevel-bins`): speed bin 0 has seven levels at 750, 670,
625, 500, 400, 275 and 0 MHz; `0x84` and `0xa5` have five (625…0); `0x73` has
four (540…0); `0x9e` repeats bin 0. The top-level `qcom,initial-pwrlevel` is 3;
each bin carries its own initial/CA-target level. The probe does not choose a
bin: the main session reads `speed_bin` and calls `pwrlevelBinForSpeed`.

### SMMU

`kgsl_smmu` is `qcom,qsmmu-v500` with `#iommu-cells = <2>`,
`qcom,skip-init`, `qcom,use-3-lvl-tables`, `qcom,no-dynamic-asid` and one global
interrupt. The second iommu cell is the SMR mask (`arm_smmu_of_xlate`), not a
context bank:

| Node | SID | Mask | Exact SIDs | Native bank plan | ASID |
| --- | --- | --- | --- | --- | --- |
| `gfx3d_user` | 0 | `0x401` | 0, 1, `0x400`, `0x401` | 0 | 1 |
| `gfx3d_secure` | 2 | `0x400` | 2, `0x402` | 1 (not in first bring-up) | 2 |
| `gmu_user` | 4 | `0x400` | 4, `0x404` | 2 (not in first bring-up) | 3 |
| `gmu_kernel` | 5 | `0x400` | 5, `0x405` | 3 | 4 |

The bank numbers are not in the DT: downstream allocates them dynamically and
queries `DOMAIN_ATTR_CONTEXT_BANK`. A native driver that owns the SMMU fixes
the plan above. `qcom,no-dynamic-asid` makes ASID = bank + 1 in downstream
`arm-smmu.c`, and `smmuAsidForBank()` states that rule. `qcom,gpu-offset` on
`gfx3d_user` is `0xa8000`; `qcom,actlr` is `{0x0, 0x3ff, 0x32b}` and
`{0x400, 0x3ff, 0x32b}`.

The native `kits/ArmSmmu4K` attaches exact SIDs only, so the probe expands each
mask to its exact SID set (`expandStreamMask`). A masked SMR would need a kit
extension that does not exist here.

### Identity

`msm_gpu` `qcom,chipid = <0x6020001>`, i.e. `0x06020001`. Downstream
`adreno.h` decodes that as core 6, major 2, minor 0, patch 1, revn 620, and
`adreno_is_a650_family()` includes A620 (A620 follows A650 register specs).
The pinned mainline catalog has no `0x06020001` entry; its A620/A621 line uses
`0x06020100` with `zapfw = "a620_zap.mbn"` and `ADRENO_6XX_GEN3`. A6xx has no
chip-ID MMIO register in either tree (downstream reads the DT property), so the
probe publishes both numbers and does not claim a register read proves the ID.

## Firmware findings

Extracted read-only from `/tmp/picasso-vendor/vendor.img` (`/firmware`). No
blob is copied into this repository. CRC32 is IEEE 802.3 over the whole file;
the per-container CRCs are over payloads.

| File | Bytes | CRC32 | SHA-256 |
| --- | --- | --- | --- |
| `a620_zap.mdt` | 6860 | `ae7b941c` | `f71618f03058f36e48390a14a55ad26dea452e40aa54f7d8a156891038ba6dfe` |
| `a620_zap.b00` | 148 | `4629d3f3` | `ec9b9d5a67456384809624b14a00d15d36297e5f2e75728504cd872bc95e0947` |
| `a620_zap.b01` | 6712 | `9220d541` | `b0ed369372c0dd233f6d2f13d264da69ca5cc06962822177696d9feaf28dc205` |
| `a620_zap.b02` | 1676 | `b439bc29` | `5a4de8ad881b57d64c3697b1078ce012fb47300d914a32c50522da762e2f2bd8` |
| `a620_zap.elf` | 13964 | `8cbed8d9` | `a6642a6d7b719f8dad6cb82e0b0ccf33b5865b27cfc1933c325dbbb6bcb5f115` |
| `a630_sqe.fw` | 32008 | `8b648790` | `a0989f6d2b2b371bfedbfafeef40a49293b7128fa74be9f2af25ab9f595997e0` |
| `a650_sqe.fw` | 31460 | `59c1d422` | `a7067444f8af6ce2e856e5b3de42b120a3bda3a215f828276035723b2e9f8d81` |
| `a650_gmu.bin` | 41308 | `c190814d` | `78e792d35b640bc8d23c89b204e728ee6632ed62fbff4b5f1cf42dd7285ef6c5` |

### ZAP

`a620_zap.mdt` is a 32-bit little-endian `EM_QCOM` (`0xa4`) ELF with three
program headers. `.b00`, `.b01` and `.b02` are the segment payloads in header
order; `.b01` is byte-identical to `mdt[0x94:]`, and `.elf` places the same
payloads at their `p_offset`:

| Segment | Type | Vaddr | Filesz | Memsz | CRC32 | Source |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 0 (metadata) | 0 | 148 | 0 | `4629d3f3` | `.b00` (ELF header + phdrs) |
| 1 | 0 (hash) | `0x6000` | 6712 | 8192 | `9220d541` | `.b01` |
| 2 | 1 (`PT_LOAD`) | `0x5000` | 1676 | 1676 | `b439bc29` | `.b02` (code) |

`e_entry = 0x5000`, loaded bytes = 1676. The hash segment's filesz is larger
than its memsz; that is legal because it is not a `PT_LOAD`. The ZAP is
authenticated and loaded by PIL/TrustZone, not by this kernel; the probe only
validates the container.

### SQE

Both files start with a 4-byte zero preamble. Linux `adreno_fw_create_bo`
copies `fw->data + 4`, `fw->size - 4`, so the payload is the microcode. The
first payload dword's low 12 bits are the version read by
`a6xx_ucode_check_version`:

| File | Preamble | Payload bytes | Payload CRC32 | Version | Mainline floor |
| --- | --- | --- | --- | --- | --- |
| `a630_sqe.fw` | 0 | 32004 | `444cfcc3` | `0x188` | `0x190` (too old) |
| `a650_sqe.fw` | 0 | 31456 | `728929d7` | `0x087` | `0x095` (too old) |

The vendor image ships both and `a650_gmu.bin`, but no A620-specific GMU/SQE
names. The stock kernel's firmware-name string order introduces only
`a620_zap` for A620 and reuses the already-listed `a630`/`a650` names, which is
the downstream pattern for A620 = `a630_sqe.fw` + `a650_gmu.bin`. The board
therefore defaults to that pair and exposes `a650_sqe.fw` as
`sqeAlternate()`; the probe accepts either pinned image and refuses anything
else. The pinned mainline catalog uses `a650_sqe.fw` + `a621_gmu.bin` for its
A620/A621 entry, and the vendor image has no `a621_gmu.bin`. This is an
integration decision that hardware must settle.

Both vendor SQE payloads are below the mainline security floors. The probe
reports the version and refuses to proceed unless
`sheng-gpu-allow-old-sqe=1` is passed.

### GMU

`a650_gmu.bin` is the `struct block_header {address,size,type,value}` TLV list
walked by `a6xx_gmu_fw_load`. It has 16 blocks: 9 metadata (`size == 0`, type 1
or 7) and 7 data blocks (type `0xdeadbeef`). Total payload 41052 bytes;
container image CRC `0b2afd6d`; entry 0 (CM3 vector table in ITCM).

| Block | Address | Size | Destination |
| --- | --- | --- | --- |
| 09 | `0x00000000` | `0xc0` | ITCM |
| 10 | `0x00000200` | `0x140` | ITCM |
| 11 | `0x00000400` | `0x36f8` | ITCM |
| 12 | `0x00004000` | `0x54f4` | icache |
| 13 | `0x00157000` | `0x398` | icache |
| 14 | `0x10004000` | `0xec0` | DTCM |
| 15 | `0x10007fdc` | `0x18` | DTCM |

The A620 is `ADRENO_6XX_GEN3` (A650 family), so `a6xx_gmu_memory_probe` gives:
ITCM `0..0x4000`, DTCM at `0x10004000`, icache at `0x4000` with
`SZ_16M - SZ_16K`, dummy `SZ_4K` at `0x60000000`, and 16 KiB log/HFI buffers in
the uncached range. The board's `GmuLayout` fixes log at `0x60001000` and HFI at
`0x60005000`. This differs from the A740 defaults in `NativeAdrenoGmuMemory`
(dummy 16 KiB, debug 28 KiB), so a GMU context for A620 must use the board
layout, not `NativeAdrenoGmuDefaultSizes()`.

## What the existing driver expects from a board

Read from `drivers/NativeAdreno` (the A740 origin):

- `NativeAdrenoBoard`: `parseReg` (big-endian cells, 1–2 address/size cells,
  ADT little-endian quirk, no zero/overflowing window) and `parseIommus`
  (phandle + specifier cells, first cell is the SID).
- `NativeAdrenoGpuBoard`: `reserve`, `bootGmu`, `programSmmu`, `quiesce`.
  `NativeAdrenoGpuConfig`: powered GPU MMIO, ring/fence cache, time and clock
  callbacks, explicit start timeout, ring/fence/SQE IOVAs and the A7xx power-up
  list IOVA.
- `NativeAdrenoGmuPlatform`: five windows in `ShengGmuWindows` order (core,
  RSCC, PDC, GPU, CX misc); an A6xx board must leave CX misc absent. Services
  need real `sha256`, `now_us` and `wait_us`.
- `NativeAdrenoGmuContext`: fixed GMU payload sizes/IOVAs, one context bank,
  ASID and stream slot as caller inputs, native-4K lower region.
- `ShengGmuConfig`: icache/dummy/debug/log/HFI buffers, chip id, firmware
  pointer/bytes and a poll budget.
- `NativeAdrenoFirmware`: exact byte length plus SHA-256 rules. The main driver
  validates SHA-256; this port adds CRC32 and structural validation.
- `NativeAdrenoCpInit`: 4 KiB-aligned `sqe_iova`, optional power-up list on
  A6xx, `is_a6xx = 1`, `sqe_base_programmed` when the base was already written.
- `NativeAdrenoService.cpp`: the SM7250 GPUCC/GCC CBCR and GDSC offsets, GMU
  window bases, `iommu-cells`/stream IDs, weak firmware images, optional
  command-db and the IRQ mask path.

## Probe behaviour

`native_adreno_probe()` parses `sheng-gpu=<n>` (1–5), configures the core and
starts a `picasso-gpu` kernel thread. The thread calls `tick()` at most eight
times, sleeping 20 ms between stages; one tick runs at most one stage. The boot
monitor is never blocked by a synchronous loop. Every failure records a
negative code, calls `abort` once and stops; nothing is retried.

| Stage | Real work in this directory | Needs |
| --- | --- | --- |
| 1 | Map GPUCC/GCC/GPU, power CX, enable the recorded clock branches, read GDSC/CBCR state; read `RBBM_STATUS`/`RBBM_STATUS3`/`VBIF_VERSION`/`CP_HW_FAULT` only when GX reports powered | CX rail/GDSC and clocks working |
| 2 | Map SMMU, read IDR0/1/2, SCR0 and GFSR, validate the bank plan, publish it | `native_adreno620_smmu_attach` hook |
| 3 | Check GMU/SQE/ZAP size and CRC, parse the GMU TLV and SQE/ZAP containers, enforce the SQE version floor | `native_adreno620_gmu_boot` and `native_adreno620_hfi_handshake` hooks |
| 4 | Nothing hardware-specific | `native_adreno620_cp_start`, `cp_submit`, `cp_wait` hooks |
| 5 | Nothing | `native_adreno620_draw` hook (NativeMetal/Turnip/IR3) |

Published globals: `native_adreno_stage_req`, `native_adreno_stage`,
`native_adreno_step`, `native_adreno_error`, `native_adreno_identity[8]`,
`native_adreno_identity_count`, `native_adreno_gx_powered`,
`native_adreno_smmu_id[3]`, `native_adreno_smmu_banks/sids`,
`native_adreno_fw_crc/bytes`, `native_adreno_gmu_blocks/bytes`,
`native_adreno_sqe_version/security`, `native_adreno_zap_segments`,
`native_adreno_fence`, `native_adreno_ticks` and `native_adreno_done`
(0 running, 1 finished, 2 failed). Each stage prints a `PICASSO_GPU …` line.

The weak hooks are the integration seam. The kernel does not link the probe's
stage 3 to `ShengGmuColdStart`, because the only implementation in this tree is
`NativeAdrenoGmuStub.c`, which returns success without touching hardware. The
probe refuses to call it.

## Host tests

```sh
cd drivers/NativeAdrenoPicasso/Test
make test
```

`make test` builds with the system `cc`/`c++` (`-Wall -Wextra -Werror`) and
runs. It covers:

- CRC32 vectors and descriptor checks.
- Split `.mdt`/`.bNN` and combined `.elf` parsing, segment table, entry, loaded
  bytes and CRC; magic/machine/truncation/entry/memsz/short-segment refusals.
- SQE preamble, payload, version and security flag, including the vendor
  `0x188`/`0x087` values.
- GMU TLV parsing, destination classification, data-block overlap, unmapped
  address, bad size, truncation and missing CM3 vector table.
- DT parsing (BE, ADT little-endian 64-bit quirk, iommu SID + mask), mask
  expansion to exact SIDs and the board tables.
- RCG divider math, counter tick math, OPP bins and interconnect levels.
- Probe success (stage order and call counts), requested-stage gating, every
  failure code, abort-once, no retry and bounded tick count.

Real vendor validation (not part of `make test` because the blobs are not in
the repository):

```sh
make build
ADRENO620_VENDOR_DIR=/tmp/picasso-gpu-fw ./adreno620_tests
```

Observed: all eight vendor files match the board sizes and CRC32, the ZAP
segment table and combined ELF agree (`entry=0x5000`, loaded 1676 bytes), the
SQE versions are `0x188`/`0x087` (both below their floors), and the GMU
container is 16 blocks / 41052 payload bytes with image CRC `0b2afd6d`.

Kernel syntax check (recorded XNU flags, no build):

```sh
# Uses out/picasso/tree/build/xnu/ios-obj/.../IOPlatformExpert.cpo.json,
# substitutes Adreno620Probe.cpp, drops -o/-c/-W and adds -fsyntax-only.
# Observed: EXIT 0, no warning from this directory.
```

## Integration patch list (for the main session)

Items 1, 5 (build-time embedding instead of a ramdisk) and 6 (the boot-monitor
call) are implemented; the remaining items are open. The rest of this list is
unchanged from the port's original handoff.

1. **Build the new sources.** Add to the NativeAdreno archive (or a new
   `build-adreno-picasso` target) and the include path:
   - `-I drivers/NativeAdrenoPicasso`
   - `Adreno620Board.cpp`, `Adreno620Firmware.c`,
     `Adreno620ProbeCore.cpp`, `Adreno620Probe.cpp`.
   The probe core and board are C++17; the firmware parser is C11. The adapter
   uses the same XNU compile record as `scripts/build-arm-driver.py`.
2. **Bring over the missing kits** (absent from this worktree, present in the
   older upstream cache): `kits/AdrenoFirmware` and `kits/AdrenoPM4`.
   `drivers/NativeMetal` is also absent. `scripts/build-arm-driver.py` already
   lists the AdrenoFirmware sources and include paths; it skips missing files
   and falls back to the stub today.
3. **Do not link the stub behind the probe.** Keep `NativeAdrenoGmuStub.c` for
   the old path, or exclude it from the probe target. Implement
   `native_adreno620_gmu_boot()` against the real kit and
   `native_adreno620_hfi_handshake()` against `AdrenoHfiStartupRun`; the probe
   has already checked the image CRC and container before calling them.
4. **Provide the A620 GMU startup inputs.** The kit needs A620 performance
   levels/votes, CNOC/DDR command-db addresses and BCM data. The A740
   `ShengGmuSource.h`/`ShengGmuVotesSource.h` generated tables are not
   chip-correct for A620. These values cannot be derived from this DTS; they
   must come from the A620 catalog and the device command-db.
5. **Firmware staging.** Put the eight files on a ramdisk, e.g.
   `/usr/share/firmware/qcom/picasso/{a620_zap.mdt,a620_zap.b00,a620_zap.b01,
   a620_zap.b02,a630_sqe.fw,a650_gmu.bin}` (and optionally `a650_sqe.fw`,
   `a620_zap.elf`), then fill `native_adreno620_firmware` with the mapped
   pointers and call `native_adreno_set_firmware()` before
   `native_adreno_probe()`. The setter copies the pointer table, not the bytes;
   the buffers must stay mapped. Hashes are in `Adreno620Board.cpp`; the probe
   rejects a size or CRC mismatch.
6. **Call the entry point.** From the boot monitor, after the platform expert
   and physical mapping are available and before the GPU service claims the
   device, call `native_adreno_probe()`. It returns immediately. Use one
   `sheng-gpu=<n>` per boot:
   `1` CX/clocks/identity, `2` SMMU plan/attach, `3` GMU, `4` CP, `5` draw.
   Add `sheng-gpu-allow-old-sqe=1` only when accepting the vendor SQE versions
   is intentional.
7. **Implement the hooks** (all optional; a missing one stops its stage).
   The probe declares them with C linkage, so define them as
   `extern "C" int …` in a C++ translation unit:
   ```c
   int native_adreno620_smmu_attach(const adreno620::ProbeSmmuPlan *plan);
   int native_adreno620_gmu_boot(const uint8_t *gmu, size_t gmu_bytes,
       const uint8_t *sqe, size_t sqe_bytes);
   int native_adreno620_hfi_handshake(void);
   int native_adreno620_cp_start(void);
   int native_adreno620_cp_submit(uint32_t *fence);
   int native_adreno620_cp_wait(uint32_t fence);
   int native_adreno620_draw(void);
   ```
   The SMMU hook can use `NativeAdrenoGmuContextBringup` for the GMU bank and
   `NativeAdrenoSmmuContext` for the GPU bank. The CP hooks need the GPU SMMU
   context that `NativeAdrenoService.cpp` still refuses to build.
8. **Existing files that need edits (not made here):**
   - `scripts/build-arm-driver.py`: source/include lists or a new target.
   - `drivers/NativeAdreno/NativeAdrenoGmuMemory.cpp`: the A740 size defaults
     (`dummy 0x4000`, `debug 0x7000`) do not match A620; use the board layout.
   - `drivers/NativeAdreno/NativeAdrenoGpu.cpp`/`NativeAdrenoCpInit`: the
     `NativeAdrenoGpu` path always builds an A7xx CP init; A620 needs
     `is_a6xx = 1` and no power-up list, as `NativeAdrenoService.cpp` already
     does in its manual path.
   - `board.toml`: add `sheng-gpu=<n>` when the integration is ready; the
     current `sheng-gpu-probe`, `sheng-gpu-cx-power` and `sheng-gpu-gcc-probe`
     are read-only experiments.
   - `Info.plist`/personality only if the provider-based path is used; the
     probe maps physical windows directly and needs no personality.
9. **Publish the device only after stage 4.** Stages 1–3 do not authorize a
   Metal device or user client. `native_adreno_done == 1` means the requested
   stage completed, not that the GPU can render.

## Requires hardware or the userland stack

- GMU cold start and HFI: the AdrenoFirmware kit, A620 vote/startup tables and
  the device command-db are not in this worktree. The linked stub must not be
  trusted.
- ZAP authentication/loading through PIL/TrustZone.
- SMMU global setup/quirks, TBUs and secure contexts (`gfx3d_secure`,
  `gmu_user`); the probe only plans the first bring-up banks.
- GX power and GPU register reads; stage 1 deliberately skips them until the
  GX status reports powered.
- CP ring, SQE execution and the no-op IB fence: the GPU SMMU context is still
  being built by the main session.
- NativeMetal, metal2vulkan, Turnip/NIR/IR3, IOSurface handling and
  compositing into `NativeMobileDisplay`: not in this repository.
- Any statement about hardware success.

## Open questions

- A620 SQE/GMU pair: vendor `a630_sqe.fw` + `a650_gmu.bin` versus mainline
  `a650_sqe.fw` + `a621_gmu.bin`. The probe accepts the pinned files it knows
  and refuses everything else.
- Both vendor SQE images predate the mainline security floors; hardware may
  still accept them, but the probe refuses by default.
- Chip id encoding differs between the DT (`0x06020001`) and the pinned
  mainline catalog (`0x06020100`); no A6xx MMIO chip-ID register exists.
- Context bank numbers are dynamic downstream; the fixed plan is a native-driver
  choice that needs hardware confirmation.
- The GX GDSC status offset (`0x1010`) and CX status offset (`0x1540`) come
  from the existing service, not the DT (the DT gives only the GDSCR
  addresses).
- `GCC_DDRSS_GPU_AXI_CLK` has no recorded CBCR; it remains a clock-framework
  vote rather than a direct write.
