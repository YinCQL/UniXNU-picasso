# Native watchdog and CPU/GOP display integration

## Result

This branch combines the timer/watchdog/AES work from
`pr/picasso-boot-timer-dispatch` with the owned CPU/GOP display path and an
8 GiB packed persistent Data seed. The image was booted with `fastboot boot`
only.

Verified on device `52cc791d`:

- `PicassoWatchdogDriver` publishes the original `IOWatchdog` policy and the
  SM7250 backend claims the APSS watchdog.
- `watchdogd` matches the service (`found=1`) and its original
  `newUserClient(type=1)` returns success.
- launchd reaches `Early boot complete. Continuing system boot.`
- `backboardd` opens the `NativeLegacyDisplay` user client successfully.
- `SpringBoard` starts and opens `AppleKeyStore` and `IOPMrootDomain`.
- The 8 GiB packed OwnerData volume mounts and the original display provider
  reports a valid `1080x2400`, 32-bit GOP mapping at `0xa0000000`.

## Images

| Item | SHA-256 |
| --- | --- |
| boot image (`watchdog-m20n.img`) | `807e418ec6405a2b6bc6035f59266f7555c3e03f6a0ef30db7ee9fdaf80a7a30` |
| kernel (`kernel.macho`) | `938cd03a590216bebaaa12454a7e4c2bbe6d74b6252a67efdee279ba264ac4bd` |
| exact payload | `12de8a38a2568c8ffacef87797e65885d519a37d3df8d47b2c11c3de56d3a55e` |
| deterministic ADT (`m20n-kernel.adt`) | `e5e6c7fb9ba5a11ea40fe76315fcfd8ac36a5b3bf394994c25f2b6ea98b74143` |
| Data seed | `73535e75603a28a003adf47bf897ec8536932587923e038f5985ccc2203b01e2` |
| trust cache | `7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7` |

Kernel UUID: `04201352-E42C-3488-8CE9-06769D924827`.

## Critical launch finding

The old packed-Data experiment reused `/tmp/m20k-kernel.adt`, which did not
contain `/arm-io/intc/virtual-timer-intid`. The native timer therefore selected
its fallback INTID 27 instead of Picasso's INTID 19. The USB watchdog could be
armed, but once the native watchdog policy claimed hardware ownership, no live
timer pet occurred and the board reset after roughly 15-30 seconds.

`make-picasso-adt.py` now accepts an exact `--random-seed HEX`. The verified ADT
reuses the original m16p random seed and adds the qualified virtual timer INTID
19 property. This keeps the packed-handoff seed stable while repairing timer
delivery.

## Relevant evidence

The complete capture is ignored by Git and retained at:

`picasso-diagnostics/20261031-stage7-watchdog-integration/hardware-cdc-singlefd.log`

Key lines:

```text
ArmCpu: virtual timer INTID=19 (ADT selection, IRQ delivery not yet verified)
picasso-watchdog: original IOWatchdog policy published, size=248; SM7250 owner active, hardware=15s policy=600/90s poll=1s
com.apple.xpc.launchd ... Early boot complete. Continuing system boot.
PICASSO_WATCHDOG match sample=1 pid=53 ... kr=0 found=1
picasso-watchdog: original newUserClient type=1 result=00000000 accepted=1
PICASSO_IOKIT ... pid=58 name=backboardd ... detail=NativeLegacyDisplay
PICASSO_DISPLAY event=4 sample=1 a=0 b=1 c=0 failure=0
PICASSO_IOKIT ... pid=27 name=SpringBoard ...
```

## Current blocker

There is no slot-189 IOSurface panic, no watchdogd SIGTRAP, and no ENOSPC in
this run. The next boundary is the userspace graphics stack:

- `backboardd` repeatedly finds no `AGXAccelerator` or `IOAcceleratorES` service.
- It also cannot find `AppleM2ScalerCSCDriver`.
- No `PICASSO_DISPLAY event=6/7` swap-validation or CPU-copy event has appeared,
  so the display client has opened but has not submitted a frame.
