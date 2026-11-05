# Idle CPU anomaly: the audiomxd ↔ launchd message storm

Status: **root cause measured and named on hardware (wf312–wf320): a failing bootstrap lookup of
`com.apple.BTAudioHALPlugin.xpc`, retried without backoff ≈2,200 times/s.**

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

- The v5 sampler takes the payload from `ikm_udata_from_header(kmsg)`, not from `hdr + 1`: a kmsg
  whose data does not fit its inline buffer keeps it in `ikm_udata` (see "The request, named"),
  and only `IKM_TYPE_ALL_INLINED` messages have the payload right after the header.
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

`wait 217` (MusicWidgets) shows three threads, all blocked at the sample, the main one inside
SwiftUI/AttributeGraph (`AGGraphGetWeakValue` and AttributeGraph frames over SwiftUI) waiting on a
synchronous XPC reply port. The widget cost is SwiftUI content rendering in bursts (one full core
in the busy window, 0.34 core in the next), not a spin — the extensions are SwiftUI render
servers and their home-screen content is simply expensive on this port.

The remaining pid-1 traffic (≈2–8/s) is ordinary XPC: chronod, contextstored, remoted,
UserEventAgent, sharingd, mediaserverd. `0x40000324` is evidently the common libxpc request id,
not an audiomxd-specific one.

Consequence: the phone's idle heat source is gone; the performance cores are no longer held at
2.1–2.35 GHz by pid 1, which is also the headroom the compositor needs to hold 60 Hz while swiping
(see the present-path record in `experiment-20261105-gpu-frame-trace.md`).

## The disable is not shippable as-is: touch depends on audiomxd

A/B on the same kernel (`a5ce6d42…`), only the job list differing:

- **wf316** (`sheng-disable-jobs=…,com.apple.audiomxd`): the lock screen is up, the panel driver
  logs the finger's contacts (`PICASSO_NVT point id=1 x=420 y=21510 …`), the HID digitizer reports
  `event driver ready`, and SpringBoard is not blocked (21 threads in normal `mach_msg2_trap` /
  `workq_kernreturn` waits) — but the UI does not react. SpringBoard spent 5.6 s of CPU over the
  following 9.5 minutes while the finger kept swiping (0.01 core): the injected touches are never
  processed.
- **wf317** (same line without `audiomxd`, i.e. the job running): **touch works**; the storm is
  back (launchd 124.2 s of CPU at 4.5 min, d1 57–70 % @ 1.89–2.01 GHz, d2 58–68 % @ 2.01–2.06 GHz).

The sync-IPC trace of a boot with audiomxd running (wf313) shows `mediaremoted → audiomxd` sync
links (275 in ~7 minutes) and clients reaching audiomxd's other services, so audiomxd's Mach
services are registered and are part of the lock screen's touch path (audio session/system-sound
feedback). Disabling the job removes the storm but also removes those services, and touch dies.

So the fix has to keep audiomxd serving while stopping its launchd loop.

## The request, named (wf319/wf320)

The v3/v4 body sampler read the payload at `ikm_header(kmsg) + 1`, which is only the user data of a
kmsg that kept the message inline (`IKM_TYPE_ALL_INLINED`). A message whose payload does not fit
`IKM_SMALL_MSG_SIZE` is `IKM_TYPE_UDATA_OOL` and keeps it in `kmsg->ikm_udata`; the bytes after the
header are zero padding. That is why the storm's request body scanned as all zeros with no strings:
the sampler was reading the wrong buffer (the 116-byte reply did stay inline, so its first words
looked sane). v5 samples through `ikm_udata_from_header()`.

wf320 (`2aeb8ca9` + v5 census, audiomxd running) then printed, 120 s into the boot:

```
PICASSO_IPC msg s=1 from=94 to=1 id=40000324 size=188 bits=111211 type=1
  body=handle|flags|name bare=handle|com.apple.BTAudioHALPlugin.xpc|lookup-handle
PICASSO_IPC dump s=1 o=0 40585043 00000005 0000f000 0000008c
PICASSO_IPC dump s=1 o=16 00000005 646e6168 0000656c 00004000
PICASSO_IPC dump s=1 o=32 00000000 00000000 67616c66 00000073
PICASSO_IPC dump s=1 o=48 00004000 00000000 00000000 656d616e
PICASSO_IPC dump s=1 o=64 00000000 00009000 0000001f 2e6d6f63
PICASSO_IPC dump s=1 o=80 6c707061 54422e65 69647541 4c41486f
PICASSO_IPC dump s=1 o=96 67756c50 782e6e69 00006370 65707974
PICASSO_IPC dump s=1 o=112 00000000 00004000 00000007 00000000
PICASSO_IPC dump s=1 o=128 6b6f6f6c 682d7075 6c646e61 00000065
PICASSO_IPC dump s=1 o=144 00004000 00000000 00000000 00000000
```

and the reply:

```
PICASSO_IPC rdump s=1 o=0 40585043 00000005 0000f000 00000044
PICASSO_IPC rdump s=1 o=16 00000003 5f636572 63657865 00746e63     rec_execcnt
PICASSO_IPC rdump s=1 o=32 00004000 00000100 00000000 5f716572     req_pid
PICASSO_IPC rdump s=1 o=48 00646970 00004000 0000005e 00000000     0x5e = 94 = audiomxd
PICASSO_IPC rdump s=1 o=64 6f727265 00000072 00003000 00000003     "error"
```

Reading the two together:

- `0x40585043` is libxpc's wire magic; the request is an XPC dictionary whose keys are `type`,
  `handle`, `flags`, `lookup-handle` and `name`, and the name is
  **`com.apple.BTAudioHALPlugin.xpc`**.
