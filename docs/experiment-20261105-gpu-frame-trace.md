# GPU frame-trace experiments, 2026-11-05

Goal (GPU优化.txt): find where the ~90 ms between frames goes and eliminate it, then the freezes.
One variable per image; every run boots the same image twice and records measured values.

## Stage 1 — wf292 second boot (no code change)

Purpose: close the IR3 cache acceptance from `handoff-20261105-gpu-next-agent.md` and establish the
current frame pacing.

Image `out/wf292/picasso/boot/wf292.img` (kernel `2e606fb6…`, addon `b70d0fb1…`, trust `75bea288…`,
659 B args, drop path). Evidence: `/tmp/wf292b-probe.log`.

| Measurement | Value |
|---|---|
| `PICASSO_M2V dir` | `files=1154 bytes=16674043` (first boot: 1144 / 16,360,988) |
| `PICASSO_M2V import` | `files=0 skipped=1154 bytes=0` — every exported entry already on disk with the same size |
| `PICASSO_M2V export` | `total=16868211 blocks=4120` (first boot ended 16,857,490 / 4117) → converged, +10,721 B |
| AMFI `code signature validation failed` | 0 |
| GPU plugin opens | 4 (backboardd ×2, SpringBoard ×2) |
| `PICASSO_GPU submits live 5` | `submissions=2089 ibs=8908 max=43 many=65 opens=4` |
| Display (steady state) | `submit=accept=transfer=retire`, `reject=0`, `copies=0` |
| Frame rate | submit 1047→1147→1247 per 10.0 s = **10.0 fps** (100 ms/frame) |
| Present cost (PF) | last 9 ms, avg 9 ms: import 44 µs, commit 7.06 ms, GPU wait 2.58 ms |
| `PICASSO_DISPLAY_GPU` | `offers=1151 done=1151 fail=0 abandoned=0 waits=1448 wait_to=296` (wait_to fixed after the UI starts) |
| Stalls | 0 `PICASSO_GPU stall:` lines; hangcheck fence advances (fence 220, retries 0) |
| `ps` (20 s samples) | busy: MusicWidgets ≈100% of a core, StocksWidget ≈100%, launchd ≈93%; backboardd ≈30%, SpringBoard ≈1% |

**Conclusion.** The IR3 cache survives and converges; the GPU path serves every frame at ~9 ms
(7 ms encode+commit, 2.6 ms wait). The frame interval is 100 ms, so **91 ms of every frame lies
outside the GPU present path** — the compositor offers a frame only every 100 ms while the vsync
source runs at 60 Hz. Nothing in the display/GPU path is stalling (0 stalls, no timeouts in steady
state).

Open question for stage 2: are the compositor's Metal command buffers committed every 100 ms (frame
production is CPU/CA-limited), or committed more often while only one in six reaches the display
(handoff/presenter defect)?

## Stage 2 — wf293: kernel-only measurement probes

Variable: instrumented kernel, same addon/trust (no plugin rebuild). Two new probes, both reusing
existing data paths:

1. `picasso-frame-trace=1` (boot arg, default off) — one line per completed GPU-served frame from
   `NativeLegacyDisplay`:
   `PICASSO_FRAME n=<i> offer_ns=<> done_ns=<> svc_us=<> gap_us=<>`
   (`offer` = swap reached the GPU client, `done` = copy complete, `gap` = offer − previous done).
2. CDC `gpu-table` — dumps the Metal bridge's 48 telemetry rows (the table the diagnostics page
   renders) into the boot log: per-command-buffer commit times and commit-to-callback (`Q`/`K`/`E`,
   `G`, `J`), pipeline/library/function compile totals (`T`), Vulkan pipeline and **shader-cache
   hit/miss/write** counts (`V`), surface/copy counters (`N`, `S`).

Acceptance for wf293:

- `PICASSO_FRAME` line count ≈ display submit count (±1%); `gap_us` p50/p95/p99 derivable.
- `PICASSO_GPU_TABLE` contains the `V cache` row and ≥10 `K` rows with strictly increasing `t=`.
- No new stalls, no AMFI rejection, display counters still `submit=accept=transfer=retire`.
- Without `picasso-frame-trace` the image prints no `PICASSO_FRAME` lines and behaves like wf292.

Rollback: `fastboot boot out/wf292/picasso/boot/wf292.img`.

## Stage 3 — unattended UI stimulus (synthetic touch)

Goal: drive the UI without a human, inside the same ≤5 min powered window. The existing
ShengHidInput machinery already synthesizes a bottom-edge swipe every 20 s
(`offline-hid-swipe=<n>`): the probe attaches at 60 s, so swipes land at 60, 80, … and the ones
after SpringBoard takes the panel (measured below at ≈240 s of monitor time) exercise the UI.

