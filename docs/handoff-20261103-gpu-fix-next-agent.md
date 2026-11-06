# Handoff: fix the Picasso GPU hang/freeze (next agent)

Date: 2026-11-03. Board: Xiaomi Picasso (SM7250 / Adreno 620 / Novatek NT36672C / 1080x2400), self-built XNU
(21H461 iPadOS root), booted only with `fastboot boot`.

This document is the state of the investigation and the concrete work list. Read it together with
`docs/handoff-20261103-gpu-render-next.md` (append-only log of every run: wf143..wf163) and
`docs/handoff-20261102-gpu-first-submit-stall.md` (earlier stall history).

## 1. The job

The system boots, the UI renders (phone layout), real touch works, swap is created after the Data volume mounts,
but the GPU eventually stops and the whole UI freezes with no recovery. Fix that.

Acceptance for this task:
- 30 continuous minutes of UI use (lock screen, home screen, Settings, app launch) with no freeze, no panic.
- After a freeze is intentionally/unintentionally hit, the kernel must recover the GPU on its own (restart hook)
  instead of staying frozen.
- Keep the existing behaviour: fastboot-only, CDC console exclusive, `boot-progress=2` present, cmdline <= 1000 bytes.

## 2. Verified facts (do not re-derive)

1. `perf index` is already raised to the DT nominal 3:
   `PICASSO_GPU hfi: initial perf index=3 (dt level=3 of 5, boot index=0 nominal)`.
   Boot-arg override `sheng-gpu-perf-index=1` works (verified on device).
2. Swap creation ordering is fixed: `vm_swap_create_file failed @ ~38s` (Data not mounted yet) then
   `vm_swap_create_file created @ 39s on /private/var/vm/swapfile0` after the Data mount wakes the create thread.
   Patch: `patches/picasso-swap-retry-xnu.patch` (commit 8a93b54), included by `unixnu`.
3. The freeze is not a BO lifetime bug. Evidence:
   - `NativeAdrenoVm::faultState` (drivers/NativeAdreno/NativeAdrenoVm.cpp:722) reads SMMU regs 0x58/0x60/0x64/0x68.
     The logged FSR is always 0x400, and 0x400 has **no bits under the kit's own FSR_FAULT mask 0xc00001fe**
     (kits/ArmSmmu4K/ArmSmmu4KProgram.c:3); 0x400 is only the AArch64 address-format bit
     (NativeAdrenoGpuContext.cpp:203-205). There is no latched translation fault.
   - The logged FAR 0x1338f508 cannot be trusted as a 64-bit address: the "high" word read from 0x64 gives values
     like 0x13c43, outside the 48-bit IAS. `handleAt(fault[1])` used only the low 32 bits while every client IOVA
     is >= GpuClientVaStart 0x100000000 (NativeAdrenoGpuContext.h:27), so it always returned 0. The old
     `PICASSO_GPU stall far=... handle=0` line was a false positive.
   - `unmap` refuses while `record->jobs` is nonzero; `submit` holds every handle named in the submit; the TLB
     invalidation on unmap is per-page TLBIVAL/ASID and sound.
4. `NativeAdrenoSubmit::hangCheck` (NativeAdrenoSubmit.cpp:983) never declared a hang during any freeze:
   no `PICASSO_GPU hang:` line in wf147c/wf153/wf155/wf159/wf161. Either (a) the 500 ms thread is starved of
   `g.lock` by the wait/stall path, (b) `madeProgress()` sees ROQ/prefetch movement and resets the retry counter,
   or (c) the completed fence keeps advancing between polls.
5. There is no board restart hook. `native_adreno_set_restart` (drivers/NativeAdreno/NativeAdrenoPublish.cpp:211)
   has no caller, so the hangcheck's else-branch only prints "no restart hook" and the CP is never reset.
   `NativeAdrenoSubmit::recover` (NativeAdrenoSubmit.cpp:1015) is complete and expects
   `NativeAdrenoRestart` = `IOReturn (*)(void *cookie, NativeAdrenoRing *ring, uint32_t completed)`
   (NativeAdrenoSubmit.h:22-24): reset the GPU, call `ring->restart(completed)`, then bring the CP up again.
