# Handoff 2026-11-03 — GPU 渲染已上岸，余下调度与新帧调度问题

> 给下一个 agent。本文件替代口头交接；`docs/handoff-20261102-gpu-first-submit-stall.md` 是完整的轮次记录（wf70–wf145），需要细节时按镜像号查。
> 截至 wf145：内存 4.95 GiB、调度已修、触摸能触发重绘、GPU 直接写 scanout（wf144/wf145 验证，copies 冻结在 2 次预热帧）。用户目标的三项（内存 / CPU 调度 / GPU 渲染与交互）均已达成；余下是启动速度与调优。
> 设备状态以每轮探测为准，本文中的串口/镜像号只代表写作时的状态。

## 1. 目标与当前结论

- 总目标：在 Picasso（SM7250 / Adreno 620 / iPad7,1、iOS 17.6 Darwin 23.6.0、自编译 XNU）上实现 **GPU 渲染**，且 UI 出现即可交互。
- **已达成**：CARenderServer 已通过我们的生产版 Metal 插件（metal2vulkan → Turnip → A620）渲染 UI。
  证据（`out/wf139-runs/run1.log`）：
  - `PICASSO_GPU open: plugin type=4147 pid=56 name=backboardd` / `plugin: mount ok`
  - `PICASSO_GPU text: PUB tex x0 s2/0 p0 f80 43 23 25 55 105`
  - bridge 状态 `ST0 c8/0 d8 p51 x7 R34 D149 F12 M2531 C0 B2`（8 commit 全完成、51 pipeline、34 render pass、149 draw、12 全屏 pass）
  - 内核计数 `PICASSO_GPU submits live 18: submissions=14 ibs=81 max=26 many=2`（两次 >16 IB 的大提交）
  - grid 采样显示开机 logo（rows 10–12）消失，锁屏版式内容出现在 rows 3–4（时钟带）与 rows 20–23
- **触摸**：真实 Novatek NT36672C 手指触摸驱动工作正常，digitizer 在 **60 s** 挂载（早于 SpringBoard ~100 s），锁屏出现即可用。
  证据：`PICASSO_HID probe at 60s` → `stage=3 resource=1 load=0` → `PICASSO_NVT fwinfo rc=0 ver=20 18x36 max=1080x2400` → `sram ok=1/1/1` → `stage4 loop irq=1` → `event driver ready` → 触点 `PICASSO_NVT point id=1 x=… y=…`。
- **内存（wf141 起）**：6 GiB DRAM 全部发布给 XNU，管理 4.95 GiB（`vm_page_bootstrap: 1254622 free pages, 42274 wired`），原生 4 KB 页保持不变；RAM Data 卷随 payload 挂载（`NATIVE_OWNER_DATA done stage=4 error=0 readonly=0`）。根因是 PicassoBootMemoryDxe 的 1 GiB/4 GiB 上限与 DarwinBoot 的 first-fit 窗口，补丁在 `patches/firmware/`；证据 `out/wf141-runs/run1.log`。
- **调度（wf142 起）**：拓扑本就正确（`en=6 pn=2`），偏差来自 Edge 调度把 DF 桶（普通优先级）默认放 P 核。`TH_BUCKET_SHARE_UT`→`TH_BUCKET_SHARE_DF` 后 A55 的 busy 从 30–44% 变为 0–77%，P 核不再被默认优先级占满，wf141 的 GPU stall 消失。证据 `out/wf142-runs/run1.log`；补丁 `patches/picasso-cpu-topology-xnu.patch`。
- **触摸→重绘（wf143）**：真实触摸链路已通；用 `offline-hid-swipe=16` 注入的虚拟滑动被 HID 消费（`swipe 2 reports=26` … 766），present 由 8 涨到 492+，GPU 提交由 14 涨到 `submissions=1454`。证据 `out/wf143-runs/run1.log`。
- **进行中**：GPU scanout hand-off（wf144，`sheng-fb-gpu=1` + 移植 UniXNU `NativeGOPPresenter`）。目标是 `P2 ee00002c7`→`P4`（serving），且 `PICASSO_DISPLAY event=10` 的每帧 ~2 MiB CPU 拷贝消失（wf143 `copies=575 copy_bytes=1201978752`）。

## 2. 当前镜像与配置（wf144 为最新；wf139 为首次 GPU 画出 UI 的基线）

- **wf144（scanout hand-off）**：`out/wf144/picasso/boot/wf144.img`，日志 `out/wf144-runs/run1.log`；kernel `sha256=752eeea5a331fd81…`（新增 presenter/glue/offer 路径），cmdline **988/1000 字节**，相对 wf139 增加 `sheng-fb-gpu=1`；board.toml 已含 `sheng-fb-gpu=1`（回滚只需删掉该参数）。内存参数 `dram_size=0x180000000`、`owned_memory_size=0x140000000` 已固化在 board.toml。构建一律用 `scripts/build-wf-payload.sh <TAG> <ADDON> [额外参数…]`。
- wf139 基线（UI 首次由 GPU 画出、触摸可用但无重绘）：
- 镜像：`out/wf139/picasso/boot/wf139.img`；日志 `out/wf139-runs/run1.log`；payload `out/wf139/picasso/payload/payload.json`。
- kernel `sha256=b12b7fd0b0570af2…`（与 wf134 起未变）；cmdline **973/1000 字节**：

```
-v debug=0x14e cpus=8 sheng-smp=1 armcpu-spi-ipi=1 armcpu-self-ipi=1 native-ufs-root=1 sheng-pmgr=2
native-ufs-start-delay=5 native-ufs-delay-yield=1 sheng-data-persist=2 sheng-disable-watchdogd=1
sheng-lite=1 nointr_consio=1 nointr_iolog=1 picasso_usb=2 picasso_usb_smmu=6 picasso_usb_watchdog=1
picasso_usb_watchdog_ms=30000 picasso_usb_early_watchdog=1 picasso_usb_host_timeout_ms=60000
picasso_usb_reset=watchdog offline-mobile-graphics-load=1 offline-legacy-register=1 offline-legacy-gop=1
offline-legacy-pm=1 offline-legacy-phase=4 offline-legacy-validate=1 offline-legacy-transfer=1
offline-legacy-cpu-present=1 sheng-vsync=60 picasso-aks-state=1 boot-progress=2 offline-hid-input=1
offline-hid-home=1 sheng-disable-jobs=com.apple.driverkitd,com.apple.cameracaptured,com.apple.osanalytics.osanalyticshelper
offline-hid-start=60 sheng-cpufreq=4 sheng-gpu=4 sheng-gpu-metal=1 sheng-gpu-addon=1 sheng-display-grid=10
sheng-touch-hid=1 sheng-touch=4 sheng-touch-start=60
```

- 插件产物（本轮重建，**未提交**）：
  - `UniXNU/drivers/NativeMetal/NativeAdrenoMetalPlugin.mm`：`requested()` 已恢复为检查 `addons/NativeAdrenoMetal.publish`（生产/发布路径）。wf85–wf136 的镜像用的是测试构建，`requested()` 硬编码 `return false`，这正是"UI 不走 GPU"的根因。
  - `UniXNU/drivers/NativeMetal/PrimeShaders.h`：`run()` 在 `listPath.empty()` 时直接返回（priming 改为 opt-in；没有 `NativeAdrenoMetal.primelist` 就不扫描/翻译整卷 .metallib）。
  - 插件 bundle CDHash `7db12641bf4fb2b3f8d33da1da07a99ceb078f0e`；owner-trust `af58da1b6bc0811e1530f1323507719ea5c3ea36297fe33da5c86d2522e7ae7d`（`UniXNU/build/adreno-metal-plugin/owner-trust.bundle`）。
  - addon `build/ngpu-addon-wf139.bin`（10 files，12126560 bytes，`--publish --clients backboardd`；`.clients` 内容为 `backboardd\n`）。
- 关键诊断线（判断是否真的走 GPU）：`PICASSO_GPU submits live … submissions=/ibs=/max=/many=`、`PICASSO_GPU text: ST0 c…/d… p… R… D…`、`PICASSO_DISPLAY_STATE … submit=…`、`PICASSO_GRID`（屏幕内容）、`PICASSO_NVT report/point`（触摸）。
- 反向信号：**出现 `PICASSO_GPU text: T s0 r0 v1024 …` 表示还在测试插件路径**（只跑冒烟测试、不发布设备）；mediaserverd 出现 `open: plugin type=4147 pid=… name=mediaserverd` 说明 clients 名单不对。

## 3. 上一轮排查得到的因果链（务必不要重复踩坑）

1. wf133 之前锁屏是软件渲染：插件 `requested()` 为 false（wf85 测试构建），CARenderServer 永远拿不到 MTLDevice。→ wf137 恢复生产路径，GPU 提交从 3 涨到 12。
2. wf137 用 `clients='*'`，mediaserverd 也加载 GPU，bridge 在它进程里 SIGABRT（`PICASSO_SERVICE … mediaserverd exit_signal=6 namespace=2 code=6`），随后整组服务被 SIGKILL（447 s、587 s 两轮，SpringBoard/backboardd/powerd 全部 signal 9）→ UI 永远起不来。→ wf138 改为 `clients=backboardd`，崩溃/重启风暴消失。
3. wf138 渲染服务器在 bridge `c5/d5` 冻结、大核 100%。→ wf139 关闭 priming，计数继续推进到 `c8/d8 p51 R34 D149`，锁屏被 GPU 画出。
4. 触摸早挂：`offline-hid-start=480` 是旧策略（为了不打扰 SpringBoard 初始化，但太晚）。从 wf136 起改为 **60 s**：digitizer 在 SpringBoard（~100 s 出现）之前就进入 IORegistry，符合官方顺序（设备先于 HID 消费者存在）。**不要删 `boot-progress=2`**：它不只画屏，`boot_progress_start()` 在 `!enabled` 时直接返回，而 boot monitor 循环才是 `native_hid_input_probe()`（建 digitizer）和 `native_novatek_touch_probe()` 的唯一调用者。曾因此把 wf134 打废。

