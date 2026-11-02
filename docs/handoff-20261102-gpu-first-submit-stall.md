# Handoff — the first client GPU submission stalls (Picasso / Adreno 620)

**Status, 2026-11-02 evening (CST).** Worktree `stage7-native-watchdog`, branch
`stage12/picasso-cpufreq-big`: `befd700` on top of `801950c` carries the diagnostics described in §7
and the first version of this document; docs-only commits on top carry the review corrections.
The Metal plugin now opens the published GPU service, allocates BOs and submits its first command
buffer, but that submission never completes: the CP stalls with `CP_HW_ERROR` and the IB page it
names reads as zero at submit time and at stall time through two independent readers. **There is no
fix yet.** The diagnostic kernel built for the next measurement is correct, but it was packed into
an image without the GPU boot arguments, so the run (wf71) never published the service and
produced no submission evidence. The immediate next step is to repack the same kernel correctly
(wf72) and run it.

The running project handoff is `docs/handoff-20261102-ui-frame-cpu-topology.md`; its §12.3 records
the wf54–wf71 result and must be kept current. This document is the focused continuation of that
section.

## 0. Starter prompt for the next agent

The prompt below is self-contained and can be pasted as the next session's first message. It is
reproduced here so the handoff survives outside the chat.

> 继续 UniXNU Picasso（Redmi K30 5G，SM7250，Adreno 620，iPadOS 17.7.11 userland）的移植工作。当前唯一目标：解释并修复第一个客户端 GPU 提交卡死（Metal 插件经 Turnip 提交的 IB 在 submit 时刻和 stall 时刻都被两个独立读取路径读为零；CP 以 CP_HW_ERROR 停住）。先完整阅读交接文档 `docs/handoff-20261102-gpu-first-submit-stall.md` 和 `docs/handoff-20261102-ui-frame-cpu-topology.md` §12.3。
>
> 当前证据：wf70 已用 pid/VM/VM next-handle 三重身份锁定卡死的是 backboardd（pid 56）的第一次提交（h=16，iova 0x100027000，756 B；wire 与 stall 均读零；fence page 经 cacheable alias 和 DRAM 都读到 2，证明读取器正常）；wf70 中同地址后有内容的记录属于 mediaserverd（pid 29）的另一个进程/VM，不是同进程重提。wf71 的诊断内核本身有效（sha256 65e94a034c1eaa2615cca149a95a1804380a8de5eb162b84e3d829aaf14634f3，含 `PICASSO_GPU map`/`stall cpregs`），但镜像漏了 `sheng-gpu-metal=1 sheng-gpu-addon=1`，GPU 服务未发布，wf71 作废。
>
> 立即动作：用同一个内核重新打包 payload（只重打 payload/boot image，不重编内核），内核参数必须带 `sheng-gpu-metal=1 sheng-gpu-addon=1`，生成 wf72；打包后必须检查 `out/wf72/picasso/payload/payload.json` 的 `kernel_args` 含这两个参数、`kernel_sha256` 与 `shasum -a 256 out/picasso/kernel/kernel.macho` 一致；然后重启到 fastboot（若已在 fastboot 则跳过）、`fastboot boot`、抓串口；读取新的 `PICASSO_GPU map wire:`、`map stall:`、`stall cpregs:`、`stall ids:`、`table switch:` 行，按文档 §6.4 的决策表判断。
>
> 必须遵守：只用 `fastboot boot`，禁止刷分区；串口 `/dev/cu.usbmodemPICASSO011`，采集前确认没有进程占用；每轮先探测设备状态（`/dev/cu.usbmodem*`、`fastboot devices`），不要沿用旧值；build 需要 `export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` 和 `export UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed`；不要 reset/checkout 共享工作树；一次只改一个变量，每个结论必须带证据（日志行/寄存器/测量）；里程碑 commit 并更新 PR #13；多看官方实现（mainline msm/a6xx、Turnip、XNU IOKit），不要新造轮子；用 subagent 做广度调查以节省上下文；继续工作，直到目标完成。

## 1. Mission and standing constraints

The governing task (unchanged): boot the real iPadOS 17.7.11 userland on the Redmi K30 5G
(`picasso`, SM7250, Adreno 620) with the UniXNU kernel, through display and GPU/Metal. Current
stage: stage 5, the Metal plugin in the image and the first client submission.

Standing constraints, all of which have been enforced in this line of work:

- **`fastboot boot` only. Never flash a partition.** The boot image is loaded and run from RAM.
- Serial console `/dev/cu.usbmodemPICASSO011`; the fastboot serial observed in these runs is
  `52cc791d` (re-check with `fastboot devices` after the reboot). Stop any process that owns the
  CDC console before starting a capture (see §9).
- Everything is built with
  `export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` and
  `export UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed`.
- Do not reset or `git checkout` the shared worktree; it carries the built images and run logs this
  document cites.
- One variable per change; a conclusion without a measured artifact is not a conclusion.
- A milestone is a commit; at major progress push and update PR #13
  (`YinCQL/UniXNU-picasso`, head `silenzio111:stage12/picasso-cpufreq-big`).
