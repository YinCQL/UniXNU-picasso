> Integration note (PR18, 2026-11-06): this is the contributor's original
> hardware report, not validation of the merged branch. The reviewed merge keeps
> the existing display geometry, brightness fixes and 20 GiB persistent Data
> identity. IPC census and frame timing diagnostics described below are not
> included. Bluetooth job enablement is migrated before launchd; radio hardware
> remains unimplemented. KeyStore compatibility stubs do not implement encryption.

# iOS16 / iPhone10,3 migration status (2026-11-06 02:00 CST)

Workspace: `worktrees/ios16-picasso`, branch `stage12/picasso-ios16` (from
`origin/feat/ios16-gpu` @ `65529dd`). Temporary files and device logs live in
`temp/`; build outputs stay in `out/`.

## Image built tonight

The iPhone 16.7.16 (20H392) userdata image is built from the pinned IPSW
(`out/ipsw/20H392` extraction) and contains the unchanged OS and SystemOS
windows:

- `out/picasso/userdata/userdata-root.img` — 8,380,219,392 bytes
- manifest sha256 `6d7ac29a...`; OS sha256 `6e87bfca...`; SystemOS sha256
  `9cd738f3...`
- flash: `fastboot flash userdata out/picasso/userdata/userdata-root.img`
  (partition capacity 50,981,744,640 bytes; the image fits)

Boot images:

- `out/picasso/boot/boot-latest.img` — 60 MiB payload, 65,105,920 bytes,
  kernel sha256 `13d15066d430e933d9422330815292583e991f3aec3e80b309f931b2f77fa33f`
- `out/ios16-gpu1/picasso/boot/ios16-gpu1.img` — 80 MiB experiment payload
  with the iOS16 GPU addon, 86,077,440 bytes; payload sha256 `32cde9b0...`
  for the addon, 960-byte kernel cmdline
- checks: `make-picasso-adt.py` produces a 15,004-byte ADT with
  `display-scale=3`, `display-height=2400` and artwork subtype 2400;
  `check-picasso-display-policy.py` passes.

## Commits made tonight

1. `df7a484 display: match the 360x800 drawable to the 1080x2400 panel`
   — adds `display-height=2400`, artwork subtype 2400.
2. `65fe3d8 hid: retry undelivered touch reports and qualify the dispatch table`
   — iPadOS `94e1c8c` retry and `cd3c8e9` vtable/class guard; kernel rebuilt.
3. `6f4f58d gpu: advertise the owner Metal plugin only when its addon is present`
   — restores `MetalPluginName`/`MetalPluginClassName` behind `sheng-gpu-addon=1`.
4. `2686825 usb: drop the DWC3 pull-up before a warm reset`
   — ports the iPadOS `2906fa3` fix so CDC `reboot-fastboot` gives the host a
   clean disconnect and the boot loader's fastboot gadget re-enumerates.
5. `a7ecd94 board: keep com.apple.bluetoothd enabled for the audiomxd touch path`
   — removes it from the offline DENY list (iPadOS `b41bce2`/`ffc849f`).
6. `157ae5a keystore: absorb the iPadOS selector surface and descriptor fix`
   — ports `f29f391`: bookkeeping selectors 4/5/9/12/13/26/27/55/101/102,
   keybag handle 0 for 14, empty configuration for 31, any context for
   7/17/31/35, and `prepare`/`complete` around the out-of-line output
   descriptor. Host check passes.

Final kernel with all changes:
sha256 `c49616bc62c65814fd083aa63c1366dd95f771b5f51747840b49fbfc4cf383d4`.
Final boot images: `out/picasso/boot/boot-latest.img` (60 MiB) and
`out/ios16-gpu3/picasso/boot/ios16-gpu3.img` (80 MiB, addon
`32cde9b0...`, 960-byte cmdline).

## iOS16 GPU addon (experiment)

`temp/gpu-ios16/` contains a native arm64/iOS16 build made from the current
NativeMetal sources with the iOS16 ABI fixes:

- `NativeAdrenoMetal.bundle` — `MinimumOSVersion=16.0`, plugin/Turnip/daemon
  all `LC_BUILD_VERSION minos 16.0` and ad-hoc 4 KiB signed.
- `ngpu-addon-ios16.bin` — 12,133,208 bytes, NGPUADD2, `--publish` and
  clients `backboardd,SpringBoard`.
- `gpu-trust.bundle` — 67,888 bytes, two original 20H392 modules plus one
  owner module with four CDHashes (SB dylib, plugin, Turnip, `adrenoprobed`).
- `BUILD.md` records the exact source edits, commands, CDHashes and the
  known limitation: the existing bridge static archive was built for iOS 17
  and only the final Mach-O was rewritten to minos 16.

