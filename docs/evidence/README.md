# CDC console captures

Raw logs captured from the Picasso CDC-ACM console (`/dev/cu.usbmodemPICASSO011`)
while the diagnostic payload booted the XNU kernel. They are the first evidence
that the gadget carries the kernel log past early boot; no partition was
written, erased or formatted for them (`fastboot boot` only).

| File | Captured | Size | SHA-256 |
| --- | --- | --- | --- |
| `picasso-usb-console-20261025-214241.log` | 2026-10-25 21:44 CST | 23 KiB | `dd9d29c91fb8ca4891c8f61602ef6a3b59f84a5c026730002a4ed9318045eeb7` |
| `picasso-usb-console-20261025-221842.log` | 2026-10-25 22:19 CST | 25 KiB | `4e8597828f7d619053d8e737401aaf1774b5354746e5c1be7eb44d16fa73b6a9` |
| `picasso-cdc-panic-dedup-v6-20261027.log` | 2026-10-27 23:26 CST | 3.3 KiB | `3a8005334c72ad776fce9d1979b6ba581a0e426152b8956ef56624ce8908be07` |
| `picasso-cdc-ep0-v12-status-20261028.log` | 2026-10-28 20:46 CST | 26 KiB | `1ec8c15aab4db9966f2cb713ab691d9a32868f0c826d3486de3dc960e8cd17e7` |
| `picasso-cdc-ep0-v12-reboot-fastboot-20261028.log` | 2026-10-28 20:49 CST | 2.5 KiB | `a73ee34905d38633ff7864b78d21ee2c12d10ceed82c777ffc6617db9d4ed447` |
| `picasso-cdc-ep0-v12-panic-test-20261028.log` | 2026-10-28 20:54 CST | 29 KiB | `42f9b6267ee25b9c77e4a046d59f0f1ce0af9c99bd6ea44acd7ef7c4b843fba1` |
| `picasso-cdc-ep0-v12-hang-test-20261028.log` | 2026-10-28 20:59 CST | 27 KiB | `582369a464c28f03a9e9dd6157292000b3f7130092473b1931772580481f85e9` |

The full-boot captures start with the `Darwin Kernel Version` banner and carry the kernel's
`printf` output through `bsd_init`. The second capture reaches the APFS/VFS
stage and ends waiting for the `Apple_HFS` root device, which is the stage-6
blocker (NativeUfs root mount), not a console failure.

The first capture is dominated by `PICASSO USB: event 0x0000c046` once the
event ring is polled ahead of the kernel thread; that noise is why the thread
hand-off (`b3233c0`) exists. Raw terminal captures were taken with:

```sh
cat /dev/cu.usbmodemPICASSO011 > picasso-usb-console-$(date +%s).log
```

Reproduce with the supported script instead, which configures termios and
survives re-enumeration:

```sh
python3 scripts/capture-picasso-usb-console.py --reconnect \
  --output docs/evidence/picasso-usb-console-$(date +%Y%m%d-%H%M%S).log
```

The 2026-10-27 capture proves that the panic text itself, including the reason,
backtrace and `Stackshot Succeeded`, reaches CDC before the kernel stops. It also
served as the regression check for the earlier doubled-character bug: the log
contains normal `panic` and `Debugger` strings, not `ppaanniicc` or
`DDeebbuuggggeerr`.

## CDC EP0/data-path qualification (2026-10-28)

The v12 hardware run qualifies the rebuilt EP0 resource lifecycle, CDC bulk data
path, panic capture and both automatic reset modes. Only `fastboot boot` was
used; no partition was flashed, erased or formatted.

```text
image    out/picasso-ep0-v1/picasso/boot/boot-stage6-ep0-v12-output.img
boot     sha256 ca1153643ab83a3d6ed28eaf08704eeb45b04bef855a426f89ab9896537e632a
payload  sha256 11fca89b82b94c75a24407aea2f539eb9a7424a6725abba8f096d7fd9c25181f
kernel   sha256 301a818066a6b42361071163b111c9f9a3899d8e78caee450849568a7d46624b
FD       sha256 024e164dce9f2dc6e8e3dc214363cb45024fec71a631460b319600385b255ecd
```

Observed results:

- The host enumerated `picasso XNU USB console`, selected configuration 1 and
  created `/dev/cu.usbmodemPICASSO011`.
- `status` was received as exactly 8 bytes; the host captured the complete
  response ending with `configured=1 host_open=1 live=1`.
- `reboot-fastboot` armed IMEM reason `0x77665500`, flushed the final CDC lines
  and returned the same Fastboot serial automatically.
- `panic-test` delivered the panic reason, register dump, backtrace and
  stackshot over CDC, then returned to Fastboot through the PSHOLD fallback.
- `hang-test` delivered its final breadcrumbs, stopped the only CPU, and the
  APSS watchdog returned the same Fastboot serial.

Two implementation defects were fixed by the same run:

- EP0 transfer resources are now ended and rebuilt on reset/connect recovery,
  and both control completion paths consume the resource before rearming.
- USB ring capture now hooks `console_write()` before
  `disableConsoleOutput` can return, so later `printf()` command responses and
  panic output are not silently discarded.

A single-variable `picasso_usb_hold=0` build
(`boot-stage6-ep0-v13-hold0.img`, SHA-256
`e9b835635397a8dbbd1e318712e7675e1c7540df7fc1b1f60f9f1382f0a69d51`)
disconnected during early boot and returned to Fastboot before a usable CDC
transcript was produced. The qualified v12 image therefore keeps
`picasso_usb_hold=1`.
