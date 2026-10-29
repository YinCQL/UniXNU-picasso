# Picasso USB console bring-up

The `usb` branch contains a CDC-ACM kernel console backed by the Picasso DWC3
controller. It is verified on hardware: the host receives the buffered boot log
and subsequent kernel output over USB, without a screen photo.

## Host capture

Bulk-IN EP3 event lines and the periodic poller heartbeat are disabled by
default. Add the kernel boot argument `picasso_usb_trace=1` to enable them for
USB debugging. This takes effect in a newly built boot image; it is not a host
capture-script option. Event processing and the explicit poll/watchdog loop
continue with tracing disabled. Other startup, control and error logs remain.

The background poller services the 15-second APSS watchdog after every
completed normal poll, rather than after 1024 passes. A busy poll lock or panic
mode does not qualify for a background pet. A few initial service timestamps
and bounded slow-interval records stay visible with tracing off; they help
diagnose automatic reboots without restoring the periodic heartbeat. See the
[launchd and reboot investigation](launchd-reboot-investigation.md) for the
device evidence and limits of this correction.

### Windows

Double-click **`capture-logs.cmd`** at the repository root before booting the phone.
No port number, Python packages or other setup is needed. The window waits for
USB VID `1209`, PID `0001` and a `PICASSO` serial identity, captures automatically,
and reconnects to that same identity if Windows changes its COM number. It does
not automatically open unrelated serial devices. Only one capture window can
run at a time.

Close the window or press Ctrl+C to stop. Raw logs are saved continuously under
`out/picasso/logs/picasso-windows-*.log`; a `.log.session.txt` sidecar records
connection errors, port identities and the number of received bytes. A zero-byte
log means no kernel bytes were received, not that the device produced no output.
Each block is written with `WriteThrough` and `Flush(true)` before being queued
for asynchronous terminal preview. A slow terminal may skip preview chunks;
the raw file retains them. Saving does not depend on the final shutdown handler.