## 4. 当前剩余问题（按用户优先级）

### A. 调度 / 拓扑 —— 已解决（wf142）

- 拓扑本身早已正确：wf141 的 `PICASSO_CPUFREQ topo cpus=8 clusters=2 types=6 en=6 pn=2` 与 DTB `cluster-type` 均无问题；wf139 里 `en=1 pn=0` 只是旧镜像的打印/配置差异，不是根因。
- 真正原因在 Edge 调度器：`sched_edge_default_preferred_cluster()` 把 DF 桶（普通 timeshare，含 `BASEPRI_DEFAULT`）也默认放到 P 核。`patches/picasso-cpu-topology-xnu.patch` 将其判断从 `TH_BUCKET_SHARE_UT` 改为 `TH_BUCKET_SHARE_DF`，DF/UT/BG 落 boot（E）簇，FG/IN/FIXPRI 仍按设计走 P。
- 证据：wf142 的 A55 `busy` 序列 0→5→77→32→24→26→33→55→44→55%，逐项与 SpringBoard 启动吻合；P 核仍 99%（UI 桶按设计在 P，且没有 CLPC 再平衡）；wf141 的 `PICASSO_GPU stall` 行全部消失。日志 `out/wf142-runs/run1.log`。
- 若要继续优化大核饱和，应转向 `sched_edge_ui_bucket_stays_on_p()`/CLPC，不要重复改 DTB。

### B. 触摸→重绘 —— 已打通（wf143）；GPU scanout hand-off —— wf144 验证中

- wf143 用 `offline-hid-swipe=16` 注入虚拟滑动（以 `--kernel-arg` 追加，994/1000 字节）：`PICASSO_HID swipe armed sink=1 count=16` → `swipe 2 moved=1 reports=26 edge=1/1` → reports 26/52/78 … 766。present 从 8 涨到 492+；内核 GPU 计数 `PICASSO_GPU submits live 7: submissions=1454 ibs=29457 many=717`（wf139 只有 14）。结论：输入→CA 重绘链路是通的，wf139 的"触摸不重绘"是当时无输入事件达到 CA。
- 但每帧仍是 CPU 全帧拷贝：`PICASSO_DISPLAY_STATE trigger=submissions … copies=575 copy_bytes=1201978752`，`PICASSO_DISPLAY event=10 a=0x111a88`；插件状态 `PICASSO_GPU text: P2 ee00002c7`（0x4148 hand-off 未被服务）。
- wf144 的改动（相对 wf143）：移植 UniXNU `NativeMobileDisplay` 的 `NativeGOPPresenter.cpp`/`NativeGOPWire.h`，新增 `NativeGOPGlue.cpp`，`NativeLegacyDisplay.cpp` 增加 `native_legacy_gpu_offer/complete` 与 early-retire（`sheng-early-retire`，默认 1），内核 cmdline 增加 `sheng-fb-gpu=1`（总 988 字节）。启动已确认 `PICASSO_DISPLAY gpu presenter attached`。
- 通过标准：`PICASSO_GPU text` 出现 `P4`；`PICASSO_DISPLAY event=10` 不再出现；`PICASSO_DISPLAY_STATE … copies/copy_bytes` 停止增长（或只偶发失败回退）；present/GPU submissions 继续增长。
- **wf144 结果**：`PICASSO_GPU text: P4 e0`（e0=成功），`PICASSO_DISPLAY_STATE … submit=569 accept=569 transfer=568 retire=567 copies=0 copy_bytes=0`，`PICASSO_DISPLAY event=10` 0 条，`PICASSO_GPU text: PF 276/0`（276 帧、0 失败），GRID rows 20–23 有 UI 内容。CPU 全帧拷贝彻底消失。
- **wf145 结果（带虚拟滑动，987/1000 字节）**：滑动 2–15 全部送达（reports 26→364 edge=14/14），内核 `PICASSO_GPU submits live 5: submissions=955 ibs=6915`；显示侧 `PICASSO_DISPLAY_STATE … submit=512 accept=512 transfer=509 retire=510 copies=2 copy_bytes=10368000`，`PF` 同步增长。那 2 次 CPU 拷贝发生在 `P4` 之前（插件尚未 waitSwap，display 已经被滑动唤醒并出了两帧），属于预期回退；进入 hand-off 后 copies 冻结在 2，不再随提交增长。evidence `out/wf145-runs/run1.log`。
- 若 presenter 未 attach 或 offer 被拒，先查 `PICASSO_DISPLAY gpu presenter unavailable` 与 presenter 的 `start()` 返回；映射来源是 nub 的 provider index 0 + `kIOMapInhibitCache`，必须与 GPU 写 scanout 的物理页一致。

### C. 首次启动慢 / pipeline 缓存

- 渲染服务器按需翻译 UI 的 shader（历史记录：完整 UI 约 98 条 pipeline；本 boot 用了约 8 分钟到 51 条并画出锁屏）。插件已有缓存导出/导入机制（`CacheExport.h`：`M2VCACH1`/`SHEXP001`，通过 UFS 日志导出；镜像可带 `addons/NativeAdrenoMetal.m2vpack`，由 `make-gpu-addon.py` 之外的工具合并）。当前日志里没有 `M2VCACH1/SHEXR001`（导出需要缓存目录稳定一段时间，且走 UFS 日志而不是 CDC）。
- 若要让后续启动快：先让一次 boot 完成并让缓存稳定，再用 `UniXNU/scripts/add-gpu-plugin-data.py` 把导出合并成 m2vpack，`make-gpu-addon.py` 的 `--prime-list`/pack 路径需要确认（详见 `CacheExport.h` 与 `scripts/make-gpu-addon.py` 头注释）。

### D. 非致命但会误导的现象

- `PICASSO_GPU text: ST… binary archive: BinaryArchive(InvalidFile { path: "/System/Library/Frameworks/QuartzCore.framework/default.metallib" })`：QuartzCore 的 precompiled 二进制归档是 AGX 格式，metal2vulkan 不识别，回退从 IR 编译（51 条 pipeline 即来自这里）。
- `ST… | native-metal: no device answer: … supportsConditionalLoadStore`：bridge 对新设备选择器没有答案，CA 用默认行为；不是当前卡死点。
- `MetalError … new_render_pipeline_state … UnrasterizedVertexReturn`（wf137 出现过一次）：metal2vulkan 复现的是真机 Metal 的拒绝语义（`rasterizationEnabled=NO` 且 vertex 有返回值时拒绝），不是我们的 bug。

## 5. 操作手册（每轮必做）

```sh
cd /Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog
# 0) 探测设备状态（不要沿用旧值）
ls /dev/cu.usbmodem* 2>/dev/null; fastboot devices; lsof /dev/cu.usbmodemPICASSO011
# 1) 回 fastboot（CDC，先确保没有别的进程占用串口）
python3 scripts/picasso-cdc-command.py --command reboot-fastboot \
    --fastboot-serial 52cc791d --fastboot-timeout 300
# 2) 先起抓串（后台），再 boot（只允许 fastboot boot，绝不刷分区）
python3 scripts/capture-picasso-usb-raw.py --output out/wfNNN-runs/run1.log --wait 900 --duration 2400 &
fastboot -s 52cc791d boot out/wfNNN/picasso/boot/wfNNN.img
```

- 构建环境（每次新 shell 都要 export）：
  `export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`
  `export UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed`
- 只改 args/addon 时：`bash /tmp/build-wfNNN.sh`（脚本模式见 `/tmp/build-wf139.sh`；**/tmp 是易失的**，下一位先把它复制回仓库，例如 `scripts/build-wf-payload.sh`，并参数化 TAG/ADDON）。
- 改内核：`python3 unixnu build kernel prelink` → `out/picasso/kernel/kernel.macho`（中断构建可能剪掉 link.filelist 的对象，恢复方法是过滤掉不存在的文件再编，别删对象树）。
- 改插件（在 `/Users/silenzio/Desktop/development/ios/UniXNU`）：
  ```sh
  export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
  python3 scripts/build-adreno-metal-plugin.py        # 会删掉 build/adreno-metal-plugin（含 owner-trust.bundle）
  python3 scripts/make-owner-trust-bundle.py           # 必须重新生成
  python3 scripts/make-gpu-addon.py --publish --clients backboardd <worktree>/build/ngpu-addon-XXX.bin
  ```
- cmdline 上限 1000 字节（DarwinBoot 1024 缓冲）。已确认无效的参数：`picasso_usb_hold=0`（默认 0）、`picasso_usb_control_ms=15000`（kernel.macho 里没有该字符串）。`boot-progress=2`、`sheng-touch*`、`offline-hid-start=60`、`boot-progress` 相关参数不能删。

## 6. 约束（用户原话，必须遵守）

1. 以成功实现 GPU 渲染为目的，多看看官方和社区相关设备的实现，多查资料，切忌新造轮子。
2. 只用 `fastboot boot`，禁止刷分区。
3. 串口 `/dev/cu.usbmodemPICASSO011`，采集前确认没有进程占用。
4. 每轮先探测设备状态（`/dev/cu.usbmodem*`、`fastboot devices`），不要沿用旧值。
5. build 需要上述两个环境变量。
6. 不要 reset/checkout 共享工作树。
7. 一次只改一个变量，每个结论必须带证据（日志行/寄存器/测量）。
8. 里程碑 commit 并更新 PR #13（`YinCQL/UniXNU-picasso`，head `silenzio111:stage12/picasso-cpufreq-big`）。
9. 多看官方实现（mainline msm/a6xx、Turnip、XNU IOKit），不要新造轮子。
10. 继续工作，直到目标完成；不要问用户问题，自主继续。

