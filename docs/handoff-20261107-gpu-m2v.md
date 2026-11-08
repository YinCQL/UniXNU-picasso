# GPU handoff: optimize the m2v (Metal -> Vulkan) route, keep the OpenGL ES route

2026-11-07, branch `stage12/picasso-ios16`, worktree `/Users/silenzio/Desktop/development/ios/worktrees/ios16-picasso`.
PR https://github.com/YinCQL/UniXNU-picasso/pull/20 (fork `silenzio111/UniXNU-picasso`).

This document is written for the next agent picking up GPU work. The **OpenGL ES route is working
and is the production path; it must not regress**. The m2v route is the long-term, iOS-update-proof
Metal implementation and is currently handicapped by a hardcoded workaround.

---

## 1. Device and tree

- Phone: Redmi K30 5G ("picasso", SM7250, Adreno 620, 1080x2400 120 Hz LCD; the driver runs 60 Hz).
- Fastboot serial `52cc791d`; iOS kernel console CDC `/dev/cu.usbmodemPICASSO011`.
- Build tree: `out/picasso/tree` (symlinks `drivers/`, `kits/`, `scripts/` to this worktree; `package/`
  is a **stale cache copy** — a package edit must be copied there and verified with `strings` in
  `out/picasso/kernel/kernel.macho`).
- ADT memory (board.toml): `dram_size = 0x180000000` (6 GiB), `owned_memory_size = 0x140000000`
  (5 GiB owned by iOS).
- Boot images under `out/<tag>/picasso/boot/<tag>.img` (86,077,440 bytes), booted with
  `fastboot boot`; never flashed.

## 2. Current verified state (GL route, gpu72)

- Kernel `a5b2336e` (this tree) + addon `75618b93` + trust `27ea321d`; cmdline 997/1000 bytes:
  `sheng-dpu-scanout=2 sheng-gpu-perf-index=4 sheng-gpu-gl=1 glwait=0 glhooks=1 sheng-brightness=1023`.
- Commits on the branch: `70f7455` (DPU zero-copy scanout restored) and `09840ea` (IOSurface table
  256 -> 1024 + LRU eviction, the solid-text fix), on top of `910b85a`/`8258315`.
- Hardware numbers: DPU flips 17,880 with `mapfails=0 fallbacks=0 latch_timeouts=0`; DPU_FRAME
  median 16.6 ms (~60 fps, best 84 fps, p10 12.6 ms) on the home screen; GPU 72k submissions with
  `bad=0 ready=0 tmo=0`; no jetsam/panic; 242 processes; idle CPU 626/1442/1481 MHz (d0/d1/d2).
- `sheng-brightness` is only a boot argument in test images. **Do not commit brightness changes**:
  upstream owns the backlight driver (see memory `picasso-brightness-upstream-owned`).

## 3. The OpenGL ES route (do not break)

How it works: with `sheng-gpu-gl=1` the plugin declines backboardd's Metal discovery and mounts
`drivers/NativeGL/eagl/eagl.c` (the canonical build source is
`temp/gpu-ios16/src/NativeGL/eagl.c`), which serves CoreAnimation's `EAGLContext` renderer on
Mesa 25.0.7 OSMesa + freedreno/ir3 over the libdrm stand-in `drivers/NativeGL/nativegl_drm.c`.
Presentation is zero-copy: CA draws into IOSurfaces, `sheng-dpu-scanout=2` makes the SDE flip the
surface directly (`PICASSO_DPU direct scanout on bank=2`), `glwait=0` makes present asynchronous.

Rules for the next agent:

- **Never remove or change `sheng-gpu-gl=1`, the DPU scanout, or the libNativeGL build path.**
  m2v experiments get their own images (drop `sheng-gpu-gl=1` from the args, keep everything else).
- After any change that touches shared code (NativeAdreno kernel driver, display driver,
  `scripts/build-wf-payload.py`, `scripts/build-gl-ios16.py`, the trust/addon chain), rebuild and
  re-boot the GL image (gpu72-class) and confirm: text correct, no tearing, ~60 fps.
- Keep `out/gpu72` intact; it is the known-good GL image and the rollback target.

## 4. The m2v route: what it is and where it stands

