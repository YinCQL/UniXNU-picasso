# Handoff — picasso UI frame path, big.LITTLE topology, GPU state (2026-11-02)

This document is for the next agent taking over the Redmi K30 5G (`picasso`, SM7250) UniXNU port.
It records what was measured on hardware, what was changed, and what is still open. Every claim is
tagged **[verified]** (with the evidence: a log file, command output, or source line) or
**[unverified]** / **[hypothesis]**.

## 0. TL;DR

- The device boots iPadOS 17.7.11 userland through Mu-Silicium UEFI + UniXNU XNU with
  `fastboot boot` only. Touch works; the UI is nearly unusable because everything is rendered on
  the CPU: only **8 full-frame scanout copies in ~5 minutes** were observed
  **[verified: /tmp/wf4-capture.log]**, and the user separately reported the home screen taking
  ~30 s to appear and touch "eventually responding" **[user report]**.
- The scanout copy is **not** memory-bandwidth bound. A first-frame benchmark measured 1 MB reads
  through the real IOSurface mapping at 19,479 ticks cold / 5,820 warm (19.2 MHz counter), 1 MB
  framebuffer stores at 5,781 ticks, while whole 10.4 MB copies alternate between ~15 ms and
  ~130 ms. The 130 ms frames are stalls (scheduling/preemption), not stores or reads.
  **[verified: /tmp/wf3-capture.log]**
- XNU had **no big.LITTLE awareness at all**: the kernel was built with `MAX_CPU_CLUSTERS=1` and
  without `__ARM_AMP__` (single-cluster `sched_clutch`), and `ArmCpu` registered every CPU with
  `ml_get_boot_cluster_type()` instead of its own type. **[verified: source + build config]**
- The device tree XNU parses is **built by the firmware, not by the boot image**: it is modelled
  `iPad7,1`, contains the eight picasso CPUs, and carries neither `cluster-type` nor
  `capacity-dmips-mhz`. Passing `--dtb` to `unixnu bootimg` does not change it.
  **[verified: /tmp/wf6-capture.log, /tmp/wf7-capture.log]**
- The **interrupt-stack panic in the diagnostic `boot_progress_irq` probe** was called a
  `current_thread()->machine.CpuDatap` race in this round; **that diagnosis was wrong**. wf9 proved
  it was a struct-layout mismatch in the prebuilt BootProgress object (`CpuDatap` read at 0x1d8,
  which is `machine.preemption_count`). Fixed and verified over 3 × 300 s — see §9.
  **[verified: /tmp/wf5-capture.log, /tmp/wf8-capture.log, out/wf9-runs/run{1,2,3}.log]**
- The two-cluster/AMP attempt is committed but the default build was returned to `sched_clutch`
  (no `__ARM_AMP__`) because the firmware DT cannot express E/P (see §6.3).

## 1. Environment and hard constraints

- Device: Redmi K30 5G (`picasso`), SM7250, serial `52cc791d`. Boot only with
  `fastboot boot <image>`; **never flash any partition**. **[verified: every session]**
- Kernel: prelinked iPadOS 17.7.11 kernelcache (`21H461`), `slide=-0x1000000`, booted with
  `-v debug=0x14e cpus=8 ... sheng-*` boot arguments (see the payload TOML).
- SSH/serial console: the CDC-ACM console on `/dev/cu.usbmodemPICASSO011`; the capture script must
  own that port (stop other captures first).
- The worktree is shared with other agents. Never `git reset`/`checkout`/`stash pop`; prefer WIP
  commits.

## 2. Build, package, and boot (all verified this session)

Kernel (the kernelcache path must be the **main checkout**, not the worktree — the worktree has no
`out/ipsw`):

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed \
python3 unixnu build kernel prelink
# artifact: out/picasso/kernel/kernel.macho (sha256 printed at the end)
```

Payload (base payload from `out/gmu14-20261102/payload.img`; drop `offline-legacy-bench=N` unless
benchmarking):

```sh
mkdir -p out/<tag>
cp out/gmu14-20261102/payload.img out/<tag>/payload.img
sed 's/ offline-legacy-bench=[0-9]//' out/wf2-20261102/unixnu.toml > out/<tag>/unixnu.toml
mcopy -o -i out/<tag>/payload.img out/picasso/kernel/kernel.macho ::/EFI/UNIXNU/PAYLOADS/kernel.macho
mcopy -o -i out/<tag>/payload.img out/<tag>/unixnu.toml ::/EFI/UNIXNU/unixnu.toml
```

Boot image:

```sh
python3 unixnu bootimg --uefi /Users/silenzio/Desktop/development/ios/Mu-Silicium \
  --edk2 /Users/silenzio/Desktop/development/ios/Mu-Silicium/Mu_Basecore \
  --fd uefi/SILICIUM_UEFI.fd --dtb uefi/mu-silicium/Resources/DTBs/picasso.dtb \
  --ramdisk out/<tag>/payload.img -o out/<tag>/picasso/boot/<tag>.img