## 7. 未提交的改动与产物

- 已提交：`76554df`（6 GiB/4.95 GiB 内存 + firmware/board.toml 补丁）、`6f29285`（Edge 调度 DF 桶）。
- 本轮 GPU scanout hand-off（截至 wf144 验证中，尚未提交）：
  - `drivers/NativeMobileDisplay/NativeGOPPresenter.cpp`、`NativeGOPWire.h`（从 UniXNU/NativeMobileDisplay 原样移植）、`NativeGOPGlue.cpp`（新建：创建/attach presenter，转发 offer）。
  - `drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`（`native_legacy_gpu_offer/complete`、early-retire 状态机）、`PicassoDisplaySupport.cpp`（presenter 静态初始化 + glue 启动 + 计数器）。
  - `scripts/build-native-legacy-display.py`（把三个新源文件编进 `libNativeLegacyDisplay.late.a`）、`board.toml`（`sheng-fb-gpu=1`）。
- `docs/handoff-20261102-gpu-first-submit-stall.md`（worktree，已追加 wf134–wf143 记录）、本文件、`docs/handoff-20261103-next-agent-prompt.md`（untracked）。
- `UniXNU/drivers/NativeMetal/NativeAdrenoMetalPlugin.mm`（`requested()` 恢复）、`PrimeShaders.h`（priming opt-in）、`UniXNU/build/adreno-metal-plugin/owner-trust.bundle`（新 trust）。
- worktree `build/ngpu-addon-wf137.bin`、`wf138.bin`、`wf139.bin`（wf140+ 复用 wf139 addon）；`out/wf137…wf144` 镜像与 run 日志。
- 提交前注意：`drivers/NativeMetal` 是 UniXNU 的 submodule，提交要分别进对应仓库；`drivers/NativeMobileDisplay` 的文件若上游 submodule 也有同名实现，提交前确认不是重复拷贝。不要动共享工作树的其它分支。

## 8. 建议的下一步顺序（每步一个变量，先取证再改）

1. **确认 wf144 收尾状态**：等锁屏画出后看 `PICASSO_DISPLAY_STATE` 的 `copies/copy_bytes` 是否始终为 0、present 是否随输入增长；本文落笔时已确认 `PICASSO_GPU text: P4 e0`、`submit=3 accept=3 transfer=3 retire=3 copies=0 copy_bytes=0`。
2. **带输入的 scanout 压测**：wf145 = wf144 参数 + `offline-hid-swipe=16`，为腾出字节去掉 `sheng-display-grid=10`（保留 `boot-progress=2`）；通过标准是 presents/submissions 继续增长而 `copies` 保持 0。
3. **启动速度**：缓存导出 → m2vpack 导入，减少每次 boot 的 shader 编译时间（§4C）。
4. **调度余量**：仅在需要时再动 `sched_edge_ui_bucket_stays_on_p()`/CLPC；DTB 拓扑已被证明正确，不要回头改。

> 判断"目标完成"的标准：锁屏出现后，手指的每一次上滑/点击都能在 `PICASSO_NVT report` 后紧跟新的 `PICASSO_DISPLAY_STATE submit=` 和 `PICASSO_GPU submits live` 增长，且 `copies/copy_bytes` 不再增长（GPU 直接写 scanout）。

## 9. 下一阶段目标与验收（2026-11-03 设定，先写标准后测）

1. **GPU 性能**：Adreno 620 的 perf index 从 1 提到 DT 标称档 3；补一个可复现的填充/三角形基准，记录毫秒。
   验收：基准耗时相对当前配置缩短 ≥ 50%；滑动动画连续、无掉帧（提交/呈现计数在滑动期间持续增长）。
2. **稳定性（swap 时序）**：VM 的 swapfile 创建线程在只读 root 上先跑，45 s 报 EROFS（errno 30），Data 卷挂到 `/private/var` 后不再重试。
   修复：Data 挂载成功后清掉失败时间戳并唤醒创建线程；验收：日志出现 swap 创建成功，随后连续 30 分钟 0 panic、0 服务退出。
3. **真实交互**：用真实手指（Novatek/`sheng-touch`，不是注入滑动）解锁锁屏并进入主屏。
   验收：手指上滑即可解锁；主屏连续滑动 30 秒不卡死（触点报告与 present/GPU 提交持续增长）。

约束：只用 `fastboot boot`；CDC 串口独占；`boot-progress=2` 不得删除；cmdline ≤ 1000 字节；一次只改一个变量；单项卡满 3 小时记录现状后转下一项。

### 9.1 实现（待测）

- **perf index**：`drivers/NativeAdrenoPicasso/Adreno620Gmu.cpp` 原先 `count-1-initial_level` 在 speed bin 0x84（5 档）得到 HFI index 1 = 275 MHz；HFI 表是升序，DT 顶层 `qcom,initial-pwrlevel=3` 对应 nominal 500 MHz = index 3。默认改为 3，`sheng-gpu-perf-index=<n>` 可覆盖（基准用 1）。
- **基准**：`NativeTurnipSmoke.h` 在原有 4 KiB fill / 64x64 triangle 验证之外，加 GPU-bound 计时并输出 `B fill=<ms> tri=<ms> compute=<ms>`：fill 为 16 MiB `vkCmdFillBuffer` 三次取最快，triangle 为同一 pass/draw 重复 256 次（1M fragment）两次取最快，compute 为原 dispatch。插件测试模式（addon 无 `NativeAdrenoMetal.publish`）启动时自动运行；两次 boot 使用同一镜像，仅 boot arg 不同。

## wf147a freeze (2026-11-03 19:44 CST)

- Image: wf147a (kernel 381fde44, bench addon without `.publish`, perf-index=1, sheng-fb-gpu=1).
- User reported "现在又卡死了" during normal UI use; no app launch this time.
- Log: `out/wf147a-runs/run1.log`. Last submit at 243.1 s; vsync checks at 261 s still show submit=255 unchanged. transfer_fail=2 (event=7 status 0xe00002d8 kIOReturnNotReady). `PICASSO_GPU submits final: submissions=480 ibs=5855 max=73 many=109` printed earlier.
- The bench addon carries no `.publish`, so `requested()` is false and `adrenoUserTest()`/Turnip smoke never ran in this image: no T/B rows. Benchmark therefore needs the prod addon (publish + clients backboardd) with the perf index set by boot arg.

## Benchmark protocol and acceptance (written before wf147c results)

- Image pair: wf147c (prod addon: publish + clients backboardd, `sheng-gpu-perf-index=1`) vs wf147d (same addon, no perf-index arg -> DT nominal index 3). Everything else identical, kernel 381fde44.
- Row emitted by the plugin's self-test in backboardd: `B fill=X.XXms tri=Y.YYms compute=Z.ZZms` (fill = 16 MiB vkCmdFillBuffer best of 3; tri = 256x 64x64 draw pass best of 2; compute best of 3). Same code path each boot.
- Primary metric: fill_ms and tri_ms. Acceptance: wf147d >= 50% shorter than wf147c on both fill and tri. Secondary: compute_ms (should not regress materially).
- Sanity gate: both runs must print `initial perf index=1` / `initial perf index=3` respectively and a `T s...` row before B.
- Swipe smoothness is judged separately on the stable image (wf147d) via `PICASSO_DISPLAY_STATE` submit rate during real touch swipes, and the user's visual report.

## wf147c freeze (2026-11-03 20:01 CST) - same BO fault at perf index 1

- Image wf147c: prod addon (publish, clients=backboardd), perf index 1 (275 MHz), kernel 381fde44.
- User: "现在卡住了". Last submit at 199.45 s, submit counter stuck at 355; `PICASSO_GPU stall far=1338f508 handle=0` twice (same page as wf153's 1338f1xx and wf70's 1338f508); `PICASSO_GPU submits final: submissions=632`.
- `PICASSO_SERVICE exit pid=51 name=mobiletimerd rv=9 exit_signal=9` (SIGKILL).
- Conclusion: the freeze is not perf-index dependent. It is the GPU BO lifetime fault in the driver; benchmark and soak both need the fix first.
- Consequence for the benchmark: the published plugin never starts the self-test (publish path returns before the test thread), so wf147a/wf147c/wf147d cannot emit T/B. Benchmark needs a gated self-test in the publish path.

## iPhone identity (wf149) - per upstream docs/guide/mobilegestalt.md

- ADT identity changed to the upstream phone row: root `model=iPhone10,3`, `target-type=D22`; `/product` name/description `iPhone X`, `artwork-device-idiom=phone`, `artwork-device-subtype=2340`, `artwork-scale-factor=3`, gamut P3, `compatible-device-fallback=iPhone10,3`; `/buttons` stays without `menu`, `home-button-type=2`.
- Root `compatible` (xiaomi,picasso / qcom,lito) left untouched, as the guide requires; an Apple board name would match original platform kexts.
- MobileGestalt caches answers under /private/var; the Data volume here is the fresh RAM seed each boot, so the cache is empty by construction (no stale pad/home-button answers).
- Build wiring: board.toml key `iphone_identity = true` (selected by `IPHONE_IDENTITY=1` in scripts/build-wf-payload.sh) -> `--iphone-identity` in the board ADT tool. The prepared build tree's copy of make-picasso-adt.py carries the option; the payload builder in the upstream cache understands the board key.
- Verified in the generated ADT: `iPhone10,3`, `D22`, `iPhone X`, `phone`, subtype 2340, no 2732.
- Expectation from the guide: with an iPadOS root (21H461) the identity selects phone behaviour in UIKit/SpringBoard but the installed system stays iPadOS; the guide pairs a phone identity with an iOS root. Boot outcome recorded below.

