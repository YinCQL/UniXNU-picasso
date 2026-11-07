# GL Submit 分阶段测量与诊断读取实验（2026-11-08）

本轮在 GL surface 快速哈希的基础上继续实验，代码、主机测试和测量证据一同归档。
所有新性能行为默认关闭；设备只使用临时启动，没有 flash。
大型产物保存在 `/Volumes/ios-archive/experiments-20261107/gl-prof01`，
工作树 `out/gl-prof01` 是符号链接。GL/Mesa bundle 复用 `gl-exp03`，没有重新编译。

GL 发布分支为 `stage12/picasso-gl-submit`，基于 fork 的 `stage12/picasso-ios16`，
仅包含此前的快速哈希和本轮 Submit 优化、诊断工具、测试及证据。此前要求仅保留本地的
m2v 实验提交，以及未提交的 GL 并行 wait 等实验，不纳入此次远端发布。

## 测量方法

`ngp=1` 开启 Submit 的累计经过时间统计：ABI 调用、内核锁等待、dispatch、submit 本体、
参数校验、退休、audit、缓存发布、VM 切换、GPU 排空、ring 提交、导入内存 acquire。
随后新增 `ibpeek`，单独测每次提交对第一个 IB 的 28 个诊断命令字的读取。
关闭统计时不读取计时时钟。报告在释放全局驱动锁后约每 10 秒输出。

这些阶段存在包含关系，不能直接相加；经过时间包含调度和阻塞，不能称作 GPU 时间。
没有插入 glFinish，没有开启 CP stamps。`ngp=1` 同时开启原有 CPU timeline/call ring，
所以两组都使用同样的统计设置。分析以相邻累计报告之差为准，排除启动期前 16 次 IB audit。

初始 `p01-stalled.log` 用于定位未被原阶段覆盖的成本；该轮曾出现用户报告的壁纸异常和
无法操作，不作为正式 FPS 对照。以下结果来自补齐触摸固件后的两组。

## 真机对照结果

两组均排除启动阶段，选择报告末帧 >=600、区间帧数增加 >=100、区间提交 >=200 的
相邻完整报告。A 有 6 个区间、12951 次提交、3180 帧推进；B 有 7 个区间、12682 次
提交、2887 帧推进。下表为选中区间总经过时间除以总提交次数。

| 阶段 | A：ngb=0，µs/提交 | B：ngb=1，µs/提交 |
|---|---:|---:|
| IB 诊断读取 | 1056.799 | 134.701 |
| Submit 本体 | 1694.992 | 799.385 |
| ABI Submit 调用 | 1703.882 | 807.443 |
| 缓存发布 | 580.910 | 607.016 |
| 全局锁等待 | 1.677 | 0.999 |
| VM 切换（包含排空） | 2.710 | 1.422 |
| mode 1 GPU 排空 | 2.224 | 0.950 |

IB 读取成本减少 **87.25%**，Submit 本体减少 **52.84%**，ABI 调用经过时间减少
**52.61%**。缓存发布没有下降，仍约 0.6 ms/提交；这也支持收益主要来自诊断读取，而非
简单的整机频率变快。VM 排空和锁等待在本轮样本中不是大头，但不能排除其他应用多 VM
并发时出现瓶颈。A 的活跃 GL 窗口提交数中位数为 4.1 次/帧，因此原诊断读回单项就会
占据数毫秒的帧预算；不能把嵌套阶段相加，也不能据此承诺固定屏幕 FPS。

同一份 GL 分析器选出 A/B 各 119/114 个活跃 20 帧窗口。窗口内 CPU 经过时间中位数的
中位数为 12.958/9.429 ms；renderer 完成频率中位数为 53.05/58.82 次/秒。
这些只是本次交互的观察量：两组都因物理触摸取消自动回放（约 boot 178 s），没有完成
任何自动 swipe，不能将它们解释为同一动画的严格 FPS 提升，renderer rate 也不是屏幕
独特帧率。两组仍有 Data 空间不足的写入错误（err 28），未清理。

A 最后 DPU 汇总为 3480 flips，B 为 3120 flips；两组 mapfails、fallbacks、latch_timeouts
均为零，未检测到 kernel panic。不能把不同运行时长的 flips 总量直接比较为 FPS。

冻结证据：`out/gl-prof01/p03a.log`、`p03b.log`，相应 `*-profile.json`、`*-gl.json`。
完整筛选区间、统计、输入和 SHA256 收录在 `docs/evidence/gl-submit-20261108.json`，
外接归档副本为 `out/gl-prof01/comparison.json`。

- A 日志 SHA256：`959f4dac470e5a8e1a24e34cd7cfd422d26e26ee678e555a0779ba5bfe2523ee`。
- B 日志 SHA256：`b19b2b1c7856168522f3c30a4020a2e81ba69e8f5f90703f43a00eea385ebaf6`。

## 实际修改

原路径每次 Submit 都调用 `NativeAdrenoVm::peekWords(...,28)` 填写 IB 诊断环。
系统 BO 没有常驻内核映射；原实现逐字调用 `native_adreno_dram_read32`。Picasso 的这个
回调每次都新建只读、禁止缓存的 4 KiB 物理映射，读取一个字，然后释放。一次常规诊断
快照可能建立/拆除同一页的映射 28 次。

