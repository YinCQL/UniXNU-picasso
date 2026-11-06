# Zero-copy scanout: the DPU fetches the compositor's surface through the display SMMU

Status: working on hardware (wf339/wf341). The CDC warm-reboot bug that blocked autonomous
iteration is fixed (wf338); the display SMMU bank is measured (wf338); the direct scanout port is
in wf339 and wf341 adds early retirement of the swap plus per-frame stage timing.

## Measured (wf341, kernel f246a71f, early retire)

```
PICASSO_DPU smmu state=80000002
PICASSO_DPU direct scanout on bank=2 window=10000000 boot=a0000000 stride=4320
PICASSO_DPU flips=360 maps=3 mapfails=0 fallbacks=0 latch_timeouts=0 latch_us_max=10556
PICASSO_DISPLAY_STATE ... submit=N accept=N reject=0 ... copies=0
PICASSO_DPU_FRAME n=30 period_us=22459 fence_us=0 latch_us=5146
```

On the home screen while interacting:

- `copies=0`, `offers=0` (the GPU presenter never sees a frame).
- Fence wait at the swap: **mean 0.3-0.5 us, max 19 us** - by the time the swap reaches the
  driver the GPU work for that frame is already retired, so the display path adds nothing.
- Latch: **mean 4.5 ms, p95 8.4 ms, max 11.2 ms** (panel frame boundary every 8.3 ms at 120 Hz).
- Frame interval over 111 samples: **median 25.2 ms (~40 fps)**, fastest 10% at 17.3 ms (58 fps),
  p95 49.5 ms (idle gaps included), 5% of frames at <=16.7 ms.
- 5 s submit-rate buckets during interaction: **42-46 fps** (wf339 baseline: 31-33 fps).

So early retirement removed the latch from the frame period (wf339 retired after the latch) and
the display no longer limits the rate: frames arrive at the driver every ~22-24 ms and each is on
screen within one latch. The remaining limit is the compositor's own frame production, upstream of
the display driver.

### Why ~40 fps and not 60

`getRefreshAttributes` reports `IOMFBDisplayRefresh` min 1/60 s and max 1/24 s (the family's
default; `sheng-vsync` above 60 overrides it). The render server paces its display timer inside
that range and settles near 40 Hz, which is the observed 24-25 ms cadence - not a render time.

## Measured (wf342, same kernel, `sheng-vsync=120 sheng-vsync-fixed=1`)

The override is visible at boot: `PICASSO_DISPLAY event=11 sample=1 a=5 b=78` (refresh properties
reported at 0x78 = 120 Hz) and `sample=2 a=4 b=78` (the refresh tick starts at 120). Direct
scanout stays on (`smmu state=80000002`, bank 2, flips=2640 maps=3 mapfails=0 fallbacks=0
latch_timeouts=0, latch mean 4.2 ms / max 13.9 ms, copies=0, fence wait 1 us, no panic).

| frame interval | wf341 (60 Hz attrs) | wf342 (120 Hz attrs) |
|---|---|---|
| median | 25.2 ms (40 fps) | **22.0 ms (45 fps)** |
| fastest 10% | 17.3 ms (58 fps) | **12.2 ms (82 fps)** |
| share <=16.7 ms | 5% | **16%** |
| minimum | 16.1 ms | **4.7 ms** |

During the lock-screen appearance the 120 Hz build runs 14.2 / 14.0 / 11.0 ms intervals
(70-90 fps); wf341 never went below 16.1 ms. The idle lock screen holds 30-40 fps (5 s submit
buckets), and the submit counters track accept exactly (submit=2176 accept=2176 reject=0
retire_fail=0). The remaining median is the compositor's own frame production, not the display
path: the swap arrives every 20-25 ms at idle and every 11-15 ms while the UI animates.

Keep `sheng-vsync=120 sheng-vsync-fixed=1` in the experiment command line (the build script does
not remember extra kernel args; a rebuild without them silently boots the copy path).

## Measured (wf339, kernel 901f0baa, `sheng-dpu-scanout=2`)

```
PICASSO_DPU smmu state=80000002
PICASSO_DPU direct scanout on bank=2 window=10000000 boot=a0000000 stride=4320
PICASSO_DPU flips=1200 maps=3 mapfails=0 fallbacks=0 latch_timeouts=0
              latch_us_total=5467013 latch_us_max=12419
PICASSO_DISPLAY_STATE ... submit=6720 accept=6720 reject=0 ... copies=0
```

- Every compositor frame is scanned out by the DPU: flips track accept, `copies=0`, the GPU
  presenter is never offered a frame (`offers=0`).
- Three surfaces rotate through the four windows; no map failures, no fallbacks, no latch
  timeouts. Latch mean 4.4 ms, max 12.4 ms.
- The compositor's submit rate measured from the state counter: **31-33 fps average** while the
  user was interacting, with bursts to 50-60. The swap was retired only after the latch, so the
  latch sat in the frame period; wf341 retires the swap at the flip instead and keeps the latch
  off the retirement path (it only gates the next flip). `PICASSO_DPU_FRAME` now prints
  `period_us/fence_us/latch_us` every 30 flips to show where the frame period goes.


