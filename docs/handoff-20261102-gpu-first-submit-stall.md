# Handoff — the first client GPU submission stalls (Picasso / Adreno 620)

**Status, 2026-11-02 evening (CST).** Worktree `stage7-native-watchdog`, branch
`stage12/picasso-cpufreq-big`: `befd700` on top of `801950c` carries the diagnostics described in §7
and the first version of this document; docs-only commits on top carry the review corrections.
The Metal plugin now opens the published GPU service, allocates BOs and submits its first command
buffer, but that submission never completes: the CP stalls with `CP_HW_ERROR` and the IB page it
names reads as zero at submit time and at stall time through two independent readers. **There is no
fix yet.** wf72 ran the diagnostic kernel packed correctly (§3.1): it confirms the zero page, names
the CP's error (`CP_INTERRUPT_STATUS` = 1, `CP_OPCODE_ERROR`), and rules out the suspected mapping
overlap (the `handle=10` in the stall line is hexadecimal, i.e. handle 16). It also exposes a
diagnostic-reader defect: the "uncached" DRAM reader used for every IB read mapped its page with
the default (cacheable) attribute. wf73 (same evidence, a truly uncached reader plus a full-buffer
and whole-VM signature scan) is the current run.

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

### 3.2 wf72 result — the CP executes the zero page and reports an opcode error

Run `out/wf72-runs/run1.log` (kernel `65e94a03…`, args include both GPU flags). Timeline:

```
2435  wire: pid=56 vm=0xfffffff345e29928 next=17 h=16 iova=100027000 bytes=756 sample=0/0/0 bo= 0 0 0 0 0 0 0 0
2447  map wire: slot=11 h=12 iova=100000000 bytes=20480 sys=1 wc=1 words=0 204cc003 3f800000 204cc004
2448  map wire: slot=12 h=13 iova=100005000 bytes=131072 sys=1 wc=1 words=40a00201 fcfc 0 0
2451  map wire: slot=15 h=16 iova=100027000 bytes=16384 sys=1 wc=1 words=0 0 0 0
2452  table switch: vm=0xfffffff345e29928 ttbr0=100000d5ccc000
2453  tunnel: call=5 status=0
2494  stall: fence=3 pending=2039547 us cpFence=2 rptr=27 wptr=36
2495  stall cp: 800005 0 0 0 900200 0 8000430e 0 24 24 27000 1 0 0 0 0 deafbead...
2496  stall smmu: 400 1338f508 13c43 70903 job=11
2497  stall cpregs: cpint=1 hwfault=0 protect=0 sqe=aa
2498  stall job: count=1 words=4: 70bf8003 27000 1 bd
2501  stall fencepage: read=0 cached=2 dram=2 handle=10
2503  stall bufinfo: describe=0 iova=100027000 bytes=16384 perm=1 read=0: 0 0 0 0 0 0 0 0
2520  map stall: slot=15 h=16 iova=100027000 bytes=16384 words=0 0 0 0
2461  wire: pid=29 vm=0xfffffff345e29d28 next=17 h=16 iova=100027000 bytes=756
      sample=0/0/70460001 bo= 70460001 31 40bb0801 fffff 70268000 48980401 1f 40980501
```

Reading it:

- **The CP consumed the whole submission and then reported an opcode error.** `CP_RB_RPTR` =
  `CP_RB_WPTR` = 0x24 (36 dwords), `CP_IB1_REM_SIZE` (0x092a) = 0, so the IB is finished, not
  blocked mid-fetch; `CP_INTERRUPT_STATUS` = 1 is `CP_OPCODE_ERROR` (bit 0 of `A6XX_CP_INT`,
  a6xx.xml). A zero page therefore is **not** a harmless no-op on this part/ucode: all-zero words
  decode as type-0 register writes (register 0, one dword), and the SQE faults on them. The fence
  packet after the IB never runs, which is why `cpFence` stays at the board probe's 2.
- **There is no mapping overlap.** `stall fencepage ... handle=10` is printed with `%llx`: handle
  0x10, i.e. 16. The wf72 `map wire` dump lists handle 10 at `0x30000000/32768` (the internal
  ring1 buffer) and handle 16 at `0x100027000/16384` alone; slots 0..15 are disjoint. H2 as
  written in wf70 is a printf-radix misreading, not a table bug.
- **`cached=0` in `sampleWord` is not a measurement on client BOs.** `cachedRead32` returns -1 when
  the physical address is outside `[gPhysBase, gPhysBase+gPhysSize)`; for these system pages it
  does, and `sampleWord` leaves `cached` at its zero initializer. Only the DRAM field is evidence
  for a system BO. (The fence page is a kernel BO inside the aperture, so its `cached=2` is real.)
- **The "uncached" DRAM reader was cacheable.** `dramRead32` mapped its page with
  `kIOMapAnywhere|kIOMapReadOnly`; without `kIOMapInhibitCache` that is a default (cacheable)
  mapping, so a zero read also populates a clean zero line that later "confirms" the zero even
  after DRAM changes. wf73 fixes the reader and adds a full-buffer and whole-VM scan through it.
- **The same client code works in mediaserverd.** pid 29's first submission is a real `tu_init_hw`
  prologue (`70460001` = `CP_EVENT_WRITE` cache-invalidate, `40bb0801 fffff` = `SP_UPDATE_CNTL`,
  `70268000` = `CP_WAIT_FOR_IDLE`) in DRAM at submit time. Whatever loses backboardd's copy is
  specific to that submission, not to the recording path or the ABI.

### 3.3 wf73 result — the stream is in DRAM at submit and a write-back zeros it

Run `out/wf73-runs/run1.log` (kernel `2a8a1257…`, both GPU flags). wf73 changes two diagnostics
only: `dramRead32` now maps with `kIOMapInhibitCache` (it previously used the default, cacheable
attribute), and a bounded scan reads the submitted BO and every mapping through that reader.

```
2316  wire: pid=56 vm=0xfffffff188ee7128 next=17 h=16 iova=100027000 bytes=756
      sample=0/0/70460001 bo= 70460001 31 40bb0801 fffff 70268000 ...
2333  scan wire: h=16 scanned=16384 nz=889 first=0:70460001 needle=70460001 at=0
2334  scan vm: needle=70460001 hits=1 h=16 off=2c0 word=70460001
2336  tunnel: call=5 status=0
2349  stall: fence=3 pending=50000 us cpFence=2 rptr=27 wptr=36
2376  scan stall: h=16 scanned=16384 nz=12 first=2c0:70460001 needle at=2c0
2385  stall: fence=3 pending=1000018 us cpFence=2 rptr=27 wptr=36
2412  scan stall: h=16 scanned=16384 nz=12 first=2c0:70460001 needle at=2c0
```

Reading it:

- **The client's stream is in the BO at submit time.** Through the truly uncached reader the
  submitted BO has 889 nonzero dwords and the `tu_init_hw` prologue at offset 0. The earlier
  "zero IB" was the cacheable diagnostic reader returning a stale zero line.
- **Between the submit and the 50 ms snapshot the first 704 bytes (11 cache lines) are overwritten
  with zeros.** The scan then finds only 12 nonzero words, the first at 0x2c0; the two later
  snapshots agree. 704 is 11×64 and the stream is 756 B, so the tail of the stream (0x2c0..0x2f3)
  survives. That is a cache-line write-back of the BO's zero fill: system BOs are zero-filled by
  the VM through cacheable mappings, and for **write-combined** BOs `allocateSystem`/`prepareSystem`
  deliberately skipped the clean ("no CPU mapping should cache these pages"), assuming the zero
  fill shared the BO's mapping attribute. It does not: the dirty zero lines stay in the CPU cache,
  the client's WC stores go around them to DRAM, and their later eviction writes zeros back over
  the command stream. The CP fetches the zeroed first packet, decodes type-0, and raises
  `CP_OPCODE_ERROR`; the fence never runs. Mediaserverd's BO escaped because its dirty lines were
  evicted before its client wrote.
- **The fix (wf74):** clean the zero fill at allocation for every system BO, write-combined
  included (`prepareSystem` and `allocateSystem` no longer test `!write_combined`). One-time pass,
  before the BO is exposed or mapped by the client.