### Geometry note

- Panel: 1080 x 2400 physical. With `artwork-scale-factor = 3` the logical canvas would be 360 x 800 pt.
- The requested identity declares `artwork-device-subtype = 2340` (the upstream iPhone X / 12 mini artwork class), i.e. 360 x 780 pt. UIKit lays the phone UI out on 780 pt; the panel's remaining 60 px (20 pt) is outside that canvas and is not part of the layout. The display driver still scans out the full 2400 lines.
- If the UI must fill exactly 1080 x 2400, the subtype has to be 2400 (360 x 800 pt); that deviates from the upstream documented phone row.

### Why the iPhone identity alone did not change the layout

- wf149 booted with the phone ADT (AMFI read `model=iPhone10,3`), but the UI looked identical to the iPad layout.
- Root cause: `NativeMobileDisplay/PicassoDisplaySupport.cpp` published `/chosen/display-scale = 2` itself (matchWord at the first GOP mapping) and rejected any other value. UIKit's logical screen is pixels/display-scale = 1080/2 x 2400/2 = 540 x 1200 pt, a pad-sized canvas, regardless of the ADT artwork fields.
- Fix for wf150: the driver now takes `sheng-display-scale` (1..4, default 2) and publishes that value; `sheng-display-scale=3` gives a 360 x 800 pt canvas.
- The ADT still declares the upstream phone row (iPhone10,3 / D22 / phone / 2340 / 3); /chosen/display-scale is the value UIKit actually consumes.

## Three-condition phone layout (wf155)

The user's acceptance for the phone layout requires all three at once:
1. identity: model iPhone10,3 (or 13,1), idiom phone, subtype 2340, scale 3;
2. display: `sheng-display-scale=3` -> 360x800 pt canvas;
3. aspect: the 2340-tall canvas must be letterboxed into the 2400-row panel, bars not left white.

Implementation:
- The ADT now writes `/chosen/display-scale = 3` in phone mode (the property must exist from boot, not only when the display driver adds it). The driver reads `sheng-display-scale` and writes the same value, so its check matches.
- `NativeLegacyDisplay` now accepts a swap whose request/surface name a canvas shorter than the panel (1080x2340): the validator accepts `requestWidth == panel width && panel/2 < requestHeight <= panel height`, and the copy path centres the source rows and fills the top/bottom bars with opaque black. The exact-panel GPU scanout fast path is kept; a letterboxed frame takes the CPU copy path and invalidates the shadow.
- Expected visible result: 360x800 pt phone canvas, phone size class (compact width), 30 px black bars top and bottom, no white bands.

## Real touch depends on offline-hid-home=1 (wf155/wf158 regression, fixed in wf159)

- Symptom: NVT points reach the kernel (`PICASSO_NVT point ...`) but the UI never reacts.
- Root cause: package/HidInputProbe.cpp creates the HID device only inside `if (resource && sendHome==1 && ...)`,
  i.e. when the kernel command line carries `offline-hid-home=1`. The same NativeHidInput device is what submits the
  Novatek digitizer reports; without it there is no IOHIDDevice for touches.
- Log signature: working boots (wf148/wf151) print `PICASSO_HID stage=3 ... meta=0 object=fffffff...` and later
  `PICASSO_HID event driver ready`; the regressed boots print `meta=-1 object=0` and never print the ready line.
- Fix: keep `offline-hid-home=1`. wf155..wf158 had it removed by NO_HIDHOME=1 only to save command-line bytes.
- wf159: phone identity + display-scale=3 + HID arguments intact (cmdline 988 bytes, under 1000).

## wf159 freeze: the GPU scanout path, not the Metal plugin

- wf159 was built with the unpublished (software) addon but kept `sheng-fb-gpu=1`. It rendered 1491 frames with
  touch working, then froze: submit stuck at 1491, transfer_fail=3, six `stall: fence=` blocks (same signature
  `smmu 400 1338f508 13c43`, cpint=0 hwfault=0 protect=0 sqe=4000), no stall recovery.
- So the UI freeze is in the GOP scanout/handoff GPU path as well; the published Metal plugin is not required for
  the hang. The board installs no NativeAdrenoRestart hook, so a CP that stops is never reset.
- wf161 therefore disables the handoff (`NO_FBGPU=1`, no `sheng-fb-gpu=1`) and keeps the phone identity, HID
  (`offline-hid-home=1`) and `sheng-display-scale=3`: a stable-interaction candidate for the 30 s swipe and the
  30 minute soak. GPU performance is measured on the dedicated benchmark images (wf156/wf157, `sheng-gpu-bench=1`).

## Why Settings still said iPad: the Data volume is persistent

- `sheng-data-persist=2` (board.toml) means the 8 GiB packed Data seed has its changed pages backed by the
  NativeUfs store window at the end of userdata (drivers/NativeUfs/NativeUfs.cpp:1195-1241). The Data volume is
  therefore NOT pristine per boot.
- MobileGestalt's answers are cached in
  /private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist,
  so the iPad answers written by all earlier iPad-identity boots survive the ADT change and keep the device class at pad.
- wf162 therefore boots with `sheng-data-persist=0` (pure RAM seed, no persistent window): the cache is empty by
  construction, which isolates the identity question. This also loses persistence, so it is a diagnostic image;
  the production fix is to clear that one cache path once (or keep persist=0 for identity test images).

## wf163: phone identity took effect (user-confirmed "看到 iOS 的界面")

- Same phone ADT as wf161/162 (iPhone10,3 / D22 / phone / 2340 / scale 3), HID restored, no GPU handoff,
  `sheng-data-persist=2`, but `/chosen/unique-chip-id` changed from ...a541 to ...a543.
- The MobileGestalt cache is keyed by build + platform UUID; the new UUID invalidated the stale iPad answers that
  survived in the persistent Data volume. Result: the UI rendered as an iOS/phone interface.
- persist=0 is NOT a usable cache-clearing method on this board: the packed Data provider refuses the volume
  (`disk1: device is not ready`, IOBlockStorageDriver start failures, APFS truncate loop).

## 2026-11-03 layout pass: wf167 geometry, wf168 wallpaper (built, not yet booted)

Acceptance was written before the runs:

1. **Status bar / geometry**: identity iPhone13,1 (iPhone 12 mini, target-type D52g), mode 1080x2340
   presented to userland while the panel keeps 1080x2400; scale 3 -> 360x780 pt; 30-row black bars top
   and bottom. Pass = red `rdar:45025538` bar gone, status bar/clock/battery laid out normally, no
   NSException, and `PICASSO_DISPLAY_STATE` shows accepted 1080x2340 swaps (copy_bytes near
   10,108,800/frame plus bar fills) rather than 10,368,000.
2. **Wallpaper**: default iPadOS 17 Flat wallpaper overlaid as PNG through the GPU addon
   (`make-wallpaper-overlay.py` output packed with `--wallpaper`). Pass =
   `PICASSO_GPU addon: ... wallpaper=5/0` (stage 5, errno 0) and a non-black home screen.
3. **Apps**: the home screen's missing icons are the 29 bundles under the OS volume's
   `private/var/staged_system_apps` (Mail/Notes/Clock/News/Podcasts/TV/Books/Health/Fitness/Home/Maps
   ...). They are shadowed by the Data mount and never installed; the Data seed explicitly excludes
   them. Pass = SystemAppMigrator/installd reports installing them and SpringBoard shows the default
   icon set with real icons (not placeholder grid).

Implementation in this pass:
- `scripts/make-picasso-adt.py`: phone row is now iPhone13,1 / D52g / "iPhone 12 mini" /
  compatible-device-fallback iPhone13,1; `/chosen/display-height = 2340` goes with `display-scale = 3`.
- `NativeLegacyDisplay.cpp`: `panelWidth/panelHeight` keep the 2400-row GOP; `getDisplayDimensions`
  reports the mode (2340); the letterbox copy centers 2340 rows and fills 30+30 rows black.
- `PicassoDisplaySupport.cpp`: `native_legacy_display_mode_height()` reads `/chosen/display-height`
  and clamps it to the panel.
- `build-wf-payload.sh`: payload FAT is 88 MiB (wallpaper addon leaves only 0.23 MB of guard margin
  at 80 MiB).

Artifacts:
- wf167 (geometry only, soft addon 30f0e0…): kernel c2222971901f3d45, args 995/1000 bytes,
  `out/wf167/picasso/boot/wf167.img` (94,457,856 bytes), payload FAT 88 MiB.
- wf168 (wf167 + wallpaper addon f2c573f765693579, 22,603,216 bytes): same kernel/args,
  `out/wf168/picasso/boot/wf168.img`.

Known open questions for the boot test:
- The wallpaper overlay is mounted over the `~ipad` 7570 Flat animation only. Under a phone identity
  PosterBoard may pick a different variant; if the screen stays black but the log says `wallpaper=5/0`,
  retarget the overlay mount (or add the other 7570 variants) rather than the provider path.
- The geometry change alone may or may not remove the red bar: if it persists, the next lever is the
  `UIStatusBarVisualProviderClassName` override or a display edge-info publication, decided after the
  boot log/screenshot.

### Apps: the staged-app copy path already exists in the kernel

- `UniXNU/patches/xnu/zzy-staged-system-apps.patch` (already applied to the kernel) copies the OS
  volume's `private/var/staged_system_apps` through `data_mount->mnt_vnodecovered` into Data at the
  first launchd exec and marks `/private/var/db/.sheng-staged-apps`. It is gated by
  `sheng-staged-apps=1`, which no previous image passed — wf166's log has no staged line.
- The 24 staged bundles that the phone `DefaultIconState.plist` names are exactly the missing icons;
  their main executables' CDHashes are already in the original static trust cache (checked
  MobileMail and Maps against `UniXNU/build/original-static-system-trust/original-static-system.bundle`),
  so they can run from Data without a new trust entry.
