# Idle CPU anomaly: the audiomxd ↔ launchd message storm

Status: **root cause measured on hardware (wf312–wf314); mitigation boot wf315 in progress.**

## Question

The home screen is very hot with the phone sitting still, and swiping the home screen runs at
33–42 fps on an eight-core SM7250. Earlier GPU work had bounded the per-frame present cost, so
the remaining question was where the CPU time goes at idle.

## Answer (measured)

**audiomxd sends launchd ~1,200–2,200 Mach messages per second and launchd replies to every one.**
launchd then holds ≈1.1 core of *system* time, and audiomxd another ≈0.24 core of user+system
time, continuously, from early boot on. Every other process in the system sends pid 1 single-digit
messages per second.

Two smaller consumers sit on top of this when the home screen with widgets is displayed:
MusicWidgets and StocksWidget together take up to ~2 cores of user time while their pages are
visible (0 % when off-screen), and backboardd ≈0.5 core while compositing.

## Evidence

### 1. Per-process cumulative CPU (CDC `ps`, wf312)

`PICASSO_PS top` prints `u=`/`s=` in **milliseconds** of cumulative CPU (`picasso_task_usage` →
`absolutetime_to_nanoseconds` → `/1e6`). Two samples 120 s apart:

| process | CPU at sample 3 | CPU at sample 4 | delta / 120 s | cores |
|---|---|---|---|---|
| launchd (pid 1) | 1,303,101 ms | 1,435,255 ms | 132,154 ms | **1.10** |
| backboardd | 113,457 | 176,643 | 63,186 | 0.53 |
| audiomxd | 253,878 | 282,409 | 28,531 | 0.24 |
| logd | 112,567 | 119,798 | 7,231 | 0.06 |
| SpringBoard | 41,263 | 48,601 | 7,338 | 0.06 |

launchd's split is ~62 % system time, i.e. it is in the kernel, not in its own userland code.

### 2. Kernel sampling profile (wf312)

launchd's hottest kernel PCs are the Mach IPC paths (`ipc_mqueue_send`/`ipc_mqueue_receive`,
`ipc_port_alloc_special`, `ipc_kmsg_alloc`), the in-kernel **sandbox kext** (`com.apple.security.sandbox`
`__TEXT_EXEC`, mapped through `out/picasso/kernel/kernel.json` extents), `kevent_register`, and
locks. That is a process serving a continuous message stream, with a sandbox evaluation per
message.

The CPU-frequency governor's own line agrees about the magnitude: at idle with the storm running
(wf313, 19:12) the two A76 performance domains were **71–81 % busy at 2.08–2.19 GHz** (d1) and
**65–77 % at 2.20–2.35 GHz** (d2), the little domain 36–60 %. That is the heat source.

### 3. Live thread dump (CDC `wait 1`)

launchd runs with three threads: the main thread blocked in `__sigsuspend_nocancel`, and
libdispatch workqueue threads (`__pthread_wqthread`/`_start_wqthread`). The workqueue thread PCs,
resolved against the device dyld shared cache (`dyld_shared_cache_arm64` on the SystemOS cryptex,
`.symbols`/`.dyldlinkedit`), are `__sandbox_ms+0x8`, `_read$NOCANCEL+0x8`,
`___open_nocancel+0x8` — the message handler performs sandbox checks and file reads per message.

### 4. IPC census (new kernel probe, wf313/wf314)

Boot argument `picasso-ipc-census=1` enables bounded per-peer counters at the one place every
Mach send reaches with the destination port locked (`ipc_kmsg_send` →
`ipc_mqueue_send_locked`). The profile block prints per-round (30 s) deltas:

```
IP to1 d=65727 93:audiomxd=65502 107:chronod=177 26:runningboardd=174 ...
IP from1 d=65716 93:audiomxd=65502 ...
IP ids 40000324=65607 40000191=638 48=563 77303074=408
```

- `IP to1` = messages delivered to pid 1 this round, ranked by sender pid+name.
- `IP from1` = messages sent by pid 1 this round, ranked by receiver.
- `IP ids` = the Mach message ids pid 1 received, ranked by delta.

Across wf313's steady-state blocks audiomxd = 67,292 / 57,977 / 54,804 of 67,484 / 58,829 /
55,066 messages per 30 s (**1,827–2,243 msg/s**), and launchd answered each one (`from1`
audiomxd ≈ `to1` audiomxd). wf314, after adding the message-body sampler, sampled 24 consecutive
messages to launchd: **all 24 from audiomxd (pid 93), `msgh_id=0x40000324`, `size=188`**, body
binary (no printable runs). The id census confirms `0x40000324` is 65,607 of 65,727 messages.

So the storm is a single XPC request/reply loop between audiomxd and launchd, message id
`0x40000324` (XPC routing-id range `0x40000000|…`), not bootstrap name lookups (those would carry
a printable service name in the body).