**wf74 result — the fix works.** Kernel `3081b7b6…` with the allocation clean. Run
`out/wf74-runs/run2.log`:

```
2377  wire: pid=56 vm=0xfffffff1e2308928 next=17 h=16 iova=100027000 bytes=756
      sample=0/70460001/70460001 bo= 70460001 31 40bb0801 fffff 70268000 ...
2378  scan wire: h=16 scanned=16384 nz=137 first=0:70460001 needle=70460001 at=0
2381  tunnel: call=5 status=0
2382  tunnel: call=6 status=0
2383  tunnel: call=8 status=0
2384  tunnel: call=6 status=0
```

The first submission is accepted and its fence is observed (`call=6 status=0`); the BO still holds
the prologue at offset 0 at wire time (the 137 nonzero dwords are the stream itself — the 889 of
wf73 included stale lines that the zero-fill clean removed), no stall or hang follows, and both
backboardd and mediaserverd continue with further submissions and BO allocations (calls 2/3/13).
The first-submission stall is fixed; the next stage is Metal device creation and sustained frames.
**[verified: out/wf74-runs/run2.log]**

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
- **`/tmp` is volatile (lost on a host reboot) and two build inputs live there.**
  `ADRENO620_FW_DIR` (default `/tmp/picasso-gpu-fw`) holds `a650_gmu.bin`, `a650_sqe.fw`,
  `a630_sqe.fw`, `a620_zap.mdt/.b00/.b01/.b02`; without it the generated
  `adreno620_embedded_firmware.c` is empty and the boot aborts at stage 3 with
  `GMU/SQE blobs were not staged`. `NOVATEK_TOUCH_FW` (default
  `/tmp/picasso-vendor/blobs/candidate1.bin`, 0x22000 bytes) is needed by the touch driver's
  archive. Both are in the kernel of any older image: mount
  `out/<tag>/picasso/payload/payload.img` (FAT, `hdiutil attach -readonly`), copy
  `EFI/UniXNU/Payloads/kernel.macho`, resolve the `_native_adreno620_fw_*` symbols against the
  `LC_SEGMENT_64` file offsets, and write the bytes out under the names above (the ZAP CRCs in
  `Adreno620Board.cpp` verify the extraction). `/tmp/build-wf-addon.sh` is also recreated from
  §6.1's description after a reboot.
  **Persistent copies now live in the project tree (git-ignored, survive a host reboot):**
  `build/picasso-gpu-fw/` (the seven GPU firmware files), `build/picasso-vendor/blobs/candidate1.bin`
  (touch firmware) and `build/local/build-wf-addon.sh` (the packing script). Build with
  `ADRENO620_FW_DIR=$PWD/build/picasso-gpu-fw NOVATEK_TOUCH_FW=$PWD/build/picasso-vendor/blobs/candidate1.bin`
  and copy `build/local/build-wf-addon.sh` to `/tmp` only as a convenience.

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

## 14. wf77–wf81 (2026-11-03): marker stall located, trust register is TZ-owned, SQE bumped

- **wf77 (reverted).** Programmed a UCHE GMEM window + `gmem_base` as a guess;
  the stall persisted. Mainline after checking: for ADRENO_6XX_GEN3 (`a650` family) `a6xx_hw_init`
  deliberately skips `UCHE_GMEM_RANGE_MIN/MAX`, and `MSM_PARAM_GMEM_BASE` returns 0 from GEN3 on
  (`TILE6_2` is a GMEM offset). The change was reverted; the port keeps `gmem_base = 0`.
- **wf78/wf79 (measured stall position).** `progressState()` now adds `CP_ROQ_AVAIL_IB1 >> 16` to
  `CP_IB1_REM_SIZE`, i.e. the true SQE position. The stall is deterministic across boots and stable
  over seconds at one packet: `CP_SET_MARKER` payload `0x15`. Per the Mesa register description
  (`adreno_pm4.xml`) only bits 0-3 are parsed: MODE = 5 = `RM6_ENDVIS`; bit 4 (0x10) is ignored by
  the firmware. It is emitted by Turnip's `tu6_render_tile` right after the per-tile `draw_cs` IB
  call, before `CP_SKIP_IB2_ENABLE_GLOBAL(0)` and the tile-store IB. At the stall: all blocks idle
  (`RBBM_STATUS = 0x800005`: only `CP_BUSY`/AHB bits), no CP fault/opcode error,
  `CP_STATUS_1 = 0x8000471f`, `RBBM_INT_0_STATUS = 0x01900000`
  (`CP_CACHE_FLUSH_TS` | `HANG_DETECT` | `UCHE_OOB_ACCESS`). Fence 7 retired; fence 8 stopped there.
- **The SQE firmware says exactly what blocks (wf81/wf82).** `afuc-disasm` (Mesa 24, built at
  `/tmp/mesa24/mesa-mesa-24.0.0/build/src/freedreno/afuc/afuc-disasm`) on the official
  `a650_sqe.fw` shows the `CP_SET_MARKER` handler: for MARKER 5 it clears `$14` bits 7/15, writes
  command bit 5 (`0x20`) to SQE control register `0x080`, then spins until bit 5 of control register
  `0x069` clears (`l890`/`l895`), then clears `0x082`/`0x080`. The identical handshake serves
  `CP_SET_BIN_DATA5` (also bit 5) and other handlers (bits 1/2), so the wait itself works — the
  ENDVIS instance waits on a hardware visibility/bin operation that never completes. That points at
  the draws the SQE launched just before the marker (asynchronous to the SQE), not at the marker
  encoding; `UCHE_OOB_ACCESS` is the only error bit and suggests the draws fault.
- **wf80: `RBBM_SECVID_TRUST_CNTL` must not be written.** Mainline's no-zap fallback writes 0 there,
  but on this device the register is TZ-owned: reading it returns the bootloader's `1`, and the
  *write* never returns — GPU init stops at that line (no `cp: sqe started`, no Metal, no
  submission), while the rest of userland boots. The port now only reads and prints it.
- **AMSBC is a separate pending test.** Mainline sets `amsbc` (bit 4) in `RB_NC_MODE_CNTL` for
  UBWC >= 3.0 (`SM7250` is UBWC 3.0), so `0x12` is the mainline-correct value. wf81 keeps the
  previous `0x2` to isolate the SQE change; switch it to `0x12` in its own boot.
- **SQE firmware floor.** The vendor `a650_sqe.fw` is version `0x87`; mainline's floor for a650 is
  `0x095`. Installed linux-firmware's `a650_sqe.fw` (version `0x112`, 31964 bytes, CRC32
  `0xf59c2772`) in `build/picasso-gpu-fw/`, kept the vendor file as `a650_sqe_vendor_0x87.fw`, and
  updated the `kSqeAlt` pin (size/CRC32/first-bytes) in `Adreno620Board.cpp` — the probe rejects any
  image that matches neither pin. The firmware is embedded at **kernel build time** from
  `ADRENO620_FW_DIR`, so the build must run with
  `ADRENO620_FW_DIR=$PWD/build/picasso-gpu-fw`; `adreno620 fw sqe 31964 bytes` in the build log and
  `native_adreno620_fw_sqe_size = 31964u` in `out/picasso/tree/build/adreno-picasso/` are the check.
- **wf81** image: kernel `5f41dca2eb1ff109c06085a4223136adfa4d3750cdd88054a5340019769e319d`,
  payload args include `sheng-gpu-metal=1 sheng-gpu-addon=1`, addon
  `0dbe0ef2…` (`build/ngpu-addon-wf60.bin`). Booted from RAM 2026-11-03 09:26 CST; device
  serial this round `52cc791d`. Log `out/wf81-runs/run1.log`. Result: GPU init and Metal publish
  succeeded with SQE 0x112 (`sqe=a650_sqe.fw version=0x112 security=1`), fence 2 retired, then the
  first client stream (pid 56 backboardd) stalled at the same `RM6_ENDVIS` packet (fence 6 pending,
  cpFence 5) with the same idle-block/`UCHE_OOB` signature. **The SQE version is not the cause.**