- wf169 = wf167 args + `sheng-staged-apps=1` and no `sheng-display-grid` (993/1000 bytes). The periodic
  line now ends `apps=<stage>/<dirs>/<files>/<MiB>/<err>`; stage 2 or 3 with files ~4214 and MiB
  ~467 means the copy completed (it is one-time; later boots report stage 3).
- Five bundle IDs in the phone default layout (`mobilephone`, `compass`, `calculator`, `Passbook`,
  `Bridge`) do not exist anywhere in this iPadOS root; those slots cannot be filled by this image.
  The 168 number in the goal counts service bundles in `/Applications`, not home-screen icons; the
  phone default layout has 30 entries (25 obtainable here).

## 2026-11-03 GPU recovery implementation (kernel built, not yet booted)

Commit `afa40a4` implements the restart hook and the reliable hang detector; kernel
`495f9b28f9dabf04` (saved as `build/kernel-gpu-495f9b28.macho`). The layout images below were
built before it and keep kernel `aef2bfb67ffb3f46` (`build/kernel-wf167-layout-aef2bfb6.macho`).

- `NativeAdrenoSubmit::hangCheck`: on a device clock only the completed fence counts as progress;
  a fence that does not move for 1 s declares a hang. The idle case clears the timestamp, so a new
  job after an idle period gets its full second.
- `nativeAdrenoHangcheck`: `IOLockTryLock` with a starvation counter, `native_adreno_hangcheck[8]`
  (polls, fence, hang fence, retries, CP-state hash, trylock misses, same-fence polls, last poll),
  and a `PICASSO_GPU hangcheck:` line once per stuck second.
- `native_adreno620_cp_restart` (installed from `publishGpu`): drain/halt SQE, cold GMU restart
  (`native_adreno620_gmu_restart`, firmware getter added) when GX is off, hw_init/SQE/program,
  `ring->restart(completed)` at the contract point, CP init, secure-mode packet (non-fatal),
  scratch-page CACHE_FLUSH_TS probe, then `NativeAdrenoRing::resync` (new) to start client writes at
  the CP read pointer without wiping the fence page.
- `recover` replay uses the unframed `submit()` on A6xx (`submitFramed` answers Unsupported there),
  after the restart set `seqno_=hung`, so `next` still matches each replayed fence.
- Forced-hang test (CP_PROTECT type-4 write or CP_WAIT_REG_MEM) is not added yet; the natural freeze
  will exercise the hook first, then `sheng-gpu-hang-test` can make it deterministic.

### Layout image set (all 88 MiB payload, B kernel)

| tag | changes | args |
|---|---|---|
| wf167 | identity/mode only, soft addon, grid | 995 |
| wf168 | wf167 + wallpaper addon `f2c573f7…` | 995 |
| wf169 | wf167 + `sheng-staged-apps=1`, no grid | 993 |
| wf170 | wf168 + `sheng-staged-apps=1`, no grid (combined candidate) | 993 |

Boot order: wf167 (geometry/status bar), wf168 (wallpaper), wf169 (apps; first boot copies ~467 MB
into Data and can take minutes), then wf170 as the combined stable layout image.

## 2026-11-03 upstream ios16 branches: classification and the status-bar path

Fetched `origin/feat/ios16` and `origin/feat/ios16-gpu` (both pinned to the same upstream
`75fba878…` as this tree). Their userland is iPhone10,3 / 16.7.16 / 20H392 with the 21H461
kext backend; ours is 21H461 / 17.7.11. The status bar finding is the useful part:
UIKit's `_UIStatusBarVisualProvider_Pad_ForcedNoCell` (present in our dyld cache) selected by
`com.apple.UIKit.UIStatusBarVisualProviderClassName` avoids the red `_UIStatusBarVisualProvider_Fallback`.

### Directly adoptable
- `package/picasso-springboard/picasso-sb-probe.m` (metric hooks for the Pad provider).
- `scripts/build-picasso-sb-inject.py` (iPhoneOS SDK + ad-hoc signing).
- The nested `zzzzzz-picasso-springboard-inject` XNU patch (kern_exec env + AMFI call-90 path
  vars). It applies cleanly to our current patch chain; carried here as
  `patches/picasso-springboard-inject-xnu.patch` and wired through `unixnu`.
- `com.apple.UIKit` provider pref and the seed-skip-setup engine (UniXNU
  `scripts/seed-data-skip-setup.py`, already at the pinned revision).
- In `feat/ios16-gpu`, nearly every file is byte-identical to HEAD (NativeWlan sources,
  NativeCpuFreq, ArmSmmu4K, boot-progress, firmware patches, UEFI/DTB). Only the WLAN
  build/entry wiring and the HID counter atomicity are genuinely new.

### Needs adaptation / conflicts
- The upstream cmdline carries `picasso_inject_sb=<path>`, `picasso_inject_apps=1`,
  `amfi_allow_any_signature=1` (108 bytes). Our image is at 995/1000. The carrier here accepts
  `picasso_inject_sb=1` as a fixed `/private/var/picasso/libpicasso-sb.dylib` default, and the
  build script trims `-v` and `picasso-aks-state=1` when `SB_INJECT=1`.
- Do not take the branch GPU/display files: they predate our GMU 500 MHz, CP restart/hangcheck,
  ring resync and 360x780 letterbox work. `package/PicassoPr13.cpp` duplicates our display symbols.
- Do not take the branch ArmCpu IPI base (952); our hardware-verified INTIDs are 1008..1015.
- Upstream has no wallpaper work; its boot-logo constant 29457 does not match our 41011.
- The branch `picasso_target.py` is 20H392/iPhone X; our seed wrapper hardcodes 21H461 / 17.7.11.

### Current simple fix (no injection needed)
1. The Data seed transform writes `com.apple.UIKit` (`_UIStatusBarVisualProvider_Pad_ForcedNoCell`,
   `StatusBarHidesDate=true`) and 21H461 Setup/migration prefs. Verified offline.
2. `NativeLegacyDisplay.cpp` now places the whole 60-row mode gap at the top (60 top / 0 bottom)
   instead of 30/30, moving the status bar 60 px down from the physical top to clear the camera.
3. `SEED_PREFS=1 GRID=0 IPHONE_IDENTITY=1 … build-wf-payload.sh wf171 NG PADDON sheng-display-scale=3`
   builds the image without `picasso_inject_sb`/`amfi_allow_any_signature`.
The injection chain remains in the tree as a fallback, dormant unless `SB_INJECT=1`.

### Boots on 2026-11-03
- wf167 (identity/mode, 30/30 bars, grid): reached the home screen, SpringBoard pid 27 healthy,
  no NSException/SIGABRT in the CDC log. Status bar is still the red Fallback bar — expected,
  because only the provider pref removes it and wf167 had no seed transform.

### Data seed identity vs the persistent store (2026-11-03)
Changing a preference in the Data seed changes its content hash, which is the persistent store's identity
(`PersistOverlay::checkHeader`): the next boot starts the store over (Dropped pages), discarding the
LaunchServices database and every runtime Data write. `wf171` (provider pref in the seed) did exactly this
and then spent >17 minutes rebuilding with all cores pegged at 100% and only one GPU submission; it never
reached SpringBoard's first frame. Booting the old seed afterwards (`wf173b`) also started over, because
the store header now belonged to the new seed; that rebuild completed in ~6 minutes and reached the home
screen (red Fallback bar, as expected without a provider pref; the 60-top/0-bottom letterbox works).

The correction is to leave the seed untouched and write the provider preference from the kernel:
`patches/picasso-ui-prefs-xnu.patch` writes a 155-byte binary plist to
`/private/var/mobile/Library/Preferences/com.apple.UIKit.plist` (and `/private/var/preferences/...`) from
`native_owner_data_prepare`, after the Data mount and before launchd, owner 501:501, mode 0644. It logs
`PICASSO_UI_PREFS provider=Pad_ForcedNoCell mobile=<err> system=<err>`. `wf176` boots the original seed
with this kernel (args 973) and reuses the store `wf173b` rebuilt. `wf177` adds `sheng-staged-apps=1`
(args 993) and the wallpaper addon.

## 2026-11-04 status-bar injection, AMFI trust, and the backboardd SIGBUS

- AMFI rejects the ad-hoc inject dylib even with `amfi_allow_any_signature=1`: `AMFI: '...libpicasso-sb.dylib':
  unsuitable CT policy 0 for this platform/device, rejecting signature.` The working path is the boot
  payload's owner trust cache: extend the owner module with the dylib's 20-byte CDHash (same module format
  as UniXNU `make-owner-trust-bundle.py`) and pass the bundle with `--owner-trust-cache`. `wf179+` boot with
  `build/owner-trust-sb.bundle`; wf178 (original bundle) never loaded the library.
- A global `-[UIScreen _isEmbeddedScreen]` YES breaks FBSDisplayMonitor (no main CADisplay). Answer YES only
  for the status-bar lookup caller: 21H461 UIKitCore calls it from `__UIStatusBarGetVisualProviderClassForScreen`
  +0x4c and reads the pref at +0x50; return site offset 0x1263b0 from UIKitCore's `__TEXT` base
  (0x189fe6000), so accept the caller range 0x126360..0x1266c4.
- The repeated SpringBoard `failed to find the main CADisplay` traps are downstream: **backboardd exits with
  SIGBUS** (`exit_signal=10 namespace=2 code=a record=not-breakpoint-exit`) about two minutes after UI start
  whenever the wallpaper overlay mount succeeded (`PICASSO_GPU addon ... wallpaper=5/0`). `wf173b`, whose
  wallpaper mount failed (`wallpaper=1/2`), has no such exit. runningboardd then kills SpringBoard with
  "backboardd respawn". The stale `/private/var/ngpu/wallpaper` survives addon rebuilds on the persistent
  Data store, so removing the addon files alone does not disable the mount.