## Probe

Patch `patches/picasso-ipc-census-xnu.patch` (applied through the prepared tree's
`patches/xnu/`; `osfmk/ipc/ipc_kmsg.c`), gated by boot argument `picasso-ipc-census=1`:

- `picasso_ipc_census_note(port, kmsg)` — called in `ipc_kmsg_send` before
  `ipc_mqueue_send_locked`, while the port lock is held. Counts per peer pid into 512 buckets for
  both directions of pid-1 traffic, plus a message-id histogram, plus (first 24 after two minutes)
  a `PICASSO_IPC msg s=… from=… to=… id=… size=… body=…` sample with printable runs from the body.
- `picasso_ipc_census_rows()` — called from `boot_progress_profile_rows()` (driver
  `BootProgress.c`, weak import) so the rows appear in every 30 s profile block on the CDC console.

## Mitigation (wf315): audiomxd disabled

`sheng-disable-jobs=com.apple.cameracaptured,com.apple.audiomxd` on the kernel command line (the
board's builtin switches are read after the command line, which wins; `BootWatchdogPolicy.c` writes
the disabled.plist). Same kernel (`a5ce6d42…`), same GPU pair (`50d38150…`/`01c2d018…`), census on.

| metric (30 s census block) | wf313/wf314, audiomxd running | wf315, audiomxd disabled |
|---|---|---|
| messages to launchd | 55,066 – 67,484 (1,830 – 2,250 /s) | 2,892 at boot; **247 then 106 idle (8 then 3.5 /s)** |
| of which `0x40000324` | 65,607 (of 65,727, one peer) | 1,142 at boot, spread over all clients; absent at idle |
| top sender | audiomxd, ~2,180 /s | SpringBoard 18 /s at boot; chronod/contextstored ≈ 5 /s idle |
| launchd CPU, cumulative | 240.9 s at 3.5 min (wf313), 264.4 s at 6 min (wf314) | 34.9 s at 4.5 min |
| launchd steady Δ between `ps` samples | 132.2 s / 120 s = **1.10 core** | 1.95 s / 60 s = **0.032 core** |
| A76 domains (governor line, idle) | 71–81 % busy @ 2.08–2.19 GHz (d1), 65–77 % @ 2.20–2.35 GHz (d2) | **2–4 % @ 0.92–1.06 GHz** (d1/d2) |

Per-process deltas over the last 60 s idle window in wf315:

| process | Δ / 60 s | cores |
|---|---|---|
| MusicWidgets | 60,313 ms | 1.005 |
| StocksWidget | 59,690 ms | 0.995 |
| backboardd | 7,352 ms | 0.123 |
| SpringBoard | 3,173 ms | 0.053 |
| **launchd** | **1,946 ms** | **0.032** |
| chronod | 1,426 ms | 0.024 |

The governor's little domain now shows its busiest CPU at 100 % @ 1.80 GHz: that is the two
widget extensions, one full core each. So after removing the storm the idle picture is: big cores
idle at ~1 GHz, the two widget extensions each holding a little core flat out while the home
screen shows them. That is the next lever if idle heat still matters; it is widget content, not the
system.

The remaining pid-1 traffic (≈4–8/s) is ordinary XPC: chronod, contextstored, remoted,
UserEventAgent, sharingd, mediaserverd. `0x40000324` is evidently the common libxpc request id,
not an audiomxd-specific one.

Consequence: the phone's idle heat source is gone; the performance cores are no longer held at
2.1–2.35 GHz by pid 1, which is also the headroom the compositor needs to hold 60 Hz while swiping
(see the present-path record in `experiment-20261105-gpu-frame-trace.md`).

## Reproduce

The probe patch lives at `patches/picasso-ipc-census-xnu.patch` and was applied through the prepared
tree's `patches/xnu/` (a symlink into `.cache/upstream-…/patches/xnu`, picked up by
`build-xnu-4k-baseline.sh`'s patch fingerprint). It is not registered in `unixnu`'s embedded patch
list; a fresh `unixnu prepare` (different fingerprint) must copy it into the new tree before the
kernel build, or register it there.

```
DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
UNIXNU_KERNELCACHE=/Users/silenzio/Desktop/development/ios/UniXNU/out/ipsw/21H461/kernelcache.decompressed \
python3 unixnu build kernel prelink
bash scripts/build-wf-payload.sh <TAG> <ADDON> picasso-ipc-census=1
fastboot boot out/<TAG>/picasso/boot/<TAG>.img
python3 scripts/capture-picasso-usb-console.py --port /dev/cu.usbmodemPICASSO011 \
  --wait 150 --reconnect --duration 480 --send ps --send-after 180 --send-interval 60 \
  --output /tmp/<TAG>-probe.log
```

Useful greps: `PICASSO_IPC msg`, `IP to1`, `IP ids`, `PICASSO_PS top`, `PICASSO_PS begin`.