`ngb=1` 只让这次诊断读取复用已有的 `native_adreno_dram_read_words` 回调。
请求按 BO 尾部、物理 span、4 KiB 页边界切分；普通 112 字节快照只需一次映射。
保留全部 28 字诊断内容，处理短读和错误，缺少批量回调时回到原逐字路径。
不保留映射，不改 CPU/GPU 缓存属性、维护范围、fence、VM 切换、PM4 命令或 DPU 呈现。
其他调用 `peekWords` 的路径默认仍使用原模式。

## 隔离构建与触摸恢复

新根复制了原 build/cache，而生成头文件的绝对链接仍指旧根；已只在私有树重定向
235 条存在对应目标的链接，记录在 `rebased-links.json`。私有 display archive 的 brightness
源码版本与旧 archive 的 manifest 不一致，按当前源码重建并通过原 ABI 校验。两组共用
这个 display，不把与旧镜像的差异归因到本次优化。

发现并纠正另一项构建问题：baseline shell 即使复用多数 archive，也会重建 Novatek
驱动。隔离根找不到默认固件，先前 `gl-p01a` 明确打印
`PICASSO_NVT stage4 disabled: firmware is not supplied`。因此启动参数为 stage4 并不
证明物理触摸可用。该轮立即结束并恢复 `gl-u01`。`gl-p01` 还显式使用了 stage0。
这些镜像及日志保留，不继续作为可交互或正式 FPS 对照的依据。壁纸异常原因尚未确认。

最终重建明确传入：

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
ADRENO620_FW_DIR="$PWD/out/picasso/gpu-firmware" \
NOVATEK_TOUCH_FW="$PWD/temp/touch-fw/novatek_nt36672c_g7a_fw01.bin" \
python3 ./unixnu build kernel --out out/gl-prof01 -j 3
```

固件 131072 bytes，SHA256 `f62db7d12c372834e3d0d1ea95665a338a1c64ec955d45e4b91f921ba0690ce2`。
最终 `kernels/gl-p03.macho` SHA256：
`a3d36f0742f4135d96b107fa5e5d475ff0338cf971452cf27da17859a62285a5`。

对照 `gl-p03a` / `gl-p03b` 使用同一内核、display、GL/Mesa、固件和 payload 输入；
stage4、相同的三次自动滑动计划（均被物理触摸取消）、GPU perf index 4、DPU scanout 2、快速哈希、串行 wait。
仅 `ngb=0/1` 不同。每个镜像的命令和 SHA256 保存在 `gl-p03*-boot.json`。
必须检查真实的固件写入、`sram ok=1/1/1` 和 `stage4 loop irq=1`，再开始测量。

## 验证

`scripts/check-native-adreno-profile.py` 构建真实 VM/memory/SMMU/ring/submit 代码，
没有复制一份优化算法来测试。VM 新增测试比较批量与逐字的结果，覆盖连续及不连续物理页、
跨页、BO 尾部补零、短读、失败、非法返回值、缺失回调和空请求。现有提交 suite 在
批量开关两种模式下都校验相同 PM4/fence、错误拒绝、所有权、audit、恢复与重放逻辑。
普通构建和 ASan+UBSan 均通过。原 suite 故意保留 poisoned 对象，因此 sanitizer 构建
关闭 leak 检查；地址和未定义行为检查仍开启。软件 peer 不证明硬件像素正确性。

```sh
python3 scripts/check-native-adreno-profile.py --tree out/gl-prof01/picasso/tree \
  --output out/gl-prof01/host-check
python3 scripts/check-native-adreno-profile.py --tree out/gl-prof01/picasso/tree \
  --output out/gl-prof01/host-check-asan --sanitize
python3 scripts/tests/test_native_adreno_profile.py
```

累计分析器 5 项测试通过；profiler 的 disabled-clock、嵌套、显式结束和并发原子累计
测试通过。NativeAdreno ARM64/iOS17 41 个对象及 prelink 通过。

`protected-after-batch.json` 记录 gpu72、原 kernel、原 libNativeGL 和 OSMesa 的前后
SHA256，四项保持一致。未清理 Data 分区或旧实验产物。CDC capture 始终保留同一个 reader。

## 当前可交互镜像与回滚

最终临时启动 `out/gl-p03u/picasso/boot/gl-p03u.img`，内核仍为 `gl-p03.macho`。
`ngb=1 ngp=0`，快速哈希 `glh=1`，并行 wait 关闭 `glw=0`；stage4，未配置自动手势。
镜像 SHA256：`7d49eaa6115d2b9fddc5ec3c3130298a8aab64d63063e69bf3b68be05b74df25`。
启动读回确认 mode 1 GPU 自检通过、批量开关开启、131072 字节触摸固件写入、
`sram ok=1/1/1`、`stage4 loop irq=1`、HID 计划 `home=0 swipe=0`，用户态 GL 已完成初始帧。
这是可供用户取舍的实验镜像；代码归档和发布不改变编译默认值，也不执行 flash。

`p03u-capture.json` 保存同一个 CDC capture 的偏移，交互 smoke 证据为 `p03u-smoke.log`
与 `p03u-smoke.json`。用户看到的壁纸状态不能仅由这些计数器确认。

需要回滚时保留 CDC reader，先从 CDC 发送 `reboot-fastboot`，再执行：

```sh
fastboot -s 52cc791d boot out/gl-u01/picasso/boot/gl-u01.img
# 更早的已验证 GL 回滚镜像：
fastboot -s 52cc791d boot out/gpu72/picasso/boot/gpu72.img
```
