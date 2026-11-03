# Stage 7 UI Trace / Chrono Stub Handoff

Updated: 2026-11-01 16:49 CST  
Status: paused by the user during the first `chrono-pmap2` device run.

## 0. TL;DR（五分钟速览，先读这一节）

- **已经走到哪**：XNU、launchd、backboardd、SpringBoard 全部在跑；屏幕上只有 backboardd 提交的 3–4 帧 Apple logo，**SpringBoard 从未提交过帧**。
- **今天拆掉的墙（运行时 stub，不改镜像）**：
  1. `lsd`（LaunchServices）`performJournalRecovery` 死锁 —— **已验证有效**，拆掉后 SpringBoard 等 `peer=99` 的现象立刻消失；
  2. `chrono`（ChronoServices）`_subscribeToExtensions` 同步 XPC —— **桩还没成功打上**（页翻译失败），见 §4/§8。
- **2026-11-01 17:40 突破**：只加一个 boot argument `sheng-lite=1`（内核/ADT/Data 逐字节不变）就解开了 chronod 同步 XPC 墙，**SpringBoard 渲染出锁屏**（真机目视确认）。详见 §13。
- **旧阻塞（已绕开，保留作分析）**：SpringBoard 主线程停在 `mach_msg2_trap+8`，最新等待目标 **`peer=96` = chronod**（判读方法见 §3.0）。
- **下一步**：锁屏已到；主屏（桌面）还缺输入设备路径，见 §13.4。
- **现在不要做**：不要碰 GPU/ADT；不要改缓存属性或 `ml_io_map`；不要新建诊断工具。
- **别误读的日志**：`shared_region: ... [1(launchd)] ... vm_shared_region_start_address() failed`
  是 launchd 的探测告警，**正常启动也会出现**（run4 成功那次就有 2 条，之后照常 open
  `dyld_shared_cache_arm64`）。不要把它当失败判据。
- **最大的坑**：`ps` 输出量大，16:11 那次就是它把 USB 打断（`Errno 5`），导致后面的定时命令全丢。**不要用 `ps` 当触发条件。**

## 1. Worktree and device

- Repository: `/Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog`
- Branch: `stage7/watchdog-integration`
- Device serial: `52cc791d`
- The worktree is intentionally dirty. Do not clean, reset, or revert it.
- No commit was made for this round.
- A background raw-USB capture was stopped when the user paused the task. Do not assume the device stayed in any particular boot phase after that point.
- At the last read-only check, `/dev/cu.usbmodemPICASSO011` still existed; Fastboot was not present. Re-check before sending commands.

## 2. What the 15:24 log meant

The earlier `/tmp/ui-trace-run.log` was produced before the UI-trace CDC mirror was
added. The official boot trace renderer writes rows into the framebuffer and the
old build did not also send those rows to USB, so a log with no trace text was
expected rather than evidence that tracing had failed.

The mirror is now implemented in:

- `drivers/NativeMobileDisplay/boot-progress/BootUiTraceProbe.c`

`boot_ui_trace_snapshot()` sends each completed page through
`picasso_usb_log_write`, including a `PICASSO_UI_TRACE page=...` header. Later
captures in `/tmp/picasso-lsd-run4.log` and
`/tmp/picasso-postfix-probe.log` contain `PICASSO_UI_TRACE page=1/4` through
`page=4/4`, so the evidence path now works without taking a photo.

## 3. Proven userland blockers

### 3.0 `peer=` 是"在等谁"的第一手证据（优先看这个，再看栈）

`PICASSO_UI_TRACE` 每条 `trap=` 行都带 `peer=`，页脚有图例：

```text
peer e0=owner(-1=none) e1=not Mach2 e2=no remote e3=no space e4=lookup failed e5=receive arg candidate
```

- `peer=<pid>` + `e0`：接收方进程号（`e0=owner`）；
- `peer=-1`：端口没解析出 owner，`e1..e5` 给原因，**不代表没在等**。

实测映射（2026-11-01；pid 每次 boot 会漂移，用 `PICASSO_SYNC` 的 `name=` 或 `ps` 复核）：