- Disassembly of `libxpc.dylib` from the device dyld shared cache pins the routine: `msgh_id` is
  `0x40000000 | routine`, and the dictionary keys and the reply keys (`port`, `non-launching`)
  belong to the helper at `0x1dd66a0cc` reached from `_bootstrap_look_up3`,
  `_xpc_endpoint_create_bs_named` and the pipe constructor. `0x324` is the **bootstrap
  look-up** routine; `0x20000000` is the reply id that `_xpc_pipe_interface_routine` validates.
- The reply carries the demux envelope (`req_pid`, `rec_execcnt`) and an `error` entry, and **no
  `port`**, so the lookup failed every time and the client repeated it immediately.

`com.apple.BTAudioHALPlugin.xpc` is declared as a MachService by
`/System/Library/LaunchDaemons/com.apple.bluetoothd.plist`; no other job provides it. On this boot
bluetoothd had not checked that service in, so the HAL client inside audiomxd (CoreAudio's
Bluetooth-audio plug-in lookup) spun on the failure: wf320's blocks read

```
IP to1 d=66584 94:audiomxd=66441 ...      (≈2,215/s)
IP to1 d=65843 94:audiomxd=65760 ...
IP to1 d=65510 94:audiomxd=65447 ...
```

with launchd at 56.2 s user + 108.6 s system ≈ 4 minutes into the boot. The next question is whether
the service is missing because Bluetooth is not initialised on this port (no BT firmware path in
this kernel) or because bluetoothd's check-in is rejected; either way the lever is to make the
look-up succeed (or stop the client from retrying), not to disable audiomxd.

## Root cause: `sheng-lite=1` disables bluetoothd

`com.apple.BTAudioHALPlugin.xpc` is a MachService of `com.apple.bluetoothd`
(`/System/Library/LaunchDaemons/com.apple.bluetoothd.plist`), and its HAL driver lives in
`/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver`. The board's own "lite" job list
(`drivers/NativeMobileDisplay/boot-progress/BootWatchdogPolicy.c`, `lite_jobs`) **disables
`com.apple.bluetoothd`** as a radio-less daemon. The lookup therefore fails with the demux `error`
entry seen in the sandbox above, and CoreAudio's Bluetooth-audio HAL client retries it without
backoff for the whole boot: the measured 1,800–8,400 requests/s against launchd, and the ~1 core of
launchd system time that heats the phone.

So the storm is not a system defect but the consequence of a bring-up job policy: a daemon disabled
for having "no radios" is the exact Mach service the audio daemon waits on. The fix is to keep the
job enabled (`lite_jobs` no longer lists `com.apple.bluetoothd`); the lookup then resolves and the
retry loop ends.

### Measured: wf332, the wf298 image with bluetoothd re-enabled

wf331 (rebuilt kernel, `bluetoothd` enabled) confirmed the lookup stops appearing in the IPC samples,
but wf330 and wf331 both panicked before the lock screen in `task_restartable_ranges_synchronize`
(`wait_while_equals32` timing out with `trr_ipi_ack_pending=0x00010000`, the same signature as the
2026-11-02 GIC SPI-IPI bug in `docs/handoff-20261102-ui-frame-cpu-topology.md` §10.4), so the fix was
re-tested on the last known-good image: wf332 = wf298 (kernel `29f21e4f`, addon `b70d0fb1`, trust
`75bea288`, all args identical) with only the 20 bytes of the `lite_jobs` `com.apple.bluetoothd`
label overwritten with commas, in both the payload and the boot image's copy of the kernel
(`out/wf332/README-patch.txt`). The desktop came up, touch worked, no panic.

Two `ps` samples 60 s apart:

| process | Δ / 60 s | cores |
|---|---|---|
| launchd | 786 ms | **0.013** |
| audiomxd | 150 ms | **0.0025** |
| MusicWidgets | 54.5 s | 0.91 |
| backboardd | 51.9 s | 0.87 |
| SpringBoard | 8.1 s | 0.13 |

The governor's A76 domains read 20–23 % busy @ 1.9–2.2 GHz at idle (70–90 % @ 2.2–2.35 GHz with the
storm). That closes the idle-heat question.

Swipe frame rate is unchanged, though: 160 frames in 4.27 s = **37.5 fps**, and the frame trace splits
each frame's 24.0 ms present into 1.1 ms scheduling, ≈7.6 ms commit and ≈13.3 ms fence wait (the
plugin's own PF row: `t20 a19 i36 c7565 w13319`). The compositor's full-frame copy waits behind
SpringBoard's own GPU render during the swipe; the CPU-side storm was never on the frame path.

A second, independent failure mode found the same evening: with `com.apple.audiomxd` itself
disabled, its MachServices stay registered with launchd, so clients block forever waiting for the
audio daemon and the compositor stops submitting frames (4 frames, then nothing, display `wait_to`
≈ every wait). That is why "disable audiomxd" is not a usable mitigation; see the A/B above.

## Operational hazard found the hard way: `wait <name>` wedges the console

`wait SpringBoard` on wf320 ran `picasso_live_named_state()` inline in the CDC command handler;
that function calls `proc_iterate(PROC_ALLPROCLIST, …)` while the handler holds the console lock,
and the process-list lock and the console lock then deadlock. The kernel stays up (USB remains
enumerated) but the CDC endpoint stops answering and the host cannot open the port again. The
deferred `ps` path (`ps_pending`, served from `ud_poll_serialized`) exists exactly to avoid this;
`wait <pid>` has the same hazard through `proc_find`. Do not use `wait` over CDC on a boot that is
still needed; recover with a power-button reset.

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
