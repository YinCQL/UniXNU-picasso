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
4. ~~**Display path**: replace the full-frame copy with `native_legacy_copy_diff()` or raise the
   display work loop's QoS.~~ **DONE in wf15 (§11).** The copy runs on backboardd's user-client
   thread, not the display work loop, so its QoS is the caller's; the shadow diff removes the
   device stores for unchanged blocks (21–25 ms vs 135–187 ms).

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

## 11. P2 resolved — the scanout copy only stores dirty blocks (wf15)

The full 10.4 MB copy per swap alternated between ~15 ms and ~130–187 ms (wf14:
`PICASSO_DISPLAY event=10` a=0x27a2f2–0x371673 ticks, b=0x9e3400 every frame).
The copy is not bandwidth bound (§4.2); the slowdown is the device stores and the
scheduling around them.

The client always submits full-frame dirty rectangles, so rectangle-based dirty copying
would be a no-op. The tree already had the right facility:
`native_legacy_copy_diff()` (`drivers/NativeMobileDisplay/NativeLegacyRequest.h:147-181`)
compares 64-byte blocks against a cached shadow (rows packed at `width*4`) and stores only
the blocks that changed. It had no caller.

`NativeLegacyDisplay.cpp` now keeps that shadow (`copyShadow`, `IOMalloc(width*height*4)`,
10.4 MB wired): the first real frame (or any frame after a blank/plain-path copy) does a full
diff fill, later frames store only dirty blocks. Unsupported geometry or an allocation
failure falls back to `native_legacy_copy_pixels()`; the blank swap invalidates the shadow.
`event=10`'s `b` field now reports the bytes actually stored, not `length`.

Measured (same kernel/payload/args except this change):
- wf14 full copy: every frame b=0x9e3400, a=0x27a2f2–0x371673 ticks (135–187 ms).
- wf15 shadow diff: first frame b=0x9e3400, then e.g. b=0, 0x51a40, 0, 0x2d0c0, 0x4c700,
  0x44400, all at a=0x62ab–0x7662c ticks (21–25 ms). No 130 ms sample in 13 frames.
  **[verified: out/wf15-runs/run1.log]**
- 300 s, one boot, 0 panics; topology and scheduler unchanged (wf14 policy).

The frame *count* is unchanged (the UI is still CPU-render bound with the GPU off): P2
removes the copy stall from the presentation path, P3 must supply the render path. The
shadow compare still scans the whole 10.4 MB each swap (~20 ms); if the client ever sends
partial dirty rects, that pass can be avoided too.


## 12. P3 — GX power-on: resolved (wf17–wf38)

**Result.** The GMU cold start, HFI startup and first GX performance vote now complete on hardware:

    vote status=0 fw=0 words=0 skipped=0 perf=1 bw=255
    oob polls=2 info=80000001 settle=0 status=000000c0->00000000 on
    hfi: stage=7 id=10 status=0 fw=0 words=0 gx=0
    hfi: run status=0 last-stage=7 skipped=0
    PICASSO_GPU end stage=3 step=4 error=0 requested=3 ticks=3 done=1

`GX_HM_GDSC_POWER_OFF` and `GX_HM_CLK_OFF` (bits 6/7 of `GMU_SPTPRAC_PWR_CLK_STATUS`) clear, the
GPU_SET OOB acked, START acked, and the boot continues into userland with no panic.
**[verified: out/wf38-runs/run1.log]**