- Kernel gate `sheng-ui-extras` (or its own arg per build): bit0 `0x1` = `/Applications` union mount,
  `0x2` = the two extra 7570 wallpaper variants, `0x4` = the base 7570 Flat overlay. Default 7 when absent.
  The base mount's source string must stay in `native_gpu_addon_prepare` scope for the other two mounts.
  News.app is renamed to `News.app.hidden` before the apps mount; it is not the crash trigger, keep for later.
- The board's only CDC control channel is `bsd/dev/picasso_usb.c` (help/status/ps/.../hang-test/reboot-fastboot).
  UniXNU's `debug_terminal.py` screen.snapshot/input.touch protocol is not compiled into this kernel, so
  there is no framebuffer screenshot or per-point touch injection yet; virtual swipes come from
  `offline-hid-swipe` in `package/HidInputProbe.cpp` (build-wf-payload trims it today).
- User decision for the night run: skip wallpaper, make status bar + 29 staged apps stable, then GPU
  stability is the priority.


## 2026-11-04 06:40 correction: the SpringBoard trap was a display-policy failure, not backboardd

All four recorded SpringBoard SIGTRAPs un-slide to **0x19f3df2ec**, a fixed `brk #0` inside
`-[FBSDisplayMonitor _initWithBookendObserver:transformer:]` (FrontBoardServices), reached from
`+[FBDisplayManager sharedInstance]`'s once block: `[[CADisplay displays] firstObject]` was nil, the
"failed to find the main CADisplay" branch (0x19f3df29c..0x19f3df2ec) traps. The macOS-side cache
extraction used for this is `/tmp/dsc-sys` (SystemCryptex `DawnUpdate21H461.J120J121J207J208SystemCryptex`).

`[CADisplay displays]` comes from `query_displays` -> `_CARenderServerGetServerPort(0)` +
`_CASGetDisplays()`, i.e. the render server's display list. The chain that emptied it since wf178:

1. wf178 dropped the `sheng-display-scale=3` boot argument to make room for the injection pair.
2. `native_legacy_gop_mapping_observed` compares the boot argument (default 2) with `/chosen`'s
   `display-scale` (the phone identity writes 3) and returns false on mismatch: `PICASSO_DISPLAY mapped ok=0 ... scale=2`.
3. `startHardware()` returns that false, so the stock start prints `start_hardware failed`, the
   IOMobileFramebuffer service never starts, and **no user client is ever opened** (`PICASSO_DISPLAY event=3/4`
   absent; wf191.log has none).
4. With no IOMFB display, backboardd's render server has no display; CADisplay is empty; any process that
   first uses `FBDisplayManager` (SpringBoard ~30-50 s after start) traps. runningboardd gives up after
   four corpses. The wallpaper/apps/injection flags are unrelated.

Evidence: wf177 (old kernel, `sheng-display-scale=3`, ok=1) has `event=3/4` client opens (backboardd
matches `IOMobileFramebuffer`, creates `NativeLegacyDisplay`) and no trap in a 5-minute run; wf178..wf191
(ok=0) have `start_hardware failed`, no clients, and exactly four SpringBoard traps.

**Fix** (`drivers/NativeMobileDisplay/PicassoDisplaySupport.cpp`): when `sheng-display-scale` is absent,
read the identity's own `/chosen` `display-scale` and use it as the expectation instead of the 2 default,
so the driver and the layout UIKit reads cannot disagree. No command-line byte is spent.

Test results with kernel sha256 `258a0182fbce7bd1daf34642f745d56bb90f86bd3288ac393d63535095b0c548`:

- `wf193` (inject, apps mount, grid, `sheng-fb-gpu=1`): `mapped ok=1 scale=3`, no trap, SpringBoard stayed
  alive; backboardd matched IOMobileFramebuffer and opened clients, but **never called a framebuffer method
  and never kicked a swap** (`PICASSO_FB` 0, `vsync: kick` 0, submit 0) in 5.5 minutes. liveactivitiesd
  crash-looped.
- `wf194` (same minus `sheng-fb-gpu`): ran 4+ hours; no trap, no SpringBoard/other service exits, **zero
  IOMobileFramebuffer clients** — backboardd never performed its window-server IOKit pass (`PICASSO_IOKIT` has
  only AKS calls). submit stayed 0, screen stayed on the kernel boot-progress page.
- Old `wf177` baseline still presents frames, so the regression is in the new userland/kernel features, not
  in the display-policy fix itself. Candidates to isolate: injected dylib into SpringBoard/PosterBoard
  (`wf196`, no injection), the `/Applications` union mount (`wf195`, `sheng-ui-extras=0`), the kernel-written
  UIKit prefs. Both isolation images are built (`out/wf195`, `out/wf196`).

**Phone availability**: after the 4-hour `wf194` run, `reboot-fastboot` was sent over the CDC console and the
phone did reset (`PICASSO CDC: reboot-fastboot requested; ... reset begin mode=0`, IMEM fastboot reason
0x77665500 read back), but it never enumerated as a fastboot device or a CDC console again (no
`fastboot devices` entry, no `/dev/cu.usbmodemPICASSO011`, MAC USB tree has only the hub/SSD/Ethernet). It
took about nine minutes and then re-appeared in fastboot by itself at 06:46 (`fastboot devices` ->
52cc791d). So the reset did reach the bootloader, but USB enumeration recovered late (the Mac had slept in
between); do not assume the phone is lost when the console disappears right after `reboot-fastboot` — wait
and poll. Keep a physical power-button recovery step documented anyway.

### Next actions when the phone is reachable again

1. First boot `out/wf195` (same new kernel, `sheng-ui-extras=0`, injection on, no `sheng-fb-gpu`):
   if SpringBoard/backboardd reach the display, the `/Applications` union mount is what stalls the UI.
2. If wf195 stalls, boot `out/wf196` (`TRIM_ARGS=1`, no injection, apps mount on): if that presents, the
   injected dylib in SpringBoard/PosterBoard is the suspect; then harden it (no `dladdr` per call; resolve
   the UIKitCore base once) or narrow its target list.
3. Watch the first three minutes for `PICASSO_DISPLAY event=3/4` (backboardd matched the IOMFB) and
   `PICASSO_FB`/`vsync: kick` (the render server started the display and swapped).
4. The face of a successful boot in this port: `PICASSO_DISPLAY mapped ok=1`, `event=3/4` client opens,
   `PICASSO_FB sel=4/5` from backboardd, `PICASSO_DISPLAY vsync: kick`, `DISPLAY_STATE submit>0`.

The older problem remains after the UI is back: backboardd exits SIGBUS ~10-11 minutes into a working UI
(wf176/wf177), which is the GPU/compositor stability item the user wants fixed next. That crash has no
PC in the kernel record; capturing the fault address in the signal/exception path is the first step there.

## 2026-11-04 07:30 the second blocker: `picasso-aks-state` is a boot dependency, not a diagnostic

With the display-policy fix in place, the render server still never presented. Comparing the old working
baseline (`wf177`) with every new image found the split:

- wf177: `PICASSO_AKS started=1 ephemeral=1 state_only=1`, `PICASSO_AKS ready=1 at display probe`; keybagd,
  containermanager and securityd AppleKeyStore selectors return 0; SpringBoard walks its service set up to
  powerd/mediaremoted and the render server calls `PICASSO_FB sel=4/5` and kicks swaps.
- wf191/wf193/wf194/wf197/wf198: `PICASSO_AKS ready=0 at display probe`, no `started=` line; securityd's
  AppleKeyStore lookups return `e00002f0` (service not found); SpringBoard's link set grows to
  callservicesd and then stops, the little CPU sits at 300 MHz/6% busy, and no swap is ever submitted.
  wf194 showed the same picture for four hours; wf197 for 28 minutes.

`picasso_keystore_probe()` (drivers/AppleKeyStoreState/PicassoKeyStore.cpp) publishes the state-only
`AppleKeyStore` service **only when `picasso-aks-state=1` is on the command line**. build-wf-payload.sh
trims that argument in the SB_INJECT/TRIM_ARGS paths as a "state-only diagnostic" — that comment is wrong
for this port: without the service, keychain/keybag clients fail and SpringBoard's boot crawls through
timeouts, so the first frame never arrives. wf176/wf177 had the argument, which is why they reached the UI.

Fix under test: put `picasso-aks-state=1` back (as an explicit extra argument, 21 bytes). Images:
`wf199` = wf198 (no grid, extras=0) + aks-state (single-variable test); `wf200` = wf191-style
(inject, extras=1, no fb-gpu) + aks-state + grid, dropping `sheng-staged-apps=1` (the Data copy and its
marker already exist) to stay under 1000 bytes.

**Confirmed on wf199** (07:32 boot, same kernel, no grid, extras=0): `PICASSO_AKS started=1 ephemeral=1`,
`ready=1`; backboardd opened the display (`event=3/4` x4), called `PICASSO_FB sel=4/5`, armed the vsync
timer (`kicked=1`), and presented real frames: `PICASSO_DISPLAY frame kind=0 nonblack=203029`,
`event=6/7/8/9/10/11` all present, 12 FB calls / 8 kicks, zero service exits or crashes in the capture.
So the two regressions since wf178 were (1) the missing `sheng-display-scale` (no main CADisplay) and
(2) the trimmed `picasso-aks-state=1` (no AppleKeyStore service -> keychain clients time out -> SpringBoard's
boot crawls and no swap is ever submitted). Neither is related to the wallpaper, the inject dylib or the
apps union mount.

`wf200` = inject + apps mount (`sheng-ui-extras=1`) + grid + aks, no fb-gpu, with `sheng-staged-apps=1`
dropped (the Data copy already exists) to fit the command line; it is the candidate for the morning
status-bar (real bar instead of red) and 29-app check.

