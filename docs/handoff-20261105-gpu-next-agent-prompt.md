# Prompt for the next agent (copy/paste)

你在 `/Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog` 工作。板子是 Xiaomi Picasso（SM7250 / Adreno 620 / NT36672C 触摸 / 1080x2400），跑自建 XNU + iPadOS 17.6（21H461）用户态，只能用 `fastboot boot` 启动，CDC 串口独占，`boot-progress=2` 不能删，内核 cmdline 上限 1000 字节。

你的任务：**继续 GPU 优化**。GPU 刚刚恢复：之前的 addon/owner-trust 不配套导致 AMFI 拒绝插件、GPU 完全没起来；现在 wf292 上插件能打开、能提交、能出画面。下一步是完成 IR3 着色器缓存的第二启动验证并量化收益，然后用实测数据找 GPU 路径的剩余瓶颈，一次一个变量。

先读（必须）：
1. `docs/handoff-20261105-gpu-next-agent.md` —— 当前状态、回归根因、验证数据、命令、约束、未提交改动清单。
2. `docs/handoff-20261104-statusbar-gpu.md` 和 `docs/handoff-20261102-gpu-first-submit-stall.md` —— 历史（红色状态栏、首次提交 CP_HW_ERROR）。
3. `UniXNU/scripts/build-adreno-metal-plugin.py`、`UniXNU/scripts/make-gpu-addon.py`、`drivers/NativeMetal/NativeAdrenoMetalPlugin.mm`（`MESA_SHADER_CACHE_DIR` 的设置点）。

当前状态（不要重复验证）：
- 设备在 fastboot（serial 52cc791d），最后验证镜像 `out/wf292`：kernel `2e606fb6…`、addon `b70d0fb1…`（publish IR3）、trust `75bea288…`、drop 路径 659 B args。
- wf292 硬件实测：AMFI 拒绝 0；`PICASSO_GPU open: plugin` ×4（backboardd ×2、SpringBoard ×2）；`submissions=446`；display `submit=704 accept=704 reject=0`；hangcheck fence 推进、无 retry。
- 关键教训：owner-trust 必须与 addon 的插件构建配套（`owner-trust.json` 的 `own_cdhashes` 要含插件 CDHash `6dc9959b…`）。wf281+ 用旧 wf253 addon 配新 trust → 每轮 `unsuitable CT policy 0 … rejecting signature`，GPU 消失。修复：`make-gpu-addon.py --publish --clients backboardd,SpringBoard` 从当前 `UniXNU/build` 重打 addon，配当前 trust。非 publish addon（缺 `NativeAdrenoMetal.publish`/`.clients`）AMFI 干净但插件不发布。
- IR3 缓存：wf292 首启 `PICASSO_M2V import files=0 skipped=1144`（16,360,988 B），导出增长到 16,857,490 B（4117 blocks）。第二启动验证待做。
- 不要追 LaunchServices store 回放/无图标问题（用户已叫停）。需要完整应用列表时用默认 drop 路径（不带 `picasso-ls-restore`），SpringBoard 会枚举全部 12 个 staged 应用；restore 路径只有 3 个。
- 红色状态栏（snapshot RGB `ffbf342b`）是已知遗留，不是本任务优先项。

按顺序执行：
1) 第二启动 wf292：`fastboot boot` + 采集；检查 `PICASSO_M2V import files>0`、导出是否收敛；记录第二次启动的 SpringBoard GPU 打开/首帧时间作为 IR3 收益基线。
2) 若导入正常：做 A/B（同内核，publish addon 但插件不带 IR3 改动或关闭 `MESA_SHADER_CACHE_DIR`），量化首帧/编译时间差；结论写进 PR #17。
3) 用 `ps`（20 s 采样）和 `PICASSO_DISPLAY_STATE`（submit/accept/copies/transfer）分离合成器成本与 shader 编译成本，找出剩余瓶颈；一次一个变量做镜像。
4) 每轮先写验收标准再上机；单项卡满 3 小时就记录现状转下一项。

构建/启动命令（精确）：
- 内核：`DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed python3 unixnu build kernel prelink`
- 打包：`bash scripts/build-wf-payload.sh TAG ADDON [extra args...]`；ADDON 必须是 publish 且与 trust 配套（核对 `out/TAG/picasso/payload/payload.json` 的 kernel/addon/trust sha）。
- 插件/信任：`python3 UniXNU/scripts/build-adreno-metal-plugin.py` → `python3 UniXNU/scripts/make-gpu-addon.py --publish --clients backboardd,SpringBoard OUT`；`codesign -dvvv` 核对插件 CDHash 在 `owner-trust.json` 的 `own_cdhashes` 中。
- 启动：设备在 fastboot → `/opt/homebrew/bin/fastboot boot out/TAG/picasso/boot/TAG.img`，立刻采集：`python3 scripts/capture-picasso-usb-console.py --port /dev/cu.usbmodemPICASSO011 --wait 150 --reconnect --duration 330 --send ps --send-after 30 --send-interval 20 --output /tmp/TAG-probe.log`
- 回 fastboot：经采集工具发 `reboot-fastboot`（`--send reboot-fastboot --send-after 3 --duration 75`；端口打开/配置约 37 s，窗口太短会漏发）。
- CDC 命令只能经采集工具注入（串口独占）：`ps`、`snapshot`、`save-ls`、`ls-scan`、`reboot-fastboot`。

约束（必须遵守）：
- 只用 `fastboot boot`，绝不刷 boot/recovery；不动 device tree/分区表；只写受检 store 窗口。
- 一次一个变量；每个阶段独立镜像并写回滚说明；每阶段启动两次对比 + 断电测试。
- `boot-progress=2` 不能删；cmdline ≤1000 B；绝不 `sheng-data-persist=0`；绝不删 `offline-hid-home=1`。
- CDC 串口独占；一次只启动一台；不要 reset/checkout 共享 worktree。
- 实测值+证据汇报；里程碑提交并更新 PR #17；保持手机身份（iPhone13,1）和应用默认值烘焙；固件 blob 不入库。