- `SpringBoard` remains running but is querying AppleKeyStore and power-domain
  services.
- `audiomxd`, `CommCenter`, and `mediaserverd` are killed by the original
  userspace watchdog health checks; this is independent of the display class.

The next experiment should trace IOMobileFramebuffer selectors 4/5/72 and decide
whether the missing frame is caused by an absent accelerators/front-board
dependency or by a request-format mismatch. Adding a fake GPU success is not a
valid next step.

## Microtime repair

The 19.2 MHz timebase exposed a second independent defect: XNU divided the
subsecond remainder by the truncated integer divisor 19. This moved microtime
backwards at each one-second boundary and produced negative APFS transaction
durations.

The follow-up image adds only `patches/xnu/picasso-timebase-microtime.patch`,
which scales the remainder by `USEC_PER_SEC` and divides by the real second
divisor. Timer INTID, watchdog deadlines, clock frequency and calendar
adjustment are unchanged.

| Item | SHA-256 |
| --- | --- |
| microtime boot image (`watchdog-microtime.img`) | `ff3144f6098bb05a5fdfb5c3f7f7a8da4ba11ecd6c16b8a23484cf8afcb16290` |
| microtime payload | `3be26410fd3efcdb88c439770de93a7bf8547e612b70a3ea2c9a363ad8aefdf3` |
| microtime kernel | `8ec6f3c6c84d3760163fda24eebc58eccf220291e07f12fb9232656772292e0a` |

Kernel UUID: `47F3E939-9B1F-305F-8C08-B7F852C012AA`.

The device again reaches launchd early boot, native IOWatchdog matching,
backboardd display-client open, and SpringBoard. The capture no longer contains
negative `tx took` values or `attrlistbulk_iterator` time underflow. The
Framebuffer method trace is still not enabled, and no swap-validation or
CPU-copy event has appeared; CPU-pixel presentation therefore remains
unqualified on this combined image.

The userspace watchdog later panicked after 492 seconds because `audiomxd` had
no successful checkin; `SpringBoard` had also not checked in since monitoring
was enabled. `backboardd`, `mediaserverd`, `logd`, `thermalmonitord`,
`runningboardd`, `CommCenter` and `configd` were still exchanging checkins.

## Persistent versus volatile Data A/B

A 128 MiB packed volatile Data test used the same kernel, timer, ADT, trust
cache and display startup, with only `sheng-data-persist=0` and the seed changed.
It reached launchd early boot and then immediately reported ENOSPC while
`free_blocks` was still 1,586/2,000 blocks. It did not produce a display swap.
This rejects slow persistent UFS writes as the explanation for the missing
first frame and confirms that small volatile Data is not a useful fallback.

Moving the persistent store away from stale writes was also rejected. Reusing
the exact 8 GiB seed and kernel with `sheng-log-reserve=1025` caused repeated
NativeRamBlock/APFS page-in `EINVAL`, followed by SpringBoard, logd and timed
crashes. The qualified layout remains the default 1024 MiB reserve; changing it
is not a safe way to simulate a fresh persistent store.

## First userspace frame over the CPU/GOP path

The clean microtime and framebuffer-trace rebuild exposed a separate UFS
timeout after command 128605: the one-second READ(10) deadline expired with
`OCS=0xf`, permanently quarantined the host, and caused every later APFS read
to fail with `EINVAL`/`NotReady`. The qualified follow-up keeps the controller
untouched but extends only the block-read deadline to ten seconds and reports
the final DB/IS/HCS/HCE state if a transfer really stalls.

The same run used `sheng-data-reset=1` once to discard the checksum-invalid
persistent overlay. The fresh Data volume mounted without checksum errors or
ENOSPC.