- Check official implementations (mainline `msm`/`a6xx`, Mesa/Turnip, XNU IOKit) before inventing.
- Use subagents for broad, read-only investigation to keep the main context targeted.

## 2. Where the work stands

The plugin path works up to and including submission:

- The staged `AGXAccelerator` is published (`publishMetal()`, `Adreno620Cp.cpp`) and backboardd
  (pid 56) opens it with the plugin's open type `0x4147`:
  `PICASSO_GPU open: plugin type=4147 pid=56 name=backboardd`, `plugin: mount ok opens=1`.
- Turnip in the plugin probes the ABI (v2), allocates five BOs, maps them and CPU-maps them; the
  tunnel calls 2/3/13 all return 0, and each client view maps the BO's own first page
  (`PICASSO_GPU view: token=1..5 ... first=<BO span 0>`).
- Its first command buffer is accepted: submit returns 0 (`tunnel: call=5 status=0`). The submit
  record is one buffer: handle 16, GPU address `0x100027000`, 756 bytes inside a 16 KiB BO. The
  kernel translates it into `CP_INDIRECT_BUFFER` (`70bf8003 27000 1 bd`: opcode `0x3f`, address
  `0x1_000027000`, 0xbd = 189 dwords) and appends it to the ring.
- The CP does not complete it. Every wait on its fence times out (`tunnel: call=6 status=e00002d6`,
  `kIOReturnTimeout`); the driver records the stalled fence 3 and the hang check runs
  (`hang: no restart hook ...`). The board installs no restart hook, so the hang is counted only.

## 3. The evidence (wf70)

Run: `out/wf70-runs/run1.log` (10 456 lines). Kernel from `out/wf70/picasso/payload/payload.json`:
sha256 `e97cd19d7d7c102a9fcb4eb00f18f25e5d9c3738c3f871ff951d3c2a2e570cb1`; args include
`sheng-gpu-metal=1 sheng-gpu-addon=1`. Addon blob sha256
`0dbe0ef2c4e765b9d5f25dcf3eb197ef7b3141e97a5898f8f0e10e0f26cc655a` (unchanged since wf60).
All lines below are from that log.

Timeline:

```
2263  PICASSO_GPU plugin: mount ok opens=1                       (backboardd, pid 56)
2279  PICASSO_GPU view: token=1 bytes=20480 wc=1 pages=5 first=e3e29000 span=e3e29000/4096 spans=5
2283  PICASSO_GPU view: token=2 bytes=131072 wc=1 pages=32 first=e4486000 ... spans=31
2287  PICASSO_GPU view: token=3 bytes=4096 wc=1 pages=1 ...
2291  PICASSO_GPU view: token=4 bytes=4096 wc=1 pages=1 first=e469c000 span=e469c000/4096 spans=1
2295  PICASSO_GPU view: token=5 bytes=16384 wc=1 pages=4 first=e42de000 span=e42de000/4096 spans=4
2296  PICASSO_GPU wire: pid=56 vm=0xfffffff2e32cad28 next=17 h=16 iova=100027000 bytes=756
      map=100027000/16384 sample=0/0/0 bo= 0 0 0 0 0 0 0 0
2297  PICASSO_GPU table switch: vm=0xfffffff2e32cad28 ttbr0=100000e3f6a000
2298  PICASSO_GPU tunnel: call=5 status=0
2299  PICASSO_GPU tunnel: call=6 status=e00002d6                 (repeats)
2311  PICASSO_GPU stall: fence=3 pending=50482 us cpFence=2 rptr=27 wptr=36
2312  PICASSO_GPU stall cp: 800005 0 0 0 900200 0 8000430e 0 24 24 27000 1 0 0 0 0 deafbead...
2313  PICASSO_GPU stall smmu: 400 1339f508 13443 70813 job=11
2314  PICASSO_GPU stall job: count=1 words=4: 70bf8003 27000 1 bd
2316  PICASSO_GPU stall ib1 100027000 read=0: 0 0 0 0 0 0 0 0
2317  PICASSO_GPU stall fencepage: read=0 cached=2 dram=2 handle=10
2318  PICASSO_GPU stall ids: pid=56 fence=3 jvm=0xfffffff2e32cad28 vm=0xfffffff4b0106d80
      space=0xfffffff2e32cad28 snext=17 buf0=16
2319  PICASSO_GPU stall bufinfo: describe=0 iova=100027000 bytes=16384 perm=1 read=0: 0 0 0 0 0 0 0 0
2320  PICASSO_GPU stall ib1c: 0/0 0/0 0/0 0/0 0/0 0/0 0/0 0/0
2337  PICASSO_GPU hang: no restart hook read=0 ttbr0=100000e3f6a000 cpFence=2 rptr=27 wptr=36
...   (the same fence is counted as hung four times; the stall block repeats at the 50 ms and 1 s snapshots)
3438  PICASSO_GPU wire: pid=29 vm=0xfffffff2e32ca928 next=17 h=16 iova=100027000 bytes=756
      map=100027000/16384 sample=0/70460001/70460001
      bo= 70460001 31 40bb0801 fffff 70268000 48980401 1f 40980501
```

