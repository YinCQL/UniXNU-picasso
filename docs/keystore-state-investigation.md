# Apple logo and missing keystore state after build80

The user confirms that the latest image displays an Apple logo, remains there,
and was **manually disconnected**. The snapshot of
`picasso-windows-20261030-102439-727.log` is 132,313 bytes with SHA-256
`7ffb60b3645c222edd7b7d1ae82ea491b56609c0eba29f9eea95a5135429d3c7`.
Evidence is retained under `out/logo-stall-20261030/`.

[Compose build80](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40990177120)
succeeded at `5480c0470099c5672ea29520f009e3e3dfb04443`. Artifact
`12949321897` / `picasso-bootimg-80` includes the actual SDK display objects and
the successful final vtable/alias/archive guard. The independent Android/FAT
reader verified the embedded kernel, loader, Data seed, ADT and trust cache, plus
the compressed firmware and appended DTB. FAT content totals 55,487,518 bytes.

| Artifact | Value |
| --- | --- |
| Kernel SHA-256 | `956a0e3ff575a497397a796e66285cb34b6357679587aace79187d90710343d1` |
| Kernel UUID | `65775849-7964-3B7D-9E43-23CE86C384EE` |
| Boot SHA-256 | `914184afea3ae15f6fb05544863aa6cfb9cd2b31b99f5787eadb0a81fc24bcb7` |

This capture has no panic or kernel UUID. Image attribution therefore rests on
the user's confirmation and matching new feature traces, rather than a logged
UUID comparison. Its end is not evidence of a reboot.

## What the capture establishes

The 1080x2400 GOP mapping, original framebuffer UserClient, CPU copies and
original retirement path all succeed. The previous `failed to find the main
CADisplay` SpringBoard exception is absent. Four sampled frames each contain
41,011 nonblack pixels, consistent with the reported logo. Diagnostics stop
after four samples per event, so this does not establish that only four frames
were submitted or that all later frames succeeded.

backboardd's first instance fails its watchdog ping; its second instance opens
the display and submits the observed frames. SpringBoard remains present and
has four failed `AppleKeyStore` lookups. mediaserverd reports a vital task still
running, but the old 160-byte description ends just before its name. audiomxd
and CommCenter also fail supervision. Repeated liveactivitiesd crashes previously
produced only the corpse-limit message. APFS transactions and other system
activity continue to the end. No whole-kernel deadlock is established.

`AppleM2ScalerCSCDriver`, `AppleCLCD` and AGX matching failures, and the single
`No Default FB Surface found` message, do not by themselves identify the stalled
userspace dependency. The CPU path demonstrably presents the boot image. The
capture does not prove that keystore state is the only missing dependency.

## State service and target ABI

The tutorial's [AppleKeyStoreState component](https://github.com/UniXNU/AppleKeyStoreState)
and its [platform integration guide](https://unixnu.com/porting/platform#block-and-os-services)
describe the missing interface. The component was read at
`42ff83e08b2265fe741d4adaf084bebe003452ba`. Its general successful bag/bookkeeping
cases are not used by this adapter.

The selected SystemOS image supplies the original AppleKeyStore and MobileKeyBag
frameworks. The shared-cache header layout was derived from Apple's dyld
`a571176e8e00c47e95b95e3156820ebec0cbd5e6` header. The two ZBM-compressed LINKEDIT
files were decoded with libzbitmap `574abeae25b25c3319b9b6a9225cc7464e08ae88`;
CodeDirectory blocks of the five files needed for these observations verify.
Raw source images and decoded cache files remain outside Git.

| Original 21H461 entry / address | Observed contract |
| --- | --- |
| Connection helper `0x202fc0554` | Open type 0, then selector 0 with no inputs or outputs; failed handshake closes the connection |
| `aks_get_lock_state`, `0x202fbfc38` | Selector 7, one sign-extended context scalar and one scalar output |
| `aks_get_device_state`, `0x202fbfc04` | Selector 17 through the common state helper |
| `aks_get_extended_device_state`, `0x202fc19fc` | Selector 35 through the same helper |
| Common helper `0x202fc1330` | One context scalar, no scalar output, up to 4096 bytes of DER output |
| Decoder `0x202fc14dc` | State dictionary fields including `ss`, `sls` and `bh` |
| `MKBGetDeviceLockState`, `0x1b5192a4c` | Consumes the decoded lock-state word at offset 4 |
| MKB default context initializer `0x1b5192658` | Ordinary device context 0; alternate persona context is separate |

The new adapter answers session setup and device-state queries only for context
0 in the explicitly selected unprotected ephemeral environment. The state has
no passcode and no protected content. It rejects bag/persona contexts, keybag
handle queries and every key operation. It validates all input/output counts,
refuses asynchronous requests and reports a short descriptor write as failure.
An unsupported selector may still expose another boot dependency on the next
run; the adapter does not claim to implement secure storage or a working keychain.

The class is built into the existing late display archive using the actual SDK
compile record and initialized from the post-IOBSD hook. Its source name and
runtime classes retain the upstream names. The board argument and runtime
ephemeral property gate publication. The builder restricts the OS profile and
Data mode; the original Data artifact check still verifies the selected seed
and refuses persistent storage configuration.

## Diagnostics and verification

Service descriptions read at most 960 bytes, bounded by the actual kcdata
description item. After build81 they print metadata separately and use at most
eight 128-byte pieces to fit the observed capture limit. Each exact service
name retains four report tickets across restarts. liveactivitiesd is added to
the selected names. The watchdog policy and exit control flow are unchanged.
Display failures have a separate four-report allowance, and failed retirement
is observable after a copy. Successful frames cannot consume the failure budget.
AKS logging now has separate 24-line budgets for SpringBoard, keybagd, securityd
and other callers (96 total). It records caller/selector/result and the integer
context only for state queries. Key-operation payloads are never logged.

Build81 confirms service publication, SpringBoard matching/open and successful
state queries. Its startup remains incomplete. Current findings, new Data
headroom and diagnostic verification are in the
[build81 investigation](build81-startup-investigation.md); the checks below
retain the preceding change's evidence.

Completed local checks:

- `replay-v1/report.json`: two passes of all 62 patches; generated sources are
  identical across passes.
- `fault-v3/results.json`: 72 ordinary and 72 ASan cases, including continuation
  past byte 160, the 960-byte cap, unterminated/empty/malformed kcdata items,
  independent service limits, SIGKILL and the existing saved-fault boundaries.
  The forbidden private BSD task-field compilation still fails as intended.
- `state-host-v3/results.json`: ordinary and ASan execution of the actual state
  service, covering session lifecycle, failed start/attach, unsupported key and
  bag operations, device-context filtering, DER output bounds and short writes.
- `display-host/results.json`: ordinary and ASan framebuffer helper/ownership
  regression checks, original ABI checks and ARM64 copy-helper compilation.
- `cache-abi-evidence.json` and `ci80-packaging.json`: original cache integrity
  and independent build80 packaging evidence.

The new adapter and diagnostics still require a complete SDK build and a new
device capture. build80 qualifies the preceding display code only. On the next
run inspect `PICASSO_AKS started=1`, selector 0/7/17/35 results, SpringBoard's
next behavior, and the complete mediaserverd task description. Progress beyond
the logo, audio, input and persistent Data remain unqualified.