| 观察值 | 进程 | 说明 |
|---|---|---|
| `peer=99`（run2/run3） | `lsd` | LaunchServices 死锁墙 |
| `peer=96`（run4，lsd 修好后） | `chronod` | ChronoServices 墙 |
| `peer=1` | `launchd` | bootstrap / job 查询 |
| `peer=27` | SpringBoard 自身 | 自引用 |

**判读顺序：先看 `peer` 是不是一个已知服务，再决定要不要符号化。** 之前有两轮在"`link=4` 时 `target=0`"上打转，其实同一页的 `peer=` 已经写了答案。

### LaunchServices deadlock

SpringBoard was waiting through:

`SpringBoard -> lsd -> LSInstallProgressService journal recovery`

More precisely:

`performJournalRecovery -> dispatch_sync(_installControlsQueue)`

while `restoreInactiveInstalls` and the database-blocking XPC thread were waiting
for the LaunchServices seeding group. **推断（非直接证据）**：展开 seed 后
`/private/var/db/lsd` 基本为空，这个 boot 镜像可能根本没有 LaunchServices 的
install journal 或 `.csstore`，因此跳过空 journal 恢复是合理的，且不会关闭
authentication、AMFI 或文件系统策略。若后续想验证这个推断：改用一份带真实
LaunchServices 数据的 seed 对照即可（见 §11 的 OOBE/seed 策略）。

The patch changes the `-[_LSInstallProgressService performJournalRecovery]`
entry from:

```text
0xd10243ff / 0x6d0523e9
```

to:

```text
ret / nop
```

Successful evidence from `/tmp/picasso-lsd-run4.log`:

```text
PICASSO_LSD_STUB resolved name=lsd pid=99
PICASSO_LSD_STUB applied part=journal-entry slide=12360000 address=19a12532c physical=eab7e32c
PICASSO_LSD_STUB result pid=99 applied=1 requested=1; restarting lsd
```

After that restart, SpringBoard advanced. Do not repeatedly kill `lsd`; one
successful application is enough.

### ChronoServices synchronous XPC

SpringBoard's new main-thread blocker is:

`CHSChronoServicesConnection _subscribeToExtensions`

The captured stack in `/tmp/picasso-postfix-probe.log` contains:

```text
0x1ddae0b58 mach_msg2_trap+8
0x1a18e9538 _dispatch_mach_send_and_wait_for_reply+0x20c
0x1ef9d3df0 __xpc_connection_set_event_handler_slow+0x520
0x1b17dc498 -[BSXPCServiceConnectionMessage _sendSynchronously:]+0x188
0x1b17dba60 ... createImplementationOfProtocol ... +0x7d4
0x1a6af9f18 ___53-[CHSChronoServicesConnection _subscribeToExtensions]_block_invoke+0xbc
```

That boot had cache slide `0x12360000`. The intended Chrono patch targets are:

- Subscription method: unslid `0x194799d9c`, slid `0x1a6af9d9c`
- Orientation method: unslid `0x18f66a248`, slid `0x1a19ca248`

The subscription call-site PC is slid `0x1a6af9f18`; it is in the same 16 KiB
page as the subscription patch target. Nevertheless, `pmap_find_phys` returned
zero for both targets.

The current low-cost hypothesis is that these user shared-cache pages are
reachable through the task pmap's nested shared-region pmap, but
`pmap_find_phys(task_pmap, address)` did not resolve them in the CDC poller
thread context.

## 4. Changes made in this round

Source of truth:

`patches/unixnu-source-kernel.patch`

The Chrono patch now contains `picasso_user_page()`:

1. Try `pmap_find_phys(pmap, address)`.
2. If that returns zero and `pmap->nested_pmap` exists, retry
   `pmap_find_phys(pmap->nested_pmap, address)`.
3. Report whether the page came from `root`, `nested`, or `none`.

`PICASSO_CHRONO_STUB` now also prints:

```text
pmap=... nested=... min=... max=... slide=...
```

and miss lines include root/nofault lookup results.

The LaunchServices patch now uses the same helper and reports `where=...`. This
was necessary because the next boot produced a miss even for the previously
working `lsd` patch:

```text
PICASSO_LSD_STUB resolved name=lsd pid=100
PICASSO_LSD_STUB miss slide=1686c000 address=19e63132c
```

That miss confirms the page-translation problem is not Chrono-specific.