Reading it:

- **The stalled job is backboardd's first submission.** `wire` names pid 56, VM
  `0xfffffff2e32cad28`, handle 16, iova `0x100027000`, 756 bytes; the stall for fence 3 names the
  same job (`jvm`/`space` `0xfffffff2e32cad28`, `buf0=16`, `snext=17` = the next handle at submit).
  `describe(16)` at stall time still finds the 16 KiB mapping at `0x100027000`.
- **The page is zero through both readers, and both readers are validated on this boot.**
  `sample=0/0/0` means `sampleWord` succeeded and the cacheable kernel alias and the fresh uncached
  DRAM read both returned 0. The fence page control reads 2 through both (`cached=2 dram=2`), so the
  readers work. The stall's `bufinfo` and `ib1c` reads agree: zeros.
- **The later "content" record is a different process.** Line 3438 is pid 29 (mediaserverd; its
  `open: plugin ... pid=29` is at 2890) in a different VM (`0xfffffff2e32ca928`). It names the same
  handle/iova/bytes because Turnip's deterministic allocator and the client IOVA heap produce the
  same addresses in each process. This corrects the wf69 reading: that was not the same process
  resubmitting with data. The boot-time probe at line 731 prints the same VM pointer only because
  `checkCpSwitch()` creates a temporary VM and destroys it after the probe; whether mediaserverd's
  later VM is that object or a new one at a reused address is not established by the pointer alone.
  Do not read pointer identity across wf70 lines 731 and 3438 as the same address space; the wf72
  `map wire`/`map stall` dumps (handles and region flags) settle it.
- **The second VM pointer is the published kernel VM.** `stall ids ... vm=0xfffffff4b0106d80` is
  `NativeAdrenoSubmit::vm_`, the default VM installed at bring-up; the job's own VM is `jvm`. The
  per-process table was switched in (`ttbr0=100000e3f6a000`, ASID 0x1000, line 2297), so the switch
  path works.
- **Handle 10 covers the same address as handle 16.** `stall fencepage ... handle=10` is
  `space->handleAt(0x100027000)`; the job named handle 16 and `describe(16)` succeeds. `handleAt`
  and the address-based readers (`sampleWord`, `peekWords`, `peekWordsUncached`) scan the mapping
  array from the first slot and return the **first** mapping that covers the address, without
  considering the region (`upper`). So either two live mappings cover one GPU address (which the
  per-region `preflight` overlap test is supposed to reject), or the first match is an upper-region
  mapping at the same numeric address. The wf71 diagnostic mapping dump exists to decide this.

### 3.1 Why the zero IB may not be the root cause

A page of zero dwords decodes as type-0 PM4, which Adreno programmers normally treat as a no-op.
**If** that holds on this part and ucode, executing the IB is harmless, and the CP should then
execute the ring's `CP_EVENT_WRITE`/`CACHE_FLUSH_TS` fence and write fence 3. It does not:
`cpFence` stays 2 and `RBBM_INT_0_STATUS = 0x900200` includes `CP_HW_ERROR` (bit 9). Whether type-0
is really harmless here is itself unverified; `stall cpregs` says whether the SQE reported an
opcode error at all. The
`CP_INTERRUPT_STATUS` register, which names the SQE error (opcode/ucode/hw-fault/register
protection/AHB/parity/illegal instruction), **was never read** on this stall. wf71's kernel adds
exactly that read (`native_adreno_cp_fault_regs`, `stall cpregs:`), together with the mapping dump.

Two consequences for the next agent:

1. Do not assume the zero page is the cause until `stall cpregs` names an error that the zero page
   explains. The CP may be failing at the ring's next packet or on a table/context fetch while the
   IB content is irrelevant.
2. The `STALL` readers may be reading the wrong mapping (handle 10) if two mappings overlap; the
   wire-time zero could be a reader artifact rather than the client's BO content. The dump is the
   discriminator.

## 4. Ruled out so far

- **ABI version skew.** The kernel reports `NativeAdrenoAbiVersion = 2` and the deployed client
  proceeds past its version check (`tu_knl_darwin.cc` `dw_open_connection`); a mismatch would
  refuse every later call.
- **A cache-mode mismatch as the reason the IB reads zero.** Turnip asks for a write-combined CPU
  view (`kIOMapWriteCombineCache`, `tu_knl_darwin.cc:493`) and a non-cacheable GPU mapping
  (`writeback=0 shareability=2`, line 437); those are two different mappings. The kernel's `view:`
  probe shows the CPU view maps the BO's own pages (`wc=1`, `first=<span 0>`, all five views in
  wf68), and the fence page — written by the CP — reads 2 through both the cacheable alias and DRAM,
  so the readers are sound. Whether a client write could still be lost in a write-combine buffer is
  not settled by this, but the view does not point at other memory.
