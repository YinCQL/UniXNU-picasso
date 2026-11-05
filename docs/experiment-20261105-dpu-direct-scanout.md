# Zero-copy scanout: the DPU fetches the compositor's surface through the display SMMU

Status: in progress. The CDC warm-reboot bug that blocked autonomous iteration is fixed (wf338);
the display SMMU bank is measured (wf338); the direct scanout port is in wf339.

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