**这是一次回归，必须先判断再继续改**：`lsd` 在 `slide=0x12360000` 时能命中
（`physical=eab7e32c`），换到 `slide=0x1686c000` 后连它都 miss。两种可能：

1. (a) 新 boot 的 slide 变了，地址算式有偏差；
2. (b) `picasso_user_page()` 的 nested-pmap 逻辑把原本可用的主路径弄坏了。

**先用 §5 的已知可用镜像（`lsd-stub2`，sha256 前缀 `77200509`）做一次对照**，
确认 lsd 还能命中，再决定是否继续改 pmap。**不要在没有对照的情况下继续叠加
pmap 改动**——今天已经因为"同时改多处"浪费过一整轮。

已知可用的成功记录（run4，slide `0x12360000`）：

```text
PICASSO_LSD_STUB applied part=journal-entry slide=12360000 address=19a12532c
  physical=eab7e32c before=d10243ff/6d0523e9
PICASSO_LSD_STUB result pid=99 applied=1 requested=1; restarting lsd
```

注意：run4 里 lsd 被 patch 了两次（pid 99 → 123）。这不是"反复杀 lsd"，而是
lsd 重启后换了新 pid；**只有新 lsd 再次卡住时才需要再打一次**，不要主动循环。

Important coupling: `picasso_patch_lsd_task()` calls the helper defined by the
Chrono patch earlier in `osfmk/kern/task.c`. Do not remove or reorder those
patches without moving or duplicating the helper.

## 5. Build and runtime evidence

### `chrono-pmap1`

This image had the nested-pmap fallback only in the Chrono patch; `lsd` still
used its original root-pmap lookup.

- Boot image:
  `out/chrono-pmap1-20261101/picasso/boot/chrono-pmap1.img`
- Kernel SHA-256:
  `3977f43231770d4975389a609b8ed59158803f785a63a8bfb036c960b0548be3`
- It booted with slide `0x1686c000`.
- `stub-lsd` found PID 100 but missed address `0x19e63132c`.
- The Chrono stub was therefore never reached.
- This also invalidates the assumption that the lookup failure was specific to
  the Chrono subcache.

### `chrono-pmap2`

This is the current test image. Both Chrono and lsd use the nested-pmap fallback.

- Boot image:
  `out/chrono-pmap2-20261101/picasso/boot/chrono-pmap2.img`
- Boot image SHA-256:
  `c9c6ca3822fda56761c66357f4b5353ea8805607d4896e41ffd1d746ccacef67`
- Kernel:
  `out/picasso/kernel/kernel.macho`
- Kernel SHA-256:
  `25aaa786ec9601af4a71481e540c2f10acb5eb6c083eac47e527451856edd129`
- Payload:
  `out/picasso/payload/payload.img`
- Payload SHA-256:
  `b006fb55de62b4399673863e30626d4f9b7b9afe6dbb7f083bf0bfb303c618ff`

The image was sent with `fastboot boot`; no partition was written.

The first capture was interrupted after roughly 30 seconds while launchd was
still verifying dyld split caches. Its partial log is:

`/tmp/picasso-chrono-pmap2-run.log`

It contains successful CDC `status` traffic but no result for `stub-lsd` or
`stub-chrono`. The capture process was stopped when the task was paused.

The planned timed commands were:

- 100 seconds: `stub-lsd`
- 130 seconds: `stub-chrono 27`
- 150 seconds: `wait SpringBoard`

Those timings are not proven and may be too early. Prefer reacting to
`Early boot complete`, `PICASSO_UI_TRACE`, or a `ps` result when possible.

## 6. Device-control runbook

The macOS tty path can block indefinitely. A `/dev/cu.usbmodemPICASSO011` node
may exist while `open()` still hangs. Prefer direct libusb capture.

If direct libusb reports access denied or cannot find the CDC interface, reset
the USB device itself without resetting or rebooting the phone:

```sh
python3 - <<'PY'
import usb.core
d = usb.core.find(idVendor=0x1209, idProduct=0x0001)
if d is None:
    raise SystemExit("Picasso CDC not found")
d.reset()
print("USB reset complete")
PY
sleep 2
```

After that, use:

```sh
python3 scripts/capture-picasso-usb-raw.py \
  --duration 30 \
  --wait 10 \
  --output /tmp/picasso-current.log \
  --send status \
  --send-interval 15 \
  '--timed-command=1=stub-lsd'
```