**Root cause (measured).** The GMU votes the DDR/CNOC BCMs while it powers GX. The port sent
mainline's generic A630 bandwidth table, whose RPMh BCM addresses do not exist on SM7250, so the
first GX vote timed out inside `GmuPwrGxBwVote` (F2H debug queue id 100, code `0x007300b8`). The
fix is the A620 table: one bandwidth level (the firmware's "off" level), DDR commands
MC0/SH0/ACV read from this device's command DB (0x50000/0x50004/0x5007c) and the CNOC command CN0
(0x50074) instead of the generic `{0x50034, 0x5007c, 0x5004c}`, which contains no CN0. This
matches the upstream A620 series (Ishu Brar, 2026-10-23): "Without bandwidth levels the GMU falls
back to the generic A630 table, whose BCM addresses do not exist on SM7250, and the first GX
performance vote times out."

**Second defect found on the way: the command-DB parse was 144 bytes off.**
`rsc_hdr.header_offset`/`data_offset` are offsets from the end of `cmd_db_header` (cmd-db.c
`rsc_to_entry_header`/`rsc_offset`); the port indexed them from the mapping base. The entry walk
still lined up (24-byte stride), but every aux table was read from the entry/id bytes: the boot
vote printed `gmu-vote=6d6d0800` (ASCII "mm") and `xo.lvl`/`vrm.soc` were reported missing. After
the fix: `gfx[8]=0,40,80,c0…`, `mx[7]=0,10,80,c0…`, `cx[8]=0,10,40,80…`, GMU vote
`0x00400202`, GPU vote for the DT level `0x00400201`, `xo.lvl=0x300a0`, `vrm.soc=0x43400`. The
PDC is then programmed exactly as vendor `a6xx_load_pdc_ucode` does for a620: sequencer ucode,
TCS1 `{mx=1, cx=0, xo=0, vrm+4=0}`, TCS3 `{mx=2, cx=2, xo=3, vrm+4=1}`, `PDC_ENABLE=0x80000001`.
Upstream's A620 series states the same requirement ("the A620 has no PDC setup from AOP, so the
PDC sequencer is programmed from the driver (xo/mx/cx addresses from cmd-db)"; the VRM command
goes into both TCS banks).

**What the firmware answered while it was still refusing.** Every failed vote produced
`dbg id=100 err=007300b8 "GmuPwrGxBwVote"` on the F2H debug queue (queue 2) while the command
queue advanced to the write index and no response appeared on queue 1. KGSL's decode
(`kgsl_hfi.h`: `F2H_MSG_ERR` = id 100 carries `{error_code, ASCII}`) is what named the handler.

**Ruled out during the search** (each with a boot; logs under out/wf2x-runs):
- message order: START (id 10) acked before the vote; vote → OOB → START (vendor `gmu_start` and
  the quest2-steamos recipe); START → vote → OOB (mainline `a6xx_gmu_resume`). All failed
  identically, because every pre-wf35 run also sent the corrupt ARC votes above.
- ACD: the GMU node carries an AOP mailbox, so KGSL sets `ADRENO_ACD_CTRL` and tells the AOP
  before booting. This port has no mailbox, so ACD is now left off; the vote failed with and
  without the ACD table/feature.
- LM feature + `HFI_VALUE_LM_CS0` slope (qfprom 0x45c8 reads 0): accepted, not the blocker.
- RSCC sleep: `GMU_RSCC_CONTROL_ACK` bit0 = 0 at boot (RSC awake); the wake path exists and is
  skipped.
- OOB timing: once the vote succeeds the ACK arrives in 2 polls, so the old 10 ms budget was not
  the blocker; it is now KGSL's 100 ms anyway.
- Pre-powering GX from the CPU: mainline's `a6xx_gmu_gxpd_get` uses `gmu_gx_gdsc` whose
  `power_on` is `gdsc_gx_do_nothing_enable` (verified in gdsc.c), i.e. it does not toggle the
  GDSC; the GMU owns GX.

**Remaining notes.**
- PDC ownership on this target: mainline treats the whole A650 family as AOP-owned, vendor a620's
  core does not; this port programs the PDC from the driver, matching vendor and the upstream A620
  series. Both the full CPU-PDC path and the earlier enable-only path were tried; the bandwidth
  table was the discriminator.
- The AOP QMP mailbox message `{class: gpu, res: acd, value: 1}` is not implemented, so ACD stays
  disabled.
- Firmware pair: this port uses the vendor image's `a650_gmu.bin` (+ `a630_sqe.fw`, `a650_sqe.fw`,
  `a620_zap`), which is also the vendor picasso a620 core's pair; upstream a620 uses
  `a621_gmu.bin`. The first vote comes up on the vendor firmware.
- The OOB request is left asserted after `gxPowerOn` returns; the next step (CP/SQE, `hw_init`)
  should clear it with `HOST2GMU_INTR_SET` bit 31 once the GPU registers have been touched.

### 12.1 Stage 4 — CP/SQE bring-up: resolved (wf39–wf47)