This is a build artifact only; it has not been device-qualified. The kernel
advertises the plugin only when the payload carries `sheng-gpu-addon=1`, so
the normal 60 MiB image keeps the old CPU-only behavior.

## Device state (2026-11-06 06:30 CST)

The phone got a responsive fastboot back at 02:03 CST: a hub-port VBUS cycle
followed by ~75 s of settling returns the boot loader's protocol, and small
`fastboot flash`/`getvar` commands then work. The raw 8.4 GiB userdata image
cannot be flashed with fastboot 34/37: the size check reaches
`copy_avb_footer()`, which reads the whole image into memory and dies with
`Failed reading from userdata`. Two independent fixes are prepared:
`out/picasso/userdata/userdata-root.sparse.img` (Android-sparse, 7.3 GiB,
2,045,952 blocks) takes fastboot's sparse path and skips the AVB footer copy,
and `temp/pt28/platform-tools/fastboot` (28.0.1) predates `copy_avb_footer`
entirely. The watcher tries the raw image with fastboot 28 first, then the
sparse image.

A `fastboot boot temp/tiny-boot.img` (2.1 MiB, no payload) then booted the UEFI
firmware successfully, but that image has no payload in its ramdisk, so the
firmware sat in its boot menu without exposing USB. A VBUS cycle does not reset
a running UEFI session; the phone now needs a physical power-button press
(long press) before fastboot is reachable again. Do not use the payload-less
image as a diagnostic again.

`temp/overnight-control.py` is the detached watcher. When a responsive
fastboot appears it will flash the sparse userdata once, then alternately boot
`out/picasso/boot/boot-latest.img` and `out/ios16-gpu2/picasso/boot/ios16-gpu2.img`
(the USB-reset-fix kernel, HID retry, 2400 ADT and GPU addon), capture CDC,
send `status`/`snapshot` and leave the working image running.

## Next steps on a responsive device

1. `fastboot flash userdata out/picasso/userdata/userdata-root.img`
2. `fastboot boot out/picasso/boot/boot-latest.img`
3. capture `/dev/cu.usbmodem*` and confirm stage ①/②: AMFI rejects zero,
   `bsd_init` -> launchd -> SpringBoard lock screen.
4. If the baseline reaches CDC, test `picasso-cdc-command.py --command
   reboot-fastboot`, then repeat with `ios16-gpu1.img` and look for
   `PICASSO_GPU cp: metal ... plugin=declared`, `PICASSO_GPU open: plugin`,
   `submissions` growth and `PICASSO_DISPLAY_STATE ... copies=0`/`flips` growth.

## GPU pipeline creation fixed: the uchar3 function constant (2026-11-06 18:40 CST)

CoreAnimation's Metal backend aborted backboardd on every pipeline because
`CA::OGL::MetalContext::create_fragment_shader` could not make its fragment
function. The exit reason does not keep the abort payload, so the abort-text
probe in `patches/picasso-abort-text-xnu.patch` reads it in
`abort_with_payload_internal` and prints it from the kernel. On gpu9 (old
plugin) every backboardd instance reported:

    PICASSO_ABORTTEXT pid=50 name=backboardd code=7 size=73 \
      text=function=fixed_frag_lph_cph spec=A2Xhf constant type 51 is not qualified

`MTLDataTypeUChar3` is 51. CA specializes `fixed_frag_lph_cph` with a 3-byte
`uchar3` function constant; the plugin's `constantBytes()` table declined every
3-wide vector ("3-wide vectors are not qualified"), so
`newFunctionWithName:constantValues:error:` returned nil, CA retried once and
aborted with `OS_REASON_COREANIMATION`.

The fix is in `temp/gpu-ios16/src/NativeMetal/NativeBridgeMetalFrontend.mm`
(standalone diff: `temp/gpu-ios16/patches/ios16-uchar3-function-constant.patch`):
`constantBytes()` now sizes all 3-wide vector types (`Float3` 12, `Half3` 6,
`Int3`/`UInt3` 12, `Short3`/`UShort3` 6, `Char3`/`UChar3`/`Bool3` 3, `Long3`/
`ULong3` 24) instead of returning 0. The plugin, addon and owner-trust bundle
were rebuilt as one set (`ngpu-addon-ios16.bin` 12,136,128 bytes;
`gpu-trust.bundle` sha256 `83716f41f291bf5031c138b2ccc6d26b8c99486f36f678ae2992716164a97e9f`)
and packed as `out/gpu10/picasso/boot/gpu10.img` with the abort-text kernel
`7cff78a5b3cca1cf9a05b84da1929069de0f771dfaac58a5a0ce8ff3e809159a`.