To return to Fastboot without touching the host power button:

```sh
python3 scripts/capture-picasso-usb-raw.py \
  --duration 1 \
  --wait 5 \
  --output /tmp/picasso-reboot-fastboot.log \
  --send status \
  --send-interval 10 \
  --reboot-fastboot \
  --fastboot-timeout 45 \
  --fastboot-serial 52cc791d
```

`scripts/picasso-cdc-command.py` and
`scripts/capture-picasso-usb-console.py` use the tty path and are not preferred
while the device is wedged.

## 7. Resume procedure

1. Re-check state with `fastboot devices -l`, `ls /dev/cu.usbmodem*`, and
   `ioreg`/PyUSB. Do not flash or erase anything.
2. If already in Fastboot, boot the current image:

   ```sh
   fastboot boot \
     out/chrono-pmap2-20261101/picasso/boot/chrono-pmap2.img
   ```

3. Start a raw CDC capture with a `status` heartbeat. Wait for
   `Early boot complete` and the first `PICASSO_UI_TRACE` pages.
4. 发 `stub-lsd`（**无参数**会按进程名解析 pid，不需要先 `ps`）。触发条件优先
   用日志里出现 `PICASSO_SYNC ... name=lsd`，**不要用 `ps` 当触发**——`ps`
   输出量大，16:11 那次就在 `ps` 之后把 USB 打断了（`Errno 5`），后续的定时
   命令全部丢失。结果里看 `where=` 和 `applied=`。
5. Once lsd is patched, let SpringBoard advance to the Chrono blocker. Send
   `stub-chrono 27` using the actual SpringBoard PID if it is not 27.
6. Confirm:

   ```text
   PICASSO_CHRONO_STUB pmap=... nested=...
   PICASSO_CHRONO_STUB applied part=subscribe-nil ... where=root|nested
   PICASSO_CHRONO_STUB applied part=orientation-portrait ... where=root|nested
   PICASSO_CHRONO_STUB result ... applied=2 requested=2
   ```

   The function only restarts SpringBoard when `applied == 2`.

7. Recapture `wait SpringBoard`, display submission/retirement counters, and
   process state to identify the next blocker.

## 8. If `where=none` remains

Do not start changing cache attributes, MMU flags, or `ml_io_map` yet. The
evidence still says the address is executable and the target is in the same
16 KiB page as a live return PC, so a generic cache-attribute explanation is
not sufficient.

Collect these next, one run at a time:

1. Full `PICASSO_CHRONO_STUB pmap=... nested=... min=... max=...` line.
2. Root and nested `pmap_find_phys` results at both:
   - the method entry
   - the known live subscription call-site `address & ~0x3fff`
3. The task map entry for the address:
   - `is_sub_map`
   - `vme_submap`
   - `use_pmap`
   - mapped start/end
4. Whether `vm_map_pmap(vme_submap)` differs from both the task root pmap and
   `pmap->nested_pmap`.

If the target page can be resolved through an intermediate submap, use that
pmap only for the physical lookup, then retain the existing dcache/icache flush
and instruction verification.

## 9. Build commands

```sh
cd /Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog

DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed \
python3 unixnu build kernel prelink

python3 unixnu build payload \
  --kernel out/picasso/kernel/kernel.macho \
  --data-ramdisk build/owner-data-8g-noautodim-20261101.dbr \
  --trust-cache /Users/silenzio/Desktop/development/ios/worktrees/stage6-nativeufs/build/original-static-system-trust/original-static-system.bundle \
  --unique-chip-id 01dec0dec051a541

python3 unixnu bootimg \
  --uefi /Users/silenzio/Desktop/development/ios/Mu-Silicium \
  --edk2 /Users/silenzio/Desktop/development/ios/Mu-Silicium/Mu_Basecore \
  --fd /Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog/uefi/SILICIUM_UEFI.fd \
  --dtb /Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog/uefi/mu-silicium/Resources/DTBs/picasso.dtb \
  --output out/<name>/picasso/boot/<name>.img
```

### 单变量纪律（今天反复踩）

- `make-picasso-adt.py` 默认用**随机种子**生成 ADT，每次重建 ADT 都会变。
  重建前先固定 `--random-seed`，否则"这次失败是谁的锅"永远说不清。