Two blockers found and handled:

1. The payload builder strips `offline-hid-swipe=16` from `board.toml`, so it must be passed as an
   extra kernel argument.
2. `PICASSO_HID swipe unavailable started=1 digitizer=0 sink=1` — the swipe path needs the
   combined digitizer device (`sheng-touch-hid=1`); without it the probe refuses to arm.
3. The real Novatek panel reports a phantom bottom-edge contact at startup
   (`PICASSO_NVT point x=5210 y=23990`) and claims the shared touch sink
   (`PICASSO_HID physical contact; automatic swipes stopped`). `offline-hid-swipe-force=1` keeps the
   unattended sequence alive; it is opt-in and default-off.

Automation schedule per boot (monitor clock, measured): HID probe 60 s, backboardd GPU frames
~87 s, apps mount ~125 s, SpringBoard GPU open ~240 s. Swipes at 60..360 s therefore cover the
boot UI and the first 2–3 gestures after SpringBoard starts. The capture window is ≤260 s from
port open and ends with `reboot-fastboot`, so the device is never powered more than ~5 min.

## Stage 4 — first timeline result (wf293, `picasso-frame-trace=1`)

The trace ran on wf293 (kernel `3dea0159…`, no touch digitizer). A late capture window
(`/tmp/wf293-reboot3.log`, 137 s) held the steady UI:

| Metric | Value |
|---|---|
| frames | 1371 |
| interval | p50 **99.98 ms**, p95 101.41, p99 101.96, max 102.89, mean 100.00 |
| service (offer→done) | p50 **9.96 ms**, p95 27.57, p99 28.32, mean 11.37 |
| gap (offer − previous done) | p50 **90.00 ms**, p95 91.57, p99 92.11 |
| per-10 s frame counts | exactly 100 every bucket for 13 buckets |
| display counters | `submit=accept=transfer=retire`, `reject=0`, `wait_to` stops growing |

Interpretation (user): the 100 ms interval is the **clock app icon's own update rate**, not a
compositor limit — the screen was static otherwise, so CA only presents when content changes.
The GPU path itself needs 10 ms per frame and could serve far more. Therefore the 90 ms "gap" in
a static screen is expected, and the number that matters is the frame cadence **during a gesture
animation**. Stage 5 measures exactly that with the synthetic swipe (the phone must be observed
while the gesture runs, not on an idle screen).

## Stage 5 — wf295: timeline during synthetic swipes

Kernel `29f21e4f…` (adds offer→take→complete→done decomposition to PICASSO_FRAME, configurable
swipe interval, USB 500 mA request). Payload args (797 B):
`picasso-frame-trace=1 offline-hid-swipe=16 offline-hid-swipe-force=1 sheng-touch-hid=1
offline-hid-start=180 offline-hid-swipe-interval=8`.

Result: **the capture missed the window** — the CDC port disconnected exactly when the delayed
capture opened it (the kernel had been writing PICASSO_GRID rows with no host draining; opening a
stale port triggers the gadget reset), so `/tmp/wf295-probe.log` is empty and the user saw no
swipe. Lesson: (a) start the capture before/at boot so it opens the port as it enumerates;
(b) a drained raw read (`os.read` without touching modem lines) proves the console is alive even
when the capture tool sees a disconnect; (c) the 5-minute battery budget is counted **from the
lock screen**, not from boot, so a capture may run ~9 minutes total.

Second wf295 attempt split stage-1's assumptions: the physical Novatek panel reports a phantom
bottom-edge contact and owns the shared sink, so the plan for the next image is to keep the
physical probe from starting at all (`sheng-touch-start=100000`) and let only the synthetic
combined digitizer publish.

## Stage 6 — wf296: gestures on the lock screen, physical panel disabled

Kernel `29f21e4f…`, args (823 B): `picasso-frame-trace=1 offline-hid-swipe=30
offline-hid-swipe-force=1 sheng-touch-hid=1 offline-hid-start=200 offline-hid-swipe-interval=10
sheng-touch-start=100000`.

One capture from before boot (`--wait 150 --duration 545`), `gpu-table` at port+120/230/340/450,
`ps` at 280, and `reboot-fastboot` at 490. Swipes run on the monitor clock at 200, 210, …,
490 s, so ~23 gestures land on the lock screen after it appears (≈240–270 s). PASS requires
`PICASSO_HID swipe armed sink=1 count=30` and `swipe N moved=1` lines, and a frame-trace burst
inside each gesture window. A `swipe unavailable` or `moved=0` line names which half (device
creation vs. sink publication) failed.