- **Dirty CPU cache lines masking the writes.** The fence page reads 2 through both the cacheable
  alias and a fresh uncached mapping (wf68 and wf70); the IB page reads zero through both. A dirty
  line would show in the cacheable read; it does not.
- **The board's probe path.** The board's own NOP submission on a fresh VM completes
  (`cpcheck: mode=1 result=0 ... fence=2`), and the boot-time wire probe reads its own IB word
  (`pid=0 ... sample=0/70108000/70108000`). The ring, the table switch and the fence path work for
  the board.
- **A stale same-process resubmission.** The "second wire record with content" is mediaserverd
  (pid 29), not backboardd (see §3).

Still open, not ruled out: the client writing a different BO/offset than the one it submits; a VM
mapping overlap/aliasing; the CP failing on the ring's next packet or the context/table rather than
on IB content; a client-side recording bug in the Darwin KNL adaptation.

## 5. Open hypotheses and the measurement that separates them

- **H1 — the client's stream is in a different BO (or at a different offset).** The 756 bytes were
  recorded through `bo->map`, but the submitted entry names another BO/offset; or the kernel's
  address-based read resolves a different mapping. *Discriminator:* the wf72 `map wire` dump lists
  every mapping of the submitting VM with its own first four words. If the Turnip stream
  (`70460001 31 40bb0801 ...`) appears at another mapping/handle, the client wrote elsewhere. If no
  mapping's head has it, scan the whole submitted range (not just the first eight words) through
  the named handle.
- **H2 — two mappings cover one GPU address.** `handleAt` returned 10 while the job named 16;
  `preflight` checks overlaps per region, while the readers ignore region and slot order. *If two
  live lower-region mappings overlap, the map path has a validation bug to find (`order_`,
  `insert`/`erase`, `preflight`).* If one is an upper-region mapping, the reads are wrong by
  construction and the CP's table (lower region) must be checked separately. *Discriminator:* the
  `up`/`slot` fields of the dump, and whether handle 10's slot precedes handle 16's.
- **H3 — the client never wrote that BO.** The plugin's recording and submission are on the same
  thread; the runtime's primer records begin/barrier/barrier/end then raw `device.queue_submit`.
  *Discriminator:* compare the client's `tu_cs_entry` (`bo->darwin_handle`, `entry.offset`,
  `entry.size`) with the kernel's wire record, and instrument `darwin_bo_map`/`tu_cs_add_entry`
  (client side) to print the map pointer and the first words written.
- **H4 — the CP error is not about the IB.** The ring's next packet, the fence write, or the
  context table fetch is failing. *Discriminator:* `stall cpregs` (`CP_INTERRUPT_STATUS` bits:
  0 opcode, 1 ucode, 2 hw fault, 4 register protection, 5 AHB, 6 VSD parity, 7 illegal
  instruction), plus `CP_HW_FAULT`, `CP_PROTECT_STATUS` and the SQE sample.

## 6. Immediate next experiment: wf72

The kernel to use is the one already built and validated at
`out/picasso/kernel/kernel.macho`, sha256
`65e94a034c1eaa2615cca149a95a1804380a8de5eb162b84e3d829aaf14634f3` (it contains the strings
`PICASSO_GPU map %s`, `stall cpregs` and the symbol `_native_adreno_cp_fault_regs`). It contains the
wf71 diagnostics: the full mapping dump at wire time (`wire_prints <= 4`) and at stall time (first
two stalls), `CP_INTERRUPT_STATUS`/`CP_HW_FAULT`/`CP_PROTECT_STATUS`/SQE sample on a stall, and the
first `table switch:` lines.

**wf71's lesson:** the kernel was fine; the *image* was packed without `sheng-gpu-metal=1
sheng-gpu-addon=1` (see `out/wf71/picasso/payload/payload.json`). Without them `publishGpu()` and
`publishMetal()` never run, no `IOAcceleratorES` service exists, backboardd's opens return
`0xe00002f0` (`kIOReturnNotFound`), and there is no submission at all. Always verify the packed
arguments before booting.

### 6.1 Build the payload and boot image (no kernel rebuild)

```sh
cd /Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog
shasum -a 256 out/picasso/kernel/kernel.macho          # must be 65e94a03...
/tmp/build-wf-addon.sh wf72 build/ngpu-addon-wf60.bin sheng-gpu-metal=1 sheng-gpu-addon=1
```

`/tmp/build-wf-addon.sh TAG ADDON [extra args...]` restores `board.toml` on exit, packs
`--owner-trust-cache .../owner-trust.bundle` and `--gpu-addon build/ngpu-addon-wf60.bin`, and writes
`out/wf72/picasso/payload/payload.img` and `out/wf72/picasso/boot/wf72.img`. The addon blob (the
userspace plugin, `NGPUADD2`) is unchanged since wf60; the kernel is the only changed component
relative to wf70.

### 6.2 Verify before booting (mandatory)

```sh
python3 - <<'PY'
import json,hashlib,pathlib
p=json.loads(pathlib.Path('out/wf72/picasso/payload/payload.json').read_text())
sha=hashlib.sha256(pathlib.Path('out/picasso/kernel/kernel.macho').read_bytes()).hexdigest()
print('kernel_args:', p['kernel_args'])
print('kernel sha:', p['kernel_sha256'], 'match:', p['kernel_sha256']==sha)
print('addon sha:', p['gpu_addon_sha256'])
assert 'sheng-gpu-metal=1' in p['kernel_args'] and 'sheng-gpu-addon=1' in p['kernel_args']
assert p['kernel_sha256']==sha
PY
```

Expected addon sha `0dbe0ef2c4e765b9d5f25dcf3eb197ef7b3141e97a5898f8f0e10e0f26cc655a`.

### 6.3 Reboot to fastboot, boot, capture

Probe the device state first (`/dev/cu.usbmodem*`, `fastboot devices`, `lsof`). If it is already in
fastboot, skip the reboot step; if it is running, reboot to fastboot first. The capture tool and
the CDC command tool must not run at the same time as another reader of the console.

```sh
cd /Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog
# 1) reboot to fastboot through the CDC console (stops the console; waits for fastboot)
python3 scripts/picasso-cdc-command.py --command reboot-fastboot \
    --fastboot-serial 52cc791d --fastboot-timeout 300