```

Run + capture (start the capture immediately after `fastboot boot`; the CDC port appears when the
kernel console initializes):

```sh
fastboot boot out/<tag>/picasso/boot/<tag>.img
python3 scripts/capture-picasso-usb-raw.py --output /tmp/<tag>-capture.log \
  --wait 900 --duration 300 --fastboot-timeout 240 --fastboot-serial 52cc791d
# return to fastboot without touching the device:
python3 scripts/picasso-cdc-command.py --command reboot-fastboot \
  --fastboot-timeout 240 --fastboot-serial 52cc791d
```

Build pitfalls that cost time this session **[verified]**:

- `xcrun: error: SDK "iphoneos" cannot be located` unless `DEVELOPER_DIR` is exported.
- `UNIXNU_KERNELCACHE=.../out/ipsw/21H461/kernelcache.decompressed` must point at the main
  checkout (`/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/...`).
- Adding a new kernel patch requires updating `unixnu`'s `prepare()` (it copies the patch into the
  prepared tree and includes it in the fingerprint). The patch then applies with the other
  `patches/xnu/*.patch` files, ordered by filename (the port's convention is `zzz…-picasso-*`).
- Deleting `out/picasso/tree/build/xnu` forces a bootstrap pass. The bootstrap pass runs with
  `XNU_BOOT_PROGRESS=0`, and the build script's archive builders must be skipped in that pass
  (`scripts/build-xnu-4k-baseline.sh` was patched to do this; without it the HID/cpufreq/GPU
  builders fail looking for compile records).

## 3. Verified milestones before this round

- **CPU frequency (Goal A)**: the SM7250 OSM domains d1 (gold) and d2 (prime) are now driven past
  the unreliable `REG_ENABLE` bit; the LUT gate (`domainUsable`) is used instead. d1 reaches
  2,208,000 kHz and d2 2,400,000 kHz. **[verified: /tmp/cpf6-capture.log]**
  - `PICASSO_CPUFREQ d1 enable=0 raw=00000002 usable=1 ... best=7@2208000kHz rows=8`
  - `PICASSO_CPUFREQ d2 enable=0 raw=00000002 usable=1 ... best=7@2400000kHz rows=8`
  - The 0x9c0 cycle counter delta independently matches the decoded MHz.
  - PR: https://github.com/YinCQL/UniXNU-picasso/pull/12 — **state MERGED** **[verified via `gh`]**
- **GPU (Goal B)**: GMU cold start and HFI startup work end to end; the a650-family PDC path
  (`PDC_ENABLE=0x80000001`, no TCS programming) removed the ~122–128 s restartable-range panic that
  GPU-on boots used to hit. cpf6 ran 360 s with zero panics.
  **[verified: /tmp/cpf6-capture.log]**
  - `PICASSO_GPU gmu: cold start complete`
  - `PICASSO_GPU gmu: PDC enabled (AOP-owned TCS; mx=30010 cx=30000 xo=0 vrm=0)`
  - `PICASSO_GPU hfi: run status=0 stage=8 transport=0 fw=0 commands=4 polls=8 elapsed=4661 skipped=0`
  - `PICASSO_GPU hfi: core firmware started`
  - cpf6 boot args contained `sheng-gpu=3`; the current wf images use `sheng-gpu=0` (GPU stage off).
- **Touch**: the Novatek NT36672C driver reports contacts, and SpringBoard receives them.
  **[verified: /tmp/wf4-capture.log contains `PICASSO_NVT report contacts=1` / `PICASSO_NVT point id=1 x=… y=…`]**

## 4. What was measured this round

### 4.1 UI frame production is the bottleneck

In a 300 s wf4 boot (GPU stage off), `PICASSO_DISPLAY event=10` reported only **8 copies total**,
and no `PICASSO_DISPLAY_STATE` dump (it is emitted every 64 submissions) ever appeared, i.e.
fewer than 64 swap submissions in 5 minutes. Copy times:
129.7, 127.9, 128.7, 15.2, 16.4, 21.5, 138.2, 129.4 ms. **[verified: /tmp/wf4-capture.log]**

The user independently reported the home screen taking ~30 s to appear, and that touch eventually
responds. So input is fine; rendering/presentation is the bottleneck.

### 4.2 The copy is not bandwidth bound (bench2, wf3)

`offline-legacy-bench=2` (in `drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`) times, on the
first real swap, reads through the swap's own source mapping and through a second
`kIOMapCopybackCache` mapping of the same descriptor (two passes each), a cached control buffer,
and a store into the real GOP mapping. All times in cntpct ticks (19.2 MHz):
**[verified: /tmp/wf3-capture.log]**

| probe | ticks | ms | notes |
|---|---|---|---|
| src pass 0 / 1 (default map) | 19479 / 5820 | 1.01 / 0.30 | opts=0x1001 (default cache) |
| alt pass 0 / 1 (copyback map) | 36428 / 5914 | 1.90 / 0.31 | opts=0x1301 |
| ctrl store / read | 4385 / 3964 | 0.23 / 0.21 | cached buffer |
| fb store 1 MB | 5781 | 0.30 | write-combined destination |

A full 10.4 MB copy should therefore take ~5–15 ms, which matches the fast samples. The ~130 ms
samples are thread stalls. The same boot later panicked (see §6.1); whether bench2 itself
contributed is **[unverified]**.

### 4.3 big.LITTLE: XNU did not know about the big cores

- `pexpert/pexpert/arm64/board_config.h`, BCM2837 block: `MAX_CPU_CLUSTERS 1` and **no**
  `__ARM_AMP__`; `__ARM_AMP__` is only defined by the Apple H13/H14 headers. The compiled kernel's
  `sched_prim.o` references `_sched_clutch_dispatch` (not `sched_amp_dispatch`).
  **[verified: source and `nm`]**
- `drivers/ArmCpu/ArmCpu.cpp` registered every CPU with
  `info.cluster_type = ml_get_boot_cluster_type()` (both the boot and secondary paths), so all CPUs
  looked like the boot cluster.
- Observed load: little cluster busy 91–98 %, the single A76 gold 27–35 %, the A76 prime 31–36 %
  (governor's per-domain sample, `/tmp/wf4-capture.log` and `/tmp/wf3-capture.log`).

### 4.4 The device tree XNU parses is not the one passed to `bootimg`

The boot image embeds `uefi/mu-silicium/Resources/DTBs/picasso-topology.dtb` (sha256
`dd026351…`, verified by extracting the appended FDT from the gzip kernel blob and by
`boot.json`'s `dtb_sha256`). Nevertheless a boot with a diagnostic read of the live SecureDT
reported:
**[verified: /tmp/wf6-capture.log]**

```
PICASSO_CPUFREQ topo cpus=8 clusters=1 types=1 en=0 pn=0 cl-e=0 cl-p=0
PICASSO_CPUFREQ topo cpu=6 phys=600 cluster=0 type=0        (type 0 = CLUSTER_TYPE_SMP)
PICASSO_CPUFREQ topo dt cpu600-cluster-type missing
PICASSO_CPUFREQ topo dt model iPad7,1
PICASSO_CPUFREQ topo dt /cpus/cpu@0 capacity missing
PICASSO_CPUFREQ topo dt /cpus/cpu@600 capacity missing
```

The live DT also has **no `capacity-dmips-mhz`** (wf7 boot), so it is not the picasso DTB file
at all: it is a firmware-built tree containing the eight CPUs (`reg` 0x0…0x700) but none of the
Qualcomm topology properties. A capacity-based E/P fallback therefore cannot work against this
DT. **[verified: /tmp/wf7-capture.log]**

So the live DT is a picasso-derived tree (8 CPUs, `reg` 0x0…0x700) whose `/model` is
`iPad7,1` and that lacks the properties added to the boot-image DTB. The UEFI FD (`uefi/SILICIUM_UEFI.fd`) contains
no raw FDT (the picasso DTB is therefore embedded compressed, or the firmware builds the DT at
runtime) — **[hypothesis]**: the runtime DT comes from the FD/firmware, not from the boot image's
appended DTB. Rebuilding the FD with a modified `Resources/DTBs/picasso.dtb` was not attempted
(the local Mu build needs Python deps such as `coloredlogs` that are not installed).

## 5. Changes committed this round

Commits `3ce14fc`, `c1d2369` and `16d00ab` on `stage12/picasso-cpufreq-big` (remote `fork` =
`silenzio111/UniXNU-picasso`) include:

- `drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`: per-copy timing, bench=1 (boot) and
  bench=2 (first swap) benchmarks, sample window widened to 32 for event 10.
- `drivers/NativeMobileDisplay/PicassoDisplaySupport.cpp`: state dump every 64 submissions.
- `drivers/ArmCpu/ArmCpu.cpp`: report each CPU's own topology cluster type via
  `armcpu_cluster_type()` (falls back to the boot type if the topology lookup fails).
- `drivers/NativeCpuFreq/CpuFreqProbe.cpp`: one-line topology dump plus live-DT reads
  (`cluster-type`, `/model`, `capacity-dmips-mhz`) under `sheng-cpufreq`.
- `patches/picasso-cpu-topology-xnu.patch` (new, applied last): BCM2837 board config gets
  `MAX_CPU_CLUSTERS 2`, and `ml_parse_cpu_topology()` classifies CPUs by `capacity-dmips-mhz`
  (fastest group = P) when `cluster-type` is absent. Commit `16d00ab` **removed the
  `__ARM_AMP__ 1` line again**, so the default build is clutch; re-add it only with a working
  E/P source.
- `unixnu`: copies the new patch into the prepared tree and fingerprints it.
- `uefi/mu-silicium/Resources/DTBs/picasso-topology.dtb` (new): picasso DTB + `cluster-type`
  (`E` for cpu@0…500, `P` for cpu@600/700). Verified to differ from the original only by those
  8 properties (`dtc` round-trip diff). Inert on this firmware: XNU does not parse it (§4.4).
- `scripts/build-xnu-4k-baseline.sh`: in the bootstrap pass (`XNU_BOOTSTRAP=1`) the archive
  builders are skipped and their `BOOT_PROGRESS` checks relaxed; they run in the normal pass once
  the compile records exist.
- Untracked GPU/WLAN sources from earlier work (`drivers/NativeAdrenoPicasso/`,
  `drivers/NativeWlan/`) were committed so they are not lost.

## 6. AMP experiments and what went wrong

### 6.1 wf5: AMP enabled with a flat topology; the panic seen here is the IRQ-probe race

With `MAX_CPU_CLUSTERS=2` + `__ARM_AMP__=1` but the topology still flat (all `CLUSTER_TYPE_SMP`,
because the live DT has no cluster types), `processor_bootstrap` treats the boot cluster as P, and
every CPU lands in one P pset (`ecore_set == NULL`). The boot printed the flat topology line and
then, minutes in, panicked:
**[verified: /tmp/wf5-capture.log]**

```
panic(cpu 2 caller 0xfffffff007cbb968): Unexpected abort while on interrupt stack.
  at pc 0xfffffff007c58ed4, lr 0xfffffff007c58ed0
  esr: 0x96000005  far: 0x000000005c77c000
```

Symbol resolution (current kernel, `nm` + `llvm-objdump`): `pc` is `_boot_progress_irq+0xc8`, at
`ldr x9, [x0, #0x1d8]; ldrh w8, [x9]` after `bl _current_thread`. In this XNU
`getCpuDatap()` is `current_thread()->machine.CpuDatap`, so the interrupted thread's saved
`CpuDatap` was `0x5c77c000` and the load of `cpu_number` aborted.

**This panic is NOT caused by the AMP change.** The clutch build (wf8, no `sched_amp_dispatch`,
verified with `nm`) panicked at the *same instruction* (`_boot_progress_irq+0xc8`,
`ldr x9,[x0,#0x1d8]; ldrh w8,[x9]`) within ~45 s; wf4 and wf7 each survived their 300 s windows
without it. It is an intermittent race in the diagnostic IRQ probe: in interrupt context
`current_thread()->machine.CpuDatap` can be transiently stale/garbage (the thread pointer in
TPIDR and the thread's saved CpuDatap are updated at different points of the switch). The probe
must not dereference it unguarded. **[verified: /tmp/wf8-capture.log]**

**Recommended fix:** in `boot_progress_irq()`, either take the CPU number from the TPIDR_EL0
encoding / `ml_get_cpu_number_local()` instead of `getCpuDatap()->cpu_number`, or validate the
pointer before dereferencing it. Until then, boot with `boot-progress=0` for stability runs (the
probe is diagnostics-only; the boot-progress display and UI trace disappear with it).

**Correction (wf9, §9):** it was not a race at all. The BootProgress object was compiled against a
struct layout with `CpuDatap` at 0x1d8; the kernel core uses 0x1d0, so the probe read
`machine.preemption_count` (value 1) as the pointer. The TPIDR fix is still right — an
interrupt-context probe should never take the local CPU from the interrupted thread — but the
prebuilt-object mismatch was the actual panic.

### 6.2 wf6: same kernel + live-DT diagnostics, flat topology confirmed

Ran 120 s without a panic (short window); the DT lines above come from this boot.

### 6.3 wf7: capacity fallback cannot work against the firmware's DT

The `capacity-dmips-mhz` fallback in `ml_parse_cpu_topology()` was built on the assumption that
the live DT is the picasso DT. The live-DT reads in wf7 disprove that: both `cluster-type` and
`capacity-dmips-mhz` are missing, and `/model` is `iPad7,1` (see §4.4). The fallback therefore
stays inert and the topology stays flat (`clusters=1 types=1`).

On hardware, the AMP build in that configuration booted, reached SpringBoard, and the 300 s
capture ended with **0 panics**, but the UI stayed just as slow: 3 frame copies (14.8, 14.7,
128.6 ms) and no `PICASSO_DISPLAY_STATE` dump (fewer than 64 submissions) in the whole window.
**[verified: /tmp/wf7-capture.log]** (The interrupt-stack panic seen in other boots is the probe
race from §6.1, not this configuration.)

Consequences for the next agent:

- **A DT-based E/P split is blocked by the firmware.** The runtime DT is built by the UEFI
  firmware (the boot image's appended DTB is not used), and it does not carry the Qualcomm
  topology properties. To get a DT-based split, rebuild the UEFI FD from a Mu-Silicium checkout
  whose `Resources/DTBs/picasso.dtb` carries `cluster-type`.
- **A kernel-side split is the workable shortcut.** In `ml_parse_cpu_topology()`, when neither
  `cluster-type` nor capacity is present, classify by the DT `reg`/MPIDR value for this board:
  `phys_id` 0x600 and 0x700 are the two A76s (P), 0x0…0x500 the A55s (E). The live DT does carry
  those `reg` values (verified in the topology dump), so this needs no firmware change.
- **The committed default was returned to the stable scheduler.** The board patch keeps
  `MAX_CPU_CLUSTERS 2` but no longer defines `__ARM_AMP__`, so the kernel builds with
  `sched_clutch` again (the configuration of the verified-stable wf4 image). Re-add
  `#define __ARM_AMP__ 1` only together with a working E/P classification. wf8 is the
  clutch-baseline image built from this state; its on-device result is recorded below.

**wf8 result (clutch baseline, `MAX_CPU_CLUSTERS 2`, no `__ARM_AMP__`):** the kernel contains no
`sched_amp_dispatch` (`nm`), the topology dump is again flat (`clusters=1 types=1 en=0 pn=0`), and
the boot panicked within ~45 s in `_boot_progress_irq` — the probe race described in §6.1, not the
scheduler. **[verified: /tmp/wf8-capture.log]**

## 7. Next steps, in priority order

1. ~~**Fix the `boot_progress_irq` race first.**~~ **DONE in wf9 (§9).** The probe now takes its
   CPU number from TPIDR_EL0, reaches `cpu_data_t` through `cpu_datap()`, and never dereferences
   the interrupted thread's `CpuDatap`. Three consecutive 300 s boots, one boot each, zero
   panics. Before the next `__ARM_AMP__` build, read the build-order hazard in §9.2.
2. **GPU (Goal B)**: continue from the cpf6 state (GMU + HFI verified). Next per mainline
   `a6xx_gpu.c` for the a650 family: GX GDSC/clock, `a6xx_pwrup`/`hw_init`, CP/SQE and ring. This is
   the only path to a smooth UI; CPU rendering cannot reach usable frame rates.
3. **Topology**: add the phys-id E/P classification in `ml_parse_cpu_topology()` (the live DT does
   carry `reg` 0x600/0x700), or rebuild the UEFI FD with a DT that names the clusters. Only then
   re-add `__ARM_AMP__ 1` and verify the topology dump shows `en=6 pn=2`.
4. **Display path**: replace the full-frame copy with `native_legacy_copy_diff()` (already in
   `NativeLegacyCopy.h`/`NativeLegacyRequest.h`) or raise the display work loop's QoS so the copy
   stops being preempted. Note the source mapping is already cacheable (opts=0x1001).

## 8. Open questions / risks

- The runtime DT's origin is **[unverified]**; the FD is the leading hypothesis.
- ~~The interrupt-stack panic is a race in the diagnostic `boot_progress_irq` probe.~~ Resolved:
  it was an ABI offset mismatch in a prebuilt object, not a race (§9). The "intermittent"
  observation is explained by which side of the bootstrap→normal pass the archive was built on.
- `bench2` takes a second mapping of the source; it was active in the boot that later panicked
  (wf3). Treat it as a diagnostic only.
- The device returns to fastboot on panic automatically (`IMEM Fastboot reason armed`), so a
  failed image is recoverable with `fastboot boot`.

## 9. P0 resolved — the boot_progress_irq panic was an ABI mismatch (wf9)

### 9.1 Root cause and evidence

- wf8's panic is at `pc = 0xfffffff007c4f874 = _boot_progress_irq+0xc8`, executing
  `ldrh w8, [x9]` with `x9 = 0x1`, `far = 0x1`. The preceding instruction
  (`lr = ...+0xc4`) is `ldr x9, [x0, #0x1d8]` after `bl _current_thread`; `x0` equals the
  panic's `TPIDR_EL1` thread pointer. So the probe loaded `[thread+0x1d8]` as `CpuDatap`.
  **[verified: /tmp/wf8-capture.log, kernel disassembly]**
- The kernel core's own layout puts `machine.CpuDatap` at **0x1d0**: `genassym` emits
  `ACT_CPUDATAP 464` (`.../osfmk/DEVELOPMENT/assym.s:19`), and `_machine_thread_create` stores
  the NULL `CpuDatap` with `stp x8, xzr, [x0, #0x1c8]` while `str wzr, [x0, #0x1d8]` is
  `machine.preemption_count`. Value 1 at 0x1d8 is exactly a preemption count.
  **[verified: pcb.o, assym.s, llvm-objdump]**
- The wf8 BootProgress.o was built against an **AMP-era header layout**, where
  `__ARM_AMP__` → `__AMP__` → `CONFIG_SCHED_EDGE=1` adds 8 bytes before `machine`, making
  `CpuDatap` 0x1d8. The `xnu-work` snapshots at 12:07/12:23/12:29 still had
  `#define __ARM_AMP__ 1`; 12:40 and 12:42 dropped it. `struct machine_thread` itself never
  changed; only the AMP conditional block did. **[verified: snapshot diffs, clang record layouts]**
- The build order is what mixed them: `build-xnu-4k-baseline.sh:85` runs
  `build-boot-progress.py` **before** `make`/installhdrs (line 878), and the archive identity
  fingerprint covers only the archive bytes. The 12:42 pass compiled BootProgress.c against
  the bootstrap-era record and stale `EXPORT_HDRS` (AMP layout) while `make` compiled the
  kernel core from the refreshed non-AMP headers. **[verified: build-xnu-4k-baseline.sh,
  kernel.py bootstrap path, .boot-progress-mode timestamps]**

### 9.2 Fix and build-order hazard

- `drivers/NativeMobileDisplay/boot-progress/BootProgress.c` no longer calls `getCpuDatap()` in
  interrupt context. The CPU number comes from `TPIDR_EL0` (`cswitch.s set_thread_registers`
  reloads it from the running CPU's `cpu_data` on every switch), and `cpu_data_t` is reached
  through `cpu_datap(cpu)` with a bounds check against `ml_get_max_cpu_number()`.
  `bp_kernel_chain()` takes `cpu_int_state` from that per-CPU pointer. The probe is now
  independent of the `struct thread` layout.
- **Hazard for the next ABI/header change (in particular enabling `__ARM_AMP__` for P1):**
  the boot-progress archive is compiled before `installhdrs` in the same build, so the first
  normal pass can still link an archive built with the previous layout. After any such change,
  run `unixnu build kernel prelink` twice, or delete
  `out/picasso/tree/build/boot-progress/libBootProgress.a` and
  `out/picasso/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/.boot-progress-mode` after
  `make` has refreshed `EXPORT_HDRS`, then rebuild. Verify with `llvm-objdump`: the shipped
  `_boot_progress_irq` must not load `[x0,#0x1d0]`/`[x0,#0x1d8]` at all now; for any other
  object that does touch `machine.CpuDatap`, its offset must equal `ASSYM ACT_CPUDATAP`
  (0x1d0 without AMP, 0x1d8 with AMP).
- The console tty path (`scripts/picasso-cdc-command.py`) hangs on this device; use
  `scripts/capture-picasso-usb-raw.py` with `--send`/`--reboot-fastboot` instead. **[verified
  this round: the tty invocation blocked until timeout, the libusb one worked]**

### 9.3 Acceptance on hardware (wf9)

Image: wf8 payload and boot args unchanged (`sheng-gpu=0`, `boot-progress=2`), only
`PAYLOADS/kernel.macho` replaced; built kernel sha256
`8edab2a37a925ebde446a881ef71da0c4e7daefba4e6c0e252affcdca4fed092` (old wf8
`c6b42517…`), extracted payload kernel hash identical.

| round | window | PSHOLD boot markers | panic(cpu / Unexpected abort / Debugger | userland |
|---|---|---|---|---|
| 1 | 13:14:51–13:20:03 | 1 | 0 | SpringBoard/backboardd sync, NVT touch, CDC live |
| 2 | 13:20:03–13:25:16 | 1 | 0 | same |
| 3 | 13:25:16–13:30:29 | 1 | 0 | same |

Logs: `out/wf9-runs/run{1,2,3}.log`, `out/wf9-runs/harness.log`; each round ends with an
active `PICASSO_SYNC`/`PICASSO_CPUFREQ` sample, not a panic. The diagnostic IRQ probe
(`boot-progress=2`) stayed enabled throughout, i.e. the previously crashing path ran for
900 s of booted userland without a fault.

## 10. P1 progress — E/P topology, AMP, and a latent GIC IPI bug (wf10–wf12)

### 10.1 The E/P classification works

`ml_parse_cpu_topology()` now falls back to the DT `reg` when neither `cluster-type` nor
`capacity-dmips-mhz` exists (`capacity_max == 0` branch in
`patches/picasso-cpu-topology-xnu.patch`): `phys 0x600/0x700` are P, `0x0..0x500` are E.
With `__ARM_AMP__ 1` restored, the live dump reads
`topo cpus=8 clusters=2 types=6 en=1 pn=0` at probe time and the late print added in
`BootProgress.c` (monitor loop, once `en+pn == num_cpus`) reads
`topo online cpus=8 clusters=2 types=6 en=6 pn=2 cl-e=1 cl-p=1`.
**[verified: out/wf11-runs/run1.log lines 84-92 and 804, out/wf12-runs/run1.log]**

### 10.2 Clutch cannot be used with two psets

With the classification but without `__ARM_AMP__` (wf10), the P CPUs create a second
`PSET_SMP` in `pset_node0`. `sched_steal_thread_enabled()` is
`bit_count(pset->node->pset_map) > 1`, now true, and `sched_clutch_steal_thread()` returns
`THREAD_NULL` **without dropping the pset lock** while `thread_select()`'s caller contract
says it returns unlocked. The subsequent `pset_lock(pset)` is recursive on pset0
(`0xfffffff007d69a00 = pset0+0x100`) and panics in `lck_ticket_contended`.
**[verified: out/wf10-runs/run1.log; sched_clutch.c:3069, sched_prim.c:723,2884-2892]**
So the intermediate "classification under clutch" build is not viable; AMP (edge) is the
path, which is what the final build does.

### 10.3 Edge leaves the P pset unused

With AMP the kernel selects `sched_edge_dispatch` (`nm sched_prim.o`: `U _sched_edge_dispatch`),
but nothing ever places a thread on cluster 1:
`sched_clutch_bucket_group_init()` seeds every bucket's `scbg_preferred_cluster` with
`pset0.pset_cluster_id` (E, the boot cluster), the edge migration matrix forbids E→P
(`migration_allowed = 0, steal_allowed = 0`), and this port has no CLPC/perfcontrol caller of
`sched_edge_tg_preferred_cluster_change()`. Also `sched_edge_cpu_init_completed()` is only
called from `AppleARMSMP.cpp` under `USE_APPLEARMSMP`, which BCM2837 does not define, so
`native_psets`/`foreign_psets`/`sched_edges` stay zeroed.
**[verified: source lines; out/wf11-runs/run1.log and out/wf12-runs/run1.log
`gov d0 busy≈98 d1/d2 busy=0`]**

### 10.4 The wf11 panic was a GIC SPI-IPI routing bug (fixed in wf12)

wf11 ran the AMP/E-P topology and, after ~2.5 min in userland, `backboardd` panicked in
`task_restartable_ranges_synchronize`: `wait_while_equals32[thread+...]` stuck at
`0x00010000` (`trr_ipi_ack_pending`) for 125 ms. The AST IPI was never acknowledged because
`ArmCpu::spiIpiConfigure()` wrote each core's `GICD_IROUTER` at `0x6000 + 8*INTID`; GICv3
places SPI routers at `0x6000 + 8*(INTID-32)`, so the writes went to `INTID+32` (outside the
implemented range) and INTIDs 1008..1015 kept their reset route (affinity 0, the boot CPU).
The affinity word was also encoded in MPIDR/DT layout instead of the IROUTER's
Aff0[39:32]/Aff1[47:40]/Aff2[55:48]/Aff3[63:56]. Consequently non-boot cores stayed
`SIGPdisabled`; `cpu_signal_internal()` refuses to signal them and `cause_ast_check()` ignores
the failure, so the pending bit never clears. `armcpu_spi_attach()` had the same off-by-32.
**[verified: drivers/ArmCpu/ArmCpu.cpp, panic out/wf11-runs/run1.log:2652]**

Fix: `gicdRouterOffset(intid)` and `gicdRouteFromAffinity(aff)` helpers used by both
`spiIpiConfigure()` and `armcpu_spi_attach()`. wf12 is otherwise the wf11 kernel/payload:
1 boot, 300 s, **0 panics**, topology still `en=6 pn=2`. **[verified: out/wf12-runs/run1.log]**

### 10.5 Edge with the sheng (SM8550) bucket policy uses both clusters (wf14, accepted)

The classic `sched_amp` detour worked (wf13: P busy 33–61 %) but is not the project's
precedent. The sibling sheng port's `patches/xnu/zzyf-sheng-amp.patch` keeps edge and adds
the missing policy, so this port now does the same (same hunk logic, adapted to the tree):
- `sched_edge_default_preferred_cluster(bucket)`: fixed-priority, foreground and
  user-initiated buckets (`bucket < TH_BUCKET_SHARE_UT`) start on the first P cluster;
  utility/background buckets start on the boot (E) cluster.
- `sched_edge_ui_bucket_stays_on_p()`: P-preferred UI buckets do not migrate or get stolen
  to E.
- `sched_edge_clutch_bucket_group_preferred_cluster()` falls back to `pset0` until the P
  pset exists (cores register asynchronously).
- The E→P edge allows `sce_steal_allowed = 1`, so an idle P core can take E work; there is
  no performance controller on this port.

`__ARM_AMP__`/`MAX_CPU_CLUSTERS=2` stay as before. wf14 (same payload, classification + AMP
+ this policy, edge): one boot, 300 s, **0 panics**, early topology dump
`cpus=8 clusters=2 types=6 en=1 pn=0` with cpu0–5 type E / cpu6–7 type P. The governor now
shows `d1 busy=100 d2 busy=100` (edge alone: 0), i.e. the A76s carry work.
**[verified: out/wf14-runs/run1.log]**

Caveats:
- `d0` still reads ≈100. That field is the busiest *single* CPU in a short window, and the
  boot-progress diagnostics plus background work keep one A55 hot; it is not a cluster
  average. A measurement-only boot (wf13, `sheng-cpufreq=1`) showed the per-CPU 10 s
  averages: A55 20–70 %, A76 50–78 %. **[verified: out/wf13-cpuusage-run/run1.log]**
- The once-only `topo online` print added in §10.1 did not appear in wf14 (it did in
  wf12/wf13); the monitor thread appears to freeze/starve before it fires. The early dump
  plus the governor data still prove the split.
- UI frame production is unchanged (5 full-frame copies per 300 s): P1 moves *where* the
  CPU work runs; the missing GPU render path is P3 (§7.2/§7.3).

The `sched_amp` variant remains available as a build switch (§10.3/§10.5 history) but is
not the default: it is not what sheng ships, and `CONFIG_SCHED_EDGE` also gates
`struct thread`'s edge fields, so switching it changes the ABI (§9.2).