- 每轮只改一个变量：**kernel / ADT / Data seed 三者只动一个**。
- 连续重建两次还没上机实验，就停下来先做一次设备实验。
- 反面教材（今天真发生过）：15:27 那轮同时换了镜像和 seed，结果启动没过
  userland，花了一整轮才排除。**同一轮里只允许一个真变量。**

## 10. Key logs and files

- `/tmp/ui-trace-run.log`: old, pre-mirror log; absence of trace is expected.
- `/tmp/picasso-lsd-run4.log`: successful lsd patch and SpringBoard advance.
- `/tmp/picasso-postfix-probe.log`: SpringBoard stack in Chrono synchronous XPC.
- `/tmp/picasso-stub-chrono2.log`: both Chrono page lookups missed.
- `/tmp/picasso-chrono-pmap1-lsd5.log`: lsd lookup missed at slide `0x1686c000`.
- `/tmp/picasso-chrono-pmap2-run.log`: interrupted pmap2 boot capture.
- `patches/unixnu-source-kernel.patch`: all kernel-side patches, including the
  current pmap fallback work.
- `drivers/NativeMobileDisplay/boot-progress/BootUiTraceProbe.c`: CDC mirror of
  the official UI trace pages.

## 11. 成功判据、OOBE 策略与下一轮组合

### 本阶段唯一成功判据

```text
PICASSO_FB begin sel=5 ... pid=<SpringBoard pid>
```

即 **SpringBoard 自己提交一帧**（不是 backboardd 的 logo 帧）。在此之前，
"进程都在跑""trace 正常"都不算成功。

### OOBE 与桌面的关系（别混淆）

| seed | 预期画面 | 能否进桌面 |
|---|---|---|
| 原始（`SetupDone=false`） | Setup / Hello（OOBE） | ❌ 大概率卡在激活（无 Wi-Fi、无激活记录），**这是预期行为，不是 bug** |
| `seed-data-skip-setup` 过的 | 锁屏 → 主屏 | ✅ 官方 sheng 同版本、同样无 Wi-Fi，走的就是这条路 |

**"跳过 OOBE"才是通往桌面的路径**：OOBE 只用来证明"渲染通了"，不用来进桌面。

额外收益：`seed-data-skip-setup` 会写 `com.apple.springboard` 偏好并创建
`/private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}`，把几乎
空的 Data 变成"像真机 Data 的骨架"，**很可能顺带缓解 lsd 的空 Data 退化**。
值得作为独立对照实验（kernel 不变、只换 seed）。

### 下一轮建议（单 boot、多步、可判定）

1. 用已知可用镜像起步：
   `out/lsd-stub2-20261101/picasso/boot/lsd-stub2.img`（sha256 `77200509…`）；
2. 等 `Early boot complete` 和第一条 `PICASSO_SYNC ... name=lsd`；
3. 发 `stub-lsd`（无参，按名解析），确认 `applied=1`；
4. 等 `peer` 从 `99` 变成 `96`（lsd 墙已过，进入 chronod 墙）；
5. 发 `stub-chrono <SpringBoard pid>`（pid 用 `PICASSO_PS` 或 `PICASSO_SYNC` 复核），
   确认 `applied=2 requested=2`；
6. 看 `peer` 是否变成新目标，或直接看有没有 `pid=<SB>` 的 `PICASSO_FB begin sel=5`；
7. 全程用 `status` 心跳，**不要用 `ps` 当触发**。

### 时间盒

- 2 小时内没出现第一帧：停下来写清"卡在谁、peer 是多少、栈是什么"，不要继续堆改动。
- 连续两次重建都没上机：强制先跑一次设备实验。

## 12. Guardrails

- Do not clean the worktree or revert uncommitted changes.
- Do not write or erase partitions for this experiment; use `fastboot boot`.
- Keep the lsd patch. It is a justified empty-journal recovery workaround.
- Do not repeatedly restart lsd after one successful patch.
- Do not change more than one runtime variable per boot.
- Keep watchdog/CDC heartbeat active during long runs so the device can return
  to Fastboot without hand interaction.
- Preserve the current Data seed and trust cache:
  - Data: `build/owner-data-8g-noautodim-20261101.dbr`
  - Trust cache:
    `/Users/silenzio/Desktop/development/ios/worktrees/stage6-nativeufs/build/original-static-system-trust/original-static-system.bundle`