# 2) start the capture before booting (it waits for the CDC device to appear)
mkdir -p out/wf72-runs
python3 scripts/capture-picasso-usb-raw.py --output out/wf72-runs/run1.log \
    --wait 600 --duration 480          # run in the background or a second terminal
# 3) boot the image from RAM; never flash
fastboot -s 52cc791d boot out/wf72/picasso/boot/wf72.img
```

If `fastboot devices` shows a different serial, use that value. The capture lasts 480 s from the
moment the device is found; the plugin mounts and submits within roughly the first minutes of
userland boot (wf70: mount at log line 2263, submit at 2296, stall at 2311).

### 6.4 Lines to read and the decision table

```sh
grep -aE "PICASSO_GPU (wire|map (wire|stall)|stall|hang|table switch|plugin: mount|open: plugin|tunnel: call=5)" \
    out/wf72-runs/run1.log
```

The new lines are `map wire:` / `map stall:` (one line per live mapping: slot, handle, iova, bytes,
spans, permissions, wb/sh/up, sys/wc/imp, first four words read through the uncached reader) and
`stall cpregs:` (cpint/hwfault/protect/sqe).

- **The stream is at another handle.** H1: the client wrote a different BO than it named. Next:
  instrument the client's `darwin_bo_map`, `tu_cs_add_entry` and `darwin_submit_add_entries`
  (`.cache/mesa-work/src/freedreno/vulkan/tu_knl_darwin.cc`, `tu_cs.cc`; line references in §11);
  compare the `tu_cs_entry` `{bo->darwin_handle, offset, size}` with the kernel's wire record.
- **The stream is not at any mapping's head.** Scan the whole submitted range: the kernel currently
  prints the first eight dwords at the wire iova and four per mapping; extend the diagnostic to
  read all `buffer.bytes` (756 B) through the *named handle* and print the first nonzero offset (see
  §8 item 2). A stream at a nonzero offset inside the same BO means the client's base-pointer/offset
  arithmetic is wrong.
- **Two live mappings cover `0x100027000` (handles 10 and 16).** H2. Check their `up`/`slot`
  fields and the order of insertion. If both are lower-region live mappings, the overlap should
  have been rejected by `preflight()`; find how it passed (`order_`/`insert`/`erase` and the
  `orderAt` comparison) and fix it. If handle 10 is upper-region, the address-based readers are
  reading the wrong table; read through the named handle and check which table the CP runs
  (`ttbr0` of the job's VM).
- **`cpregs` names an SQE error.** Follow the bit to the failing engine. `CP_OPCODE_ERROR` with a
  zero IB suggests a fetch after the IB or a wrong IB length; `CP_HW_FAULT_ERROR`/`CP_AHB_ERROR`
  point at a table/context access; `CP_REGISTER_PROTECTION_ERROR` at a protected register write.
  The CP's `CP_HW_FAULT` and `CP_PROTECT_STATUS` values are printed in the same line.
- **`cpregs` is 0.** `CP_INTERRUPT_STATUS` may have been cleared or sticky; compare with
  `RBBM_INT_0_STATUS` and read the CP registers at wire time (before the timeout), not only after.

## 7. What is in the tree (committed with this document)

Base commit `801950c` ("picasso: register AGXAccelerator and submit through an A620 user client").
The commit adds the fixes already validated on hardware and the diagnostics used by wf63–wf71:

- `drivers/NativeAdrenoPicasso/Adreno620Cp.cpp`
  - `info.flags = NativeAdrenoDeviceClientIova` in `publishGpu()` — the fix for the zeroed VMA heap
    (Bug #1, wf59→wf61).
  - `NativeSandboxGrantProcess` literal process-name list — the kernel's grant compares
    `proc_selfname` literally, so `*` never matches (accelerator and grant, §12.3).
  - `native_adreno_dram_read32` / `native_adreno_cached_read32` board readers (temporary uncached
    mapping and `phystokv` alias) installed at `native_adreno620_cp_start`.
  - `native_adreno_cp_fault_regs()` — reads `CP_INTERRUPT_STATUS`, `CP_HW_FAULT`, `CP_PROTECT_STATUS`
    (`0x824`) and one `CP_SQE_STAT` sample; called by the submit stall print through a weak import.
  - The boot-addon report thread (`sheng-gpu-addon=1`): addon mount stages, submission counters,
    plugin diag, mount failures, table-switch and CP-check counters.
- `drivers/NativeAdreno/NativeAdrenoRing.h` — `noteExternalSubmit()` (Bug #2, wf61→wf62): adopts the
  write pointer/fence of the board's post-publish probe so the next submit is not refused as a full
  ring.
- `drivers/NativeAdreno/NativeAdrenoPublish.cpp` — view probe (`PICASSO_GPU view`), tunnel call
  status, plugin open lines, mount-failure line, hang-without-restart line, and the diag accessors
  consumed by the board report.
- `drivers/NativeAdreno/NativeAdrenoSubmit.cpp` — `wire` print (pid, VM, next handle, handle, iova,
  bytes, mapping extent, `sampleWord`, BO head), `table switch` print, and the stall block: CP/SMMU
  state, `stall cpregs`, `stall job` (the PM4 words the kernel built), `stall ib`, `stall ib1`,
  `stall fencepage`, `stall ids`, `stall bufinfo`, `stall ib1c`, and the mapping dumps.
- `drivers/NativeAdreno/NativeAdrenoVm.{h,cpp}` — `dumpMappings(tag, limit)`: every live mapping
  with slot/handle/iova/bytes/spans/permissions/attributes/region/kind and its first four dwords
  through the uncached reader.
- `docs/handoff-20261102-ui-frame-cpu-topology.md` — §12.3 wf54–wf71 narrative with the wf70/wf71
  results.
- `docs/handoff-20261102-gpu-first-submit-stall.md` — this document.

The diagnostics are deliberately bounded (`wire_prints < 8`, `stall_prints < 6`, one dump per tag,
`tunnel_calls <= 32`, etc.) so a stalled fence cannot flood the console. They are evidence
instrumentation, not production behavior; remove them once the failure is understood and fixed
(§10).

## 8. If wf72 is inconclusive

In priority order, one change at a time:

1. **Read through the named handle, not the address.** Add a VM helper that reads `k` dwords from a
   mapping found by `find(handle)` (the BO's own `NativeAdrenoMemory`), bypassing the first-match
   address scan. Print the whole submitted range `[buffer.iova, buffer.iova+buffer.bytes)` at wire
   time. This answers "is the client's stream in handle 16's BO at all" without the overlap
   ambiguity.
2. **Extend the wire print to the full buffer.** Print the first 32 dwords and a scan result
   (`first nonzero offset`, `nonzero count`) of the submitted range; a stream at a nonzero offset
   points at client offset/pointer arithmetic.
3. **Instrument the client path** (read-only sources):
   `darwin_bo_map` (the map returned by `BoCpuMap`/`IOConnectMapMemory64`), `tu_cs_add_bo` /
   `tu_cs_add_entry` (the entry offset/size units and the BO pointer), `darwin_submit_add_entries`
   (the `{darwin_handle, iova, bytes}` it sends). Print `bo->iova`, `bo->darwin_handle`, the map
   pointer and the first words at the map; the plugin's text channel is closed by the sandbox, so
   route the output through the existing marker/tunnel report or the kernel's `wire` record.
4. **Check the CP's next packet.** The ring block after the job's IB packet is the
   `CP_EVENT_WRITE`/`CACHE_FLUSH_TS` fence. Dump the ring words at the job's write pointer and the
   fence packet's address, and verify that address is mapped in the process VM with write
   permission (the fence page read proves the mapping exists, but not that the CP's write is
   permitted).
5. **If a genuine VM overlap exists, fix validation** (make `preflight`/`order_` reject two live
   ranges in one address space, and make the address-based readers region-aware) before touching
   the client.

## 9. Environment, build and run (complete)

- Worktree: `/Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog`,
  branch `stage12/picasso-cpufreq-big`. The build tree is `out/picasso/tree`; `tree/drivers`
  symlinks to this worktree, but `tree/package` is a stale cache copy (copy package edits and
  verify with `strings`; see the project memory).
- Kernel build (only needed if the kernel changes):
  ```sh
  export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  export UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed
  cd /Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog
  python3 unixnu build kernel prelink        # archives 1..11, XNU, prelink; result: out/picasso/kernel/kernel.macho
  ```
  The prelink slide is fixed at `-0x1000000` by `prelink-picasso.py` (it is part of the cached
  build recipe). Interrupting a build can prune XNU objects named by `link.filelist`; the recovery
  is to filter the list to files that exist and rebuild (never delete the object tree).
- Payload + boot image: `/tmp/build-wf-addon.sh TAG ADDON [extra args...]` (see §6.1). The script
  restores `board.toml` on exit; do not run two builds concurrently.
- Images: `out/<tag>/picasso/boot/<tag>.img`; payload report `out/<tag>/picasso/payload/payload.json`.
- Reboot to fastboot over CDC:
  ```sh
  python3 scripts/picasso-cdc-command.py --command reboot-fastboot \
      --fastboot-serial 52cc791d --fastboot-timeout 300
  ```
  The CDC command list (`--command help`): `help | status | ps | wait <pid|name> | stub-chrono <pid>
  | stub-lsd <pid> | panic-test | reset-test | start-graphics | register-display |
  publish-display | publish-display-start | hang-test | reboot-fastboot`.
- Boot from RAM only: `fastboot -s 52cc791d boot out/<tag>/picasso/boot/<tag>.img`.
- Capture the raw console (libusb; avoids the macOS ACM/tty hang):
  `python3 scripts/capture-picasso-usb-raw.py --output out/<tag>-runs/run1.log --wait 600 --duration 480`.
  Start it before `fastboot boot`; it waits for the CDC device. Stop every other reader
  (`lsof /dev/cu.usbmodemPICASSO011`, a previous `capture-picasso-usb-raw.py`,
  `picasso-cdc-command.py`, `screen`, `cat`) first: two readers make the capture fail or drop
  lines.
- The device's current state can be probed read-only: `/dev/cu.usbmodem*`, `fastboot devices`,
  `ioreg -p IOUSB -l`. Do not freeze these values in the handoff: probe them every round.

## 10. Pending work after the stall is fixed

1. **Complete the first submission and the fence.** Verify `tunnel: call=5/6` return success, the
   fence advances, and the plugin reports device creation (marks `0x68xx`/`0x69xx`). Then confirm
   the subsequent submissions (per-frame command buffers) retire.
2. **Metal registration and the render server.** Verify the plugin's `_MTLRegisterDevices` path,
   subsequent submissions, telemetry, and the UI/display hand-off (stage 5).
3. **Install the missing `native_adreno_line_audit` board hook** if write-combined auditing is
   needed; today `auditRange()` returns `kIOReturnUnsupported` and the driver's WC distrust path is
   inert. The wf68/70 evidence says the current failure is not a dirty line, so this is not first.
4. **Remove the bounded diagnostics** added for wf54–wf71 once the failure is fixed, in a separate
   commit, and keep only the counters the board report needs. The one-variable discipline applies:
   do not mix a fix and a diagnostic cleanup in one commit.
5. **Boot-time reduction task** (from the governing goal, §13 of the project handoff): the UEFI
   menu timeout change is measured (22.61 s → 19.67 s). Next candidates to measure:
   `picasso_usb_control_ms=15000`, `native-ufs-start-delay=5`, `offline-hid-start=480`,
   `sheng-touch-start=60` (the latter two gate probe timing, not the console byte).
6. **Commit and PR.** This line's diagnostics are committed with this document; at the next
   verified milestone (the first submission completes, or the failure is conclusively named), push
   and update PR #13 with the evidence lines and the run directory.

## 11. Code map

Kernel side (this worktree):

- `drivers/NativeAdreno/NativeAdrenoSubmit.cpp`
  - `submit()` — validates every buffer against the VM (`describe`), builds one
    `CP_INDIRECT_BUFFER` per buffer (`SubmitIndirectBuffer = 0x3f`, 3 payload words: iova lo/hi,
    size in dwords), holds the job, `dsb sy`, prints the wire state, audits/publishes the mapped
    BOs, appends the ring block.
  - `noteStall()` — the stall/hang diagnostics (`stall ...` lines) and the CP fault-register hook.
  - `useVm()` — the TTBR0 table switch (`table switch:` line).
- `drivers/NativeAdreno/NativeAdrenoRing.{h,cpp}` — `noteExternalSubmit()` (adopt the board probe's
  wptr/fence), `ibBefore()`/`fenceIova()`/`fenceValue()`, `submit()` (a6xx plain profile when
  `setA6xxPlain(true)`), `wait()`.
- `drivers/NativeAdreno/NativeAdrenoVm.{h,cpp}` — `map`/`unmap` (`preflight` per region),
  `describe`, `handleAt`, `dumpMappings`, `sampleWord`, `peekWords`, `peekWordsUncached`,
  `faultState`, `publishMapped`, `auditRange`.
- `drivers/NativeAdreno/NativeAdrenoPublish.cpp` — service publication, `AGXDeviceUserClient`
  (`clientMemoryForType`, `externalMethod` tunnel dispatch, `mount`, hangcheck).
- `drivers/NativeAdrenoPicasso/Adreno620Cp.cpp` — board bring-up (`native_adreno620_cp_start`),
  `publishGpu()` (VM/ring/submit attachment, `NativeAdrenoAbiDeviceInfo`), `publishMetal()`
  (AGXAccelerator staging and properties), diagnostics/report.

Client side (read-only reference copies):

- `/Users/silenzio/Desktop/development/ios/UniXNU/.cache/mesa-work/src/freedreno/vulkan/tu_knl_darwin.cc`
  — `darwin_bo_init`, `darwin_bo_map` (BoCpuMap + `IOConnectMapMemory64` with
  `kIOMapAnywhere|kIOMapWriteCombineCache`), `darwin_submit_add_entries` (~626),
  `darwin_submit_chunk`/`darwin_queue_submit` (~800–900), `darwin_device_init`.
- `.../vulkan/tu_cs.cc` — `tu_cs_add_bo` (~141), `tu_cs_add_entry` (~223), `tu_cs_get_offset`,
  `tu_cs_end` (~288); `tu_cs.h` — `tu_cs_emit_pkt7`/`CP_INDIRECT_BUFFER` (4 dwords per packet).
- `/Users/silenzio/Desktop/development/ios/UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/device/prime.rs`
  — `Primer::batch`: pool/buffer, begin, two `vkCmdPipelineBarrier`s, end, raw
  `device.queue_submit` (the first client submission's shape).
- Plugin source (main checkout, not this worktree): `drivers/NativeMetal/NativeAdrenoMetalPlugin.mm`
  and `scripts/build-adreno-metal-plugin.py`; the plugin bundle is packed into the addon by
  `scripts/make-gpu-addon.py`.

## 12. Traps and lessons (all encountered)

- **The wf71 image trap (new).** Building a payload with `/tmp/build-wf-addon.sh` does not add
  `sheng-gpu-metal=1`/`sheng-gpu-addon=1` by itself; they are extra arguments. A run without them
  looks normal in the boot log but never publishes the GPU service; the only signal is
  `result=e00002f0` from the plugin's opens and no `PICASSO_GPU open: plugin` line. Always check
  `payload.json` before booting.
- **zsh `=` expansion.** `echo ===` / `echo ====` fails with `=== not found` and aborts the
  compound command; use `echo "---"`. Quote include globs (`--include='*.cpp'`).
- **Serial ownership.** The capture and CDC tools both open the same USB device; using them
  simultaneously drops or blocks data. Check `lsof` and stop stale readers first.
- **Pointer identity across boots/processes.** A VM pointer printed at boot can be an object that
  is later destroyed and whose address is reused (`checkCpSwitch` creates and destroys a VM around
  the probe). Identify a VM by pid + job + `nextHandle`, not by a raw pointer alone.
- **Build tree symlinks.** `out/picasso/tree/drivers` points at this worktree, but
  `out/picasso/tree/package` can be a stale cache copy; package edits must be copied and verified
  with `strings`. Submodule fetches need the `insteadOf` SSH rewrite (HTTPS GitHub clones fail).
- **IDE/clang diagnostics** (missing `ArmSmmu4KLive.h`, undeclared `IOMallocZero`, etc.) are
  indexer noise with a broken include path; the real kernel build (exit 0) is authoritative.
- **`link.filelist` pruning** after an interrupted build (§9).
- **`fastboot` serial** is not the CDC device name (`/dev/cu.usbmodemPICASSO011`); use
  `fastboot devices`.
- **Never flash.** Only `fastboot boot`.

## 13. Artifact and hash reference (measured 2026-11-02)

- wf70 kernel (valid for wf70, but it lacks the wf71 diagnostics): `e97cd19d7d7c102a9fcb4eb00f18f25e5d9c3738c3f871ff951d3c2a2e570cb1`;
  log `out/wf70-runs/run1.log`, 10 456 lines; args include `sheng-gpu-metal=1 sheng-gpu-addon=1`.
- Current diagnostic kernel (to use): `65e94a034c1eaa2615cca149a95a1804380a8de5eb162b84e3d829aaf14634f3`,
  at `out/picasso/kernel/kernel.macho`; contains `PICASSO_GPU map %s`, `stall cpregs`,
  `_native_adreno_cp_fault_regs`.
- wf71 payload: kernel sha `65e94a03...`, but args without `sheng-gpu-metal=1
  sheng-gpu-addon=1`; log `out/wf71-runs/run1.log`, 5 521 lines, no plugin mount, open failures
  `e00002f0` (e.g. lines 2290, 2292, 2746, 2752, 4507, 5118; nine in all). **Do not boot this image
  for GPU work.**
- Addon blob (userspace plugin): `build/ngpu-addon-wf60.bin`, sha256
  `0dbe0ef2c4e765b9d5f25dcf3eb197ef7b3141e97a5898f8f0e10e0f26cc655a`; used by wf70 and to be
  reused by wf72.
- Owner-trust bundle: `/Users/silenzio/Desktop/development/ios/UniXNU/build/adreno-metal-plugin/owner-trust.bundle`.
- Data seed: `build/owner-data-8g-noautodim-20261101.dbr`.
- Kernelcache: `/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed`.
- PR: #13, `YinCQL/UniXNU-picasso`, head `silenzio111:stage12/picasso-cpufreq-big`, open.