### Diagnostic kernel for the compositor bus error (`patches/picasso-fault-dump-xnu.patch`)

The historical backboardd SIGBUS (`exit_signal=10`, `namespace=2`, `code=a`) had no PC because the kernel only
snapshots user state for BRK. The new patch captures the same bounded snapshot for unhandled user data aborts
(`handle_user_abort` carries the fault address), extends the exit matcher to SIGBUS/SIGSEGV, and prints
`FAR=` (the fault address) with `record=data-abort-snapshot`. It is wired into the patch chain by `unixnu`
(last, z-count 31) and builds as kernel sha256 `b19623e7ed45d97...` (log `out/build-kernel-wf201.log`).
`out/wf201` carries that kernel with the same command line as wf200; run it when the SIGBUS reproduces and
symbolize the reported PC/ESR/FAR.

## 2026-11-04 08:00 the red bar: PAC-signed return address in the arm64 inject library

`picasso-sb-probe.m` is built arm64 (`scripts/build-picasso-sb-inject.py`) and inserted into arm64e
SpringBoard/UIKitCore. UIKit calls `-[UIScreen _isEmbeddedScreen]` through `objc_msgSend` with `bl`, so the
LR our hook reads from `__builtin_return_address(0)` carries a pointer-authentication signature in the high
bits. An arm64 build copies x30 as a plain integer, so `dladdr(caller)` and the `0x126360..0x1266c4` offset
comparison both saw a garbage address: wf181–wf184 loaded the library and still got the red Fallback bar.
wf179's global YES only "worked" because it never examined the caller (the FBS trap in that run is explained
by the concurrently broken display policy, not by the hook).

Fix: mask to the 47-bit user VA (`& 0x00007fffffffffff`), then resolve UIKitCore with
`class_getImageName(objcGetClass("UIScreen"))` + `_dyld_get_image_header` instead of trusting `dladdr`'s
shared-cache view; keep dladdr as a fallback. Rebuilt dylib: 69,056 bytes, CDHash
`65f10a18c007b2f65a92d34d3ebd23dc8a7fe5a5`, embedded into `build/ngpu-addon-sb-wf203.bin` (mode 0755).

A rebuilt dylib changes the CDHash, so the pre-boot trust bundle must be regenerated; the old
`build/owner-trust-sb.bundle` still listed the previous hash `f8908bd5…`. New reproducible tool:
`scripts/build-sb-trust-bundle.py OWNER.bundle LIB.dylib OUTPUT.bundle` (verified byte-identical to the
earlier hand-built bundle for the same input). wf203 = wf200's command line + `amfi_allow_any_signature=1`
(`SB_INJECT=1` default), `OWNER_TRUST=build/owner-trust-sb.bundle`, kernel `d8218d65…` (the RGB grid-probe
kernel, which also carries the fault-dump patch), args 998/1000.

Grid evidence from wf200 before the switch: row 00 all-zero, row 01 a uniform `555555555555` band (a solid
colour, not glyphs on black) — consistent with the red Fallback bar; `PICASSO_GRID_RGB` (wf202+ kernel)
prints the exact BGRX for rows 0–6 to settle it.

wf200 stability: UI up at ~07:33, 15 frames, SpringBoard (pid 26) alive at 07:52, **no backboardd SIGBUS
in 19 minutes** with the wallpaper overlay absent — supports the wallpaper-mount → SIGBUS correlation in
the 2026-11-04 section above.

### wf203 / wf204: the red bar survives the PAC fix and even an unconditional YES

- wf203 (PAC-masked caller check, `amfi_allow_any_signature=1`, new trust bundle, args 977): UI up 80 s
  after boot, `mapped ok=1 scale=3`, submit frames, no trap — but row 01 reads `PICASSO_GRID_RGB 01
  ffbf342b` in all 12 columns, the same solid red Fallback band, and `PICASSO_SBINJECT library ran` still
  never appears. `PICASSO_INJECT dyld policy pid=27 old=69 new=6b result=0` confirms the env/MAC path runs.
- wf204 = wf203 with `picasso_is_embedded` returning YES unconditionally (no caller inspection at all):
  **identical red band**, no trap, no AMFI rejection. So either the library is not actually loaded in the
  process drawing the lock-screen bar, or that bar is not selected by this UIKit function at all.
- New fact: every probe so far sampled the **lock screen** (rows 03–06 = the big clock). We had never
  unlocked the phone. wf205 therefore re-enables the kernel's own virtual touch (`offline-hid-swipe=16`,
  stripped by build-wf-payload.sh in every other image; `package/HidInputProbe.cpp:204` sends a bottom-edge
  swipe), keeps `sheng-fb-gpu=1` (GPU work) and the grid, and drops `amfi_allow_any_signature` to fit
  1000 bytes (986). If the home-screen bar is black the red we have been chasing is lock-screen-only.

### wf205: GPU scan-out present, virtual touch delivers, bar still red

- wf205 = wf204 arguments + `sheng-fb-gpu=1` restored + `offline-hid-swipe=16`, minus
  `amfi_allow_any_signature` (986/1000 bytes).
- Unlike wf193, the GPU hand-off does not stall: `PICASSO_DISPLAY frame kind=0 nonblack=209515` at t+164 s,
  **193 presents in 11 minutes**, `transfer_fail=1/194`, no service exit, no SIGBUS, no SpringBoard trap.
  The wallpaper overlay was mounted on this boot (`PICASSO_GPU addon ... wallpaper=5/0`, stale Data files),
  so the historical wallpaper→backboardd-SIGBUS combination did **not** reproduce with this kernel/addon.
- Virtual touch works: `PICASSO_HID swipe 2 moved=1 reports=26 edge=1/1`, 16/16 swipes sent from t≈68 s
  every 20 s. The screen nevertheless stays on the lock screen (white wallpaper, black clock, red bar
  sampled again at 12 columns `ffbf342b`); no keypad ever draws, so either the swipe does not reach the
  lock-screen recognizer or no passcode-protected unlock happens without a passcode.
- GPU: `PICASSO_GPU cp: metal published`, but `metal diag opens=0 foreign=0` and `client: calls=0` in the
  early samples — backboardd never opens the accelerator user client, so the Metal plugin is not driving
  anything. `NativeAdreno: refusing to publish without a started CP` also appears before publication.
- Lock screen aside, every process that could draw the bar has been probed: scoped hook (wf203, wf205) and
  unconditional YES (wf204) both leave it red. That points at the library not being mapped at all rather
  than at the caller test, so the next kernel logs the actual `open(2)` of the injected path:
  `patches/picasso-inject-open-probe-xnu.patch` (wired into `unixnu`, z-count 33, applies last) prints
  `PICASSO_INJECT open=<n> pid=<pid> name=<proc> path=...` for any process. Kernel sha256
  `bf9b113830caf99f78438010f09599b025a46ba73d6a54c24a0e54ec8f013a18`, image `out/wf206` (same command
  line as wf205).
- Note: a stale `out/picasso/tree/build` made `unixnu build kernel prelink` fail at the object scan
  (`genassym.o ... not recognized as a valid object file`); `rm -rf out/picasso/tree/build/xnu` fixed it.

### What UIKit's "embedded" actually means (21H461, disassembled)

`-[UIScreen _isEmbeddedScreen]` (0x18a00b1a8) evaluates, when the internal override preference is
default, `byte(self+0xcd) && !-[UIScreen _isExternal]`:
- `byte(self+0xcd)` is written in `-[UIScreen initWithDisplayConfiguration:forceMainScreen:]`
  (0x18a2bcbe4) as `forceMainScreen || [displayConfiguration isMainDisplay]`.
- `-[UIScreen _isExternal]` (0x18a00b21c) reads the FBSDisplay at `self+0xa0` and returns
  `[display isExternal]`, or YES when that object is nil.
So the red Fallback bar means the generated display is either not main or reported external by
FrontBoard. The internal preference path (0x1f48cd270) can only force NO, never YES.
`+[UIScreen _embeddedScreen]` (0x18aa7a5a4) returns the first screen that answers YES, confirming that
no screen in this system is embedded. The durable fix is therefore either (a) make the injected
`_isEmbeddedScreen` hook work in the drawing process, or (b) make FrontBoard's FBSDisplay report
main/built-in so UIKit answers YES by itself.

### wf206/wf208: the injected library IS mapped and AMFI accepts it

- `patches/picasso-inject-open-probe-xnu.patch` (kernel `bf9b1138…`, image wf206) prints the namei path
  copy for any `OP_OPEN` of `libpicasso-sb.dylib`. Result:
  `PICASSO_INJECT open=6 pid=27 name=SpringBoard path=/private/var/mobile/Library/picasso/libpicasso-sb.dylib`
  (three times), then `open=9 pid=175 name=PosterBoard ...`. dyld honours the insert in both UI processes.
- `patches/picasso-inject-signature-probe-xnu.patch` (kernel `a688888c…`, image wf208) reports AMFI's
  verdict from `mac_vnode_check_signature`: `PICASSO_INJECT signature error=0 flags=24000003 signer=0
  platform=2 path=...libpicasso-sb.dylib`. The ad-hoc library is therefore accepted, mapped, and its
  constructor runs — the trust bundle works without `amfi_allow_any_signature`.
- Yet the bar is still `ffbf342b` in wf206 and wf208, and it was unchanged with the unconditional-YES
  probe (wf204). That leaves the constructor's own installation as the suspect: either `objc_getClass`/
  `class_getInstanceMethod` fails at constructor time in these processes, or `method_setImplementation`
  does not affect the shared-cache dispatch.
