# Prompt for the next agent (copy/paste)

你在 `/Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog` 工作。板子是 Xiaomi Picasso（SM7250 / Adreno 620 / NT36672C 触摸 / 1080x2400），跑自建 XNU + iOS 17.6（21H461）用户态，只能用 `fastboot boot` 启动，CDC 串口独占，`boot-progress=2` 不能删，内核 cmdline 上限 1000 字节。

你的唯一任务：**修好 GPU 卡死**。现在系统能启动、能进手机档界面、真实触摸可用、swap 正常，但 GPU 会周期性 stall 并最终永久挂起，整屏冻结且不恢复。

先读（必须）：
1. `docs/handoff-20261103-gpu-fix-next-agent.md` —— 完整现状、已验证事实、修复计划、构建/启动命令。
2. `docs/handoff-20261103-gpu-render-next.md` —— wf143..wf163 每次上机的日志与结论。
3. `drivers/NativeAdreno/NativeAdrenoSubmit.cpp` / `NativeAdrenoRing.cpp` / `NativeAdrenoVm.cpp` / `NativeAdrenoSubmit.h`，
   `out/picasso/tree/drivers/NativeAdrenoPicasso/Adreno620Cp.cpp` 和 `Adreno620Gmu.cpp`。

关键结论（不要重复验证）：
- 身份/布局已经解决并固定：wf166 用 iPhone10,3 / D22 / phone / 2340 / scale3，内核在 Data 挂载后、用户态前删除
  MobileGestalt 缓存（日志 `PICASSO_GESTALT removed ...`），界面已是 iPhone 风格（用户确认）。请以 wf166 的配置为
  基线（见交接文档第 5 节）。红色 `rdar:...` 状态栏是 internal iPadOS root 的内部构建特性，不属于 GPU 任务。
- 不是 BO 生命周期问题：FSR 恒为 0x400，按 kit 自己的 `FSR_FAULT=0xc00001fe` 没有任何 fault 位；0x400 只是 AArch64 地址格式位；日志里的 FAR 0x1338f508 不可信（0x64 不是 FAR-hi），且 client IOVA 从 0x100000000 起，旧 `handleAt(低32位)` 永远返回 0。
- 卡死与 Metal 插件无关：不发布插件、只走 `sheng-fb-gpu=1` 时一样卡（wf159 在 1491 帧后冻结）；关掉 handoff 后 adrenoprobed 的自测也能制造同样的 stall。
- 真正缺口：`native_adreno_set_restart` 没有任何调用者，板子没有 restart hook；`NativeAdrenoSubmit::recover` 已经写好，只等一个 `NativeAdrenoRestart` 回调。同时 `hangCheck` 在所有卡死里从未触发。
- stall 处理器每 50 ms 就触发一次（`cpFence=fence-1`、`rptr<wptr`），做 `invalidateTlb()+kick()`；`sheng-gpu-unstick` 位掩码已实现（bit0 TLB 失效、bit1 重踢 WPTR、bit2 总线 drain），wf155..wf161 用 `=2` 仍会永久挂起。

按文档第 4 节执行：
1) 把 stall 的 SMMU 诊断改成只在真正有 fault 位时打印、64 位 FAR、上限 ≤6 条；给 hangCheck 加导出的计数器并每秒打印一次，确认是线程被 `g.lock` 饿死还是 `madeProgress` 误判。
2) 实现并安装 Adreno620 的 restart hook：复用 `hardwareInit()/uploadSqe()/programCp()/cpInit()/leaveSecureMode()`（Adreno620Cp.cpp:502/637/662/694/719）和 `native_adreno620_gmu_boot/hfi_handshake`（Gmu.cpp:1460/1544），按 `NativeAdrenoSubmit.h:22` 的契约在 reset 与 CP init 之间调用 `ring->restart(completed)`，最后用 `submitNoop/waitFence` 探针确认成功；从 `publishGpu()` 调 `native_adreno_set_restart`。GX 关闭时要有 GMU 冷启动路径。
3) 让 hangCheck 在 fence 1 秒无进展时必定报 hang（必要时只以 completed fence 计 progress）。
4) 用故意挂起的自测（CP_WAIT_MEM 等永不满足的条件）验证 `hang:` -> restart -> replay -> UI 恢复；再重调 `sheng-gpu-unstick` 选最优档。
5) 稳定性验收：手机档镜像（wf163 配置）连续 30 分钟无冻结、无 panic；触摸交互 30 秒连续滑动不掉帧。

注意：
- 每次上机前先写验收标准再看结果；单项卡满 3 小时就记录现状转下一项。
- 改内核用 `python3 unixnu build kernel prelink`（需要 `DEVELOPER_DIR` 和 `UNIXNU_KERNELCACHE`，见文档第 6 节）；构建 payload/启动/抓串命令都在同一节。
- 插件与 owner-trust 必须成对：`build-adreno-metal-plugin.py` -> `make-owner-trust-bundle.py` -> `make-gpu-addon.py`。
- 不要动 `sheng-data-persist=0`（Data 块设备会起不来），不要删 `offline-hid-home=1`（触摸 HID 设备只在 sendHome==1 时创建），不要追 0x1338f508。
- 多派 subagent 做源码调查，减少上下文占用；不要提问，自主推进。
