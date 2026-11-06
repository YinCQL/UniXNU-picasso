> Integration note (PR18, 2026-11-06): this is the contributor's original
> hardware report, not validation of the merged branch. The reviewed merge keeps
> the existing display geometry, brightness fixes and 20 GiB persistent Data
> identity. IPC census and frame timing diagnostics described below are not
> included. Bluetooth job enablement is migrated before launchd; radio hardware
> remains unimplemented. KeyStore compatibility stubs do not implement encryption.

# Hardware boot record — 2026-11-06 09:54 CST

This file is the evidence log for the first iOS 16 boot on the picasso hardware
(serial 52cc791d). Raw console: `temp/runs/morning-0954/console.log` (26 min,
14,989 lines) and `temp/runs/morning-0954/console-full.log` (full capture).

## What ran

The overnight watcher (`temp/overnight-control.py`) had been waiting since
07:09. At 09:50 it found a responsive fastboot and did the whole chain without
host intervention:

    fastboot(28) flash userdata out/picasso/userdata/userdata-root.img   rc=0  213 s
    fastboot boot out/picasso/boot/boot-latest.img                       rc=0   24 s

fastboot 28.0.1 splits the 8.4 GiB raw image into its own sparse stream
(`Sending sparse 'userdata' 10/10`), so `copy_avb_footer()` never sees it.

## Stage results

| Stage | Evidence | Result |
|---|---|---|
| AMFI | `AMFI: queried model name from device tree: iPhone10,3`; `Enabling developer mode since protected data is not available`; `PICASSO_INJECT ... signature error=0 flags=24000003 platform=2` | pass |
| Root | `NATIVE_OWNER_UNION path=/sbin/launchd error=0`; `disk5s1 Root Volume`; `PICASSO_EXEC ok pid=1 path=/sbin/launchd` | pass |
| Userland | SpringBoard, backboardd, lsd, `PICASSO_STAGED stage=2 dirs=5575 files=3840 bytes=510877688 failed=0` | pass |
| KeyStore | `PICASSO_AKS` selectors 17, 0, 7, 256 from backboardd and DumpPanic, all `result=0` | pass |
| Touch | `PICASSO_NVT fwinfo rc=0 ver=20 18x36 max=1080x2400`, SRAM `crc=04` match, real contact `x=5110 y=23990` -> `y=23450` | pass |
| Display | 463 CPU-copy frames, `stored_bytes` up to 670,912 | pass |
| GPU | `PICASSO_GPU cp: metal stage=5 status=00000000 registered=1 plugin=absent device_qualified=0`; fences advancing | pass (CPU path) |
| CDC | `PICASSO CDC: configured=1 host_open=1 live=1 panic=0 events=4496` | pass |

15 apps were launched and injected: Calculator, Compass, Freeform, MobileCal,
MobileNotes, MobilePhone, MobileSMS, MobileTimer, PosterBoard, Preferences,
Reminders, Spotlight, iMessageAppsViewService, plus SpringBoard and dyld.

## CDC -> fastboot round trip

`python3 scripts/picasso-cdc-command.py --command reboot-fastboot
--fastboot-timeout 150 --fastboot-serial 52cc791d` re-enumerated as
`52cc791d fastboot` with no button press. This validates the DWC3 pull-up fix
(`2686825`) on hardware: the host saw a clean disconnect and the boot loader's
gadget came up.

## Open defect: deterministic SpringBoard abort

SpringBoard aborts every 1-3 minutes (8 aborts in the first 26 min, 45 in a
70 min A/B run), and each abort is preceded by SpringBoard matching
`IOAcceleratorES`. The signature is identical across every instance:

    code=1 (or 2) ESR=92000007 FAR=<guard page, prot=0> PC=<fixed unslid>

The fault PC is the same in both runs after subtracting the reported
`cache_slide` (`0x223b0000` -> `0x1f5feda4c`, `0x2f420000` -> `0x20305da4c`,
both `0x1d3c2da4c` unslid). Resolving that address against
`dyld_shared_cache_arm64.symbols` (20H392 SystemOS) gives the dylib at
`cache+0x53c21000`; the nearest exported symbol is
`_ioSurfaceAcceleratorCopyDebugDescription + 0xa648`.

The `PICASSO_ASTC` lines that follow each abort are printed *from inside the
abort handler* (`picasso_user_abort` in `patches/picasso-image-mapping-xnu.patch`),
so they describe the faulting frame rather than cause it: the handler copies
the faulting frame's `x19` and reports an IOSurface descriptor for a
1080x2400 4x4-block surface with 4320-byte rows
(`size=96000000438`, `block=100000004`).

