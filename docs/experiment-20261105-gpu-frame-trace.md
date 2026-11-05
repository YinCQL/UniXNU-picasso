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