## 13. 2026-11-01 17:34–17:40：`sheng-lite=1` 解开 chronod 墙，锁屏出现

### 13.1 做法（单变量，内核/ADT/Data 零改动）

内核参数只存在于 payload FAT 卷的 `EFI/UniXNU/unixnu.toml` 里。直接在卷里改这一行，再用
`unixnu bootimg` 原样重新封装，就能只改 boot argument：

```sh
cp out/picasso/payload/payload.img out/lite1-20261101/payload.img
mcopy -o -i out/lite1-20261101/payload.img ::/EFI/UniXNU/unixnu.toml /tmp/unixnu-lite1.toml
# 在每个 args = "..." 行尾添加  sheng-lite=1
mcopy -o -i out/lite1-20261101/payload.img /tmp/unixnu-lite1.toml ::/EFI/UniXNU/unixnu.toml
python3 unixnu bootimg --uefi .../Mu-Silicium --edk2 .../Mu_Basecore \
  --fd uefi/SILICIUM_UEFI.fd --dtb uefi/mu-silicium/Resources/DTBs/picasso.dtb \
  --ramdisk out/lite1-20261101/payload.img \
  --output out/lite1-20261101/picasso/boot/lite1.img
```

改动前后用 `mcopy` 取出并用 `shasum` 验证：

| 组件 | 改前 | 改后 |
|---|---|---|
| `Payloads/kernel.macho` (XNU) | `25aaa786…` | `25aaa786…` |
| `Payloads/kernel.macho.adt` | `cedfef73…` | `cedfef73…` |
| `Payloads/scratch-data.img` (Data seed) | `7b2e6e4c…` | `7b2e6e4c…` |

唯一差异是 `unixnu.toml` 的 `args` 多了一个 `sheng-lite=1`。

- 镜像：`out/lite1-20261101/picasso/boot/lite1.img`
- boot image SHA-256：`b99f7b9036824bc05284e9d794cf4ebd519842b886673e886c3c8aacc6951134`
- 走 `fastboot boot`，未写任何分区。

### 13.2 结果

- 内核侧确认参数生效：
  `sheng bring-up watchdogd launch policy: 0 (hardware watchdog not implemented)`
- SpringBoard 不再卡在 chronod：启动后短暂等待 launchd/backboardd 后继续推进，
  UI trace 线程数从 `T45` 升到 `T72`。
- **锁屏画面出现在真机上**（用户目视确认）。这是 M4 的“官方锁屏”部分。
- 旧的成功判据 `PICASSO_FB begin sel=5 pid=<SpringBoard pid>` 在 CDC 日志里没有直接出现
  （该 boot 的 FB 记录只有 backboardd 的 pid），但画面已经由 SpringBoard 渲染出来，
  说明合成路径与日志采样点不一致，不应再把这条日志当作唯一判据。

### 13.3 为什么会有效（与 §3 的关系）

`sheng-lite=1` 在 `drivers/NativeMobileDisplay/boot-progress/BootWatchdogPolicy.c` 里写
`/private/var/db/com.apple.xpc.launchd/disabled.plist`，禁用一整套在本机（无网络、无账号、
无相机）只会互相阻塞的作业：`identityservicesd`、`locationd`、`CommCenter`、`bluetoothd`、
`photoanalysisd`、`coreduetd`、`duetexpertd`、`PerfPowerServices` 等。

之前 SpringBoard → chronod 的同步 XPC 一直不返回，表现为 `peer=95/96`。chronod 本身要经过
这些作业/服务；把它们从启动链上去掉后，chronod 能正常应答，SpringBoard 的主线程也就通过了
`-[CHSChronoServicesConnection _subscribeToExtensions]`。**之前围绕 pmap 页翻译做的 stub 不是
必需路径**；§3/§8 的 pmap 结论保留，但不再是通往 UI 的主线。

### 13.4 下一步：从锁屏到主屏（桌面）

锁屏需要一次 Home / 上滑手势。本树没有任何输入设备驱动，也没有 HID 注入：