### Ruled out so far

- **Published Metal accelerator.** `sheng-gpu-metal=0` was built and booted
  (`out/nogpu/picasso/boot/nogpu.img`, 984-byte cmdline). The accelerator was
  not published (`metal` lines absent) and the abort signature was byte-for-byte
  the same, so the publication is not the trigger.
- **CA compression prefs.** `PICASSO_CA_PREFS mobile=0 system=0` confirms the
  iPadOS `picasso-ca-prefs-xnu.patch` ran and wrote both plists before
  backboardd; the known `'&BGA'` failure mode is therefore already covered.

### The GPU/addon image is a second, worse failure

`out/ios16-gpu3/picasso/boot/ios16-gpu3.img` (80 MiB, addon `32cde9b0…`,
`sheng-gpu=4 sheng-gpu-metal=1 sheng-gpu-addon=1`, skip-setup seed) was booted
for comparison. It reaches further than any previous run in one respect:

    PICASSO_GPU cp: metal stage=5 status=00000000 registered=1 plugin=declared device_qualified=0
    PICASSO_GPU cp: metal published
    PICASSO_GPU open: plugin type=4147 pid=50 name=backboardd

the plugin is declared and **backboardd opens it** — the first time the iOS 16
userland has taken the Metal path. But backboardd then dies and is relaunched
every ~10 s for the whole run (pids 50, 172, 187, 196, 203, 213, 220, 1249,
1995, each opening the plugin again), with no `PICASSO_ABORT` for it, and
SpringBoard still relaunches. The addon unpacked
(`PICASSO_GPU addon: extensions=3/2 daemons=5/0 wallpaper=1/2`).

### What the destination check rules out

`patches/picasso-image-mapping-xnu.patch` was extended to qualify the decode
*destination* (`astc-dst`, `astc-dst-last`) as well as the source. On the next
boot (`temp/runs/val-usb500.log`):

    astc-dst      address=119cd2000 start=119cd2000 bytes=9e4000 prot=3 tag=70
    astc-dst-last address=11a6b53f0 contains=1

`0x9e4000` = 10,371,072 bytes contains the full 10,368,000-byte 1080x2400 RGBA
destination, so the surface is **not** undersized and the decode does not run
off its end. The fault is somewhere else:

    kind=fault address=105f05000 contains=0 start=105f07000 bytes=6000 prot=3 tag=3 resident=6

The faulting address is an *unmapped gap* 0x2000 below a 24 KiB `prot=3` region
with VM tag 3 (a small malloc zone). The access is therefore 8 KiB below a small
mapping — a heap access in the decoder's own frame, not the texture buffer.

### Working hypothesis for both loops

Both failures are on the render-server side and both involve IOSurface
lifetime: SpringBoard aborts *inside an ASTC decode* into a surface, and
backboardd dies once it owns the plugin's surfaces. A surface that the render
server tears down while a decode/submit still references it would produce a
write into an unmapped guard page (SpringBoard) and a dead render server
(backboardd). The next probes should therefore watch the IOSurface user client
lifetime, not the decoder.

### Next probes

1. Boot with `sheng-gpu=0` so the CP never programs the ring, and see whether
   the abort persists. This separates "GPU driver state" from "userland
   IOSurface path".
2. Dump the full local-symbol range around `cache+0x53c21000` with
   `temp/preflight/dsc-symbol.py` to name the function instead of the nearest
   exported neighbour.
3. The faulting thread's user stack (`PICASSO_ABORT stack depth=...` lr chain)
   contains the caller; resolve those lr values the same way.

## Diagnostics landed and verified on the device

`c26b049` ports the frame timeline, the touch pipeline row and the pid-1 IPC
census (plus the ASTC destination check). Booted as `temp/runs/val-rows.log`
(kernel `b8409b0c…`, cmdline 987 B with `picasso-ipc-census=1`):

- `TOUCH d_frames=0 d_hid=0 d_begin=0 d_edge=0 d_skip=0 phys=0 count=0` — one
  row every 10 s, from the CPUFREQ governor tick.
- `IP to1 d=43462 …` / `IP from1 d=22327 …` and
  `IP ids 40000388=114 400002bc=57 202=40 203=20` — the census works. Note
  launchd *sends* ~20k messages per 10 s window and receives ~43k; the
  `0x40000388` id is the same XPC id the audiomxd storm was found on.
- `PICASSO_FRAME` prints nothing on this image: it traces GPU-served frames and
  no GPU client accepts a frame here. The CPU path's cost is already the
  `PICASSO_DISPLAY copy … ticks=` row (≈239k ticks per 64 samples ≈ 12.5 ms).