**Result.** `sheng-gpu=4` now runs mainline's A650-family `hw_init`, uploads the SQE microcode,
starts the CP, submits `CP_ME_INIT`, submits a no-op IB and waits for its `CACHE_FLUSH_TS` fence:

    PICASSO_GPU cp: sqe payload=31456 first=016dd087
    PICASSO_GPU cp: sqe started cntl=00000001 status=00c00015 int=00000000 fault=00000000
    PICASSO_GPU cp: me_init submitted wptr=9 shadow=0 rptr=0
    PICASSO_GPU cp: me_init idle
    PICASSO_GPU cp: noop ib submitted wptr=18 fence=1
    PICASSO_GPU gx: oob released
    PICASSO_GPU cp: ready fence=1
    PICASSO_GPU end stage=4 step=4 error=0 requested=4 ticks=4 done=1

The probe's own `cp_submit`/`cp_wait` then return a second fence (`fence=2 wait returned 0`), and the
boot continues into userland (PID1 scans, `panic=0`). **[verified: out/wf47-runs/run1.log]**

**Root cause: the GPU context did not route `gfx3d_user`'s secure aliases.** The device-tree
`iommus = <&adreno_smmu 0 0x401>` expands sid 0 / mask 0x401 to the exact SIDs 0, 1, 0x400 and
0x401; the port attached only the first two ("the non-secure GPU traffic uses 0 and 1"), leaving the
secure aliases unrouted. The CP runs in secure mode until the zap shader switches it out
(`CP_SET_SECURE_MODE`), so releasing `CP_SQE_HALT` with those aliases faulting wedged the GPU block
and the SoC watchdog reset the device every time, with the console dying at the SQE start.

**How it was isolated** (each a boot; `sheng-gpu-cp-limit=<1..7>` stops after a phase):
| limit | stops after | outcome |
|---|---|---|
| 2 | `hw_init` | survives, userland continues (wf41) |
| 4 | ring programming | survives (wf42) |
| 5 | CP register readback | survives (wf43) |
| 6 | `CP_SQE_CNTL=1` | **reset** with a630 SQE (wf39/wf44) and with a650 SQE (wf45) |
| 7 (default) | full stage | succeeds once the secure aliases are routed (wf47) |