### wf296 result

- `PICASSO_HID swipe armed sink=1 count=30`; every gesture `moved=1 reports=+26 dispatch=+26`
  — the reports are published and dispatched to the HID stack.
- **The UI never reacted.** The lock screen was up (frames at 479.98 / 539.98 / 599.98 s — one
  per minute, the lock-screen clock), but no swipe produced a frame burst, and the user saw no
  change on the panel.
- Telemetry stayed healthy the whole run: `V cache hit48 miss0`, `G commit-to-complete
  mean=16.4 ms`, 19 commits, no stalls, no AMFI rejection.
- Differences from the 2026-11-01 configuration that did unlock the panel (hid10, user-confirmed):
  hid10 kept the **physical Novatek touch driver** running and attached the combined HID device
  at **t=540 s**, after the lock screen; wf296 disabled the physical driver
  (`sheng-touch-start=100000`) and attached at 200 s. The synthetic reports are dispatched
  internally in both cases, but the window server only appears to consume them when the physical
  digitizer service exists.

## Stage 7 — wf297: hid10 recipe (physical touch on, late attach)

Args (798 B): `picasso-frame-trace=1 offline-hid-swipe=25 offline-hid-swipe-force=1
sheng-touch-hid=1 offline-hid-start=300 offline-hid-swipe-interval=10`. Physical Novatek probe
runs at its default 60 s; the combined device attaches at 300 s (lock screen is up by ≈250 s);
swipes every 10 s from 300 to 540 s. Capture 520 s from before boot, `gpu-table` at
port+140/260/380/460, `ps` at 320, `reboot-fastboot` at 480.

PASS: the user sees the lock screen unlock and/or the frame trace shows bursts (expected
≈16.7 ms intervals) at the gesture times. FAIL: same `moved=1` with no UI reaction → the
synthetic digitizer's events are not admitted by the window server in the GPU era and the touch
path itself needs its own investigation.

### wf297/wf298 result and the touch finding

- wf297 (physical Novatek driver on, HID attach at monitor 300 s): `swipe armed sink=1`, every
  gesture `moved=1 reports=+26 dispatch=+26`; the user-visible lock screen did **not** move.
- The **physical finger works normally on the same screen** (user-confirmed). Physical and
  synthetic contacts share one publisher (`native_touch_*` snapshot) and one HID device
  (`NativeHidInput`), and the event driver opens it (`NativeHidInput:… open by IOHIDEventDriver`),
  so the device path is admitted; only the synthetic *reports* are not acted on. That is a
  touch-path bug of its own, not a GPU defect.
- wf298 (clean `picasso-frame-trace=1`, no touch automation) recorded the compositor during
  SpringBoard startup. **This is the first animation data of the session**:

| | idle frames (1671) | burst frames (395, interval <60 ms) |
|---|---|---|
| service p50 | 9.8 ms | 33.8 ms |
| take (offer→client) | 0.13 ms | 0.83 ms |
| **present (client)** | **9.7 ms** | **32.6 ms** |
| completion handling | 0.01 ms | 0.04 ms |
| interval | 99.8 ms | 15–45 ms (histogram peaks 15–20, 20–25, 30–40) |

  The PF breakdown shows the variable part is the **GPU wait**, not the CPU encode:
  import ≈ 40 µs always, commit (encode+commit) ≈ 6.0–6.3 ms always, wait ≈ 2.5 ms idle but
  **10–33 ms during animation**. Burst runs reach 147 and 64 consecutive frames at ≈35 ms
  (≈28 fps), with 25 frames at 15–20 ms (≈55–65 fps).
- Conclusion: while the screen is static the GPU is idle and the 90 ms gap is content-driven;
  **during animation the GPU is the bottleneck** (present wait 26 ms + CA's own commit-to-complete
  mean 16–22 ms), capping animation at ≈28–35 fps. This refines GPU优化.txt's "GPU idle 96%":
  the average is idle, but every animated interval is GPU-bound.

## Stage 8 — wf299/wf300: GPU clock A/B during the same boot animation

