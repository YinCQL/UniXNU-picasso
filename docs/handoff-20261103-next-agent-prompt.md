# 给下一个 agent 的提示词（可直接粘贴）

你在 `/Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog` 工作。目标：在 Picasso（SM7250 / Adreno 620 / iPad7,1、iOS 17.6、自编译 XNU）上完成 GPU 渲染，并让 UI 一经出现即可交互。先完整阅读 `docs/handoff-20261103-gpu-render-next.md` 和 `docs/handoff-20261102-gpu-first-submit-stall.md` 的 wf126–wf139 段，然后再动手。

## 当前结论（不要重新发明）

- **GPU 渲染已经打通**：wf139 镜像里，CARenderServer 通过我们的生产版 Metal 插件（metal2vulkan→Turnip→A620）渲染 UI。证据：`PICASSO_GPU text: ST0 c8/0 d8 p51 x7 R34 D149 F12`、`PICASSO_GPU submits live … submissions=14 ibs=81 max=26`、grid 中锁屏版式（rows 3–4 / 20–23）出现。
- **触摸已可用且及时**：真实 Novatek 驱动在 60 s 挂载（`offline-hid-start=60`），digitizer 早于 SpringBoard（~100 s）进入 IORegistry；`sheng-touch=4 sheng-touch-hid=1 sheng-touch-start=60`。
- 两个遗留问题：**A) 调度/拓扑异常（用户最新反馈：负载全压大核）**；**B) 锁屏首帧后渲染服务器空闲，触摸不触发重绘**（present 停在 8、last_submit 498.5 s）。

## 必须遵守

1. 只用 `fastboot boot`，禁止刷分区。
2. 串口 `/dev/cu.usbmodemPICASSO011`；采集前确认无进程占用（`lsof`）。
3. 每轮先探测设备状态（`ls /dev/cu.usbmodem*`、`fastboot devices`），不要沿用旧值。
4. build 前 export `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` 和 `UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed`。
5. 不要 reset/checkout 共享工作树；`drivers/NativeMetal` 是 UniXNU 的 submodule，提交要分开进对应仓库。
6. 一次只改一个变量；每个结论必须带证据（日志行/寄存器/测量）。
7. 里程碑 commit 并更新 PR #13（`YinCQL/UniXNU-picasso`，head `silenzio111:stage12/picasso-cpufreq-big`）。
8. 多看官方/社区实现（mainline msm/a6xx、Turnip、XNU IOKit），不要新造轮子。
9. 继续工作直到目标完成；不要问用户问题，自主继续。

## 不要踩的坑

- **不要删 `boot-progress=2`**：它启用 boot monitor 循环，而该循环是 `native_hid_input_probe()`（60 s 建 digitizer）和 `native_novatek_touch_probe()` 的唯一调用者。删掉会让触摸和虚拟滑动全部失效（wf134 因此作废）。
- **不要用测试版插件**：`NativeAdrenoMetalPlugin.mm` 的 `requested()` 必须检查 `NativeAdrenoMetal.publish` 文件；硬编码 `return false` 会让 CA 退回软件渲染。看到 `PICASSO_GPU text: T s0 r0 v1024…` 说明还在测试路径。
- **clients 名单只给 backboardd**：`clients='*'` 会让 mediaserverd 加载桥并 SIGABRT，随后整组服务被 SIGKILL（wf137）。
- **priming 保持 opt-in**：`PrimeShaders.h` 在无 `NativeAdrenoMetal.primelist` 时必须跳过；否则它会独占 translator，渲染服务器卡在 c5/d5（wf138）。
- cmdline 上限 1000 字节；已确认可删的无效参数：`picasso_usb_hold=0`、`picasso_usb_control_ms=15000`。
- 插件重建会删除 `UniXNU/build/adreno-metal-plugin`（含 owner-trust.bundle），必须随后 `make-owner-trust-bundle.py` 再打 addon。
- `/tmp/build-wf*.sh` 是易失的，先把 `/tmp/build-wf139.sh` 复制回仓库并参数化。

## 建议的第一步

1. 探测设备状态；wf139 可能仍在运行（串口已释放）。若要基线，先重启一次 wf139 并完整抓串（流程见 handoff §5）。
2. **调度优先（用户当前关注）**：`PICASSO_CPUFREQ topo cpus=8 clusters=2 types=6 en=1 pn=0 cl-e=1 cl-p=1`，DT 缺 `cluster-type` 与 `capacity-dmips-mhz`。读 `UniXNU/out/feeds/xnu-0b4b910d/osfmk/arm64/machine_routines.c` 的 `ml_setup_topology()` 与 `machine_routines.h` 的 `cluster_type_t` 常量，在 Mu-Silicium 的 `picasso.dtb` 源里补 cpu0–5 / cpu6–7 的 `cluster-type`（按 SM7250：6×A55 + 2×A76），重新生成 DTB、重打 wf140，验证 `en/pn/cl-e/cl-p` 与负载分布。
3. **触摸→重绘链路**：给 `native_touch_hid_reports` / `native_hid_input_observed(6,…)` 加周期性打印，确认触点时 `touchSend` 被调用；若 HID 已送达而 CA 不产帧，查 CA display link/vsync（wf132 的 kick）与首帧后的帧调度。
4. **GPU scanout（解决剩余卡顿）**：把 UniXNU `drivers/NativeMobileDisplay` 的 `NativeGOPPresenter.cpp`/`NativeGOPWire.h` 与 `native_legacy_gpu_offer/complete` 移植进本树（本树 `NativeAdrenoStubs.c` 里的 hand-off 是空实现），开 `sheng-fb-gpu=1`，验证 `P2 ee00002c7` → `P4`（serving）且 `PICASSO_DISPLAY event=10` 的 65 ms 全帧拷贝消失。
5. **启动速度**：用 `CacheExport.h` 的导出/m2vpack 导入让后续 boot 命中 pipeline 缓存。

完成后按约定提交里程碑并更新 PR #13；每次实验在 handoff 文档追加"镜像号 + 结论 + 证据行 + 下一步"。不要问用户，直接继续。