- 本树 `package/`、`drivers/` 里没有 `offline-hid-*`、`sheng-buttons`、`sheng-touch-hid`
  的实现（这些属于 sheng 的 `drivers/ShengHidInput/NativeHidInput.cpp` +
  `package/HidInputProbe.cpp`，本仓库未移植）。
- sheng 的无人值守路径：`offline-hid-input=1` 建立离线输入源，
  `offline-hid-auto-home=<秒>` 自动发 Home 事件（`offline-hid-auto-home-count/interval`）。
  iPad7,1 身份是带 Home 键的机型，理论上 Home 事件即可解锁。
- 可行的下一步（都属于新的单变量实验）：
  1. 移植 `NativeHidInput` + `HidInputProbe`，带 `offline-hid-input=1
     offline-hid-auto-home=<秒>` 启动；
  2. 或至少在 CDC 控制台增加一个“post Home HID event”的命令，复用同一套 IOHID 路径；
  3. 触控控制器（picasso 的触摸 IC）驱动是另一条更大的线，不作为第一步。

注意：`fastboot boot` 每次都会重置运行时状态，`sheng-lite` 写入的 `disabled.plist` 会随
Data 保留；去掉该 boot argument 再启动即可恢复原策略。

## 14. 2026-11-01 20:10–20:13：合成上滑解锁，**桌面已到达**

§13.4 的“本仓库未移植”作废：`NativeHidInput`/`HidInputProbe` 已移植并接入内核。

### 14.1 结果

hid10（kernel `7733b0f3…`、镜像 `d9965477…`，ADT/Data 与 hid9 相同，仅 kernel + args）
在 `t=540 s` 挂上合并 HID 设备，之后每 20 s 一次底边上滑：

```
PICASSO_HID probe at 540s
PICASSO_HID swipe armed sink=1 count=16
NativeHidInput:0x10000021a open by IOHIDEventDriver:0x10000021d
PICASSO_HID swipe 1 moved=0 reports=0          ← 事件驱动尚未挂上
PICASSO_HID event driver ready (classic IOHIDEventDriver below the device)
PICASSO_HID swipe 2 moved=1 reports=26
PICASSO_HID swipe 3 moved=1 reports=52 …
```

用户确认：**锁屏已解锁，桌面可见**。每次手势 26 帧（25 帧移动 + 1 帧抬起）全部提交。

### 14.2 三个已证伪/已修正的坑

- **单独的 digitizer 设备（`sheng-touch-split=1` 路径）在 picasso 上不会被匹配**。
  hid9：`touch started=1`，但整段日志只有一次 `open by IOHIDEventDriver`，6 次滑动全部
  `moved=0`。改用上游验证过的合并设备：键盘设备带 `sheng-touch-hid=1`，digitizer 在同一
  描述符里，由它注册 sink。
- **`native_touch_set_sink` 的 weak 空定义会吞掉回调**。驱动已经没有轮询 seqlock 的定时器，
  sink 存不进就永远没人调用 `touchSend`。探针必须提供强定义（保存 sink 并在每帧发布后调用）。
- **在探针里同步跑手势会阻塞 boot monitor**。hid9 在 `t=420 s` 阻塞约 2.5 min，面板交接/UI
  trace/进程快照全部停摆，到 `t≈690 s` 仍无锁屏；hid7（同一内核、不阻塞）`t≈500 s` 就有。
  现在探针只“布置计划”并立即返回，boot monitor 每轮调用 `native_hid_swipe_tick(now)`，
  到点执行**一个**约 0.4 s 的手势。

### 14.3 构建树陷阱

`out/picasso/tree/drivers/*` 是指向工作区的软链，但 `out/picasso/tree/package` 指向
`.cache/upstream-…/package` 的**缓存副本**。改了 `package/HidInputProbe.cpp` 必须显式
`cp` 到 `out/picasso/tree/package/`，否则编出来的内核里还是旧探针（hid7 就白跑了一轮）。
验证方法：`strings kernel.macho | grep "PICASSO_HID swipe"`。

### 14.4 锁屏之后

- 滑动计划共 16 次（到 `t≈860 s` 为止），解锁后继续的滑动会打开 App Switcher；这是计划内
  的副作用，不是故障。