**Second fix: the SQE firmware pairing.** The embedded firmware unit staged `a630_sqe.fw` as the
primary SQE. Vendor's `adreno_gpu_core_a620` and upstream's a620 series both pair this GPU with
`a650_sqe.fw` (a620 is an A650 derivative, same as the GMU's `a650_gmu.bin`); the generator now
embeds a650 as primary and a630 as the alternate. The vendor image's `a650_sqe.fw` is version
0x087, below mainline's 0x095 floor, so the probe reports that and continues only with
`sheng-gpu-allow-old-sqe=1`.

**Also in this change.** The GPU-context class gained a streams variant
(`NativeAdrenoGpuContextBringupStreams`, up to 8 exact streams) so the four SIDs attach in one
transaction; the integration quiesces slots 1–4 first and attaches 0/1/0x400/0x401 to bank 0/ASID 1
(`route=0/4` verified by the kit). The CP hook (`Adreno620Cp.cpp`) follows mainline `hw_init` for
an a650-family device with the vendor a620 values for PC_DBG_ECO_CNTL bit 9, the hang-detect span
(0x3ffff), UBWC 3.0/hbb14 (SM7250's DT values) and the a650 GBIF list; the OOB GPU_SET request is
released only after the no-op fence lands (`native_adreno620_gmu_release_gpu_set`).

**Ring soak.** `sheng-gpu-cp-stress=200` submits 200 no-op fences after the OOB release, 50 ms
apart. All 200 landed (`stress done count=200`), `GMU_SPTPRAC_PWR_CLK_STATUS` stayed 0 (GX on)
across the idle gaps, and stage 4 still finished `done=1` with the boot continuing into userland
(wf48). A production submit path can therefore reuse this ring while the GPU stays powered; a
long-idle then submit cycle (power collapse) is still untested.

**Production ring adoption.** `NativeAdrenoRing` now has `adopt()` (take over a board-programmed
ring without register writes, no fence-page clear, HW_APRIV shadow as the read pointer) and an A6xx
fence profile (`CP_EVENT_WRITE` with the CACHE_FLUSH_TS event | IRQ; framed A7XX thread-control
submissions are refused). With `sheng-gpu-ring-smoke=1` the CP stage adopts the live ring after
hw_init and submits one `CP_INDIRECT_BUFFER` through the production class:

    PICASSO_GPU cp: ring smoke submit=00000000 fence=2 observed=2 result=0
    PICASSO_GPU cp: ring smoke ok next=27 seqno=2
    PICASSO_GPU end stage=4 step=4 error=0 requested=4 ticks=4 done=1

The boot continues into userland (wf49). This is the submission path the Metal side will use; the
next step is to attach `NativeAdrenoSubmit` and publish the service on top of it.

**Service publication.** `sheng-gpu-publish=1` attaches `NativeAdrenoSubmit` to the adopted ring and
the GPU context VM, fills `NativeAdrenoAbiDeviceInfo` with this chip's values (chip id 0x06020001,
hbb 14, UBWC 3.0, `va_size = 2^ias - 0x100000000` with ias 48) and calls `native_adreno_publish()`:

    PICASSO_GPU cp: publish status=00000000 service=0xfffffff5a80ca9c0 va_size=ffff00000000
    PICASSO_GPU cp: published
    PICASSO_GPU end stage=4 step=4 error=0 requested=4 ticks=4 done=1

The boot continues with `panic=0` (wf50). No accelerator is registered and the Metal plugin is not
in this image, so no client can submit; this proves the per-process VM/table-switch and userclient
plumbing on A620. The A740 `NativeAdrenoService::start` path is **not** the way forward: its a6xx
branch is an unreachable skeleton (it never builds a GPU context, its SQE copy keeps the 4-byte
header, ring/SQE IOVAs stay zero, and its cold start is the generated A740 one). The live owner
stays this probe/board code; the accelerator (`AGXAccelerator`, `IOAcceleratorES`) is registered
programmatically after IOGPUFamily loads, and the Metal plugin bundle itself arrives through the
boot addon mount.

**Next milestones** (ranked, from the service-path survey): (1) register the accelerator with
`MetalPluginName`/`MetalPluginClassName`/`NativeSandboxGrantProcess` (the wiring exists in the
sheng reference, not in this worktree); (2) give `NativeAdrenoSubmit` an A620 plain-submission
profile (the current one always emits A7XX thread-control framing) and open one user client; (3)
enable the Metal plugin end-to-end with the addon mount and sandbox patches. Only the secure
content/CP aperture paths are fundamentally blocked on zap/PIL.

**Not done yet.** ZAP is not loaded (no secure PIL/SCM path), so the CP stays in the mode the boot
chain left it and no `CP_SET_SECURE_MODE` packet is sent; stage 4 proves the CP runs. The next
piece is the SMMU/Metal path (stage 5) and then the userland GPU stack.

### 12.2 Stage 5 — accelerator registered and one A620 user-client submission (wf51–wf53)

**Accelerator registration.** The staged class already rides in the kernel image: an upstream tree
script, `scripts/build-native-adreno-accelerator.py`, compiles
`drivers/NativeAdreno/NativeAdrenoAccelerator.cpp`, removes its initializer from
`__mod_init_func`, globalizes `__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp`, and adds the late
object plus the three audited `IOAcceleratorES` import bindings to `libNativeAdreno.a` (the kernel
link is whole-archive, so `nm` shows `AGXAccelerator`, its metaclass, the allocator and the
initializer). What was missing was the call site. `sheng-gpu-metal=1` now runs, after publishing
the service: `OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily")`, the original
`IOAcceleratorES` metaclass (class size 136) through `OSMetaClass::getMetaClassWithName`, the
staged initializer between `preModLoad`/`postModLoad`, then an `AGXAccelerator` instance with
`MetalPluginName=NativeAdrenoMetal`, `MetalPluginClassName=NativeAdrenoMetalDevice` and
`NativeSandboxGrantProcess=*`, attached to `IOService::getPlatform()` and registered:

    PICASSO_GPU cp: publish status=00000000 service=0xfffffff9bf8b1560 va_size=ffff00000000
    PICASSO_GPU cp: metal stage=5 status=00000000 registered=1
    PICASSO_GPU cp: metal published

The boot continues with `panic=0`, and userland sees the class: backboardd and SpringBoard probe
`provider=AGXAccelerator` (`PICASSO_IOKIT begin op=0 ... provider=AGXAccelerator`), and
backboardd's type-1 open is logged and refused cleanly, as designed (the original IOGPU library's
foreign open type is not the plugin's). **[verified: out/wf51-runs/run1.log]**

**A620 submission profile.** `NativeAdrenoSubmit::setA6xxPlain(true)` makes a submission the
caller's ring words plus the ring's own `CP_EVENT_WRITE`/`CACHE_FLUSH_TS` timestamp fence, instead
of A7XX thread-control framing; `submitFramed` stays refused on an A6xx ring. `sheng-gpu-client-test=1`
then opens one `NativeAdrenoUserClient` on the published VM/ring/submit, queries the ABI, allocates
an IB and a target, maps both in the client aperture (`GpuClientVaStart`), writes three
`CP_MEM_WRITE` packets, submits through the production path, waits on the CP fence and reads the
target back:

    PICASSO_GPU cp: client open status=00000000 ias=48
    PICASSO_GPU cp: client query status=00000000 abi=2 in=48 out=40
    PICASSO_GPU cp: client submit status=00000000 fence=2
    PICASSO_GPU cp: client fence wait=00000000 matched=4/4
    PICASSO_GPU cp: client close=00000000
    PICASSO_GPU cp: client test ok

Stage 4 ends `done=1` and the boot continues. This is the whole path the Metal plugin will use
(minus the IOKit wrapper and sandbox), now proven on A620. **[verified: out/wf53-runs/run1.log]**

**Trap found on the way (wf52).** User BOs from `allocateSystem` have **no kernel mapping** until a
client view maps them ("mapped nowhere until a client view maps it"), so the first version of the
test dereferenced `memoryOf(handle)->mapping() == nullptr` and panicked in `clientTest()`
(`Adreno620Cp.cpp:848`, data abort, `far=0`, `x9=0xdeadbeef`). The self-test now allocates one
contiguous run (the kernel-mapped `allocate()` path) and checks both mappings before use. The
plugin itself uses system BOs through its own client views and is unaffected.

**Remaining for Metal.** The plugin bundle is not in this image: `build-adreno-metal-plugin.py`
needs the Turnip `libvulkan_freedreno.dylib` (iOS), the bridge archive and the recorded replay,
then `make-gpu-addon.py` plus the boot-time addon mount (`--gpu-addon`/`sheng-gpu-addon`) and the
XNU sandbox grant patch. The GPU-side prerequisites (accelerator registration, service, client
submission) are now in place.

### 12.3 Stage 5 — the Metal plugin in the image and the first client submission (wf54–wf68)

**The image pairing.** The plugin speaks NativeAdreno ABI v2 (`NativeAdrenoAbiVersion = 2`, submit
record 3104 B). Turnip's `dw_open_connection` probes the version before anything else and refuses
every later call on a mismatch, so a kernel from this tree must be paired with an addon built from
the same tree: `build-turnip-darwin.py --target ios` (the iOS `libvulkan_freedreno.dylib`),
`build-adreno-metal-plugin.py` (`NativeAdrenoMetal.bundle`), `make-owner-trust-bundle.py`
(`owner-trust.bundle`), `make-gpu-addon.py --publish` (the addon blob), then
`unixnu build payload --gpu-addon <blob>` with kernel args `sheng-gpu-metal=1 sheng-gpu-addon=1`.
The plugin source is `drivers/NativeMetal/NativeAdrenoMetalPlugin.mm` in the main checkout (not in
this device worktree); its crash reporter and the 0x68xx/0x69xx marker protocol live there. Every
`out/wfNN` image embeds its own addon; never pair one tag's addon with another tag's kernel.

**Accelerator and grant.** §12.2's `NativeSandboxGrantProcess=*` does not work on this XNU: its
`native_sandbox_grant` compares the first 16 characters `proc_selfname` reports, literally. The
board lists `backboardd,SpringBoard,mediaserverd,WidgetRenderer_D,iconservicesagent,posterboardd,Wallpaper`.
backboardd (pid 56) and mediaserverd (pid 29) reach `newUserClient` with the plugin's open type
0x4147 and mount (`PICASSO_GPU open: plugin type=4147 ...`, `plugin: mount ok opens=N`).

**Bug #1 — a zeroed address heap (wf59→wf61).** After GetDeviceInfo, every client process died in
`util_vma_heap_alloc` (the plugin's SIGSEGV; mark 0x690B proved the crash handler ran, while the
text channel stays closed because backboardd's sandbox refuses `/dev/console`). Turnip allocates
`device->vma` only when `NativeAdrenoDeviceClientIova` is set in DeviceInfo.flags; the board
published flags=0, and `darwin_bo_init` still allocates from that heap on the first BO, walking
`list_entry(NULL)`. Fix: `info.flags = NativeAdrenoDeviceClientIova` (`Adreno620Cp.cpp`). wf61:
tunnel calls 2/3/13 (BoAlloc/BoMap/BoCpuMap) all status=0; the next refusal moved to Submit.

**Bug #2 — the write pointer the board moved (wf61→wf62).** Submit then returned 0xe00002d5
(kIOReturnBusy): the board's post-publish probe (`checkCpSwitch`) had advanced the CP's read
pointer past the class's adopted `next_`, so `used = (next_ - rptr) & mask` wrapped to 8182 of 8192
words (nearly full ring). Fix: `NativeAdrenoRing::noteExternalSubmit(wptr, value)` takes the probe
pair's write pointer and fence after the board's submission (`Adreno620Cp.cpp`). wf62: tunnel
call=5 status=0; call=6 (WaitFence) then returned 0xe00002d6 (timeout), which is the next problem.

**The first client submission stalls (wf63–wf68).** The submission is accepted and the per-process
page-table switch lands (TTBR0 readback 0x100000ec661000, ASID 0x1000). The board's own NOP
submission on a fresh VM completes (`cpcheck: mode=1 result=0 steps=0/0/0/0/0 fence=2`), so the
CPU-written TTBR0 switch and the fence path both work. Turnip's submission then leaves the CP busy:
RBBM_STATUS 0x800005, CP_HW_FAULT 0, CP_STATUS_1 0x8000430e, SMMU FSR 0x400 (the AArch64 format
value; no fault), CP_RB_RPTR/WPTR 0x24, IB1 0x1_00027000 with REM 0, fence page still 2. The job is
one IB of 4 words (`70bf8003 27000 1 bd`, i.e. CP_INDIRECT_BUFFER to 0x1_00027000, 0xbd dwords).
Reading the IB through the client VM's own mapping after installing the board's DRAM reader
(`native_adreno_dram_read32`) gives eight zero dwords at 0x1_00027000 (wf67:
`stall ib1 100027000 read=0: 0 0 ...`): the command stream Turnip submitted is not in the BO's
DRAM. wf68 measures the two candidate causes: the client view's pages against the BO's first span
(`PICASSO_GPU view: token=... first=... span=...`), and the same IB words through the kernel's
cacheable direct map and through DRAM (`stall ib1c: cached/dram ...`), controlled by a CP-written
page the driver already reads (the fence page, `stall fencepage: ...`). wf68 answered all three:
every client view maps exactly the BO's own first page (`view: token=1..5 ... first=<span0>`), the
fence page reads 2 through both the cacheable alias and DRAM (so both readers work), and the IB's
eight words read zero through both (`stall ib1c: 0/0 ...`). There is no dirty cache line: the
command stream the client named is not in that BO at all. wf69 records the submit-time state instead
of the post-stall state: for the first submissions it prints the wire buffer (handle, iova, bytes),
the mapping's extent (`describe`), the word at the wire iova through both readers, and the BO's first
eight words, with the board's own NOP probe as the control. wf69's probe read its own IB word
(`70108000`); the first client submission read its whole page as zero (`h=16 iova=100027000
bytes=756 map=100027000/16384 sample=0/0/0`), and a second submission about 100 ms later, naming the
same handle and address, read a valid Turnip stream (`70460001 31 40bb0801 fffff 70268000 ...`). The
stalled job is the first (fence 3); the board's own probe (fence 2) completed. The stall record also
reported mapping handle 10 at the IB address while the submit named 16, so the ring's address may
name a different BO by stall time. wf70 therefore identifies every wire record and the stall by pid,
VM and the VM's next handle, and reads the stalled job's own buffer handle.

**wf70 result — the stalled job is backboardd's first submission, and its page reads zero in both
readers.** The wire record is `pid=56 vm=0xfffffff2e32cad28 next=17 h=16 iova=100027000 bytes=756
map=100027000/16384 sample=0/0/0 bo= 0 0 0 0 0 0 0 0`; the stall for fence 3 names the same job
(`stall ids: pid=56 fence=3 jvm=0xfffffff2e32cad28 ... snext=17 buf0=16`), `stall bufinfo` still
describes handle 16 as 16384 B at that address and reads zeros, and the fence page reads 2 through
both the cacheable alias and DRAM (`stall fencepage: read=0 cached=2 dram=2`), so the readers are
validated on this boot. The later wire record with a valid Turnip stream is pid 29 (mediaserverd),
a different process and VM (the same addresses because Turnip's allocator and the client IOVA heap
are deterministic), so wf69's "second submission with content" was another process, not a
resubmission. The stall also finds a live mapping with handle 10 over the IB address while the job
named 16 (`stall fencepage ... handle=10`): either two live mappings cover one GPU address, or the
address-based readers return the first matching mapping and ignore the region. A page of zeros
decodes as type-0 PM4 (normally a no-op; that it is harmless on this part is itself unverified),
while `RBBM_INT_0_STATUS` carries `CP_HW_ERROR` and `CP_INTERRUPT_STATUS` (which
names the SQE error) was never read, so whether the CP fault is caused by the IB content at all is
still open. The wf71 kernel therefore adds a full mapping dump at wire and stall time and the CP
fault registers. **[verified: out/wf70-runs/run1.log]**

**wf71 result — the diagnostic kernel is valid, the image is not.** The kernel
(sha256 `65e94a034c1eaa2615cca149a95a1804380a8de5eb162b84e3d829aaf14634f3`) contains the new
diagnostics, but the payload was packed without `sheng-gpu-metal=1 sheng-gpu-addon=1`:
`publishGpu()` never ran, backboardd's plugin opens returned `0xe00002f0` (kIOReturnNotFound), and
no client submission occurred. Repack the same kernel with those arguments as wf72 (verify
`payload.json`) and run it; the continuation handoff is
`docs/handoff-20261102-gpu-first-submit-stall.md`. **[verified: out/wf71-runs/run1.log;
out/wf71/picasso/payload/payload.json]**

**One operational trap.** `unixnu build kernel` runs the archive stages before the XNU pass, and
those archives read `out/picasso/tree/build/xnu/ios-obj/.../link.filelist` (written by the previous
XNU pass) to survey kernel symbols. Interrupting a build leaves the XNU object tree pruned while
that list still names the removed objects, so the next build dies at archive 1 with
`nm: ... startup.o: No such file or directory`. The list is only a survey input and the next XNU
pass rewrites it: filter the missing entries (keep the lines whose files exist) and rebuild; the XNU
pass recreates the objects and the file. Do not delete the object tree.


## 13. Boot time: the UEFI menu timeout is 3 s of the first-console time

Method: `/tmp/measure-boot.sh` starts a libusb reader on 1209:0001 before issuing `fastboot boot`
and prints the host time to the first CDC console byte (`Darwin Kernel Version ...`). Each image
was measured twice.

- baseline (`timeout = 3`, out/wf28-20261102): 22.637 s, 22.581 s → **22.61 s** mean, 0.06 s spread
- `timeout = 0`, `fallback_timeout = 1` (out/wf16-20261102): 19.650 s, 19.682 s → **19.67 s** mean

The 2.94 s (13.0 %) difference is the UEFI menu's input wait. Neither image changes anything else
about the boot (same kernel, same args, `sheng-gpu=0`). The default payload TOML now uses
`timeout = 0` with `fallback_timeout = 1` so an unattended boot skips the menu while a keypress
still opens it.

Other candidates not yet measured: `picasso_usb_control_ms=15000` (host enumeration wait),
`native-ufs-start-delay=5`, `offline-hid-start=480` and `sheng-touch-start=60` only gate the
probe timing, not the console byte, so they do not affect this metric. The first console byte is
printed by the XNU kernel; earlier time is UEFI + payload load.
