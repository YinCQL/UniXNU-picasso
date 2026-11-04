# Handoff 2026-11-04 — status bar, apps, GPU (Picasso / SM7250)

Read `docs/handoff-20261103-gpu-render-next.md` first for the older history; this file is the current
state at the end of the 2026-11-04 session. All commands run from the worktree
`/Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog`.

## TL;DR

- **Correction, 2026-11-04**: the user now confirms that Apps display normally in the current
  image; the red status bar remains. This supersedes the intermediate report that the current
  App display was abnormal. Do not claim that every App was launch-tested. `found=6ef` proves
  only nine path lookups, not LaunchServices registration or successful App launches. The old
  probe guessed three wrong names: use `MobileNotes.app`, `MobileSlideShow.app`, `AppleTV.app`.
  Staged copying did complete in wf177 (`apps=2/6574/4214/445/0`, stage/dirs/files/MiB/error);
  wf177-recheck reports the completion marker (`apps=3/0/0/0/0`). The same persistent Data is used
  later, so absence of `sheng-staged-apps=1` alone does not prove missing source bundles.
- **Apps investigation**: the current bindfs lacked `vfs_vget` and advertised neither persistent
  object IDs nor paths from IDs. UniXNU-sheng already implements these for LaunchServices and
  documents an additional stale LaunchServices cache case ("only Settings"). A scoped backport is
  in `patches/picasso-bindfs-fileid-xnu.patch`; wf219 device validation gives `found=fff fileid=fff`
  (all 12 `Info.plist` round trips). wf220 then removed only the active LaunchServices cache via
  `patches/picasso-ls-rebuild-xnu.patch`; the user now reports normal App display. The sequence
  makes stale cache a plausible contributor, but does not prove causation or every App launch.
- **Status bar**: still red while Apps display normally. It is exactly `_UIStatusBarVisualProvider_Fallback`'s own background
  (`systemRedColor` at 75% alpha = the sampled `ffbf342b`). UIKit only reads the provider preference when
  `-[UIScreen _isEmbeddedScreen]` is YES, and the injected library that was supposed to force that **never
  produced execution evidence** — dyld opens and AMFI-validates it, but no constructor, no `+load`, no
  marker has been observed. The earlier `__init_offsets` explanation is false: the actual dyld-1165.3
  source handles `S_INIT_FUNC_OFFSETS` for initializers.
- **GPU**: with a *published* addon the Metal plugin really runs (`submissions=304 ios=2790 opens=95`) but
  every GPU fence hangs (`hang_fence=305`) and the render server switches scanout to the compressed
  `&BGA` surface (`0x26424741`) that the CPU display path rejects (`accept=0 reject=3013 reason 4`).
  backboardd then respawns, runningboardd kills SpringBoard, no desktop. With an unpublished addon the
  CPU path presents frames normally (193+ frames/11 min, no SIGBUS).

## The red bar, in detail

- `-[_UIStatusBarVisualProvider_Fallback setupInContainerView:]` (UIKitCore `0x18ae97100`) calls
  `+[UIColor systemRedColor]`, `colorWithAlphaComponent:0.75`, `setBackgroundColor:`. That is the band.
- `__UIStatusBarGetVisualProviderClassForScreen` (`0x18a10c360`) reads
  `UIStatusBarVisualProviderClassName` **only when** `_isEmbeddedScreen` (call at `0x18a10c3ac`, return
  site `0x18a10c3b0`) answers YES; otherwise it logs rdar://45025538 and takes the geometry table, which
  yields Fallback on this device.
- `-[UIScreen _isEmbeddedScreen]` (`0x18a00b1a8`) = `byte(self+0xcd) && !-[UIScreen _isExternal]`. The
  byte is written in `-[UIScreen initWithDisplayConfiguration:forceMainScreen:]` (`0x18a2bcbe4`) as
  `forceMainScreen || [displayConfiguration isMainDisplay]`. `_isExternal` (`0x18a00b21c`) asks the
  FBSDisplay at `self+0xa0` and treats nil as external.