## Why the first flip attempt could not work

wf336 (`sheng-sde-flip=1`) pointed the active pipe at the compositor's surface and skipped every
frame: `getPhysicalSegment(0)` returned 4 KiB segments against the 10,444,768 bytes a 1080x2400
frame needs. The compositor's IOSurfaces are page-scattered, and the DPU reads one linear range.

## What sheng already does (ported here)

The upstream display driver (GopFramebuffer/NativeLegacyDisplay.cpp) and its UFS-side SMMU code
(NativeUfs.cpp) solve exactly this: the display pipe's address registers are 32-bit while kernel
allocations live anywhere, so the surface's 4 KiB pages are mapped into 32 MiB windows at
0x10000000 through the display's apps-SMMU bank. 0x80000000..0xffffffff stays identity mapped
(two 1 GiB blocks) so the boot loader's scanout keeps working; the compositor's frames then reach
the pipe with no copy at all.

## Measured on Picasso (wf338, `sheng-sde-probe=1`)

```
PICASSO_SMMU scr0=2d0406 idr0=4c017e51 idr1=6000003e groups=81 banks=62 pages=128 gfsr=0
PICASSO_SMMU match smr[3]=84020b40 layout=id-low s2cr=2 bank=2 type=0
PICASSO_SMMU cbar=1f000 cba2r=1 sctlr=e0 ttbr0=000000000 tcr2=28063 tcr=0 mair=0 fsr=400
PICASSO_SMMU valid-smrs=8
PICASSO_SDE contig 1048576 ok pa=cd040000 seg=1048576 len=1048576
PICASSO_SDE contig 12582912 ok pa=cd140000 seg=12582912 len=12582912
PICASSO_SDE contig 67108864 ok pa=cdd53000 seg=67108864 len=67108864
```

- The display stream (DT `iommus = <&apps_smmu 0xb40 0x402>`) matches **SMR[3] = 0x84020b40**:
  the SMR layout is the same as sheng's (ID in bits 15:0, mask in 30:16).
- Its S2CR is **0x2**: type 0 (stage-1 translate) to **context bank 2**.
- CB2's CBAR is 0x1f000: type 1 (S1 translate). **SCTLR = 0xe0**: translation off, so today the
  pipe fetches physical addresses (SRC0_ADDR = 0xa0000000, the GOP's PA).
- The bank's TTBR0/TCR/MAIR are zero; TCR2 = 0x28063. Everything needed to take over the bank.
- Contiguous allocation works (1/12/64 MiB), but it is not what the compositor uses.

## The port (wf339, `sheng-dpu-scanout=2`)

In `drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`:

1. `dpuStart()` maps the apps SMMU, finds the display stream's SMR/S2CR entry, and checks the
   bank is S1_TRANS with translation off.
2. `dpuEnable()` allocates 66 contiguous 4 KiB pages (L1 + 64 L3 tables), builds:
   - `L1[0] = self` (the walk starts at level 0 although T0SZ=32),
   - `L1[2] = 0x80000000 | attrs | 1`, `L1[3] = 0xc0000000 | attrs | 1` (1 GiB identity blocks),
   - `L1[128+t]` -> one L3 table per 2 MiB of the 4 x 32 MiB window at 0x10000000,
   writes TTBR0/TCR2 (PASIZE 5, SEP 7)/TCR/MAIR, flushes the TLB, enables SCTLR (M, CFRE, CFIE),
   and watches the bank's FSR for 50 ms: any fault undoes every write (the boot scanout survives).
3. `dpuMap(slot, pages, count)` fills a slot's L3 with `pa | attrs | 3`, cleans it, flushes the
   TLB and returns `0x10000000 + slot * 32 MiB`.
4. The swap path: wait for the GPU's fences (`native_adreno_fences`) because the compositor
   submits when its command buffer is scheduled; map the surface into a free slot (never the one
   on screen or the one waiting to latch); write SRC0_ADDR/SRC_YSTRIDE0 and `CTL_FLUSH = 1`
   (VIG0); poll the flush bit every 500 us (50 ms timeout) and retire the swap on latch.
   A blank frame or a mapping failure flips back to the boot memory first (the copy paths draw
   there).
5. Four slots rotate; `directStats()` prints flips/maps/fallbacks/latch times every 60 flips.

Offsets are the ones wf335 measured for this DPU (VIG0 at 0x5000, source address +0x14, stride
+0x24, CTL0 at 0x2000, flush +0x18 bit 0), not sheng's DPU 9.2 offsets.

## Also fixed here: the CDC warm-reboot bug

`reboot-fastboot` used a watchdog/PSCI warm reset that leaves the DWC3 running and its D+ pull-up
asserted; the host never saw a disconnect, kept the dead CDC device, and the boot loader's fastboot
gadget never enumerated (so the next image could only be booted after a physical power cycle).
`picasso_usb.c` now stops the controller (RUN_STOP=0, wait DEVCTRLHLT, clear PHY suspend,
CSFTRST) before the reset, and stops it early in the console's own start before any guard can
fail. Verified on wf338: `reboot-fastboot` -> fastboot enumerates -> boot -> console streams.