One variable: `sheng-gpu-perf-index`. The HFI log on this unit is
`perf gpu=5 gmu=2 votes=5 bin=84` — **speed bin 0x84**, whose table is 625 / 500 / 400 / 275 MHz
(the generic DT 275–750 table does not apply). Default is index 3 = 500 MHz; index 4 = 625 MHz
(the bin's top). Index 7 is out of range and is silently ignored (`boot index=7 nominal`), which
made wf299 a **repeat of the 500 MHz baseline** and gives the run-to-run variance for free.

- wf298 (index 3, 500 MHz): burst present 33.8 ms (take 0.83, present 32.6), idle present 9.9 ms;
  PF commit p50 6.94 ms, wait p50 2.52 ms; burst runs 147/64 frames at ≈35 ms.
- wf299 (index 7 rejected → 500 MHz): repeatability check. Result: **the startup animation did not
  reproduce** — the boot reached SpringBoard ≈4.6 min in (vs ≈4.0 min for wf298) and produced only
  10 frames over the remaining 4 min of the capture (a static lock screen), no burst runs. The LS
  rebuild makes the boot-phase animation workload non-deterministic, so the boot animation is not a
  usable A/B workload.
- wf300 (index 4, 625 MHz) was built but not run: without a repeatable animation the comparison
  could not attribute a difference to the clock. The controlled-workload conclusion is that a
  repeatable stimulus is required; the synthetic swipe is the intended one (blocked by the touch
  issue above), and a manual finger swipe would also do it.
- What the A/B did establish: the HFI log names the speed bin (`bin=84`, levels 625/500/400/275)
  and silently ignores out-of-range indices, so any future clock experiment must use index 4 and
  must be judged on a repeatable animation, e.g. the idle present's GPU wait (2.5 ms at 500 MHz)
  or a gesture burst.








## Stage 9 — wf301: Home-button discriminator for the touch device

The physical finger works and both physical and synthetic contacts share one HID device, so the
next question is whether the device's **non-digitizer** reports are consumed. `offline-hid-auto-home`
sends Consumer/Menu (Home, report 2 bit 0) through the same device's keyboard collection.

Args (793 B): `picasso-frame-trace=1 offline-hid-start=250 offline-hid-auto-home=300
offline-hid-auto-home-count=8 offline-hid-auto-home-interval=20`; capture 560 s from before boot,
`gpu-table` at port+150/320/440.

- Home presses (`PICASSO_HID home report down=… value=…`) starting at monitor ≈300 s.
- PASS (device consumed): the UI reacts — app switcher/home animation, frames burst.
- FAIL (device reports ignored): no reaction at all → the HID device is not what delivers the
  working physical touch, and the physical path must be re-traced (the shared-sink assumption is
  wrong).

**Result — PASS.** One Home press produced `PICASSO_HID home report down=1 value=1 result=0` at
log line 5362 and frames jumped 9 → 234: **218 frames followed the press**. The device's
non-digitizer reports ARE consumed by the UI, so the HID device path (device → event driver →
backboardd → SpringBoard) is sound and only digitizer contacts are dropped — that narrows the
synthetic-touch bug to the touch-specific admission path, not the transport. It also gives the
autonomous animation stimulus the clock A/B needed.

## Stage 10 — wf302/wf303: GPU clock A/B on Home-press animation

Same image as wf301 except `offline-hid-auto-home=60` (presses at attach+60 s, then every 20 s;
8 presses clamped by the probe's `count>8` limit). Both runs use the identical capture: port open
from before boot, `gpu-table` at port+150/320/440, `reboot-fastboot` at 455/465 s.

### wf302 — index 3, 500 MHz (baseline)

HFI log: `initial perf index=3 (nominal)`. 2437 frames, 16 `home report` lines = 8 press+release
pairs; every press was followed by an animation burst (windows after each `down=1` line):

| press (kernel s) | frames | fast intervals | fast p50 |
|---|---|---|---|
| 300 (wake) | 259 | 80 | 20.1 ms |
| 375..496 (7×) | 228–232 | 41–46 | 33.3 ms |

- burst frames (interval <60 ms): n=381, service p50 **10.0 ms**, take 0.22, present 9.8,
  completion 0.01 ms — **identical to idle** (9.9/0.14/9.7).
- PF while presses run: commit p50 6.92 ms, **wait p50 2.51 ms, max 16.24 ms**.
- Thus the Home animation is paced at 30 fps by the compositor's own offer cadence
  (interval 33.3 ms, ~23 ms of it *after* the previous present finished = gap), not by the GPU:
  per-frame GPU work is the idle 10 ms. This contrasts with the wf298 SpringBoard launch bursts
  (present 33.8 ms, GPU wait 26–33 ms): heavy multi-layer animations are GPU-bound, the simple
  Home animation is CA-paced.

### wf303 — index 4, 625 MHz

Same capture; only `sheng-gpu-perf-index=4` added. HFI: `initial perf index=4 (dt level=3 of 5, boot
index=4 from boot arg)` — the 625 MHz clock was applied.

| | wf302 (500 MHz) | wf303 (625 MHz) | delta |
|---|---|---|---|
| burst frames | 381 | 391 | — |
| burst service p50 | 10.0 ms | 9.7 ms | −0.3 ms (3%) |
| burst present p50 | 9.8 ms | 9.5 ms | −0.3 ms |
| fast interval p50 | 33.0 ms | 33.0 ms | 0 |
| PF commit p50 | 6.92 ms | 6.90 ms | 0 (CPU) |
| PF wait p50 / max | 2.51 / 16.24 ms | 2.40 / 16.10 ms | −0.11 ms |

All 8 Home presses were consumed in both runs with the same frame counts per window (±2%). The
25% clock increase bought 4% of the GPU wait and nothing of the cadence: the 33 ms burst interval
is the compositor's animation rate, not a GPU limit. **The clock is not the lever for this
workload**; per-frame GPU work is ~2.4 ms at either clock.

## Stage 11 — wf304/wf305: where the commit span goes, and non-blocking present

One plugin build (`adreno-metal-plugin` rebuilt; owner trust `9a8c6509…`, addon `c1a8c2b3…`,
CDHash `71d7e9b7…`), same kernel `29f21e4f…`, same args as wf303. Two changes, both off by
default except the new PF fields:

- The PF line now carries `b<create> e<encode> u<submit>` next to `i`/`c`/`w`; `c = b + e + u`.
  (`NativeLegacyDisplay` line; the bridge's text stream truncates at 63 characters, so `u` is
  derived as `c − b − e` — the first run's `e3` was a truncated `e3024`.)
- `sheng-present-nonblock=1`: commit the scanout blit and return without the completion wait.

### wf304/wf305 — split (wf305's lines are complete; the split does not depend on the wait mode)

| field | p50 | p90 | max |
|---|---|---|---|
| import | 0.040 ms | 0.065 | 0.092 |
| command-buffer creation (`b`) | 0.032 ms | 0.052 | 0.083 |
| **encode (`e`)** | **2.95 ms** | 3.05 | 25.9 |
| **submit (`u = c − b − e`)** | **4.02 ms** | 4.21 | 24.7 |
| commit (`c`) | 7.03 ms | 9.07 | 40.8 |
| GPU wait (`w`) | 0 (non-block) / 2.32 (wf304) | — | 15.1 |

The Metal-side encode is not free: **3 ms to create the linear texture over the surface pages and
encode the one copy** — m2v's `newTextureWithDescriptor:offset:bytesPerRow:` path (a 2D texture over
a buffer is a linear image where the device lays one out as Metal does, else a shadow image the
encoders copy through; Turnip takes the fallback — the DRM-modifier warning in the same telemetry).
The submit call is another **4.0 ms**, of which the kernel's per-submission write-combine
re-assertion is the next suspect (wf306 adds its cost to the 30 s console line).

### wf305 — non-blocking present

PF `w0` on every frame; present service 9.8–9.9 → **7.3–7.5 ms**, and the animation cadence is
unchanged (burst interval p50 32.7 vs 33.0 ms, same press windows). The 2.3 ms wait is therefore
off the frame's critical path at this pacing — it is wasted CPU, not a pacer. AMFI clean, plugin
opened 4×, no display failures (`failures=0`).

### wf306 — kernel-side submit attribution (result)

Kernel `143f7526…` adds `wc_us` / `wc_reset` / `cache_us` (cumulative) to the 30 s
`PICASSO_GPU submits live` line: time in `NativeAdrenoReassertWriteCombined`, pages it reset, and
cache-maintenance time. Same addon/trust/args as wf304.

**The write-combine re-assertion is not the cost: `wc_us=0 wc_reset=0` for the whole run** — the
submitted BOs are not on the write-combined path, so the walk never runs. Cache maintenance is
~1.0 ms per submission (`cache_us` 3.52→4.12 s over 600 submissions between two samples); the
rest of the 4.0 ms submit is in m2v/Turnip user space (scheduler hand-off and the inline
`vkQueueSubmit`; FAST-PATHS documents that handing the submission to the scheduler thread "is not
built").

### What the present path costs, and what it does not

Per frame at either clock: import 40 µs + create 32 µs + **encode 2.95 ms** (linear-texture
creation over the surface + one copy; Turnip takes m2v's shadow-image fallback) + **submit
4.0 ms** + wait 2.3 ms (removable with `sheng-present-nonblock=1`). None of it paces the UI: the
idle screen updates at 10 fps by content, animations at 33 ms by CA's own schedule. The measured
levers, in order of size, would be m2v's texture-over-buffer encode and its inline submission —
both outside this tree — not the kernel, not the GPU clock.