6. The freeze happens in every GPU path, not only the Metal plugin:
   - wf147c: published plugin, perf1 -> frozen at submit 355, 6 `stall:` blocks, mobiletimerd SIGKILL.
   - wf159: **unpublished** (software UI) but with `sheng-fb-gpu=1` -> 1491 frames then frozen, transfer_fail=3,
     6 `stall:` blocks, same signature (`smmu 400 1338f508`, cpint=0 hwfault=0 protect=0 sqe=4000).
   - wf161: no handoff and unpublished, but the addon's `adrenoprobed` still runs `adrenoUserTest` at boot and
     produced hundreds of GPU submissions and 50 ms stalls; the UI (CPU copy) stayed alive but the GPU work stream
     still stalled.
7. Stall signature: every "stall" fires exactly at the 50 ms threshold, `cpFence = fence-1`, `rptr < wptr`, and the
   stall handler runs `vm_->invalidateTlb(); stalled->kick();` at stage 1 (50 ms) and `drainBus` at stage 3
   (200 ms). A fence that takes >50 ms is ordinary work (jobs last ~5 ms per earlier measurements); the handler is
   firing on slow frames, and its TLB invalidate runs while the job is still executing.
8. `sheng-gpu-unstick` was added (NativeAdrenoSubmit.cpp:53-61, bit 0 = TLB invalidate, bit 1 = WPTR kick,
   bit 2 = bus drain; default 7). wf155..wf161 ran with `=2` (kick only, no TLB invalidate). Stalls still occurred;
   the permanent hang still happened. So the TLB invalidate is not the sole trigger (but it is still suspect as an
   amplifier).

## 3. Ranked hypotheses for the permanent hang

H1 (leading): a CP-side hang with no SMMU fault and no hangcheck/recovery. The CP stops consuming (`rptr < wptr`,
`cpFence = fence-1`) and never restarts because (a) the board has no restart hook, and (b) hangCheck never fires
for reasons above. The primary fix is the restart hook + a reliable hang detector.

H2: the 50 ms unstick handler's TLB invalidation corrupts in-flight page walks and can turn a slow job into a hang.
Possible second fix: raise the thresholds (e.g. 250 ms / 1 s), or remove the TLB invalidate entirely.

H3: a specific userspace submission pattern (2D blit, `CP_INDIRECT_BUFFER` chains) intermittently wedges the CP.
The stall snapshots show the job waiting on a run of 2D BLIT packets; the `stall a2d` decode lines in
wf147c/wf153 logs are the evidence. If H1/H2 fixes make recovery reliable, this is only a trigger, not the bug.

## 4. Work plan

Step 0 (required reading): this doc, `docs/handoff-20261103-gpu-render-next.md`, the driver sources listed below,
and the upstream `docs/guide/mobilegestalt.md` (only for the layout context).

Step 1 - make the diagnostics truthful and bounded.
- In `noteStall` (NativeAdrenoSubmit.cpp:640-700), print the SMMU fault line only when
  `fault[0] & 0xc00001fe`; reconstruct FAR as 64 bits only after proving 0x64 is FAR-hi; bound to <=6 prints;
  include the FSR value and the job's vm_next. (An uncommitted false-positive print was already replaced in the
  worktree; verify it is the bounded version.)
- Add in `hangCheck`: counters (polls, hang_fence, last fence, progress_retries, last state) exported next to
  `native_adreno_recovery[]` and printed once per stuck second from the hangcheck thread. This answers whether the
  thread is starved (add an `IOLockTryLock` counter) or `madeProgress` keeps returning true.

Step 2 - implement the board restart hook (the real fix).
- Add `native_adreno620_cp_restart(cookie, ring, completed)` in
  `out/picasso/tree/drivers/NativeAdrenoPicasso/Adreno620Cp.cpp` (same file as `native_adreno620_cp_start`).
  Reusable pieces: `hardwareInit()` (Cp.cpp:502), `uploadSqe()` (637), `programCp()` (662), `cpInit()` (694),
  `leaveSecureMode()` (719 gate on the same `sheng-gpu-zap` arg), `native_adreno620_gmu_boot` (Gmu.cpp:1460),
  `native_adreno620_hfi_handshake` (Gmu.cpp:1544).
- The hook must: drain the bus (`ring->drainBus`), stop SQE, if GX is on re-run hardwareInit/uploadSqe/CP start
  without power-cycling, call `ring->restart(completed)` between the reset and the CP init (see the contract in
  NativeAdrenoSubmit.h:22), then probe with `submitNoop`/`waitFence` and return success only if the probe completes.
  If GX is off, add a `native_adreno620_gmu_restart()` that clears the one-shot `s.booted`/`s.hfiStarted` flags and
  re-runs gmu_boot + handshake + `native_adreno620_gmu_release_gpu_set`.