| Item | SHA-256 |
| --- | --- |
| UFS ten-second kernel | `779a729343f338805a10e5c60dcf7e2bc172a23ed799effda1c1d822e503be43` |
| exact payload (`watchdog-v4-ufs10-clean`) | `6e61b8c84d0667c0d61f267a2896cafd67763845ce36eca117a8b84b031d7c80` |
| boot image (`watchdog-v4-ufs10-clean-reset.img`) | `2937a3591a0d8776d8f5042c69cc5e9dc5667c8af9e7a9d4e6b6f7053512489c` |

The complete capture is:

`picasso-diagnostics/20261031-stage7-watchdog-integration/hardware-cdc-watchdog-v4-ufs10-clean.log`

The final userspace display path is now proven end to end:

```text
PICASSO_FB begin sel=4 ... pid=58 ... result=0
PICASSO_FB begin sel=5 ... in_bytes=1288 ...
PICASSO_DISPLAY event=6 ... failure=0
PICASSO_DISPLAY frame kind=0 nonblack=41011
PICASSO_DISPLAY event=8 sample=1 a=1 ...
PICASSO_DISPLAY event=10 sample=1 a=4590b8 b=9e3400 c=0 failure=0
PICASSO_DISPLAY event=7 sample=1 a=0 b=9e3400 c=0 failure=0
PICASSO_DISPLAY event=9 sample=1 a=1 ...
```

This is the Apple logo visible on the physical panel, not a bootloader-side
splash. A replacement `backboardd` subsequently submitted three more complete
frames. `SpringBoard` started and remained alive; it queried the power and
platform services, but no later frame has been observed.

The first `backboardd` instance was killed by the original userspace watchdog
before it checked in. Its replacement checked in and remained stable. The next
boundary is therefore the post-logo SpringBoard/backboardd wait, not raw
framebuffer output. `SpringBoard` also reports no `AGXAccelerator`; that remains
a high-value dependency to investigate, but it is not yet proof that the logo
or all remaining UI work requires AGX.

## Long-run watchdog and Fastboot recovery

The v14 policy build isolates the original userspace watchdog policy while
keeping the native hardware pet active. It was captured for more than 16
minutes after early boot with `sheng-disable-watchdogd=1`; the former
`no checkins from watchdogd` panic did not recur. The boot log shows both
expected markers:

```text
picasso-watchdog: first policy step and hardware pet complete (watchdogd-policy-disabled=1)
PICASSO_WATCHDOG_POLICY: watchdogd disabled for this boot
```

More than 100 user processes started. `SpringBoard` (PID 27) and `backboardd`
(PID 57) remained alive. The display path reported three accepted, transferred
and retired nonblack frames (`copy_bytes=31104000`), followed by a fourth
event sequence from `backboardd`. No UFS timeout appeared in this run.
`SpringBoard` and `backboardd` both failed an `AGXAccelerator` lookup with
`e00002f0`; the sampled SpringBoard main thread was waiting in
`_mach_msg_trap`, which is a normal IPC wait state but does not yet identify its
peer or synchronous caller.

The CDC reset path was also exercised on this build. `reboot-fastboot` armed
IMEM with `0x77665500`, disconnected CDC, and the same device serial reappeared
in Fastboot automatically. The intended recovery design remains:

- the invalid boot-partition header forces normal boots into Fastboot;
- the host-gated APSS watchdog returns a hung boot to Fastboot after 60 seconds
  without host activity;
- CDC `reboot-fastboot` provides the controlled software-reset path.

Only `fastboot boot` should be used for the macOS image. The boot partition was
not restored or rewritten.

| Item | SHA-256 |
| --- | --- |
| v14 policy kernel | `ecf9a6004e43ef230f458c00b9eef616b01bb33bcd17c775dd7a6d3c7cbeba24` |
| v14 policy payload | `c8b882c3e05f7db4665331e2fe16c711298f7d15d9405eaa2564c78ee052f7f6` |
| v14 policy boot image | `0e9a08ded08095c420a98eda643546e04dda5dcace1f0067f12ae145840fcee1` |