Hardware result (gpu10, `picasso-wled=0`): no `PICASSO_ABORTTEXT` and no
`PICASSO_SERVICE exit` at all; the GPU scanout presenter reaches `P4 e0`
(serving) and reports `PF <frames>/0` rows at vsync, e.g. 6,631 frames served
with 0 failures in 62 s (~60 fps) on the 1080x2400 panel. This is the first run
where backboardd survives CA fragment pipeline creation and the display keeps
receiving presented frames; touch injection is still a separate open item.

## Zero-copy scanout prepared; battery cut the run short (2026-11-06 19:10 CST)

The iPadOS zero-copy path is already ported in this tree (`3732ae6`
`sheng-dpu-scanout=2` through the display SMMU slot window, `6ce848a` early
retire at the flip, `3103dd0` the build fix). The kernel carries the direct
scanout code and the fence interface (`native_adreno_fences` in
NativeAdrenoPublish.cpp); only the payload has not enabled it. With GPU pixels
proven by gpu10, `out/gpu11/picasso/boot/gpu11.img` (`74f952e8...`) is gpu10
plus `sheng-dpu-scanout=2`; its joined command line is 999 bytes, just under
the 1000-byte DarwinBoot limit.

At 18:45:39 the gpu10 session stopped printing and the phone disappeared from
USB; the hub reports no connected port and the user reports the battery was
flat. The kernel asks for 500 mA of USB power (config descriptor 250 x 2 mA),
but the idle audiomxd storm plus the Setup launch loop and GPU submits
(42,498 in that boot) outran the charger. The phone is charging; while
`temp/hold-in-fastboot` exists, `temp/control-watch.sh` keeps it in fastboot
instead of booting iOS below charge.

Setup diagnostic images for the next powered session (kernel sha256
`cf3099a44573a86c70a0b0b414e6b75cf98e1dd032f3a60c14620ef5bcb3df59`, which adds
Setup to the `PICASSO_UI_EXIT` probe in `11b4b3b`):

- `out/gpu12/picasso/boot/gpu12.img` (`5bb194a3...`) — probe only, default
  app injection.
- `out/gpu13/picasso/boot/gpu13.img` (`f342bbb7...`) — same kernel with
  `picasso_inject_apps=0`, isolating whether the statusbar dylib injected into
  Setup is why the app relaunches every ~20 s.

`out/gpu10/picasso/boot/gpu10.img` (`46c5ba2f...`) remains the proven GPU
baseline.

## Zero-copy direct scanout verified with the Setup UI (2026-11-06 20:12 CST)

After the phone was reconnected and charged, `out/gpu11/picasso/boot/gpu11.img`
(`74f952e8...`, gpu10 plus `sheng-dpu-scanout=2`) booted and the direct path
engaged on the first userland draw:

    PICASSO_DPU smmu scr0=2d0406 idr0=4c017e51 idr1=6000003e groups=81 banks=62 pages=128
    PICASSO_DPU smmu state=80000002
    PICASSO_DPU direct scanout on bank=2 window=10000000 boot=a0000000 stride=4320

By 20:12 the run had 17,400 flips over ~9.3 minutes. In steady state the
DPU frame period is 16.4-18.6 ms (~60 fps) with `fence_us=0` (the submit fence
is already retired when the flip happens) and `latch_us` between 0.5 and
13.7 ms; `maps=3 mapfails=0 fallbacks=0 latch_timeouts=0`, so the three surface
slots are reused and no flip fell back to the copy path. The 53-98 ms period
spikes line up with app launches (the Setup relaunch loop), not with the DPU
latch. `PICASSO_GPU submits live` reached 17,315 submissions / 59,221 IBs with
`max=44` IBs per submit, and `PICASSO_INJECT Setup` opened the injected dylib,
i.e. real userland command buffers are being presented through the direct path.

The panel content is the actual setup assistant: the user reports a red status
bar and the "你好" Hello screen. A CDC `snapshot` at the same moment matches it
(sampled row 0 `ffff6c64`, an orange-red bar; the remaining rows white), which
confirms `directSnapshot()` reads what the DPU scans out. The earlier all-black
snapshot was taken before the first userland draw and is not a scanout failure.
`PICASSO_DISPLAY ... nonblack=0` stays at zero by design: direct scanout
bypasses the `native_legacy_gpu_offer` accounting that feeds that counter.

Open in this image: the Setup app still relaunches (12 `PICASSO_INJECT Setup`
starts observed); the `PICASSO_UI_EXIT` Setup probe needs the newer kernel in
gpu12/gpu13, which have not been booted on hardware yet. The red status bar
tint has not been triaged.