- wf209 (kernel `cf351409…`, image `out/wf209`, addon `build/ngpu-addon-sb-wf209.bin`, dylib CDHash
  `714fac9d…`) answers exactly that: the constructor opens `/private/var/ngpu/picasso-marker-constructor`,
  then `-uikit-ok|uikit-missing`, `-swizzle-ok|swizzle-fail`, `-object-ok|object-fail`; the widened open
  probe logs the path copy even when the sandbox denies the open. Not booted: the phone's battery died
  at ~09:07, right after wf208.

### wf210/wf211: self-healing install and an apps-visibility probe

- Because the library is mapped but the bar never changes, the dylib now retries every install from the
  constructor and again at +1 s/+5 s/+15 s on the main queue (each step idempotent and re-scanning the
  method table), so an initializer that runs before UIKit realises its classes cannot lose. Every attempt
  opens `/private/var/ngpu/picasso-marker-<tag>{uikit-,swizzle-,object-,metrics-,wallpaper-}<ok|fail|missing>`
  plus `picasso-marker-constructor`; the kernel path-copy probe prints them (sandbox-independent).
  Dylib CDHash `ddf99ef1…`, bundle `build/owner-trust-sb-late.bundle`, addon `build/ngpu-addon-sb-wf210.bin`.
- New kernel patch `patches/picasso-apps-view-probe-xnu.patch` (z-count 35) looks up twelve bundles through
  the mounted /Applications (Camera, Mail, Safari, Weather, Notes, Calendar, Reminders, Maps, Photos,
  Contacts, Timer, Videos) and prints `PICASSO_APPS_VIEW found=<bits>`, separating a mount problem from a
  LaunchServices/SpringBoard one. Kernel `8121bde24d68134d…`, image `out/wf211` (same command line).
- The status bar still shows `ffbf342b` through wf208; the phone's battery died at ~09:07 and it has not
  enumerated over USB since, so wf210/wf211 have not been booted.

### The red strip is exactly the Fallback provider's own background

`-[_UIStatusBarVisualProvider_Fallback setupInContainerView:]` (UIKitCore 0x18ae97100) calls
`+[UIColor systemRedColor]`, then `colorWithAlphaComponent:0.75`, then `setBackgroundColor:` on the status
bar's content view. systemRedColor is (255, 59, 48); at 75% over black that is (191, 44, 36) — the sampled
band is `ffbf342b`, i.e. (191, 52, 43) with the compositor's rounding. So the whole red-bar symptom is the
provider selection, not a theme, a wallpaper or a kernel bar.

### wf213: one boot that answers every open question

`out/wf213` (kernel `8121bde24d68134d…`, published GPU addon `--publish --clients '*'`, dylib CDHash
`3a34d376…`, trust `build/owner-trust-sb-diag.bundle`) carries:
- the retrying, idempotent install (`constructor`, then `late1-/late2-/late3-` at +1/+5/+15 s) with
  `picasso-marker-<tag>{uikit,swizzle,object,metrics,wallpaper}-<ok|fail|missing>` opened for each attempt;
- a one-shot diagnostic inside the `_isEmbeddedScreen` hook that opens
  `picasso-marker-embedded-orig<0|1>-main<0|1>` — the unmodified method's answer and UIKit's own
  main-display byte at UIScreen+0xcd — before returning the scoped YES;
- `PICASSO_APPS_VIEW found=<bits>` for twelve bundles through the mounted /Applications;
- the documented-working GPU configuration (published addon, clients `*`, plugin CDHash `5184dad4…`).
If the marker shows `main0`, the screen is not main; if `main1` and the bar is still red, UIKit's
`_isExternal` is true and FrontBoard's display identity must be fixed instead.

### wf214: text-mode screen capture over the existing CDC console

The giulia GSC1 screenshot protocol is not compiled into this kernel, and adding a second binary stream to
the only control channel was judged too risky for tonight. Instead the console gains a read-only command:

- `PicassoDisplaySupport.cpp` exports `picasso_snapshot_virtual/{width,height,stride}` from the mapping its
  display-policy check already creates (so it exists in every image, with or without the grid argument).
- `patches/picasso-cdc-snapshot-xnu.patch` adds `ud_snapshot()`: 24 rows x 48 luma digits (`PICASSO_SNAPSHOT_ROW`)
  plus the exact BGRX of six columns for the top three rows (`PICASSO_SNAPSHOT_RGB`), through the existing
  printf/CDC path. It writes nothing and touches no endpoint.
- Send it with `python3 scripts/picasso-cdc-command.py --command snapshot --output out/sb-runs/snap.log`.
  `help` lists it between `ps` and `wait`.
Kernel `316bdac38efeebc2…`, image `out/wf214` (diag dylib `3a34d376…`, published GPU addon). This is the
screen check to use on the next boot: the status bar row shows either the red Fallback background or black
with white glyphs, and the icon grid shows the app count.

### Resume procedure after the battery is charged (exact steps)

1. `fastboot boot out/wf214/picasso/boot/wf214.img`
2. capture: `until [ -e /dev/cu.usbmodemPICASSO011 ]; do sleep 1; done; cat /dev/cu.usbmodemPICASSO011 > out/sb-runs/wf214.log`
3. once SpringBoard is up, in a second shell:
   `python3 scripts/picasso-cdc-command.py --command snapshot --output out/sb-runs/wf214-snapshot.log`
4. read, in this order:
   - `PICASSO_SNAPSHOT_RGB 01 …`: `ffbf342b` = still Fallback; black + white glyphs = fixed.
   - `PICASSO_SNAPSHOT_ROW`: 48x24 luma, icon grid and page content.
   - `picasso-marker-*` lines from the dylib: `constructor`, `late1-`, `swizzle-ok|fail`, `uikit-ok|missing`,
     `embedded-orig?-main?`.
   - `PICASSO_APPS_VIEW found=<bits>` (12 bundles; bit order: Camera, Mail, Safari, Weather, Notes, Calendar,
     Reminders, Maps, Photos, Contacts, Timer, Videos).
   - `PICASSO_GPU metal diag / submits live / client:` and any `EL0`/`data-abort` lines.
5. If `swizzle-ok` appears and the bar is still red, the next change is `_isEmbeddedScreen` returning YES for
   every caller in the UI processes (the wf204 probe) now that the late install is in place, then check for the
   `main0` case from the marker.

### wf214 (published addon): the GPU works, but the render server's surface format is rejected

Boot at 10:31:52 with the documented published addon (`--publish --clients '*'`, plugin CDHash `5184dad4…`),
`PICASSO_APPS_VIEW found=6ef` (nine of twelve probe names; the misses are name guesses, so the union mount
is fine) and `PICASSO_INJECT signature error=0` again.

New, first-of-its-kind GPU evidence:
- `PICASSO_GPU submits live 9: submissions=304 ibs=2790 max=50 many=57 opens=95` — backboardd *does* load
  the Metal plugin and submit through it under the published addon. The 304-instance / 95-open numbers are
  two orders of magnitude above every earlier run (wf205: `opens=0`, `submissions=1`).
- `PICASSO_GPU hangcheck: fence=305 hang_fence=305` — every fence is a hang fence; the GPU never retires.
- The display rejects **every** swap: `submit=3013 accept=0 reject=3013 reasons=0/0/0/0/3013/0/0/0`
  (reason 4), with
  `PICASSO_DISPLAY surface rejected width=1080 height=2340 format=26424741 planes=1 stride=1088
  allocation=10371072 length=10371072`.
  `0x26424741` is `'&BGA'`, the compressed storage format the CPU path deliberately refuses
  (`PicassoGop.h`: "CPU presentation supports linear BGRA only"). With the Metal plugin active the render
  server allocates GPU-preferred compressed scanout surfaces, so the CPU validation path can never accept
  them, the swaps never complete, backboardd respawns and runningboardd then kills SpringBoard — no desktop.
- Next GPU change: in GPU mode (`sheng-fb-gpu=1` with a published plugin) accept `'&BGA'` (and the other
  GPU-samplable formats) in the swap validation and let `native_legacy_gpu_offer` hand it to the plugin,
  instead of rejecting it on the CPU format check; the reject must at least not happen before the offer.
- Immediate recovery image for UI work: wf215 = kernel `316bdac3…` + the unpublished addon + the
  marker/diagnostic dylib, so the desktop and the status-bar markers are available again.

### Retracted hypothesis: injected library and __init_offsets

Correction on 2026-11-04: the actual dyld-1165.3 source handles `S_INIT_FUNC_OFFSETS` in
`MachOAnalyzer::forEachInitializer`, and `Loader::findAndRunAllInitializers` invokes them.
The former claim that iOS 17 skips that section in ordinary dylibs is false.
wf216 produced no trap; later `+load` and marker probes also produced no execution evidence.
Open + AMFI signature success still does not prove a completed mapping. Policy `0x6b` allows failed
library insertion, so obtain the loader error before changing initializer formats or blaming PAC.
The `+load` experiment remains in the source, with no successful execution observed.

### Stale publish marker in Data kept the GPU in production mode

wf214 installed a published addon; its `addons/NativeAdrenoMetal.publish` and `.clients` files persist in
Data. wf215/wf216 used unpublished addons but the plugin still ran the production path
(`submissions=774`, `hang_fence=775`), backboardd died and runningboardd killed SpringBoard.
`patches/picasso-addon-marker-sync-xnu.patch` (z-count 37) makes the addon the authority: when the current
addon does not carry either marker, the installer unlinks the stale Data file. Kernel `5f5a51bd9e6262ef…`,
image `out/wf217` (+load dylib CDHash `19192009…`).

### 2026-11-04 session summary

The corrected current state, the red-bar analysis, the unresolved injection failure, the GPU
`'&BGA'` surface rejection, the image evidence and the next-agent prompt are in
`docs/handoff-20261104-statusbar-gpu.md`. Read that first.