- The internal preference at `0x1f48cd270` can only force **NO**, never YES.
- The kernel writes `UIStatusBarVisualProviderClassName=_UIStatusBarVisualProvider_Pad_ForcedNoCell`
  every boot (`PICASSO_UI_PREFS provider=Pad_ForcedNoCell plist=0/0 dylib=0/0`), and that class exists in
  21H461. Embedded/external classification still needs runtime evidence; this is not proof that forcing
  this one answer is sufficient for a correct layout.
- Additional 21H461 disassembly: FBSDisplayConfiguration delegates `isExternal`/`isMainDisplay` to its
  identity. `FBSDisplayIdentity.isMainDisplay` is `type == 0` (`0x19f3c3010`); configuration construction
  selects type 0 when `CADisplay.displayId == 1` (`0x19f3cf784` onward), and copies `CADisplay.isExternal`
  into the identity (`0x19f3cf83c`). The display driver already overrides `isMainDisplay()` with `primary`.
  Trace the CADisplay ID/external path before inventing another display identity mechanism.
- QuartzCore `CADisplay.displayId` (`0x189571e18`) reads `CA::Display::Display+0x68`;
  `displayType` (`0x1895506b4`) reads `+0x190`; `isExternal` (`0x18956bd10`) updates the
  display if dirty, then returns true for type 1 or 2. Disassembly is archived in the research
  directory. None of these private object values has been sampled on device yet.

## Why the injected library has no observed execution (still unresolved)

- `DYLD_INSERT_LIBRARIES` is injected by `patches/picasso-springboard-inject-xnu.patch`; the AMFI policy
  hook sets `AMFI_DYLD_OUTPUT_ALLOW_PATH_VARS` (`PICASSO_INJECT dyld policy pid=27 old=69 new=6b`).
- The kernel probes prove dyld **opens** `/private/var/mobile/Library/picasso/libpicasso-sb.dylib` from
  SpringBoard (pid 27) and PosterBoard, and that `mac_vnode_check_signature` **accepts** it
  (`PICASSO_INJECT signature error=0 flags=24000003 signer=0 platform=2`). The ad-hoc dylib is trusted via
  the owner trust bundle, no `amfi_allow_any_signature` needed.
- Nevertheless no code from the library ever executes:
  - `/dev/console` ack line: never.
  - `kern.picasso_inject_ack` sysctl line: never (could be sandbox-denied, but see next).
  - Kernel-visible marker opens (`/private/var/ngpu/picasso-marker-*`): never, in every build
    (wf213/214/215/216/217/218).
  - **Trap probe wf216**: the constructor's first instruction was `__builtin_trap()`; SpringBoard ran
    normally, no `PICASSO_EL0`/breakpoint record. The initializer did not run.
- **Retracted hypothesis**: `__TEXT,__init_offsets` is not evidence of an unsupported initializer format.
  The root's dyld identifies itself as dyld-1165.3; Apple's matching `common/MachOAnalyzer.cpp`
  (`forEachInitializer`, around line 2437) explicitly handles `S_INIT_FUNC_OFFSETS`, and
  `dyld/Loader.cpp` (`findAndRunAllInitializers`, around line 2184) calls the enumerated initializers.
  Sources are archived under `out/research/statusbar-apps-20261104/dyld-*`.
- dyld, SpringBoard, extracted UIKitCore and the probe dylib are ARM64 ALL; the earlier PAC/arm64e
  explanation is unsupported for this image.
- AMFI policy `0x6b` includes `ALLOW_FAILED_LIBRARY_INSERTION` (`0x20`); failed insertion can therefore be
  ignored without halting SpringBoard (Apple `dyldMain.cpp`, around line 635). Open + signature success
  do not prove a completed mapping. Capture the real loader error if pursuing this path.
  `DYLD_PRINT_TO_FILE` needs both print (`0x10`) and custom-cache (`0x4`) permissions in matching
  `DyldProcessConfig.cpp`, around line 889; neither is present in `0x6b`. Avoid globally granting all
  policy bits just for a diagnostic.