- 真实触摸硬件（Novotek NT36672C）仍未接：缺 `novatek_nt36672c_g7a_fw0X.bin` 固件
  （只在 MIUI vendor 分区里）。已从 DTS 取到全部板级参数：QUPv3_1 wrapper `0x9c0000`、
  SE7 `0x984000`、SPI GPIO 6/7/8+CS 9（qup11）、IRQ GPIO5、RESET GPIO4、GIC 354、
  GCC se-clk `0x4b`、apps SMMU `0x15000000`、矩阵 18×36、SPI ≤5 MHz。

## 15. 2026-11-01 20:31–20:45：SMP 打开，8 核启动

### 15.1 ADT 早已声明 8 核

`out/*/payload.img` 里的 `kernel.macho.adt` 解码后已有 `cpu0..cpu7`（reg 0x0..0x700）
和 `/psci`（method smc）。只跑一颗核的原因只是 **`sheng-smp` 没打开**：`ArmCpu::probe`
对次核返回 nullptr，除非 `sheng-smp=1`。因此 SMP 是**纯 boot args 实验**，kernel/ADT/Data
都不需要改。

参数（993 B，仍 < 1000）：
`cpus=8 sheng-smp=1 armcpu-spi-ipi=1 armcpu-self-ipi=1`。

### 15.2 第一轮（hid11，kernel `3f902cc7…`）：3 个从核

```
PICASSO_SMP req=1 run=3 cond=1 c1=s5!f2/rc0 c2=s70/rc0 c3=s5!f2/rc0 c4=s70/rc0
             c5=s5!f2/rc0 c6=s70/rc0 c7=s5!f2/rc0
```

偶数核（0x200/0x400/0x600）进入 step7 运行，**0x600 是 A76 大核**；奇数核在
`initCPU` 的 `wakeRedistributor` 处 fail=2。隔一个找一个帧 = GICR 帧步长算错：
旧代码按 `GICR_TYPER.VLPIS` 选择 0x20000/0x40000 并在 Last 位提前停止。

### 15.3 修复：固定 0x20000 扫描整片 GICR

`drivers/ArmCpu/ArmCpu.cpp` 两处 walk（`wakeRedistributor` 与 `armcpu_gic_diag`）改为
每 0x20000 扫描，不再依赖 DT stride 或 VLPI/Last 位。

hid12（kernel `5e7ea7f3…`，镜像 `333ff5c5…`，args 与 hid11 相同）：

```
PICASSO_SMP req=1 run=7 cond=1 c1=s70/rc0 c2=s70/rc0 c3=s70/rc0 c4=s70/rc0
             c5=s70/rc0 c6=s70/rc0 c7=s70/rc0
```

**7 个从核全部运行（8 核在线），无失败。**

### 15.4 启动时间对照（主机时钟，fastboot boot 时刻为基准）

| 镜像 | 核 | Early boot complete | backboardd | 锁屏 |
|---|---|---|---|---|
| hid10 | 1 | ~100 s | — | ~500 s |
| hid11 | 3 从核 | ~100 s | ~107 s | ~350 s |
| hid12 | 7 从核 | ~65 s | ~106 s | ~310 s（用户观察） |

前期内核/UFS 阶段基本单线程，收益主要在 userland/UI；3 核已把锁屏从 ~500 s 拉到 ~350 s，
8 核用于进一步缩短 UI 阶段。

注意：launchd 的 wall-clock 时间戳基数随 ADT 随机种子变化，**不能跨启动比较绝对值**，
只能用同一启动内的差值或主机时钟。

### 15.5 构建树被 main 的新 USB 补丁弄坏

`git rebase` 到最新 main 后，`patches/picasso-usb-console-xnu.patch`（来自 8ba8517）在
control-window 改造后留下孤立的 `settle_ms` 引用，`picasso_usb.c` 编译失败；同时
`out/picasso/tree/.cache/xnu-work` 里残留的 `link.filelist` 与缺失的 `picasso_usb.o`
让 corecrypto 归档步骤也失败。临时处理：

1. 用分支 `e63ea4b` 上已验证的 patch 覆盖 `patches/picasso-usb-console-xnu.patch`；
2. 把已验证的 hid10 kernel 放到
   `out/picasso/tree/build/xnu/checkpoints/native-4k-user-abi/kernel.macho`
   供 `xnu_symbols.kernel_names()` 使用（构建后删除）。

这个上游 bug 值得单独修：main 的板级构建目前是坏的。
