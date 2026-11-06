# Prompt for the next agent: fix the GPU hang and the layout bugs

你在 `/Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog` 工作。板子：Xiaomi Picasso
（SM7250 / Adreno 620 / Novatek NT36672C / 1080x2400），自建 XNU + iOS 17.6（21H461，iPadOS internal 版）
用户态。约束：只能用 `fastboot boot`；CDC 串口独占；`boot-progress=2` 不能删；内核 cmdline ≤ 1000 字节。
不要提问，自主推进；每步先写验收标准再看结果；单项卡满 3 小时就记录现状转下一项；多派 subagent 读源码，
别重复造轮子。

你的任务有两条，按优先级执行：

## A. GPU 卡死（最高优先级）

现象：系统能启动、能进手机档界面、真实触摸可用、swap 正常，但 GPU 会周期性 50 ms stall，最终永久挂起，
整屏冻结且不恢复。与是否发布 Metal 插件无关（wf159 软件渲染 + `sheng-fb-gpu=1` 在 1491 帧后冻结）。

必读：
1. `docs/handoff-20261103-gpu-fix-next-agent.md` —— 已验证事实、根因链、修复步骤、构建/启动命令。
2. `docs/handoff-20261103-gpu-render-next.md` —— wf143..wf166 每次上机日志与结论。
3. `drivers/NativeAdreno/NativeAdrenoSubmit.cpp` / `NativeAdrenoRing.cpp` / `NativeAdrenoVm.cpp` /
   `NativeAdrenoSubmit.h`，`out/picasso/tree/drivers/NativeAdrenoPicasso/Adreno620Cp.cpp`、`Adreno620Gmu.cpp`。

不要重复验证的结论：
- 不是 BO 生命周期问题：FSR 恒为 0x400，按 `FSR_FAULT=0xc00001fe` 无 fault 位；0x400 只是 AArch64 地址
  格式位；FAR 0x1338f508 不可信，且 client IOVA ≥ 0x100000000，旧 `handleAt(低32位)` 永远返回 0。
- 缺口是 restart hook：`native_adreno_set_restart` 无调用者；`NativeAdrenoSubmit::recover` 已写好，只等
  `NativeAdrenoRestart` 回调。`hangCheck` 在所有卡死里从未触发。
- stall 处理器每 50 ms 触发一次（`cpFence=fence-1`、`rptr<wptr`），做 `invalidateTlb()+kick()`；
  `sheng-gpu-unstick` 位掩码已实现（bit0 TLB、bit1 重踢、bit2 drain），`=2` 仍会挂。

按交接文档第 4 节做：
1) 修正并限界 stall 的 SMMU 诊断（只在 `fault[0] & 0xc00001fe` 时打印、64 位 FAR、≤6 条）；给 hangCheck
   加导出计数器并每秒打印，确认是 `g.lock` 饿死还是 `madeProgress` 误判。
2) 实现并安装 Adreno620 restart hook：复用 `hardwareInit()/uploadSqe()/programCp()/cpInit()/
   leaveSecureMode()`（Adreno620Cp.cpp:502/637/662/694/719）与 `native_adreno620_gmu_boot/hfi_handshake`
   （Gmu.cpp:1460/1544），按 `NativeAdrenoSubmit.h:22` 的契约在 reset 与 CP init 之间调用
   `ring->restart(completed)`，用 `submitNoop/waitFence` 探针确认；从 `publishGpu()` 调
   `native_adreno_set_restart`。GX 关闭时需要 GMU 冷启动路径。
3) 让 hangCheck 在 fence 1 秒无进展时必定报 hang（必要时只以 completed fence 计 progress），用故意挂起的
   自测（CP_WAIT_MEM 永不满足）验证 `hang:` → restart → replay → UI 恢复。
4) 重调 `sheng-gpu-unstick` 选最优档；稳定性验收：同一镜像连续 30 分钟无冻结、无 panic；交互验收：真实触摸
   连续滑动 30 秒不掉帧。