- Install it from `publishGpu()` (Adreno620Cp.cpp:1684 area) via `native_adreno_set_restart(hook, cookie)`.
- The existing `NativeAdrenoSubmit::recover` then handles fence bookkeeping, TTBR0 and replay of fences hung+1..last.

Step 3 - make the hang detector actually fire.
- In `hangCheck`, treat a fence that has not advanced for ~1 s as a hang regardless of `madeProgress`, or make
  `madeProgress` only count the completed fence (not ROQ/prefetch state). Verify with a forced test: add a debug
  boot arg that stalls the CP deliberately (e.g. submit a CP_WAIT_MEM on a never-written address from
  `adrenoUserTest`) and confirm `hang:` + restart + replay, with the UI resuming.

Step 4 - retune the unstick handler.
- With the restart hook in place, re-test `sheng-gpu-unstick=7/3/2/0` and pick the setting with the fewest stalls
  and no permanent hang. Consider `invalidateTlb` only when a fault is actually latched.

Step 5 - verify the benchmark story (goal 1 of the previous directive, not the GPU fix itself).
- The plugin now runs the shared self-test in the published path when `sheng-gpu-bench=1` is on the boot args
  (NativeAdrenoMetalPlugin.mm: startSelfTest + bootArgument). The test prints `T s...` and
  `B fill=..ms tri=..ms compute=..ms` through the driver's text stream (NativeTurnipSmoke.h keeps the timings).
- Images already built but never booted: `out/wf156` (bench + `sheng-gpu-perf-index=1`) and `out/wf157`
  (bench, default index 3). Boot each, capture, and compare the B row. Acceptance: perf3 fill and tri both at least
  50% shorter than perf1.
- Do not rebuild those payloads blindly: the addon binary must match the owner-trust bundle in
  `/Users/silenzio/Desktop/development/ios/UniXNU/build/adreno-metal-plugin/owner-trust.bundle`. Rebuild order is
  `python3 scripts/build-adreno-metal-plugin.py`, `python3 scripts/make-owner-trust-bundle.py`,
  `python3 scripts/make-gpu-addon.py [--publish --clients backboardd] <new file>`.

## 5. Current working configuration (for reference)

- Phone layout is working and user-confirmed ("看到 iOS 的界面") in wf163:
  ADT `model=iPhone10,3`, `target-type=D22`, `/product` name "iPhone X", idiom phone, subtype 2340, scale 3,
  `/chosen/display-scale=3`; root `compatible` unchanged; `sheng-display-scale=3`; letterbox 2340->2400 in
  NativeLegacyDisplay; `offline-hid-home=1` is REQUIRED for the HID digitizer (package/HidInputProbe.cpp creates the
  device only when sendHome==1; without it `PICASSO_HID ... meta=-1 object=0` and touch dies); `unique-chip-id`
  ...a543 invalidates the persistent MobileGestalt cache (persist=2 Data keeps stale answers keyed by platform UUID).
- wf163 runs with the unpublished (software) addon and `NO_FBGPU=1` (no `sheng-fb-gpu=1`) for a stable UI.
- `sheng-data-persist=0` is NOT a way to clear the gestalt cache: the packed Data provider refuses the volume
  (`disk1: device is not ready`, IOBlockStorageDriver start failures, APFS truncate loop).

## 6. Repo/build/run cheat sheet

- Worktree: `/Users/silenzio/Desktop/development/ios/worktrees/stage7-native-watchdog`.
- Kernel build (needs Xcode env):
  `export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed`
  `python3 unixnu build kernel prelink`
- Payload: `GRID=0 bash scripts/build-wf-payload.sh TAG ADDON [extra kernel args...]`
  Options: `NO_FBGPU=1`, `NO_HIDHOME=1` (DANGER: kills touch), `NO_OFFLINE_HID=1`, `DATA_PERSIST=0|1|2`,
  `UNIQUE_CHIP_ID=<hex>`, `IPHONE_IDENTITY=1`.
- Boot: `python3 scripts/picasso-cdc-command.py --command reboot-fastboot --fastboot-serial 52cc791d --fastboot-timeout 300`
  then `nohup python3 scripts/capture-picasso-usb-raw.py --output out/TAG-runs/run1.log --wait 900 --duration 1800 &`
  then `fastboot -s 52cc791d boot out/TAG/picasso/boot/TAG.img`.