### Shared-cache bypass proposal (unimplemented; deprioritized)

Patch the shared cache in physical memory from the kernel, at boot:
- Target instruction: `0x18a10c3c4` `cbz w21, 0x18a10c448` (`34000435`). Replace it with `nop`
  (`0xd503201f`). Execution then always falls into the embedded branch at `0x18a10c3c8`, which reads
  `UIStatusBarVisualProviderClassName` (already written every boot) and returns Pad_ForcedNoCell.
- This changes provider selection for any caller of the shared function. Other-screen behavior and
  layout correctness have not been tested; the previous claim that other callers are unchanged was
  too strong. Prefer the existing display classification/provider mechanisms before this proposal.
- Ingredients already present: `current_task()->task_shared_region_slide` (used by the fault-dump patch),
  `ml_phys_read_data`/`ml_phys_write_data` on arm64 (`osfmk/arm64/loose_ends.c`), `pmap_extract` for the
  VA→PA translation, and an icache flush (`invalidate_icache`/`flush_icache_range`). Do it once, in a
  kernel thread after launchd is up, using any task that maps the shared cache (launchd's task works),
  runtime VA = 0x18a10c3c4 + slide. These are ingredients, not a validated patch procedure.
- Risk: 4 bytes of shared-cache text; a wrong address crashes every process. Verify the bytes
  (`34000435`) before writing, and log old/new values. A coarse snapshot's non-red top row alone is
  insufficient: the current snapshot contains gray (`ffbfbfbf`) and large black areas while the user
  reports an abnormal UI. Check the actual status bar, full Home Screen, and App opening.

## Community implementation to reuse for Apps

- Reference: `UniXNU/UniXNU-sheng@965460a9ac1715636c769634fd0d69f58b2bd0f9`,
  `patches/xnu-12377/zzzz-sheng-ipados27-bringup.patch` (downloaded under the research directory).
  The local `../../UniXNU/.cache/reference-sheng` is older (`28b3028`, 2026-11-01).
- Its bindfs diff keeps the lower filesystem's persistent-ID/path-from-ID capabilities, sets
  `MNTK_PATH_FROM_ID`, and implements `vfs_vget`: lower `VFS_VGET` -> path relative to bound root ->
  lookup from bindfs root. IDs outside the bound tree return ENOENT. This is a candidate explanation
  for incomplete registration on Picasso, not a proven device root cause.
- It also dynamically enumerates InternalDaemon/System data containers for `com.apple.LaunchServices-*`
  caches before launchd/lsd. A fixed UUID failed in their earlier implementation. Picasso's wf217b
  opened `.../InternalDaemon/F355BEEE-2ECA-4173-AE4B-D7EA3BC76C32/Library/Caches/`
  `com.apple.LaunchServices-5028-v2.csstore`. Fix and validate file-ID lookup before testing a controlled
  cache rebuild; do not reset all Data or alter the seed hash.
- Their `owner-staged-apps=1` mounts the sealed OS staging directory before Data hides `/private/var`.
  Picasso already has successful copy evidence, so replacing its entire copy/mount path is not the
  first change.
- Required validation: capability flags, bundle file ID + volume -> `VFS_VGET` / `fsgetpath_internal`
  round trip to `/Applications/...`, then LaunchServices registration and actual screen/App behavior.
- Layout expectation must come from this root's app list and `DefaultIconState*.plist`: 168 root
  bundles include services; they are not 168 icons. The phone layout lists Phone/Compass/Calculator/
  Passbook/Bridge absent from this iPadOS root. News is explicitly renamed `.app.hidden` by the current
  Picasso patch; its old crash rationale needs rechecking before restoration.

## GPU state and the next GPU step

- Kernel publishes the staged AGXAccelerator (`PICASSO_GPU cp: metal published`), and with the published
  addon (`make-gpu-addon.py --publish --clients '*'`) the Metal plugin loads in backboardd
  (`opens=95`, `submissions=304`, `ibs=2790`) — this is the first configuration in this session where the
  plugin is really used.
- Every fence hangs (`hang_fence=305`) and the render server then allocates `'&BGA'` (`0x26424741`)
  scanout surfaces; `native_legacy_swap_validation_observed` rejects all of them (reason 4) because
  `picasso_gop_format()` only accepts linear BGRA (`0x42475241`, planes 0). With no accepted swap the
  render server stops presenting, backboardd dies, runningboardd kills SpringBoard.
- Next GPU change: in GPU mode accept `'&BGA'` (and let `native_legacy_gpu_offer` hand it to the plugin;
  the CPU copy path must keep refusing it), or fix why the GPU fences never retire (Turnip/GMEM path —
  the older `docs/handoff-20261102-gpu-first-submit-stall.md` has the wf81/wf82 history: the GMEM path
  hung, `TU_DEBUG=sysmem` avoided it).
- **Stale addon markers**: `addons/NativeAdrenoMetal.publish/.clients` live in the persistent Data volume.
  wf214 installed them; wf215/wf216 used unpublished addons but still ran the production path. The new
  kernel patch `patches/picasso-addon-marker-sync-xnu.patch` unlinks a marker the current addon does not
  carry.

## Tooling added this session

- `scripts/build-sb-trust-bundle.py OWNER.bundle LIB.dylib OUTPUT.bundle` — deterministic owner trust
  module; the dylib CDHash must be regenerated after every dylib rebuild (verified byte-identical against
  the earlier hand-built bundle).
- `patches/picasso-inject-open-probe-xnu.patch` — logs the namei path copy of every `OP_OPEN` whose basename
  is `libpicasso-sb.dylib` or starts with `picasso-marker-`, with pid/process name.
- `patches/picasso-inject-signature-probe-xnu.patch` — logs AMFI's `mac_vnode_check_signature` verdict for
  that library in SpringBoard/PosterBoard.
- `patches/picasso-apps-view-probe-xnu.patch` — now checks twelve real `Info.plist` paths and uses
  the `/Applications` mount ID + file IDs with `fsgetpath_internal`. Logs `PICASSO_APPS_IDCAP`,
  one `PICASSO_APPS_FILEID` per bundle, and `found=<bits> fileid=<bits>`. Both masks should be `fff`.
- `patches/picasso-cdc-snapshot-xnu.patch` — CDC command `snapshot`: 24×48 luma text image plus the exact
  BGRX of six columns for the top three rows. Send with
  `python3 scripts/picasso-cdc-command.py --command snapshot --output out/sb-runs/snap.log`. This is the
  safe "screenshot" for this board (the giulia GSC1 binary protocol is not compiled in and adding it was
  considered too risky for the only control channel).
- `patches/picasso-addon-marker-sync-xnu.patch` — see above.
- Everything is wired into `unixnu` (fingerprint + z-order 32..37). Kernel builds need
  `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer` and
  `UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed`.
  If the kernel build fails on `genassym.o ... not recognized as a valid object file`, run
  `rm -rf out/picasso/tree/build/xnu` and rebuild.

## Images on disk (out/)

| Image | Kernel | Addon | What it is |
|---|---|---|---|
| `wf219` | `568a0ce99b468f4f…` | same unpublished + `+load` as wf217 | bindfs community backport; 12 path/file-ID checks pass; red bar persists |
| `wf220` | `30109663af0b2d3b…` | unpublished | active LaunchServices cache rebuilt; user reports Apps now display normally; red bar persists |
| `wf221` | `30109663af0b2d3b…` | `ngpu-addon-sb-wf221-4k.bin` | SpringBoard probe signed at 4096-byte pages (wf217 binary was 16384); not yet booted or device-verified |
| `wf217` | `5f5a51bd9e6262ef…` | unpublished + `+load` dylib | CPU-path comparison image; user reports abnormal App display; completeness unverified |
| `wf215` | `316bdac3…` | unpublished + marker dylib | previous good UI (apps confirmed by user); no marker-sync (GPU stale publish) |
| `wf214` | `316bdac3…` | published | snapshot command; GPU wedges the UI |
| `wf216` | `316bdac3…` | trap dylib | constructor-trap probe (no trap → not initialized) |
| `wf218` | `5f5a51bd…` | unpublished + `+load` | `amfi_get_out_of_my_way=1` probe; apps disappeared — do not reuse |
| `wf207` | `bf9b1138…` | published (`clients=backboardd`) | first published-addon GPU image |

Logs: `out/sb-runs/wfNNN.log`, reboot logs `out/sb-runs/wfNNN-reboot.log`, kernel builds
`out/build-kernel-wfNNN.log`.

## Hard constraints (unchanged)

- `fastboot boot` only; never flash partitions. One boot at a time.
- CDC serial `/dev/cu.usbmodemPICASSO011` is exclusive: stop the `cat` capture before
  `scripts/picasso-cdc-command.py`, and restart it afterwards.
- Kernel command line ≤ 1000 bytes (`scripts/unixnu/payload.py` checks the joined string).
- Never remove `boot-progress=2`; never set `sheng-data-persist=0`; never remove `offline-hid-home=1`.
- Do not reset/checkout the shared worktree; do not run commands from the original repo root.
- The phone may take 2–10 minutes to re-enumerate after `reboot-fastboot`; do not assume it is lost.
- `offline-hid-swipe=N` is stripped by `scripts/build-wf-payload.sh`; pass it as an extra argument to
  re-enable the virtual bottom-edge swipe (needs ~21 bytes of command line).
- Battery: a full boot + UI run drains fast; charge before starting.
- `picasso-aks-state=1` has a startup dependency; keep it. `build-wf-payload.sh` now preserves it
  under both `TRIM_ARGS=1` and `SB_INJECT=1` (the earlier "state-only diagnostic" comment was wrong).

## Prompt for the next agent (copy this)

你接手的是 UniXNU 在 Picasso（SM7250 / Adreno 620，iPadOS 17.7.11 21H461，iPhone13,1 身份）上的移植。
工作目录 `/Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog`；所有硬约束见本文件
“Hard constraints”。

先修正结论：**用户确认当前 App 显示正常，红条仍存在。** wf219 的 bindfs 文件 ID 回查通过；
wf220 只删除了活动 LaunchServices cache，随后用户确认 App 显示恢复正常。缓存重建可能是原因之一，
但目前没有隔离变量证明因果，也没有逐个实测 App 启动。不要清 Data、重置 seed 或扩大缓存清理。

状态栏红条来自 Fallback provider 的红色背景，provider preference 已写入；但 display ID/external/main
classification 链尚未定位。实际 dyld-1165.3 支持 `__init_offsets`，所以“普通插入 dylib 不处理该节”
是错误解释。open/signature 成功而没有构造函数、`+load`、marker 证据，仍需抓 dyld 的实际失败原因。
wf221 是为诊断签名页大小构建的候选镜像，显式以 4096 字节 pagesize 重签注入 dylib；尚未设备验证。
共享缓存 `cbz` 改 `nop` 只是未验证候选，不要直接当成已准备好的修复。

优先级：

1. 保持现有 App 正常显示状态；不要再做全量缓存或 Data 清理。把 wf220 缓存重建记录为可能相关因素，
   只有出现回归证据时再进一步验证 LaunchServices。
2. 先跟踪 `CADisplay.displayId`、`isExternal` 到 `FBSDisplayIdentity` 的输入。Picasso 驱动已有
   `isMainDisplay() { return primary; }`，确认该属性如何进入 FrontBoard 的 CADisplay，再决定低代价修复。
3. 如需继续查注入，先用 wf221（显式 4096-byte code-sign page size）验证 dyld 是否运行 dylib；
   如果仍无执行证据，再抓具体 dyld 加载错误。不要仅凭 open/signature 成功判断映射成功。
4. 不要先写共享缓存物理补丁。published GPU addon 的 fence hang 与 `&BGA` 拒收是独立问题，当前保留
   unpublished CPU 显示路径。

本次只读查询确认设备在 fastboot（序列号 `52cc791d`，电压 `3840mV`，SOC OK），不是正在运行 wf217。
只用 `fastboot boot`，绝不 flash；CDC `/dev/cu.usbmodemPICASSO011` 采集与命令发送互斥。
保留 `boot-progress=2`、`offline-hid-home=1`、Data 持久化设置及 `picasso-aks-state=1`；禁止
`amfi_get_out_of_my_way=1`。`TRIM_ARGS=1`/`SB_INJECT=1` 现在会保留 AKS 参数。payload cmdline <=1000 字节。
开机顺序：`fastboot boot out/<tag>/picasso/boot/<tag>.img` -> 等 CDC -> 单独抓日志；需要发命令时先停采集，
用 `python3 scripts/picasso-cdc-command.py --command snapshot|status|reboot-fastboot --output <log>`。

构建：

```sh
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed \
python3 unixnu build kernel prelink
```

## Latest checked state (2026-11-04)

The source-only investigation ended with fastboot present. Subsequent work built and booted wf219:

- Build: `out/build-kernel-wf219.log`, kernel SHA-256
  `568a0ce99b468f4f785f7c275a82b6d4fcc3f5dc21fe7dd28846aed2380c54c8`.
- Payload: same seed/addon/trust hashes and same argument set as wf217; AKS argument moved in
  ordering but was kept. Command line 986 bytes. Build:
  `IPHONE_IDENTITY=1 SB_INJECT=1 SB_INJECT_NO_AMFI=1 OWNER_TRUST=build/owner-trust-sb-load.bundle
  bash scripts/build-wf-payload.sh wf219 build/ngpu-addon-sb-wf217.bin offline-hid-swipe=16`.
- `out/sb-runs/wf219.log:904` onward: bindfs `format=9b6edf valid=7ffffff`; all twelve
  `Info.plist` lookups and file-ID -> path round trips succeed (`found=fff fileid=fff`);
  applications union stage 5/status 0, Data stage 4/error 0.
- `out/sb-runs/wf219-snapshot.log`: red top band still `ffbf342b`. Coarse icon-like changes
  do not establish complete Home Screen or working App launches. No full-resolution screen or
  LaunchServices registration inventory has been captured.
- wf220 adds opt-in `picasso-ls-rebuild=1`: reuses Sheng's dynamic container search and only
  runs before launchd when mount status and all 12 file-ID probes pass. No seed reset. Full kernel
  build/prelink passed; kernel SHA-256
  `30109663af0b2d3b1cd527c8a1213d931b7da59a8661c858740c1e7ab6061701`.
- `out/sb-runs/wf220.log:923-924`: only the active LaunchServices csstore was removed,
  `removed=1 looked=20 failed=0`. The display subsequently presented frames
  (`nonblack=198346` through `214653`) with no crash observed in the captured window.
- `out/sb-runs/wf220-snapshot-late.log`: two identical late snapshots show substantially denser
  icon-like regions than wf219; `PICASSO_SNAPSHOT_RGB 01` remains `ffbf342b`. This demonstrates
  visible improvement after the cache rebuild. The user subsequently confirmed that Apps display
  normally in the current image; the red bar remains. This does not establish that every App launches.
  The first wf220 snapshot was taken before its first compositor frame and is not a UI result.
- `out/research/statusbar-apps-20261104/app-inventory.json`: 166 root bundles with readable
  `Info.plist` and 29 staged bundles. The root services are not an icon count. Most visible/staged
  Apps support `[1,2]`; a blanket "iPad-only apps cannot run on the phone identity" is not supported.
- wf221 diagnostic image is built at `out/wf221/picasso/boot/wf221.img`; kernel SHA-256
  `30109663af0b2d3b1cd527c8a1213d931b7da59a8661c858740c1e7ab6061701`, addon SHA-256
  `b7ce0c48b8cba026f56d6076194b96bb479a5270d10a633258f2a14773cc43da`, trust bundle SHA-256
  `a1a82b66cf8c06c69462aa7739c1784138cd6fe158359e9f6babbb5f8a4dd6e0`. Payload command line is
  964 bytes and retains `boot-progress=2`, `offline-hid-home=1`, `sheng-data-persist=2`,
  `picasso-aks-state=1`, and `offline-hid-swipe=16`. Extracting the addon entries and inspecting their
  signatures confirms wf217 used 16384-byte code pages and wf221 uses 4096; the probe binary also
  changed size from 70352 to 70736 bytes, so this is not a strict one-variable A/B. It has not yet
  been booted.
- At the latest host check, `picasso XNU USB console` is enumerated, the CDC serial device has no
  open process, and `fastboot devices -l` is empty; the device is still in UniXNU userspace. Next,
  send `reboot-fastboot`, then use only `fastboot boot out/wf221/picasso/boot/wf221.img`.

The earlier 11:17 CST "wf217 UI up" entry was historical and did not validate App completeness.
Do not carry its "App normal" conclusion forward.

## 2026-11-04 later — the GPU presentation fix (wf222/wf225/wf227)

- **Root cause of the published-addon black screen**: format `0x26424741` is `'&BGA'` =
  `kCVPixelFormatType_Lossless_32BGRA`, Apple's lossless block compression, not plain rows.
  1080x2340 pads to 1088x2368 tiles and 1088*2368*4 + 65536 = 10,371,072, exactly the
  `allocation=` in the reject log. Treating it as linear BGRA is wrong. (`CVBuffer.h` documents
  fixed-size blocks and a metadata area; do not decode on the CPU.)
- **Fix**: with a published Metal device the render server reads the CA compression preferences at
  process start, so they must be on disk before backboardd. New kernel patch
  `patches/picasso-ca-prefs-xnu.patch` (registered in `unixnu`, z-order after ls-rebuild) writes
  `CAEnableDeepFramebuffer`, `CAEnableFramebufferCompression`, `CAEnableIOSurfaceCompression` = false to
  `/private/var/preferences/.GlobalPreferences.plist` and
  `/private/var/mobile/Library/Preferences/backboardd.plist` from `native_owner_data_prepare`
  (right after the UI provider pref). It logs `PICASSO_CA_PREFS mobile=0 system=0`.
  This mirrors `UniXNU/scripts/add-gpu-plugin-data.py` and the vphone-cli iOS 17.7 bring-up.
  Runtime `CFPreferencesSetAppValue`/`setenv` inside the plugin is too late (the worktree images never
  seeded the prefs before).
- **Publisher scope matters**: `--publish --clients '*'` makes mediaserverd open the plugin and abort
  repeatedly (`PICASSO_SERVICE ... mediaserverd exit_signal=6`). `--clients backboardd` alone freezes
  the UI because SpringBoard's scanout client gets no device (`PICASSO_GPU text: PF 1/0` then no swaps).
  `--clients backboardd,SpringBoard` is the working set (wf224 addon `ngpu-addon-publish-bbd-sb.bin`).
- **Measured (phone identity, iPhone13,1, scale=3, NO_FBGPU, wf225)**: submit=530 accept=530 reject=0,
  copies=530, GPU submissions=539 ibs=8516, fence advancing, no hang/fault/panic.
- **wf227** (`IPHONE_IDENTITY=1 GRID=0 NO_FBGPU=1 UNIQUE_CHIP_ID=01dec0dec051a543`, addon
  bbd+SpringBoard, args `picasso-ls-rebuild=1 sheng-staged-apps=1`, cmdline 990 B): desktop and the
  staged apps are back; submit=1024 accept=1024 reject=0, scans=5 nonblack=5, GPU submissions=784,
  fence 608, no panic; only the known mobiletimerd SIGKILL (exit 9) recurs.
- **Identity lesson**: `IPHONE_IDENTITY=1` must be passed every time; without it the ADT reverts to
  iPad7,1 (wf222/223/224) and the red status bar disappears for the wrong reason (geometry matches the
  Pad provider). The `&BGA` fix is independent of identity, but geometry/CA surface choice is not, so
  re-check reject=0 after any identity or display-scale change.
- **Next**: bake permanent boot arguments into code defaults to free command-line bytes (audit in
  progress), then re-verify identity/apps/reject=0. GPU scanout for the phone canvas is still the CPU
  letterbox copy because `native_legacy_gpu_offer` requires sourceHeight == panel height (2340 vs 2400);
  extending the presenter with a row offset would be the next GPU win.
- **Command-line baking (wf228)**: the permanent board arguments moved into driver defaults, verified
  by wf228 (`submit=256 accept=256 reject=0`, GPU `submissions=113`, HID probe at 60 s, NVT stage 4,
  CPUFREQ mode 4, no panic/service exit):
  `sheng-cpufreq=4` (CpuFreqProbe), `sheng-gpu=4` (Adreno620Probe), `sheng-gpu-metal=1`
  (Adreno620Cp), `sheng-touch-hid=1` (NativeHidInput), `sheng-touch=4` (TouchProbe),
  `sheng-touch-start=60` and `offline-hid-start=60` (BootProgress), `sheng-disable-jobs` default list
  (BootWatchdogPolicy), `sheng-vsync=60` (NativeLegacyDisplay::startVsync), and
  `offline-legacy-{gop,pm,phase,validate,transfer,cpu-present}` (NativeLegacyDisplay).
  Kept on the command line because a second consumer reads them: `sheng-gpu-addon=1` (kernel addon
  mount), `offline-legacy-register=1` (offline-component-load-probes patch), plus the injection,
  LS-rebuild, staged-apps, swipe and identity/experiment arguments. board.toml lost the duplicated
  `offline-hid-start=540` and the baked display/vsync rows. Command line: 990 -> 645 bytes.
- **Merged tree (wf229)**: after merging `fork/stage12/picasso-cpufreq-big` (the other session's
  touch/Data work), the same kernel + addon boots to the desktop with the fixes intact:
  identity iPhone13,1, CA prefs, GPU metal published, HID probe at 60 s, NVT stage 4 touch reports,
  `submit=46 accept=46 reject=0` growing, fence advancing, no panic/service exit. First compositor
  swap arrives about 6.5 minutes after boot in both wf228 and wf229, so do not treat an early empty
  display counter as a regression. The merged HID stack reports
  `swipe unavailable started=1 digitizer=0 sink=1`, so the offline auto-swipe no longer unlocks;
  real touch still works. Branch pushed as 41202dd; PR #13 has the evidence comment.
- **Apps and the vnode table (wf230..wf234)**: the staged bundles and the copy marker persist across boots
  (probe `PICASSO_PERSIST marker size=0`, `staged size=4598`), but
  `/private/var/mobile/Library/MobileInstallation/MobileInstallation.plist` never exists (`err=2`), so lsd
  needs the `picasso-ls-rebuild=1` csstore drop (`removed=1`) to rescan /Applications on every boot.
  That rescan used to hit a **1024-entry vnode table**: XNU sizes it as `sane_size/64k + 1024`, and
  `sane_size` is still 0 when `unix_startup` runs on this port. The resulting vnode jetsam killed
  backboardd/SpringBoard (`Initiating vnode jetsam : 1024 desired`), and `-no_vnode_jetsam` turned that
  into a panic (`vnode table is full @vfs_subr.c:5937`). Fix: the ADT `/defaults` node now carries
  `kern.maxvnodes = 32768`, which `PE_get_default` reads before the memory-based fallback. wf234
  (ls-rebuild + staged-apps, no jetsam override): submit=384 accept=384 reject=0, copies=383,
  GPU submissions=385, **vnode jetsam=0, service exits=0, no panic**; the desktop and apps are back.
  The proper long-term fix is still to have installd maintain MobileInstallation.plist (or to keep the
  bindfs volume/file IDs stable) so the per-boot LaunchServices rescan can be dropped.