- **wf82: force Turnip's sysmem path.** `TU_DEBUG=sysmem` (Mesa's `use_sysmem_rendering()` returns
  true unconditionally) skips GMEM, hardware binning, `CP_SET_BIN_DATA5` and the `RM6_ENDVIS`
  marker. The plugin sets it with `setenv("TU_DEBUG", "sysmem", 1)` in both device-open paths of
  `UniXNU/drivers/NativeMetal/NativeAdrenoMetalPlugin.mm` (render server at the other `setenv`
  calls near `CA_DISABLE_FRAMEBUFFER_COMPRESSION`, and `clientDevice`), before
  `native_bridge_metal_open()`. Rebuild chain:
  `python3 scripts/build-adreno-metal-plugin.py` (needs `DEVELOPER_DIR`; rebuilds the pinned Rust
  bridge, re-copies `build/mesa-tu-ios/.../libvulkan_freedreno.dylib`, re-signs 4 KiB),
  `python3 scripts/make-owner-trust-bundle.py`, then
  `python3 scripts/make-gpu-addon.py --publish --clients '*' <out>`.
  wf82 inputs: addon `383631181b9dabde1bd224a6dca5170562d426d9bc3a88c28f39f610b3319c2c`
  (`build/ngpu-addon-wf82.bin`), trust `dc9e20501b0490b75489c461f78f2b6c42a3e9b070ca43d79443f785ae0483d3`,
  plugin CDHash `812154a79c0042e710c26b4c827223219dd96bf6`, kernel unchanged from wf81. The old trust
  bundle/trust.json are backed up as `build/owner-trust-wf81.bundle` and `build/trust-wf81.json`.
  Image `out/wf82/picasso/boot/wf82.img`; log `out/wf82-runs/run1.log`.