- Serial can be claimed by the capture; kill it before sending CDC commands.
- Kernel driver sources live in `out/picasso/tree/drivers/...` and are hard-linked to the worktree `drivers/...`
  (editing either edits both). `drivers/NativeMetal` (plugin) is built by UniXNU's script, not by the kernel build.
- Logs: `out/<tag>-runs/runN.log`; every run since wf143 is documented in docs/handoff-20261103-gpu-render-next.md.

## 7. Uncommitted changes to know about (as of this handoff)

Worktree: `drivers/NativeAdreno/NativeAdrenoSubmit.cpp` (unstick mask + fault-bit print),
`drivers/NativeMobileDisplay/PicassoDisplaySupport.cpp` (`sheng-display-scale`),
`drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp` (letterbox acceptance/copy),
`scripts/build-wf-payload.sh` (NO_FBGPU/NO_HIDHOME/NO_OFFLINE_HID/DATA_PERSIST/UNIQUE_CHIP_ID/IPHONE_IDENTITY),
`scripts/make-picasso-adt.py` (phone identity + `/chosen/display-scale`),
docs. UniXNU: `scripts/unixnu/payload.py` (iphone_identity key), `drivers/NativeMetal/NativeAdrenoMetalPlugin.mm`
(gated self-test + benchmark trigger), `drivers/NativeMetal/NativeTurnipSmoke.h` (fill/tri/compute timings).
Check `git status` in both trees before committing.

## 8. What not to do

- Do not chase FAR 0x1338f508 as an unmapped BO; see fact 3.
- Do not assume the Metal plugin is required for the freeze; see fact 6.
- Do not remove `boot-progress=2`, do not flash partitions, keep the CDC console exclusive, keep cmdline <= 1000.
- Do not use `sheng-data-persist=0` to clear caches; it breaks the Data block device.
- Do not run the adrenoprobed self-test concurrently with a stability measurement: it generates hundreds of GPU
  submissions and stalls and will look like the bug.

## Update after wf166 (identity chain finally stable)

- wf163's phone UI was produced by the UUID trick, but the identity flipped back to iPad on later boots; the
  daemon-side cache clear (adrenoprobed, wf164) was too late — MobileGestaltHelper starts before launchd jobs.
- wf165 accidentally booted with `model=picasso-first-boot` (unknown prefix) and showed the iPod idiom + setup
  assistant: the newly prepared build cache's `scripts/unixnu/payload.py` did not understand the board key
  `iphone_identity`, so `--iphone-identity` was never passed. Do not select the phone identity through a board.toml
  key that the pristine upstream payload builder does not know.
- Fixed in wf166 by making the identity independent of that builder:
  - `scripts/build-wf-payload.sh` exports `PICASSO_PHONE_IDENTITY=1` when `IPHONE_IDENTITY=1`;
  - `scripts/make-picasso-adt.py` flips `ipad_identity` to `iphone_identity` when that env var is set;
  - the board key stays `ipad_identity = true`, so the upstream builder always passes `--ipad-identity`.
- Kernel clear is in place: `patches/picasso-clear-gestalt-xnu.patch` (registered in `unixnu`, applied last,
  `zzzzzzzzzzzzzzzzzzzzzzzzzzz-picasso-clear-gestalt.patch`). It unlinks the MobileGestalt cache plist and the two
  preference plists from `kern_exec` right after the writable Data mount (`vm_swap_retry_create` call site) and
  prints `PICASSO_GESTALT removed <path>` before userland. wf166 log: AMFI `iPhone10,3`, display `scale=3`,
  `PICASSO_GESTALT removed .../com.apple.MobileGestalt.plist`, SpringBoard renders the phone UI (user-confirmed).
- Known cosmetic/root-image issues, not GPU bugs:
  - The status bar is a red `rdar:45025538` bar. This is an Apple internal-build status-bar override on the internal
    21H461 iPadOS root; upstream pairs a phone identity with an iOS root. Switching to an iPhone IPSW root is the
    documented fix. Do not spend GPU time on it.
  - The boot Apple logo is gone; it is drawn by firmware/iBoot before XNU and is unrelated to the ADT/cache changes.
- Current stable phone image: `out/wf166` (kernel 697a867d, soft addon 766b431c trust, device scale 3, no handoff).
  The GPU fix work (restart hook, hangcheck) still applies to the same driver sources.