The PowerShell entry also supports optional advanced arguments:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/capture-picasso-usb-console.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/capture-picasso-usb-console.ps1 -List
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/capture-picasso-usb-console.ps1 -Duration 180
```

`-Port COMx` is optional for selecting among multiple devices and pins that COM
number. `-Wait N` limits the initial wait; the default zero waits indefinitely.
`-Duration N` starts at the first successful open and includes reconnection time.
`-Output PATH` selects another file; `-NoStdout` disables kernel text preview.
`-ExecutionPolicy Bypass` applies only to this PowerShell process.

The native helper uses `CreateFile` and `SetCommTimeouts` for normal capture,
without adding line-coding, DTR/RTS configuration or purge calls. The Picasso
bulk-IN pump is gated by USB configuration, not DTR. The old `.NET SerialPort.Open`
path reported semaphore timeouts on the user's enumerated COM8. On 2026-10-29 the
minimal native path actually received 9,790 bytes; the automatic launcher also
opened that same device and captured bytes. This establishes host capture for
that running device, not a complete boot or an image/commit identity. The exact
extra initialization request that timed out remains unknown. `-ConfigureLine`
is an opt-in diagnostic for locating failures in 115200/8N1 setup; ordinary
capture does not need it.

Start capture early: the device's 256 KiB ring can overwrite older text, and
reopening a port does not guarantee replay of data already sent to USB. Continuous
saving protects received blocks, not bytes still in device/USB queues at close.

### macOS

Connect the device to the Mac and check for a CDC serial port:

```sh
python3 scripts/capture-picasso-usb-console.py --list
```

Start capture before booting the test image. It can wait for the port to appear:

```sh
python3 scripts/capture-picasso-usb-console.py --wait 120 --duration 60
```

The wait and capture durations are separate. A wait timeout means no port was
opened and no kernel log was collected. The script writes a binary-safe log under `out/picasso/logs/` and
also mirrors received bytes to the terminal. Choose `--port` when more than
one matching device is connected; use `--output PATH` to select a log path.

## Verification boundary

Local verification covers patch propagation, XNU compilation, payload and
boot-image round-trip verification, and host-side capture. Hardware testing on
2026-10-25 confirmed DWC3 device mode, USB enumeration, CDC driver binding, and
kernel-log delivery. The diagnostic payload retains the placeholder platform
seed `41a551c0dec0de01`; it is not a qualified complete OS image. All phone tests
use temporary boot only.

## Hardware attempt (2026-10-25)

Two temporary `fastboot boot` attempts were made with the generated `boot.img`;
no partition was flashed, erased, or formatted. On the second attempt, the user
observed the phone reach the screen output with many USB-related messages, then
go black, reboot, show the Redmi logo, and return to Fastboot. The host did not
observe a `/dev/cu.usbmodem*` device and captured no CDC log. Fastboot again
reported `product: picasso` and `is-userspace: no` after the reboot.

A read-only `fastboot fetch minidump` attempt failed because this bootloader
does not support Fastboot `fetch`. Do not infer that the minidump is empty; it
was not read. The undocumented `fastboot oem dump` and `fastboot oem ramdump`
commands were intentionally not issued.

This confirms that the test image reaches USB-related kernel output, but does
not establish whether the reset is caused by the DWC3 path or later boot code.
The exact last visible USB log line is still needed to narrow the failure. Keep
the phone in Fastboot and do not repeat the boot until the next image has a
specific diagnostic change.

## Hold diagnostic (2026-10-25, 11:53 CST)

The complete standalone USB patch now produces a 645-line source file and matches
the nested upstream patch. A fresh output directory was used, avoiding the earlier
truncated 636-line patch and manually edited generated source. The build passed.
Literal `[NL]` strings were replaced by newlines, and the console-ring reader now
advances its position after copying bytes. Neither fix establishes the reset cause.

Image: `/tmp/picasso-usb-hold2-out/picasso/boot/boot.img`

SHA-256: `3357959b226d204bfd3882a0a6d0f319f918c1f23b6a44f39881b113abfd0688`

Arguments:

```text
-v debug=0x14e cpus=1 native-ufs-probe=12 native-ufs-diagnostics=1 io=0x232 picasso_usb=2 picasso_usb_hold=1
```

`picasso_usb_hold=1` sleeps in `bsd_init` after gadget initialization returns. It
does not stop IOKit threads already running, and it also holds if gadget setup
returns early. Thus the hold alone cannot prove successful USB setup or exclude
all UFS activity.

The first download succeeded but Bootloader returned a load/authentication error.
After `fastboot reboot bootloader`, the same image was accepted (`Booting OKAY`,
11:53:23 CST). Host USB monitoring recorded Fastboot disconnect at 11:53:26.
Through the observation ending at 11:54:59, there was no recorded CDC device or
Fastboot return. Serial waiting timed out; no kernel log was obtained. Host USB
absence cannot distinguish a held kernel, hang, black screen, or reboot without
USB. At 11:56:20 a separate `fastboot devices` check again found `52cc791d`.
The exact return time and whether it was automatic or manual were not observed.
The phone's physical screen/reset state still needs user confirmation.

No partition was flashed, erased, or formatted. Do not repeat the same image to
infer a new result. First establish the current screen state; the next diagnostic
should isolate a USB initialization stage or compare a probe-only hold.

## CDC enumeration and kernel-log capture (2026-10-25, 21:42 CST)

The USB console now works on hardware. A temporary `fastboot boot` of the hold
image produced a real CDC-ACM device on the Mac:

```text
/dev/cu.usbmodemPICASSO011
picasso XNU USB console
VID:PID 1209:0001
480 Mb/s
```

The host captured 23,191 bytes (545 lines), beginning with the Darwin kernel
banner. The log contains the SMMU guard and bind, DWC3 reset and command
completion, UTMI setup, device connection, endpoint configuration, and
`holding at bsd_init; SMMU guard PASS`. This removes the screen-photo requirement
for the kernel phase.

The last blocking defect was event consumption, not DWC3 DMA. `ud_thread` did not
run while `bsd_init` was executing synchronously, so the controller accumulated
24 bytes of events while the counters showed `events=0/24`. Moving the same
event loop into an inline `ud_poll()` and calling it from the early sample loop
consumed the connect and control events immediately. The configured bulk-IN
path then drained the complete 256 KiB console mirror.

For the next phase, the early sample loop remains synchronous. If boot is not
held, a kernel thread is started afterward to continue polling while
`bsd_init` proceeds. The hold path does not start that thread, preventing two
consumers from racing on one event queue.

Proven hold image:

```text
/Users/silenzio/Desktop/development/ios/picasso-diagnostics/20261025-usb-console/boot/boot-hold.img
SHA-256 7f4840558755ea6e883ba93961ec92f0871adaf0e964682f523ebd3b1f004f08
```

Next live-poll image (not yet hardware-tested):

```text
/Users/silenzio/Desktop/development/ios/picasso-diagnostics/20261025-usb-console/boot/boot-live-poller.img
SHA-256 587b0f839dbd14d5e767c7c1e7c6f237797d2627ec01548d4b7d0f77f407314b
```

The board defaults now enable the console without holding boot. For diagnostic
runs that need to park at `bsd_init`, append `picasso_usb_hold=1`.

No partition was flashed, erased, or formatted. Use `fastboot boot` only.