- `temp/gpu-ios16/src/NativeMetal/` is the Metal implementation: `NativeAdrenoMetalPlugin.mm`
  (the Metal plugin that backboardd and apps load), `NativeBridgeMetalFrontend.mm` (the
  Metal -> Vulkan frontend), Turnip (`libvulkan_freedreno.dylib`), `GpuScanoutPresenter.h` /
  `GpuScanoutClient.h` (GPU present through the `sheng-fb-gpu=1` hand-off), `CacheExport.h`
  (`M2VCACH1` / `SHEXR001` pipeline-cache export/import), `NativeMetalBridgePhase.h` (the
  bridge's job/cache plumbing).
- Enable it by **not** passing `sheng-gpu-gl=1`: keep `sheng-gpu-metal=1 sheng-gpu-addon=1`, the
  DPU scanout and `sheng-fb-gpu=1`, and boot the same kernel/addon. That image is the m2v baseline.
- Pipeline caches live in `/var/mobile/Library/Caches/com.apple.backboardd/m2v` (the
  `NativeOsLogExport.mm` path is `.../m2v/zz-oslog`); `CacheExport.h` exports/imports them through
  the kernel's auxiliary blocks so the APFS overlay's lost late writes do not lose them.

**The known handicap:** `NativeAdrenoMetalPlugin.mm` hardcodes

```c
// EXPERIMENT (wf82): the first client stream stops in the SQE's CP_SET_MARKER RM6_ENDVIS
// handler, which waits on the visibility/bin hardware; force Turnip's sysmem path.
setenv("TU_DEBUG", "sysmem", 1);
```

at **line 304** (listed clients) and **line 443** (the render server). So today Turnip runs with
**no hardware binning / no GMEM tiling / no visibility stream** -- exactly the wrong mode for a
TBDR GPU, on a UI that is fill-bound (~38 MPx/frame, ~15x overdraw for 2.59 MPx of screen; RB
52-74 % busy at 50 fps with the GL route). This is the first thing to fix; it likely explains most
of why the route felt "laggy".

Prior evidence to build on:

- The plugin mounts, publishes and submits; the kernel side is `AGXAccelerator` + A620 user client
  (see the memory notes `picasso-gmu-hfi-status` and `picasso-gpu-first-submit-stall`).
- The first client stream used to stop in `CP_SET_MARKER RM6_ENDVIS` (visibility/bin handshake),
  and an earlier symptom was `CP_HW_ERROR` with a zero IB page (wf72).
- The GL route now drives the **same** submit path for 72k submissions (`bad=0`) through the
  libdrm stand-in, so the kernel ring/submit code (`NativeAdrenoSubmit.cpp`, `NativeAdrenoRing.cpp`,
  `Adreno620Cp.cpp`) is known-good in the sysmem/plain path; the m2v difference is the binning
  stream and the frontend's command-buffer construction.

## 5. Plan, in priority order

1. **Re-baseline m2v on the current kernel.** Build the Metal image (same as gpu72 without
   `sheng-gpu-gl=1`/`glwait=0`/`glhooks=1`), boot, and measure with the same probes:
   `PICASSO_DPU_FRAME` period, CDC `ps` CPU deltas, `PICASSO_GPU submits live`, and (with
   `sheng-gpu-perf=1`) RB pixels/busy. The old "Metal is laggy" conclusion predates the DPU
   scanout restore, the accelerator fixes in `910b85a`, and the cache loop.
2. **Make the sysmem workaround a boot argument** (e.g. default stays sysmem; `m2v-binning=1`
   skips `TU_DEBUG=sysmem`). Flip it, boot, and see whether the `RM6_ENDVIS` stall returns.
   If it stalls, capture CP/SQE/GMU state at the stop (CP state registers, the last IB words,
   the visibility-stream configuration) and compare with freedreno's a6xx expectations.
   The binning/visibility programming lives in the kernel CP/GMU drivers
   (`Adreno620Cp.cpp`, `Adreno620Gmu.cpp`) and the GMU firmware config (`a650_gmu.bin` etc.).
3. **Verify the present path**: an m2v frame must reach the panel through a DPU direct flip of the
   compositor's surface, not through a CPU/GPU copy into the boot framebuffer. `GpuScanoutPresenter.h`
   modes: default deferred, `sheng-present-blocking=1`, `sheng-present-nonblock=1` (A/B the wait).
   Check `PICASSO_DISPLAY`/`PICASSO_DPU_FRAME` after booting the Metal image.
4. **CPU overhead** (the m2v route's translation layer is the other half of the cost on 2x A76 +
   6x A55): pipeline-cache hit rate and persistence (`m2v` dir + `SHEXR001` import), bake an
   `NativeAdrenoMetal.m2vpack` beside the bundle to skip first-boot compiles, turn
   `METAL2VULKAN_LOG_PIPELINES` off in production, and check that the oslog export is not writing
   per frame. Compare `ps` deltas for backboardd against the GL route.
5. **Compare and decide**: if binning works and fps approaches the GL route, m2v becomes the
   long-term path (no EAGL reverse-engineering per iOS release). If not, keep GL as production and
   treat m2v as a research track; either way, do not regress GL.

## 6. Measuring (same recipes as the GL work)

- Frame rate: `PICASSO_DPU_FRAME n=... period_us=... latch_us=...` rows (every 30 flips) in the
  console log. Median of the active rows is the fps; ignore idle gaps.
- CPU/memory: CDC command `ps` prints `PICASSO_PS top rank=... u=<ms> s=<ms> r=<bytes>`; take two
  samples for per-process deltas.
- GPU: `PICASSO_GPU submits live ... result=<ok>/<bad>`, `PICASSO_GPU busy: samples=...`.
- GL diagnostics: `eagl f/i/r/R/w/d/c`, `eagl top`, `eagl fph`, `eagl uh/htop`, `eagl inew`.
- RB hardware counters: `sheng-gpu-perf=1`, `glperf=2` (GL route).
- Kernel text stream: `PICASSO_GPU text:` lines; the plugin mirrors `say()` only with `glsay=1`
  (kernel buffer is 512 bytes/line, 8192 lines per boot, `NativeAdrenoPublish.cpp`).

## 7. Device control (keep the phone reachable)

- Console commands are injected with `python3 temp/cdc-raw-send.py <command> <seconds>` while a
  capture keeps the port open. `reboot-fastboot` arms the IMEM fastboot reason; wait for
  `fastboot devices`, then `fastboot boot out/<tag>/picasso/boot/<tag>.img`.
- Keep a `scripts/capture-picasso-usb-console.py --reconnect` reader attached at all times; closing
  the last reader can reset the device after ~30 s (the console watchdog).
- A static GPU fence or repeated boot logo at 70-90 s is **normal** (multi-stage boot logo), never
  reboot on that alone.
- Boot logs go to `temp/runs/<tag>.log`.

## 8. Build recipe (both routes)

```sh
# kernel (needs DEVELOPER_DIR and the GPU firmware dir)
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
ADRENO620_FW_DIR=$PWD/out/picasso/gpu-firmware python3 ./unixnu build kernel -j 3

# userspace plugin + GL library (the first wipes the bundle, so the GL script must follow)
cd temp/gpu-ios16
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 scripts/build-ios16.py
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer python3 scripts/build-gl-ios16.py
python3 scripts/make-gpu-trust-ios16.py
python3 scripts/make-gpu-addon-ios16.py --publish --clients backboardd,SpringBoard <new-addon>.bin

# payload (GL, known-good; cmdline must stay <= 1000 bytes)
cd ../..
GRID=0 WF_DROP_LOGO_HOLD=1 \
DATA_RAMDISK=out/picasso/payload/data-skip-setup-c2a722e5fbe0-8f9c624a.dbr \
WF_ADDON=temp/gpu-ios16/<addon>.bin OWNER_TRUST=temp/gpu-ios16/gpu-trust.bundle \
UNIQUE_CHIP_ID=41a551c0dec0de01 \
scripts/build-wf-payload.sh <tag> temp/gpu-ios16/<addon>.bin \
  sheng-dpu-scanout=2 sheng-gpu-perf-index=4 sheng-gpu-gl=1 glwait=0 glhooks=1
```

The trust bundle and the addon must come from the **same** plugin build (a mismatch makes AMFI
reject the plugin). `build-wf-payload.py` strips `sheng-touch-hid=1` and can drop the boot-logo hold
(`WF_DROP_LOGO_HOLD=1`) to free bytes; the default Data seed is the **plain** seed -- always pass
the skip-setup image or the setup assistant reappears. `scripts/build-native-novatek-touch.py`
must keep the 131072-byte firmware baked (a plain kernel rebuild can silently drop touch).

## 9. Traps already paid for (do not rediscover)

- IOSurface/image table: the GL library's table is 1024 entries with LRU eviction; shrinking it
  brings back solid text blocks (glyph masks never uploaded; `vec4(t0.a)` paints every run solid).
- The kernel console text stream only prints 8192 lines; `glsay=1` is required to see `say()`.
- Trust/addon pairing, cmdline 1000-byte limit, stale `tree/package`, touch firmware, skip-setup
  data seed, CDC watchdog, boot-logo stages: all covered above.
- Do not commit brightness/backlight changes (upstream owns the driver).

## 10. Definition of done

- An m2v image (tag it separately) boots to the UI, presents through the DPU, and either runs with
  hardware binning at a measured fps close to the GL route, or documents exactly where the binning
  handshake fails (CP/SQE state, register expectations, the fix path).
- The GL route (gpu72-class) still boots with text correct, no tearing, ~60 fps median.
- Findings go into `docs/` and, if useful, into the PR.
