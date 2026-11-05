# Zero-copy scanout: let the SDE read the compositor's surface

Status: in progress. The SDE register map is read on hardware (wf335); the direct flip is implemented
behind `sheng-sde-flip=1` and not yet measured.

## Why

wf333 (async GPU present) still spends ~24 ms of wall time per swipe frame: the compositor renders
(~10 ms) and the display driver then copies its surface into the GOP framebuffer on the GPU
(present 8.1 ms = 7.6 ms commit + 2.3 ms encode, with the fence wait already hidden), and the two
are serialized because the hand-off keeps one frame in flight. 60 fps needs 16.7 ms; 120 Hz needs
8.3 ms. Removing the copy entirely is the structural fix: the display controller scans the
compositor's own surface, and a frame becomes nothing but the compositor's render.

## What the firmware actually programmed (wf335, `sheng-sde-probe=1`)

The probe is read-only and runs once in the display driver's `start()`:
an `IOMemoryDescriptor::withPhysicalAddress(0xae00000, 0x90000)` mapping of the MDP block, then a dump
of SDE_TOP, the five SSPPs, four CTLs, mixer0, PP0, the DSI INTF and the VBIF. wf335 also carries
the phone identity with `display-height = 2400` (see below).

```
PICASSO_SDE gop_pa=a0000000 len=10368000 panel=1080x2400
PICASSO_SDE version=60010000 top 60010000 0 0 200 0 8000100 ...
PICASSO_SDE sspp0 9600438 0 0 9600438 0 a0000000 0 0 237ff 3020001 80000000 0
PICASSO_SDE sspp1..4 0 ... (inactive)
PICASSO_SDE ctl0 1000005 0 0 0 0 0 0 0 0 0 0 0 0
PICASSO_SDE mixer0 0 9600438 0 0 0 0 0 0 400 ff0000 ...
PICASSO_SDE vbif 10000000 0 ...
```

Decoded with the mainline DPU register map (`dpu_hw_sspp.c` v6.1):

| register | offset | value | meaning |
|---|---|---|---|
| SSPP_SRC_SIZE | 0x5000+0x00 | 0x09600438 | 1080 wide, 2400 high |
| SSPP_OUT_SIZE | 0x5000+0x0c | 0x09600438 | same |
| SSPP_SRC0_ADDR | 0x5000+0x14 | **0xa0000000** | the GOP framebuffer's physical address |
| SSPP_SRC_FORMAT | 0x5000+0x30 | 0x237ff | linear BGRA8 family |
| SSPP_SRC_UNPACK_PATTERN | 0x5000+0x34 | 0x03020001 | |
| SSPP_SRC_OP_MODE | 0x5000+0x38 | 0x80000000 | |
| CTL_LAYER_0 | 0x2000+0x00 | 0x01000005 | mixer 0, one layer |
| CTL_FLUSH | 0x2000+0x18 | 0 | written to latch SSPP updates |
| CTL_START | 0x2000+0x1c | 0 | command-mode kickoff, unused in video mode |

The decisive fact: **SRC0_ADDR holds the GOP's physical address itself**, so this SDE reads physical
memory directly (the DT's mdss node has no `iommus`; the 0xae8f000 "sid" block is the security SID
config). A flip therefore only needs the surface's physical base, not a display-SMMU mapping.

## The panel runs at 120 Hz

The boot loader's DSI timing node (`qcom,mdss_dsi_g7a_37_02_0a_dsc_video`, the panel the kernel
command line selects) declares `panel-framerate = <0x78>` (120), `panel-width = 0x438` (1080),
`panel-height = 0x960` (2400), DSC video mode. So the SDE latches flushes at 120 Hz (8.33 ms), not
60: the 60 Hz in the driver is only the software vsync *notification* rate (`sheng-vsync` default),
and the compositor's own heartbeat. A flip that retires at the next frame boundary can pace the
compositor at up to 120 Hz; the per-frame work (compositor render ~10 ms) is what decides.

## Geometry: 1080x2400, not 1080x2340

The compositor's drawable was 360x780@3x = 1080x2340 while the panel scans 1080x2400, so the driver
letterboxed 60 rows. That mismatch makes a direct scanout impossible (the bottom 60 rows would be
undefined). `scripts/make-picasso-adt.py` now writes `display-height = 2400` (and
`artwork-device-subtype = 2400`), i.e. 360x800@3x, exactly the panel; the A/B is verified in the
built ADT (wf335: `display-height` value 0x960, wf333: 0x924), and wf335's compositor submits frames
at that geometry. The compositor's stride is 4352 (1088 px padded), the GOP's is 4320; the flip
programs `SSPP_SRC_YSTRIDE0` with the surface stride.

## The flip (`sheng-sde-flip=1`)

In `kickOnce`, before the GPU offer, the driver asks `sdeFlip()`:

1. exact geometry: `sourceWidth == panelWidth && sourceHeight == panelHeight` (anything else falls
   back to the existing copy paths);
2. stride is 4-byte aligned and at least `width*4`;
3. the surface is **physically contiguous** for `(height-1)*stride + width*4` bytes, obtained with
   `IOMemoryDescriptor::getPhysicalSegment(0, &segment, 0)`; a non-contiguous surface falls back;
4. the base fits in 32 bits;
5. writes SRC_SIZE/OUT_SIZE, SRC0_ADDR, SRC_YSTRIDE0, then `CTL_FLUSH = BIT(0)` (VIG0's flush bit in
   the DPU encoding); the hardware latches the update at the next frame boundary.

The swap is retired as the GPU path's early retire does, and `transferred` stays false, so the
compositor is never blocked by a copy.

Open questions the first boot answers: whether CA's surface is physically contiguous (the skip
print reports `seg` vs `needed`), whether its format matches the BGRA8 the pipe expects, and whether
single-buffer latching tears under swipes (the compositor's drawable rotation should cover it; if
not, the next step is two surfaces plus a flip on the software vsync tick).

## Files

- `drivers/NativeMobileDisplay/NativeLegacyDisplay.cpp`: `sdeProbe()`, `sdeEnsureMapped()`,
  `sdeFlip()`, the flip branch in `kickOnce()`.
- `scripts/make-picasso-adt.py`: `display-height`/`artwork-device-subtype` 2340 -> 2400.
- Images: wf335 = probe + 2400-row ADT (kernel 12d3b199); wf336 = probe + flip + 2400-row ADT.
