# GL route renderer optimisation, 2026-11-07

The OpenGL ES route (CoreAnimation's EAGL renderer on Mesa 25.0.7 freedreno/ir3, `libNativeGL`)
went from ~38 fps to ~50–62 fps on the home screen this session, with zero-copy direct scanout
(`sheng-dpu-scanout=2`, `kind=3` flips) unchanged. This note records what was measured, what was
fixed, and what was tried and rejected.

## Configuration

Boot arguments (996 bytes): `sheng-dpu-scanout=2 sheng-gpu-perf-index=4 sheng-gpu-gl=1 glwait=0`
plus, from gpu57 on, `glhooks=1`. `sheng-touch-hid=1` is no longer passed: `NativeHidInput`'s
`touchEnabled` defaults to true when the argument is absent, and the 16 freed bytes go to
experiment arguments under DarwinBoot's 1000-byte limit (`scripts/build-wf-payload.py` now strips
it from the board text).

## The renderer-side cost that dominated frames

Probes in `eagl.c` (kernel text channel): `eagl f/i/r/R/w/d/c` per 20-frame beat, `eagl top
<slot> <ms>/<calls>`, `eagl fph <clear>/<flush>/<end>`, `eagl uh <uploads>/<KB> <hashes>/<KB>`,
`eagl htop s<id> <KB> <calls> q<quiet> n<next_check-frames> t<changed> l<lock-fails>`.

- `glBindTexture` was the top slot at ~190 ms per 20-frame beat (~10 ms/frame). `eagl uh` showed
  0.2–0.6 MB of uploads per beat but **~535 MB of surface hashing** (65 hash calls/frame): the
  per-frame "has this surface changed" sum over non-shared shown surfaces (1080x2400 wallpaper,
  app snapshots). The hashes always found them unchanged, so they were pure CPU cost.
- An adaptive backoff was added (`quiet`/`next_check`: 2, 4, then every 8th frame for a surface
  that has not changed; a surface seen to change is checked every frame again). The first version
  only re-armed `next_check` while `quiet < 3`, so it delayed hashing by 8 frames once and then
  resumed every frame (probe: `htop s25 181160K 20 q3 n-549`). Re-arming on **every** unchanged
  check fixed it: `q3 n6`, 2–3 hashes/beat, hash volume 535 → 100–136 MB/beat.
- `image_new` now preserves `sum`/`quiet`/`next_check` when the same IOSurface is registered
  again (CoreAnimation calls `-texImageIOSurface:` more than once).

## Measured

Same desktop scrolling, active beats only (`i > 0`, `R >= 10`, `d >= 60`):

| image | what | fps median | fps max | r median | draws | calls |
|---|---|---|---|---|---|---|
| gpu49 | hash bug present, no hooks | 37.7 | 58.1 | ~20 ms | — | — |
| gpu55 | backoff fixed, full blit | 49.6 | 61.2 | 13 ms | 109 | 508 |
| gpu56 | + damage-only blits | 45.7 | 62.3 | 17 ms | 108 | 549 |
| gpu57 | + `glhooks=1` | **52.0** | **62.3** | **12 ms** | 102 | 490 |
| gpu58 | + `sheng-gpu-bw-table=1` | 50.8 | 60.6 | 15 ms | 109 | — |

Remaining top slot is `glFlush` at ~5–8 ms/frame (gpu57: 133–145 ms per beat); `frame_end`
(mirror blit + lock/unlock) is 1.2–1.4 ms/frame; p90 frame time is 21 ms, so ~10 % of frames
miss the 16.7 ms vsync budget while the median sits at 52–62.

## Rejected

- **`glblit=0`** (draw the frame into the scanned-out surface itself; `eagl at ... sh1 mr0`):
  the surface is linear/non-tiled for the SDE, so GPU fill is worse — 27.9 fps median, r 28 ms —
  and the first boot stalled. The mirror texture + one GPU blit is the better trade.
- **Damage-only mirror blits** (`gldamage=1`, scissor-box union tracked in `drawing()`):
  45.7 fps median vs 49.6 for full blits; the blit is not what the flush waits on, and the
  per-draw scissor queries cost more than they save. Default is off; the code stays behind the arg.
- **GPU bandwidth votes**: `sheng-gpu-bw-table=1` builds the 13 DTS grp3d levels through
  `AdrenoHfiBuildBandwidth` and the GX vote now carries the level's own index
  (`PICASSO_GPU hfi: bw table levels=13 ...`, `PICASSO_GPU gx: vote status=0 ... perf=4 bw=10`).
  Accepted by the GMU, but no fps change: the UI is not DDR-bandwidth-starved at 6.22 GB/s.
  Kept (correct behaviour), not a performance lever.

## Fill measurement (inconclusive)

`glfill=1`/`glf=1` sums each draw's viewport ∩ scissor. At boot the viewport is full screen and
the scissor is off; while scrolling the sum is ~243 MPx/frame — physically impossible for this
GPU — because CoreAnimation bounds its quads by their **geometry** (layer rects), not by scissor
or viewport. Attribution by "the last function CoreAnimation set uniforms for" is also wrong:
`function_uniform` is only called for a few functions, so the byte sticks at 61 ("edr", the
snapshot pass) and absorbs ~90 % of the draws. Measuring real fill needs GPU hardware counters
(RBBM/ROP output pixels), a kernel-side task.

## Files

- `temp/gpu-ios16/src/NativeGL/eagl.c`: hash backoff, image_new preservation, damage-blit support
  (off by default), fill probes (off by default), heartbeat probes.
- `scripts/build-wf-payload.py`: `sheng-touch-hid=1` stripped from the board text.
- Images: `out/gpu55..gpu60`; logs: `temp/runs/gpu5*.log`, `temp/runs/gpu56c.log`, `glab.log`.

## 2026-11-07: solid text blocks -- the IOSurface table ran out (gpu69/gpu71)

Long text (Calendar's lunar labels and many date runs) came out as solid rectangles of the text
colour: black for black dates, red for the red ones. The shader is CoreAnimation's glyph pass
("vec4(t0.a)" -- the mask is the sample's alpha) over an A008 surface, so an unreadable mask means
the texture was never uploaded. The library registers every IOSurface CoreAnimation shows in a
fixed table (256 entries) and -texImageIOSurface: returned NO once it was full: the surface's
texture answered (0,0,0,1) and every glyph quad became opaque. The home screen stays under the
cap; Calendar's month view (a surface per label) went past it -- gpu69's log reads `eagl inew ...
n254..255`, with the blocks on screen. The table is now 1024 entries with least-recently-used
eviction instead of refusal (never a target, never a texture bound this frame). gpu71 runs the
same app at n=291..293 with the text correct, and the home screen keeps gpu57's rate (16.6 ms
median, 60 fps, best 84 fps), so the fix costs nothing.