## USB 500 mA (`582a909`): measured, no boot-time change

Both descriptor copies now advertise bMaxPower 250 (500 mA). Host-measured
boot-to-first-frame, kernel timebase:

| descriptor | boot | first frame `time_ms` | aborts in 300 s |
|---|---|---|---|
| 200 mA (`boot-latest.img` `263a452c`) | timed | 85,763 ms | 3 |
| 500 mA | timed | 83,814 ms | 0 (idle screen) |

so on this setup the descriptor does not change boot time (the two runs are
within 2.3%). The transfer rate does vary by an order of magnitude between
boots (23.5 s vs 1.5 s to `fastboot boot` 63 MB), but that predates this change,
so it is link negotiation, not the power byte.

## The crash needs a live UI

The 500 mA run reached 3 SpringBoard aborts in 300 s while the phone was being
used; the `val-rows` run sat idle and logged **0 aborts and 1 SpringBoard start
in 300 s**, with `nonblack=unknown` on every frame (a blank screen) instead of
the `nonblack=29457` a drawn screen reports. The synthetic swipes and home
presses ran in both. That fits the ASTC signature: the abort happens while
SpringBoard decodes image assets, i.e. when the UI actually draws. Reproduce by
touching the phone, not by waiting.

## Host checks

`temp/preflight/sweep2.out`: 12/12 pass (adt, ui-trust, hid-plan, novatek,
ufs-cache, keystore, staged-apps, data-host, ram-data, mach-ports, microtime,
ios16-surface-abi). The repo's migration gate
(`check-picasso-iphone-migration.py`) also passes after the `2b895dc` ADT fix.

## Zero-copy direct scanout ported and working (2026-11-06 13:47 CST)

The three iPadOS display commits are now on this line: `81ed071` (SDE/apps
SMMU probe), `a180505` (`sheng-dpu-scanout=2`, four 32 MiB SMMU slots) and
`5d7b9eb` (retire at the flip, per-frame latch timing) — commits `f1f66fa`,
`b5bd519`, `934e60e`. `ed06a33` also brings the write-combine cost row
(`wc_us`/`wc_reset`/`cache_us`) onto the GPU console line. `9233cad` makes the
merged file compile under this kernel's stricter flags (each option cast to
`IOOptionBits`, `dpuWrite` marked unused; no behaviour change) and the kernel
built as `9f9f50e3…`.

The controller's own probe proves the firmware geometry and the SMMU bank:

    PICASSO_DPU smmu scr0=2d0406 idr0=4c017e51 idr1=6000003e groups=81 banks=62 pages=128
    PICASSO_DPU stream smr=84020b40 s2cr=2 bank=2 cbar=1f000 sctlr=e0
    PICASSO_DPU smmu state=80000002
    PICASSO_DPU direct scanout on bank=2 window=10000000 boot=a0000000 stride=4320

Booted as `out/picasso/boot/boot-dpu.img` (62 MB, payload cmdline carries
`sheng-dpu-scanout=2`, no addon). Host-measured: fastboot boot at 13:47:14,
first frame at 13:48:32 (77.5 s). In 5 minutes of idle lock screen:

| Metric | Value |
|---|---|
| DPU flips | 510+ (2 surfaces, `maps=2`, `mapfails=0`, `fallbacks=0`, `latch_timeouts=0`) |
| frame period | 19.1 ms in the first bucket, then ~100 ms (CA-paced static screen) |
| latch (pipe flush → latched) | 3.4–8.8 ms, max 8.8 ms |
| fence wait | 1 µs (no Metal plugin, so no GPU fences are outstanding) |
| CPU copy samples | **0** (was 463 on the first boot, 1280 in `val-rows`) |
| `PICASSO_DISPLAY frame` rows | 207, all `kind=3` (direct) |
| aborts / SpringBoard starts | 0 / 1 |

The panel is now fed by the display SMMU reading the compositor's own
surfaces: the CPU copy, its commit and its fence all leave the frame. The
`time_ms` field on this boot reads 940814 because the generic counter is not
reset by a warm `fastboot boot`; the host timestamps above are the boot-time
measurement.

### Snapshot while direct scanout owns the pipe

`patches/picasso-cdc-snapshot-xnu.patch` now calls the display driver's
`native_legacy_direct_snapshot()` first (a weak import; absent backends keep
the old path), so the CDC `snapshot` command copies the surface actually on
screen into the GOP memory before it samples it. Without that call the GOP
mapping is stale exactly while direct scanout is active.