## B. 布局 bug（其次）

身份链路已经修好并固定（wf166，用户确认界面变 iPhone 风格）：`PICASSO_PHONE_IDENTITY=1` 让 ADT 输出
iPhone10,3 / D22 / phone / 2340 / scale 3；内核补丁 `patches/picasso-clear-gestalt-xnu.patch` 在 Data 挂载后、
用户态前删除 MobileGestalt 缓存（日志 `PICASSO_GESTALT removed ...`）；`sheng-display-scale=3` 给出 360x800 pt；
`NativeLegacyDisplay` 已支持 2340→2400 信箱（居中黑边）。不要重做这几项。

需要解决的布局问题：
1. 状态栏变成红色 `rdar:45025538`：这是 Apple internal 构建的状态栏覆盖。21H461 是 internal 版 iPadOS root，
   与 iPhone 身份不匹配时触发。两条路：(a) 换 iPhone 的 iOS IPSW root（上游文档的配对方式）；(b) 找出内部
   SpringBoard 的状态栏覆盖开关并关闭（在 root 里定位 `rdar:` 字符串与设置来源）。记录你选了哪条、为什么。
2. 开机 Apple logo 消失：logo 由 iBoot/UEFI 在 XNU 前绘制，与 ADT/gestalt 改动无关。定位是 UEFI 启动路径、
   boot image 还是固件状态导致（对比有 logo 的旧镜像/启动方式），给出修复或明确结论。
3. 核对并修掉其余布局问题：360x800 pt 画布、上下黑边是否对称（当前 30/30 行）、home screen 网格、状态栏/
   手势区域是否在可视范围内；用截图或日志里的 `submit/transfer` 计数证明。

验收（B）：截图显示 iPhone 风格布局、无红色 rdar 状态栏（或明确记录 root 限制）、上下黑边正确、触摸手势与
可视区域一致。

## 环境与命令

- 工作树硬链接：`drivers/...` 与 `out/picasso/tree/drivers/...` 是同一文件；插件源码在
  `/Users/silenzio/Desktop/development/ios/UniXNU/drivers/NativeMetal`。
- 内核：`export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed`
  `python3 unixnu build kernel prelink`
- payload：`IPHONE_IDENTITY=1 GRID=0 NO_FBGPU=1 UNIQUE_CHIP_ID=01dec0dec051a543 bash scripts/build-wf-payload.sh TAG ADDON sheng-display-scale=3`
  （手机档下 `ipad_identity` 板级键保持不变，由环境变量切换；不要再用 `iphone_identity` 板级键）。
- 插件/信任必须成对：`build-adreno-metal-plugin.py` → `make-owner-trust-bundle.py` → `make-gpu-addon.py`。
- 启动：`picasso-cdc-command.py --command reboot-fastboot ...`；抓串：`capture-picasso-usb-raw.py --output out/TAG-runs/run1.log --wait 900 --duration 1800 &`；`fastboot -s 52cc791d boot out/TAG/picasso/boot/TAG.img`。
- 当前基线镜像：`out/wf166`（kernel 697a867d，trust 766b431c，scale 3，无 handoff，HID 正常，iPhone 界面）。
- 基准镜像已存在但未启动：`out/wf156`（`sheng-gpu-bench=1` + `sheng-gpu-perf-index=1`）、`out/wf157`
  （`sheng-gpu-bench=1`，默认 index 3）；启动后等 `T s...` 与 `B fill=..ms tri=..ms compute=..ms`，验收
  perf3 的 fill/tri 均比 perf1 快 ≥50%。
- 不要：把 `sheng-data-persist` 设为 0（Data 块设备起不来）；删除 `offline-hid-home=1`（触摸 HID 设备只在
  sendHome==1 时创建）；追 FAR 0x1338f508；动 `boot-progress=2`。