- **wf82 result (measured): no stall.** GPU init, Metal publish, the client's first submission and its
  fence all complete; `grep -c "PICASSO_GPU stall"` is 0 (wf81: 2), the display path runs repeated
  frame cycles (`PICASSO_DISPLAY event=6 sample=1/2`, `event=8/9/10`, `frame kind=0`), and SpringBoard
  comes up. So the hang is specific to the GMEM/hardware-binning path (the per-tile draw CS plus the
  `RM6_ENDVIS` completion handshake), not to the ring/submit/fence machinery, memory mapping, the
  SQE image, or the draws' command encoding. Next measurements: (a) does the presented frame have
  content (`nonblack=`); (b) one surface was rejected with format `0x62336138` (`b3a8`, 10-bit deep
  framebuffer — the render server's deep preference), while the display driver only takes 8-bit;
  (c) then return to the GMEM path: the `RM6_ENDVIS` handler's control-0x069 bit-5 completion still
  never arrives with binning, so compare the VSC/bin state programmed by the tile sequence.
- **wf83: run the project's own GPU test.** `NativeAdrenoMetalPlugin.mm` normally selects the
  production publish path when the addon carries `NativeAdrenoMetal.publish`; without it the plugin
  runs `adrenoUserTest()` (the raw kernel test plus the Turnip smoke: fill buffer, compute shader,
  triangle render, pixel read-back) on a thread. The Data volume keeps the `.publish` file written
  by earlier addons (`native_gpu_addon_prepare()` only writes files present in the blob; it never
  deletes strays), so the test build makes `publish::requested()` return false directly. The smoke
  path also sets `TU_DEBUG=sysmem,nocb` (was `nocb`), so the render/read-back test avoids the GMEM
  `RM6_ENDVIS` stall. The kernel mirrors each completed plugin text line to the console
  (`PICASSO_GPU text: …`, at most 64 lines, in the `0x7fff`/`0x7ffe` handler) because the periodic
  report runs only once and is empty by then. Kernel `471840b5adb79a606046773c2f31dd22e3d28d844f88b821506000bb01c93b1a`;
  addon `09f978bd2506fb47edc71117122ce05f29bf3bb8bb9810350bc6a521cbada233`
  (`build/ngpu-addon-wf83-test.bin`), trust
  `66b959f5622f9ed7e03fc464052a06de78be65ec665c5c0452908bbced33f29a`, plugin CDHash
  `9fa727f087f9b06b68991aa800fec3e87ab86d56`; image `out/wf83/picasso/boot/wf83.img`, log
  `out/wf83-runs/run1.log`. The expected lines start with `T s<stage>
  r<VkResult> v<fill> c<compute> g<triangle pixels>/<stray>` (NativeTurnipSmoke.h header) and the
  replay scan's `F <retired>/<prefixes> o<opcode> c<dwords> e<…>`.
- **wf83 result: the built-in test is ABI-stale.** With the plugin forced onto its test path, the
  display kept showing the port's boot UI (`PICASSO_DISPLAY frame kind=0 nonblack=41011`, repeated),
  i.e. no black scanout, but the test's submits were refused: `tunnel: call=5 status=e00002c2`
  (`NativeAdrenoMethodSubmit` → `kIOReturnBadArgument`) twice, and no `PICASSO_GPU text:` line ever
  completed. Cause: the plugin/test headers in the UniXNU checkout are ABI **v1**
  (`NativeAdrenoAbiVersion = 1`, 16 buffers) while the kernel driver in this worktree and the Turnip
  Darwin backend are ABI **v2** (128 buffers, extra flags/methods); the raw test and the smoke
  submit v1-shaped requests, which the v2 `Submit` validation rejects. The production path is
  unaffected because it submits through Turnip's v2 backend. To use the built-in test on this
  driver, the UniXNU `NativeAdrenoABI.h`/test would have to be updated to v2.
- **wf84: force the 8-bit framebuffer.** On wf82 the render server presented a 1080x2400 BGRA
  surface whose scanned content was black, and one 10-bit `b3a8` (2-plane) surface was rejected by
  the legacy display. The plugin now also sets
  `CAEnableDeepFramebuffer = false` (with the compression preferences, before the device opens), so
  the render server should choose the 8-bit framebuffer the display can scan out. Kernel unchanged
  from wf83 (`471840b5…`); addon `fc614ec3761a133c0db032cee6c1bb4b493788f3173af9e44d9686f06856d4c8`,
  trust `e2a64cb6dc8e22c2e7ff0aacd6108492cee521dba305af25cd6d85be9276f750`, plugin CDHash
  `0f18fdbeca158504f7ef4c2433a9acb2c60fe881`; image `out/wf84/picasso/boot/wf84.img`, log
  `out/wf84-runs/run1.log`.
- **Next GMEM measurement (not yet run).** At the `RM6_ENDVIS` stall, dump the VSC state the
  SQE/marker depends on and sample its streams: `VSC_BIN_SIZE`, `VSC_BIN_COUNT`,
  `VSC_PIPE_CONFIG(0..)`, `VSC_PRIM_STRM_PITCH/LIMIT`, `VSC_DRAW_STRM_PITCH/LIMIT`,
  `VSC_DRAW_STRM_ADDRESS` (`0xc34`), `VSC_PRIM_STRM_ADDRESS` (`0xc30`),
  `VSC_DRAW_STRM_SIZE_ADDRESS` (`0xc03`), the `VSC_DRAW/PRIM_STRM_SIZE_REG` counters, and a sample
  of the two stream buffers plus the size buffer through the job's address space. If the stream
  buffers are zero the visibility pass never wrote (VSC not running → look at power/clock/register
  state); if they hold data the ENDVIS wait is waiting on something else (the draw pipeline).
- **wf85: align the test with ABI v2.** `UniXNU/drivers/NativeAdreno/NativeAdrenoABI.h` now says
  `NativeAdrenoAbiVersion = 2`, `NativeAdrenoAbiMaxBuffers = 128`, with the submit assertion written
  as `32 + 24 * NativeAdrenoAbiMaxBuffers` (the kernel's v2 record). The plugin was forced onto the
  built-in test again (temporary `requested() { return false; }`), the addon carries no `.publish`,
  and the kernel is unchanged (`471840b5…`, with the `PICASSO_GPU text:` mirror). Inputs: addon
  `dff9413d93e0c6ac74e318dd0b46747621ae7aeb905491b38db9a38f5dc645b3`, trust
  `7c10abad571ace6daad8d74cb0c21fe2f5a2532705752d61839f5833b3477c4d`, plugin CDHash
  `640a55d7b4e7ae0a3c48e941d163d489d4ad9b1c`; image `out/wf85/picasso/boot/wf85.img`, log
  `out/wf85-runs/run1.log`.
- **wf85 result: the submit is fixed; the test now stops at its own timestamp call.** Measured:
  `tunnel: call=5 status=0` twice (wf83: `e00002c2`), zero `PICASSO_GPU stall` lines, and the
  two plugin processes (backboardd pid 56, mediaserverd pid 29) each get through Query,
  GetDeviceInfo, alloc/map/CPU-map, submit and WaitFence. Then `tunnel: call=7 status=e00002c7`
  (`kIOReturnUnsupported`): `NativeAdrenoUserClient::timestamp()` returns unsupported because the
  published client has no `timer_` attached. `runTest()`'s `check()` recorded that as the run's
  error, so `adrenoUserTest()` set `verifiedWc = -2` (skipping the write-combine run) and the
  `verified == 4 && verifiedWc == 4` gate never let `freshScan()`/`replayPhase()`/`turnipPhase()`
  start — no `streamText` line was ever completed (the first one is inside `freshScan`). The
  `report()` line was also lost: `tunnel: call=9 status=e00002c2` (`kIOReturnBadArgument`) because
  v2 `setMetadata()` validates the handle before recording, and the test deliberately uses a bogus
  handle. Neither refusal is a GPU fault.
- **wf86: timestamp is diagnostic only.** `NativeAdrenoUserTest.h` calls GetTimestamp without
  `check()`, so a driver that refuses it no longer fails the run or skips the second cache-mode run
  and the Turnip phase. Plugin CDHash `3eafe5d89e4ad9031096f08be381bfe71e272dec`, trust
  `b405de012859fa1485b1bd275855719ff84507220c2a10bf45a863069d3434ba`, addon
  `5cc7e0c739cf…`; image `out/wf86/picasso/boot/wf86.img`, log `out/wf86-runs/run1.log`.
- **wf86 result: the raw GPU test passes in every process; no later phase is visible.** Three
  processes streamed their report lines — `mediaserv s32 e0 v4/4 t0 a256/0`,
  `WidgetRen s32 e0 v4/4 t0 a256/0`, `SpringBoa s32 e0 v4/4 t0 a256/0`: 32 successful steps, no
  error, both cache-mode runs verify 4/4 dwords (backboardd, pid 56, also ran both runs — two
  submits and two `wire` dumps in the log — but its report line was the pending one and never
  printed). The kernel's `tunnel:` print stops after 32 calls, so later submits are invisible by
  design. After the reports the log shows no `freshScan`/`replayPhase`/`turnipPhase` line, no new
  opens and no stall.
- **wf87: flush every text line and test only the render server.** The driver prints a streamed
  line only when the *next* line starts, so the last line of a phase (exactly the important one)
  was invisible; `streamText`/`streamText7` now send a second start marker to flush. Phase
  boundaries also stream `Y prescan` and `Y turnip`, and the plugin's test thread runs only in
  `backboardd` (several processes were sharing the single text stream and its 64-line print
  budget). Plugin CDHash `aaa079a9b4b8ac1a9edbfabbb280f3293f439f1a`, trust
  `5553f9abfc07e2ccfe8398050c19690a4a82ed376dde495f06b385be10d34375`, addon
  `8458be94e9a56991f5aaedeee9c8328f87ffd8290ae49c2566d4d06cba80651f`;
  image `out/wf87/picasso/boot/wf87.img`, log `out/wf87-runs/run1.log`.
- **wf87 result: the replay stops at its first prefix.** With the render server alone and flushed lines the log
  shows `Y prescan` and then `F 0/21 o17 c2 e-`: none of the 21 prefixes retired; the first one
  (opcode `0x17` `CP_THREAD_CONTROL`, 2 dwords) did not complete, so the whole-IB run was not
  attempted. No `PICASSO_GPU stall` line follows, and the test thread then ends (no `Y turnip`, no
  new connection): either the prefix's submit was refused or its fence wait timed out, but the
  current line does not say which. The raw test's own submits on the same connection had just
  retired, so the ring/submit/fence machinery works; the difference is the submitted buffer
  (recorded Turnip BO addresses and contents) and the packet itself.
- **wf88: name the prefix failure.** The `F` line now carries `x<first error>` and `h<1 if the fence
  timed out>` so the next boot distinguishes a refused submit from a hung one. Plugin CDHash
  `31b9f04ee1755259e9183f57eb652ff57be7c3c4`, trust
  `ce7517bc62a47aa0d6d54c1d3a76e9c7ee1a1a898a925a08a10d6ce9f6b73752`; image
  `out/wf88/picasso/boot/wf88.img`, log `out/wf88-runs/run1.log`.
- **wf88 result: the prefix failure is a refused map, not a hang.** `F 0/21 o17 c2 e- xe00002c2 h0`:
  the error is `kIOReturnBadArgument` and the fence never timed out (`h0`). It comes from
  `replayLoad()`, not from `Submit`. Cause: every user connection is scoped by
  `core->setScope(base, GpuClientVaStart, window)` (`NativeAdrenoPublish.cpp`, `GpuClientVaStart =
  0x100000000` in `NativeAdrenoGpuContext.h`) and `NativeAdrenoUserClient::mapBo()` refuses any iova
  outside `[window_start, window_start + window_bytes)`; the replay recording (Turnip's first
  submission captured earlier) has its IB at `0x20027000` and buffers at `0x100000`…, all below the
  window. The raw test's own BOs map at `info.va_start` — 0x100000000 — and retire. So the replay
  is not a render failure; it cannot be mapped by a client of this driver.
- **wf89: skip the unmappable replay, run the Turnip smoke.** `adrenoUserTest()` now checks
  `turnipreplay::kIbIova >= GpuClientVaStart` (0x100000000). When the recording is below the client
  window it streams `Y replay skipped: recording below the client window`, skips `freshScan`,
  `replayPhase` and `replaySecondClient`, and calls `turnipPhase()` directly — the Turnip
  fill/compute/triangle test allocates its own BOs inside the window. Plugin CDHash
  `b6de7d04f067dd967bc3afad584ceedd4047a9bc`, trust
  `f7c04cffd2c46fb847cf08cb10e37cb95b44dfe190f1b9a759c008ffc1cf19e2`; image
  `out/wf89/picasso/boot/wf89.img`, log `out/wf89-runs/run1.log`.
- **wf89 result: Turnip runs end to end, its GPU work has no effect.** Lines:
  `Y replay skipped…`, `Y turnip`, a watchdog sample `W 6200 s4 u0/k0 …` (the worker was blocked
  in the kernel at `0x6200`, render begin, at the 15 s sample), then `T s0 r0 v512 c0 g0/4096`
  and `M s6501 … device: ShaderCache(ShaderCacheEr…` (the Metal bridge self-test refused at its
  shader cache). Interpretation: every Vulkan call succeeded and every fence retired
  (`s0 r0`), but (a) `fillTest` writes only the **first half** of a 4 KiB buffer and expects the
  second half untouched, so `v512` is exactly what "the fill never happened" looks like (the
  untouched half is the 512 good words); (b) the compute shader wrote 0 of 1024 words;
  (c) the render read-back has 0 triangle-or-clear pixels out of 4096. The raw test's own
  submits on the same connection just before retired and wrote memory correctly, so Submit /
  WaitFence / BO mapping are fine — this is specific to Turnip's command streams (or to the
  visibility of their writes).
- **wf90: sample what the GPU left behind.** `fillTest`, `computeTest` and `renderTest` now keep
  the first/middle/last dword they read back (`gFillSample`, `gComputeSample`, `gRenderSample`)
  and `turnipPhase()` streams a second line `S f… k… r…`. That separates "the GPU wrote nothing"
  (samples `deadbeef`/`5a5a5a5a`) from "the GPU wrote the wrong thing" (samples holding a
  pattern or zeros). Plugin CDHash `289b3036b5cdce84414a19db5f5a19f17a09831b`, trust
  `a9d1b5456d37770bb27212d736636012e717440afae3d486acdc91dd7b2066af`; image
  `out/wf90/picasso/boot/wf90.img`, log `out/wf90-runs/run1.log`.
- **wf90 result: Turnip's GPU work leaves the memory untouched.** `T s0 r0 v512 c0 g0/4096` and
  `S fdeadbeef deadbeef deadbeef kdeadbeef deadbeef deadbeef r5a5a…`: the fill buffer samples are
  still the CPU's `0xdeadbeef`, the compute buffer still `0xdeadbeef`, and the render read-back
  buffer still the CPU's `0x5a5a5a5a`. So no blit, no dispatch and no copy ran — `v512` was only
  the untouched half of the fill buffer. Every fence retired anyway. The watchdog `W 6200` sample
  symbolized (`atos`) to `tu_CreateGraphicsPipelines` → `tu6_emit_viewport` in the Turnip dylib —
  the 15 s block is pipeline creation in the compiler, not a GPU wait.
- **wf91: did the CP execute anything inside Turnip's command buffer?** `fillTest` now creates a
  `VK_QUERY_TYPE_TIMESTAMP` pool, records `vkCmdResetQueryPool` + `vkCmdWriteTimestamp`
  (`VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT`) into the same command buffer, and reads the result with
  `VK_QUERY_RESULT_WITH_AVAILABILITY_BIT` (no wait). Turnip implements the timestamp as
  `CP_REG_TO_MEM(CP_ALWAYS_ON_COUNTER)` + `CP_MEM_WRITE` — both CP-level and both inside the IB —
  while the kernel's fence is outside the IB. `turnipPhase()` streams `Q t<value> a<availability>
  r<VkResult>` as its own line (the `S` line was truncated at the stream's 63-character limit).
  Non-zero `t` means the IB ran and the pipeline stages are the problem; zero means the CP skipped
  the client's IB. Plugin CDHash `0d2e7bfb013c0e2ac6d85dffb49ef689a8b30d06`, trust
  `693690d75959f1fc08f3e3f6a89cb417b310c32bc44a3bc6dd1bcff3dddac08f`; image
  `out/wf91/picasso/boot/wf91.img`, log `out/wf91-runs/run1.log`.
- **wf91 result: the CP executes Turnip's IB.** `Q t1020e2c31 a1 r0`: the `CP_REG_TO_MEM`
  (`CP_ALWAYS_ON_COUNTER` → query buffer) and the `CP_MEM_WRITE` (availability) that Turnip
  records inside this command buffer both ran, with a sane counter value and `VK_SUCCESS` for the
  query read. So the IB is not skipped, the client's page table is in place, and the CP is
  executing the client's packets. The timestamp was recorded **before** the fill, so it proves
  the leading packets only.
- **wf92: a timestamp on each side of the fill.** `fillTest` now records two timestamps in the
  same command buffer — query 0 before `vkCmdFillBuffer`, query 1 after it — and
  `turnipPhase()` streams `Q <q0 value>/<q0 avail> <q1 value>/<q1 avail> r<VkResult>`. q1 ≠ 0
  means the CP executed the fill's state/draw packets and the failure is downstream (the 3D
  pipeline producing no writes); q1 = 0 means the CP stopped or aborted at those packets, and the
  fill's command stream is the place to look (protect/unknown packet). Plugin CDHash
  `4d06d7c3d0d57343052219f0430075ef3cc69c07`, trust
  `f2958b9942377eee1aac7b5062131b0f79cafaf571f7f00bb8640843339d2229`; image
  `out/wf92/picasso/boot/wf92.img`, log `out/wf92-runs/run1.log`.
- **wf92 result: the CP runs through the fill's commands; the pipeline still does nothing.** `Q
  6f855a49/6f855a8a 1/0 r0`: q0 (before the fill) = `0x6f855a49`, available; q1 (after the fill) =
  `0x6f855a8a`, a **later** counter value, so the CP reached the trailing timestamp and did not
  abort inside the fill's state/draw packets. (q1's availability word read 0 — secondary, Turnip's
  availability write path; the value itself proves execution.) The samples were again
  `deadbeef`/`5a5a5a5a`: the CP executes, the 3D/compute blocks do not produce writes.
- **Root-cause candidate from the official source: SPTPRAC power.** `a6xx_catalog.c`'s a620 entry
  has no GMU-wrapper quirk, so mainline's `a6xx_gmu_fw_start()` takes the legacy branch
  (`a6xx_gmu_gfx_rail_on()` + `a6xx_sptprac_enable()`) for this chip. `a6xx_sptprac_enable()`
  writes `GPU_GMU_GX_SPTPRAC_POWER_CONTROL = 0x778000` and polls `GMU_SPTPRAC_PWR_CLK_STATUS` until
  `(status & 0x38) == 0x28`; `a6xx_set_hwcg()` only toggles the *clock* control. The port boots the
  GMU through the HFI/`GMU_OOB_GPU_SET` handshake (which acks and keeps GX on) but never writes the
  SPTPRAC power register. A CP that runs while SP/TP stay collapsed is exactly the measured
  signature: CP-level turns (fence, timestamp, `CP_MEM_WRITE`) complete, every draw/blit/compute
  leaves memory untouched, and the GMEM visibility handshake (`RM6_ENDVIS`) never completes (wf81).
  Other catalog facts checked and matching the port: a620 uses `a650_sqe.fw`, `a650_protect`,
  `a620_hwcg`, `PC_DBG_ECO_CNTL = prim_fifo_threshold | bit 9`. One difference noted: mainline's
  a620 entry names `a621_gmu.bin`; the port pins `a650_gmu.bin` from the device's own firmware
  (41308 bytes), which boots and runs HFI here — revisit only if SPTPRAC still refuses.
- **wf93: power SPTPRAC.** `hardwareInit()` now performs mainline's `a6xx_sptprac_enable()` after
  the GX-on check: `gmuWrite(0x1A881, 0x778000)`, then poll `GMU_SPTPRAC_PWR_CLK_STATUS` (0x1F8D0)
  up to 100 × 1 µs for `(status & 0x38) == 0x28`, and print the before/after status. Kernel rebuilt
  from `out/picasso/kernel/kernel.macho`; the previous kernel is kept at
  `build/kernel-wf92-471840b5.macho` (sha `471840b5…`). Image `out/wf93/picasso/boot/wf93.img`,
  log `out/wf93-runs/run1.log`.
- **wf93 first attempt (kernel `79eaad56…`) aborted at GPU init — firmware embed trap.** The
  rebuild did not set `ADRENO620_FW_DIR`, so `scripts/build-adreno-picasso-firmware.py` embedded
  `/tmp/picasso-gpu-fw/a650_sqe.fw` (31460 bytes, the vendor 0x087 image), which matches neither
  pinned SQE; the probe printed `PICASSO_GPU firmware: SQE image is neither pinned a630 nor a650`
  and stopped at `stage=3`. The build must run with
  `export ADRENO620_FW_DIR=$PWD/build/picasso-gpu-fw` (that directory's `a650_sqe.fw` is the
  pinned 31964-byte v0x112 image). The kernel was rebuilt again with the variable set; its hash and
  the wf94 image/log are below.
- **wf94: SPTPRAC write from hw_init changes nothing.** Kernel `acede28f…` (rebuilt with
  `ADRENO620_FW_DIR`); the probe accepted `sqe=a650_sqe.fw version=0x112 security=1`. Measured:
  the GMU status register is live and GX comes up (`gx: oob … status=000000c0->00000000 on`), but
  after the new `a6xx_sptprac_enable` in `hardwareInit()` the line reads
  `sptprac power: status 00000000 -> 00000000 (want &38==28)` — bits 3/5 never set, before or
  after `0x778000`. The smoke result is identical to wf92 (`T s0 r0 v512 c0 g0/4096`,
  `S fdeadbeef… kdeadbeef… r5a5a…`, `Q …/… 1/0 r0`). So at `hw_init` time, with HFI already
  running, the firmware ignores the manual SPTPRAC control.
- **wf95: move SPTPRAC to mainline's place — after `gmu_start`, before `hfi_start`.**
  `a6xx_gmu_fw_start()` runs `a6xx_gmu_gfx_rail_on()` and `a6xx_sptprac_enable()` between the CM3
  release and `a6xx_hfi_start()`. `Adreno620Gmu.cpp::startCm3()` now writes
  `GPU_GMU_GX_SPTPRAC_POWER_CONTROL = 0x778000` and polls `GMU_SPTPRAC_PWR_CLK_STATUS` for
  `(status & 0x38) == 0x28` immediately after the CM3 boot handshake and before
  `GMU_HFI_CTRL_INIT`, printing `PICASSO_GPU gmu: sptprac power (pre-HFI) status=…`. If the rail
  then reports powered and the smoke writes memory, the missing SPTPRAC power-up is the root cause
  of both the silent sysmem no-op and the GMEM `RM6_ENDVIS` stall.
- **wf95 result: SPTPRAC is firmware-managed here.** `sptprac power (pre-HFI) status=000000c0`
  (GX itself still off at that point, bits 6/7) and at `hw_init` `00000000 -> 00000000`: bits 3/5
  never set. Mainline's `a6xx_sptprac_enable` only applies on the legacy GMU path; this board's
  GMU answers the *non-legacy* `GMU_OOB_GPU_SET` (set_new bit 30, ack 31) and owns the GX/SP power,
  so the manual SPTPRAC register is a no-op. The SPTPRAC lead is closed.
- **wf96: test the CP's render-mode condition directly.** Turnip wraps every sysmem draw in
  `CP_COND_REG_EXEC` with `MODE(RENDER_MODE) | SYSMEM`, and the CP decides it from the state set by
  `CP_SET_MARKER(RM6_DIRECT_RENDER)`. If that marker does not take effect, every sysmem draw section
  is skipped silently — CP reaches the end of the IB, no fault, no memory write, exactly the
  measurements of wf89–wf95 (and the GMEM counterpart would leave the visibility handshake waiting,
  as wf81 saw). `runTest()` now appends to its IB: `CP_SET_MARKER(RM6_DIRECT_RENDER)` (opcode
  `0x65`, payload 1), `CP_COND_REG_EXEC` (`0x47`, payloads `(3<<28)|(1<<27)` and body dwords 4)
  around one `CP_MEM_WRITE` of `0x600df00d` to target word 4, then an unconditional `CP_MEM_WRITE`
  of `0xc0debeef` to word 5, and streams `C s<word4> c<word5>`. `s=600df00d, c=c0debeef` means the
  marker/condition works and the failure is inside the draws; `s=deadbeef, c=c0debeef` means the
  marker never sets SYSMEM and Turnip's sections are skipped. Plugin CDHash
  `e4a87280ec504e3fe913c565c6b28d7304c41839`, trust
  `ee0a9247f36775b10dec02eaa4627c3d6f5b82bfb5b840cba82ef74f08bfb2ed`; image
  `out/wf96/picasso/boot/wf96.img`, log `out/wf96-runs/run1.log` (kernel unchanged from wf95,
  `1f63e127…`).
- **wf96 result: the marker and the render-mode condition work.** `PICASSO_GPU text: C s600df00d
  cc0debeef`: the `CP_MEM_WRITE` inside `CP_COND_REG_EXEC(MODE=RENDER_MODE, SYSMEM)` ran, and so did
  the unconditional one after it. Turnip's sysmem draw sections are therefore not skipped by the
  render-mode condition; the failure is inside the draw state or the 3D blocks. The smoke's own
  `T` line did not appear before this capture ended — but neither did the `W` watchdog sample that
  `runWatched` streams at 15 s, and `Y turnip` is printed by the same goroutine that streams them,
  so the capture ended less than 15 s after the smoke started (the boot now reaches the plugin very
  late: `Y turnip` at line 3485 of 6055). This is a capture-window artifact, not a hang; later
  runs use a 900 s window.
- **wf97: do client type-4 register writes take effect?** `runTest()` appends two type-4 writes and
  two `CP_REG_TO_MEM` (type 7, proven to run) readback packets: `CP_SCRATCH(0)` at `0x883` =
  `0xfedcba98` → target word 6, and `RBBM_GPR0_CNTL` at `0x018` = `0xc0def00d` → target word 7.
  Turnip programs every 3D state register with type-4 packets; if the readbacks do not return the
  written values, its state never reaches the hardware. `C s<w4> c<w5> a<w6> b<w7>` now streams all
  four. Plugin CDHash `a759da5278a7f60a7297551d8cee5c8ee175aee8`, trust
  `76117c6c11ffa7f23c121e2de0719aafe695077d4822192720383cfdf6c505a1`, addon
  `97302de806da…`; image `out/wf97/picasso/boot/wf97.img`, log `out/wf97-runs/run1.log` (kernel
  unchanged, `1f63e127…`).
- **wf97 result: the GPR0 write is itself a CP_PROTECT violation; the experiment aborted.** Serial:
  `text: C s0 c0 a0 b0`, then 3 s later `stall: fence=3 pending=3014471 us`, and
  `stall cpregs: cpint=10 hwfault=0 protect=600018`. `CP_PROTECT_STATUS=0x600018` decodes per
  a6xx.xml as ADDR=`0x18`, READ=0, CP_HALTED=1, ACCESS_VIOLATION=1 — exactly our type-4 write to
  `RBBM_GPR0_CNTL`, which lies in the first `a650_protect` entry `A6XX_PROTECT_RDONLY(0x00000,
  0x04ff)` (writes forbidden, reads allowed). `kCpProtectCntl = 0xB` sets `ACCESS_PROT_EN |
  ACCESS_FAULT_ON_VIOL_EN`, so the violation halts the CP and the submission's fence never retires.
  The `C` values are not measurements: `WaitFence` failed, `runTest()` returned before the
  readbacks, and `adrenoUserTest()` streams the zeroed `gCond*` statics right after `report()`.
  No `Y …` line follows and the smoke never starts. Evidence kept: the CP_PROTECT table is live
  and enforced for client submissions, and `CP_SCRATCH(0)=0x883` sits in the unprotected hole
  between `NORDWR(0x800,0x82)` and `NORDWR(0x8a0,0x8)` — the fault address proves the CP reached
  the GPR0 write *after* the scratch write without a fault.
- **wf98: scratch-only type-4 write/readback (one variable changed).** The GPR0 write is removed;
  the `CP_SCRATCH(0)` write and both readbacks stay (reading `0x018` is allowed by RDONLY and is
  the read-direction control). `a=fedcba98` means client type-4 register writes reach the hardware
  and Turnip's state programming is not a silent no-op; `a=0` means type-4 writes are dropped
  without a fault. Image `out/wf98/picasso/boot/wf98.img`, kernel `1f63e127…`, addon
  `7a4a10b3f025fb8bb32bc7c7d4d1ed8ccc990c540bbb65654f18351026faf21f`, trust
  `25cfd9ab6dacdf3b848c2f60a3623f0f81bcd02a295349979eb250ca3a9f4b3a`; log
  `out/wf98-runs/run1.log` (900 s capture window).
- **wf98 result: client type-4 register writes take effect; the whole stack below the 3D pipe is
  proven.** `C s600df00d cc0debeef afedcba98 b0`: the marker/condition still works, the type-4
  write of `0xfedcba98` into `CP_SCRATCH(0)` (0x883) landed, `CP_REG_TO_MEM` read it back, and the
  read of `RBBM_GPR0_CNTL` returned 0 (a read, allowed by RDONLY). The full smoke completed in the
  900 s window: `T s0 r0 v512 c0 g0/4096`, `S fdeadbeef … kdeadbeef deadbeef deadbeef r5a5a…`,
  `Q 71b6394b/71b63989 1/0 r0`, and `M s6501 … ShaderCache(ShaderCacheEr…`. So every layer except
  the 3D/A2D pipeline is now measured working: the client IB executes, register writes reach the
  hardware, Turnip's render-mode conditional sections run, fences retire, and the timestamps
  bracket the fill. This also closes the wf96 "missing T" question: the smoke simply starts late
  in the boot and needs a capture window longer than 600 s.
- **wf99: mainline's `a6xx_gmu_gfx_rail_on()` — the one official boot step the port never
  implemented.** `a6xx_gmu_fw_start()` (legacy path, which this tree takes for every a6xx,
  a650-family included: `gmu->legacy` is set unconditionally in `a6xx_gmu_probe`) runs, between
  the CM3 handshake and `hfi_start`:
  1. `a6xx_gmu_gfx_rail_on()`: `GMU_BOOT_SLUMBER_OPTION = 0`, `GMU_GX_VOTE_IDX = vote & 0xff`,
     `GMU_MX_VOTE_IDX = (vote >> 8) & 0xff` with the *highest* available ARC vote, then the
     `GMU_OOB_BOOT_SLUMBER` handshake (`HOST2GMU_INTR_SET` bit 22, ack `GMU2HOST_INTR_INFO`
     bit 30, ack cleared with `GMU2HOST_INTR_CLR` bit 30);
  2. `a6xx_sptprac_enable()` — the wf95 code, which could never take while GX was still off;
  3. `hfi_start`.
  The port ran (2) at the right *time* but without (1), and wf98's capture shows the GX status at
  that point is `0x000000c0` (`GX_HM_GDSC_POWER_OFF | GX_HM_CLK_OFF`): the SPTPRAC write was issued
  to a rail that was not on. `startCm3()` now performs (1) immediately before the SPTPRAC write,
  using the same speed-bin table and `arcVote()` the HFI startup already uses (highest level;
  measured speed bin `0x84` → 625 MHz for this part), and prints the vote indices, poll count,
  interrupt info, and GX status before/after. `native_adreno620_gmu_release_gpu_set()` now also
  performs mainline's second half, `a6xx_gmu_clear_oob(GMU_OOB_BOOT_SLUMBER)` (bit 30 into
  `HOST2GMU_INTR_SET`), right after the existing GPU_SET clear. Kernel
  `e3baefc99e524ccd7ff43956da90a6989fe7010316405e9f342e22bb560f322e` (rebuilt with
  `ADRENO620_FW_DIR=$PWD/build/picasso-gpu-fw`, pinned 31964-byte SQE embedded); image
  `out/wf99/picasso/boot/wf99.img` (sha `5bfce36e…`), same addon as wf98; log
  `out/wf99-runs/run1.log`.
- **wf99 result: the boot-slumber request does not exist on this firmware and, left pending, it breaks
  HFI startup.** Serial: `gmu: rail-on vote=01000404 (cmd-db 625000000 Hz) idx=4/4 polls=10000
  info=00000000 gx 000000c0->00000080` — no ack; `sptprac power (pre-HFI) status=00000080`; then
  `hfi: stage=1 id=4 status=10 fw=0`, `first mismatched response header=00000000 ret=00000000
  code=00000000`, `abort: stage=3`, `end stage=3 step=3 error=-32`. The un-acked request bit 22 stayed
  set in `HOST2GMU_INTR_SET` (mainline calls `a6xx_gmu_oob_clear(oob_boot_slumber)` when
  `gfx_rail_on` fails; this port did not), and the firmware's HFI exchange then failed. The vendor
  driver settles the question: downstream KGSL `adreno_a6xx_gmu.c` gates `a6xx_gmu_gfx_rail_on()` on
  `ADRENO_QUIRK_HFI_USE_REG` (a DT quirk), maps OOB for every non-a630/a615 target as
  `BIT(30 - req*2)` / `BIT(31 - req)`, and `adreno_has_sptprac_gdsc()` is true only for a630/a615 —
  a620 keeps the GMU owning GX and SP/TP, and the port's GPU_SET (30/31) is exactly that mapping. The
  rail-on code and the bit-30 "boot-slumber clear" are reverted (on a620 bit 30 is the GPU_SET
  *request* bit, so that write would have re-requested it). The manual SPTPRAC write is retained only
  as a diagnostic no-op.
- **wf100: leave secure mode with `CP_SET_SECURE_MODE(0)`.** Official evidence (zap shader series,
  freedreno): a5xx/a6xx GPUs power on in **secure mode**, which restricts what memory the GPU can
  write; mainline leaves it via the zap shader + `CP_SET_SECURE_MODE(0)`, or, on targets without
  zap, by writing `RBBM_SECVID_TRUST_CNTL = 0` from the CPU — which this port measured as a bus hang
  (wf80). The port has no PIL/SCM to run the zap, never sends the CP packet, and reads
  `RBBM_SECVID_TRUST_CNTL = 1` at hw_init. This is the one state known to differ from a
  rendering-clean target and it matches the measured signature exactly (the CP executes; every
  draw/blit/compute from the 3D/UCHE path leaves non-secure client memory untouched). The kernel now
  submits `CP_SET_SECURE_MODE(0)` (opcode `0x66`, one payload dword) on the ring right after
  `cpInit()`, prints `RBBM_SECVID_TRUST_CNTL` before/after and the packet's idle result, and continues
  with the no-op IB. `left=1` with `trust 1 -> 0` means the GPU switched to non-secure operation;
  `left=0` with a CP status dump means the secure world refused the packet and the zap (PIL/SCM) is
  mandatory. Kernel `3e13b81b94a005f87df310ddbc6897a22a1f03dfb4a468348402f9ad62938b0e` (rebuilt with
  `ADRENO620_FW_DIR`); image `out/wf100/picasso/boot/wf100.img`, same addon as wf98; log
  `out/wf100-runs/run1.log`.
- **wf100 result: the pinned SQE rejects `CP_SET_SECURE_MODE` with `CP_OPCODE_ERROR`; the GPU never
  leaves secure mode.** Serial: `CP_SET_SECURE_MODE(0) submitted wptr=11`, `idle rptr=11`,
  `secure mode left=1 trust 00000001 -> 00000001`, then `fence timeout wanted=1 have=0`,
  `fence rptr=20 wptr=20 status=00800005 status3=00000000 int=00800200 fault=00000000 cpint=1
  sqe_stat=000000a2`. RBBM_INT_0_STATUS `0x00800200` is `CP_HW_ERROR` (bit 9) + `RBBM_HANG_DETECT`
  (bit 23), and CP_INTERRUPT_STATUS `0x1` is **CP_OPCODE_ERROR**. The packet header/opcode
  (`0x66` with one payload dword) matches both mainline `OUT_PKT7(CP_SET_SECURE_MODE, 1)` and the
  vendor `cp_secure_mode()`, and the CP advanced its read pointer over the packet before erroring.
  So the pinned linux-firmware `a650_sqe.fw` 0x112 does not accept this opcode on this a620 CP, and
  `SECVID_TRUST_CNTL` stays 1. (Stock KGSL sends this packet only after the zap has loaded; with no
  zap it writes the register instead, which this port measured as a bus hang in wf80.)
- **Official-source facts now pinned for this part (downstream msm-4.19 KGSL at
  `kernel/msm` commit `a1e88665ac80`, and the 2026 mainline Adreno 620 series).**
  - `adreno_gpu_core_a620`: family A620, `sqefw_name = "a650_sqe.fw"`, `gmufw_name =
    "a650_gmu.bin"`, `zap_name = "a620_zap"`, `gmem_size = SZ_512K`, `veto_fal10 = true`,
    `hang_detect_cycles = 0x3ffff`, `disable_tseskip = true` (PC_DBG_ECO_CNTL bit 9),
    `protected_regs = a620_protected_regs`, `vbif = a650_gbif_regs`. The port already matches all of
    these.
  - `adreno_has_sptprac_gdsc()` is true only for a630/a615: **a620 does not power SPTPRAC from the
    CPU**, and `a6xx_gmu_gfx_rail_on()` (BOOT_SLUMBER) is gated on `ADRENO_QUIRK_HFI_USE_REG` (DT
    quirk). For every non-a630/a615 target the OOB mapping is `set = BIT(30 - req*2)`,
    `ack = BIT(31 - req)`, `clear = BIT(31 - req*2)`, i.e. GPU_SET = request 30 / ack 31, which is
    exactly what `gxPowerOn()` does.
  - KGSL's `adreno_set_unsecured_mode()`: **if the zap loaded, switch via `CP_SET_SECURE_MODE(0)`
    (`cp_secure_mode`, opcode `0x66`, one payload dword) and spin for idle; if not, write
    `RBBM_SECVID_TRUST_CONTROL = 0` from the CPU.** Mainline's zap loader (`adreno_zap_shader_load`,
    `GPU_PAS_ID = 13`) loads `a620_zap.mbn` into the `qcom,zap-shader` reserved region with
    `qcom_mdt_load` + `qcom_scm_pas_auth_and_reset`, then sends the same packet. This platform
    therefore expects the zap: the CPU register write hangs (wf80), and the CP packet was rejected
    with `CP_OPCODE_ERROR` by the pinned SQE (wf100).
  - The 2026 mainline A620 series (Ishu Brar, v4) confirms A620 is an A650 derivative on SM7250
    (SD765/765G/768G), reuses the a650 SQE/GMU firmware, needs the PDC sequencer programmed in the
    driver (xo/mx/cx from cmd-db; the port already does this) and its own BCM bandwidth table, and
    uses `gmu_cgc_mode = 0x00020202` (the pinned catalog still says `0x00020200`; the port follows
    the catalog). It was validated with working Vulkan on a OnePlus Nord.
- **wf101: run the device's own SQE instead of the pinned linux-firmware one.** The device's
  firmware partition ships a **different** `a650_sqe.fw` (version 0x087, 31460 bytes, CRC32
  `0x59c1d422`) than the pinned image (0x112, 31964 bytes), and the stock Android KMD runs 0x087 on
  this exact CP. A firmware/CP generation mismatch is a first-class suspect for "simple CP packets
  execute while draws and blits dispatch nothing" and for the `CP_OPCODE_ERROR` above. The build now
  embeds the vendor image (`ADRENO620_FW_DIR/a650_sqe.fw`; the pinned 0x112 file is preserved as
  `a650_sqe_pinned_0x112.fw`), `kSqeAlt` pins it by size/CRC/SHA-256, and the alternate's version
  floor drops to 0x080 (the 0x095 floor is mainline's a650 security-fix floor, not a CP
  compatibility floor; the boot args already carry `sheng-gpu-allow-old-sqe=1`). The
  `CP_SET_SECURE_MODE` packet is now gated on `sheng-gpu-secure-mode=1` so the SQE is the only
  variable: wf101 runs without it (does the smoke write memory with the device's own SQE?), and a
  follow-up boot of the same image with the flag re-tests the secure-mode packet against the SQE.
  Kernel `5b89210f2765d825a2f5dbf6e4c148d061566fba0d4a11c9c05b749390a7fa42` (embedded SQE size
  31460 confirmed in `out/picasso/tree/build/adreno-picasso/adreno620_embedded_firmware.c`); image
  `out/wf101/picasso/boot/wf101.img`, same addon as wf98; log `out/wf101-runs/run1.log`.
- **wf101 result: the device's own SQE is not the difference.** The probe accepted the vendor image
  (`firmware: … sqe=a650_sqe.fw version=0x87 security=1`), the CP came up (`cp: ready fence=1`,
  `end stage=4 step=4 error=0 done=1`), and the raw test is byte-identical to the pinned-SQE runs
  (`C s600df00d cc0debeef afedcba98 b0`). The Turnip smoke is also identical:
  `T s0 r0 v512 c0 g0/4096`, `S fdeadbeef … kdeadbeef … r5a5a…`, `Q 68feefef/68fef02e 1/0 r0`.
  The SQE generation is not what drops the draws; both the vendor 0x087 and the pinned
  linux-firmware 0x112 execute the same simple packets and leave the 3D/A2D path silent.
- **wf102: `CP_SET_SECURE_MODE(0)` against the vendor SQE (same image, one boot arg).** wf100 proved
  the pinned SQE answers the packet with `CP_OPCODE_ERROR`; the device's own SQE is what stock
  Android uses with the same packet. Boot the wf101 image with `sheng-gpu-secure-mode=1`. If the
  packet is accepted and the no-op fence still retires, the GPU is switched by the CP packet alone
  (the zap then only matters for the TSB/secure-world bookkeeping) and the smoke is the decisive
  read; if the fence hangs again, the zap path (PAS/SCM) is mandatory. Image
  `out/wf102/picasso/boot/wf102.img`, kernel `5b89210f…` (vendor SQE), same addon; log
  `out/wf102-runs/run1.log`.
- **wf102 result: the CP packet alone cannot switch the GPU; the zap is mandatory.** With the
  device's own SQE the packet is accepted (`CP_SET_SECURE_MODE(0) submitted wptr=11`,
  `idle rptr=11`) and `RBBM_SECVID_TRUST_CNTL` still reads 1, but the very next no-op submission's
  fence never arrives (`fence timeout wanted=1 have=0`, `end stage=4 … error=-40`). Without the
  zap, switching the CP's secure state leaves its own memory writes unusable. The CP_PROTECT
  opcode error in wf100 was SQE-specific; the real gate is the secure world.
- **Official cause, confirmed:** a5xx/a6xx GPUs power on in **secure mode, where they "can only
  render to protected buffers"** (Adreno zap-shader series). That is exactly this port's
  signature: every CP packet works, every draw/blit/compute to non-protected client memory is
  silently discarded. The only way out is the signed zap loaded through TZ (PAS) followed by
  `CP_SET_SECURE_MODE(0)`; the CPU write to `SECVID_TRUST_CNTL` is blocked on this device (wf80).
- **wf103: probe the SCM conduit and the PAS calls.** Before implementing the zap loader, measure
  whether this kernel can talk to the QCOM secure monitor at all. `Adreno620Cp.cpp` gains an
  SMC helper (same `smc #0` pattern as `drivers/ArmCpu/ArmCpu.cpp`) and `scmProbe()`, which asks
  `SCM_SVC_INFO/IS_CALL_AVAIL` (`0x06/0x01`, owner SIP, SMC64 fast call) whether
  `SCM_SVC_PIL` init_image `0x01`, mem_setup `0x02` and auth_and_reset `0x05` exist; the reply
  registers are printed at hw_init. This changes no GPU state. Kernel
  `f813105ec0f3ca92f1e1bfd2a84563df0f60fd95d54cbad095b5bbd71821890b`; image
  `out/wf103/picasso/boot/wf103.img`, vendor SQE, same addon; log `out/wf103-runs/run1.log`.
- **Zap loader design (for the boot after wf103, if the PAS calls answer).** From the decompiled
  Picasso DTB and downstream sources: `qcom,kgsl-hyp` = `qcom,pil-tz-generic`, pas-id `0x0d`,
  firmware-name `a620_zap`; `peripheral-loader.c` allocates a relocatable DMA region (the zap's
  ELF segments are QCOM_MDT_RELOCATABLE), so no reserved carveout is needed. Sequence:
  1. parse `a620_zap.mdt` as ELF32; metadata = program-header block (`phdrs[0].p_filesz` bytes)
     followed immediately by the hash segment (for split firmware it is packed in the .mdt);
  2. `PAS_INIT_IMAGE(0x0d, metadata_phys)` with arginfo `SCM_ARGS(2, VAL, RW)` = `0x82`
     (`SCM_SIP_FNID(0x02,0x01)` = `0x0201`, SMC64 SIP call = `0x42000201` STD / `0xC2000201` FAST);
  3. copy the .b00..b02 segments to `region + (p_paddr - min_paddr)`, then
     `PAS_MEM_SETUP(0x0d, region_phys, span)` (arginfo `0x03`);
  4. `PAS_AUTH_AND_RESET(0x0d)` (arginfo `0x01`) — TZ authenticates and runs the zap on the GPU;
  5. then `CP_SET_SECURE_MODE(0)` and, if it takes, the Turnip smoke is the decisive read.
  Metadata and image buffers must be physically contiguous, 4 KiB aligned and non-cacheable.
