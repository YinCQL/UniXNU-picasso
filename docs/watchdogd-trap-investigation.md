# Watchdogd trap after early boot

## Evidence from build 40901155087

The user confirms that `picasso-windows-20261029-233904-750.log` uses
[40901155087](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40901155087)
at `9ea860c79146853e8e51c22ede5ec3935d414ac9`, and the phone reboots automatically.
The capture is 149,823 bytes / 1,995 lines, SHA-256
`76281eec6c0cc9d63ab8341b725dbf94e3b8690f885d712413a3fed972d0d559`.

This build completed successfully, including real macOS APFS preparation and the
FAT/UUID/storage checks. Downloaded artifact `12910063127` (`picasso-bootimg-65`)
has kernel SHA-256 `d23f646ecf0ce5e78a2f6660e2d4161a05801a59f81fa8927ae099db304a6752`.
Its Mach-O UUID `4CA91C9F-72DA-3A3F-98F7-28D54500CC64` matches the device panic log.
The individual boot.img used on the phone has not been independently hashed.

The actual payload is 62,914,560 bytes (60 MiB), with 55,225,157 bytes of contents
against the 59,768,832-byte guard. Data is 268,435,456 logical bytes and 1,011,808
packed bytes, UUID `357CF4C4-2F4D-4CC7-AA8D-A9DCFF2ACBDC`. Both source and Data
had ownership enabled on the macOS runner; source VFS flags were 3 (read-only),
Data flags were 2 (writable).

The phone reports `NATIVE_OWNER_DATA done stage=4 error=0 readonly=0` for
`/dev/disk3s1`. Original tzinit passes its former fatal step, launchd validates
its signed cache, and logs **Early boot complete. Continuing system boot.**
The following failure is watchdogd PID 95 exiting with namespace 2 / subcode 5,
then the existing critical-process panic, panic flush and PSHOLD reset.
The failed core-file open is secondary to that process exit.

The pinned XNU definitions identify this as `OS_REASON_SIGNAL / SIGTRAP`.
`OS_REASON_WATCHDOG` is a separate namespace (20); a daemon named watchdogd
exiting does not itself prove a hardware watchdog timeout. The capture has no
EL0 PC or assertion text for this fault, so it cannot select a specific trap.

## Original binary comparison

The unchanged executable was extracted read-only from the exact original 21H461
OS DMG. `/usr/libexec/watchdogd` is 170,144 bytes, SHA-256
`ded4c688e1c2add74acf3e7f9eea5fe2c2c569180ce81492221877944d913a01`, matching the
fixed upstream's `scripts/check-watchdogd-branches.py` input. Original launchd
also matches the seed's expected hash. The original watchdogd plist retains
its `IOWatchdog` launch event and `PanicOnConsecutiveCrash` policy.

Running that script against the actual binary with explicit OS API stand-ins
reproduces these paths:

| Original connection result | Outcome / unslid PC |
| --- | --- |
| Native machine, service missing | BRK at `0x10000ab0c` |
| Service present, IOServiceOpen error | BRK at `0x10000ab54` |
| Service present, open succeeds | Returns connected |
| Virtual machine, service missing | Exits zero |
| PMU lookup required, no primary PMU service | BRK at `0x10000b160` |

These tests establish binary behavior, not the phone's actual branch. The binary
also has PMU and external-method assertions. `_os_crash` precedes the two listed
BRKs, so x0 at the trap is only a candidate string pointer, not a guaranteed
assertion message. The stable PC, image base and LR remain essential.
The entry point calls watchdog connection before PMU lookup. Four no-pmu
initializer and three PMU lookup cases were also executed with explicit API
stand-ins. In this binary, the no-pmu initializer first checks internal-diagnostic
variant status; a false result skips both the boot-argument and ADT lookup.

The original AppleARMWatchdogTimer personality expects Apple hardware
(`AppleARMIODevice`, `wdt,s5l8960x`). XNU's generic `IOWatchDogTimer` is a different
interface from the `IOWatchdog` UserClient queried here. The older guacamole
no-pmu workaround and other boards' disabled-daemon policies do not establish
a suitable backend for this board.

## Diagnostic change and next check

The final device diagnostic patch records EL0 breakpoint registers before
exception delivery. Capture uses eight fixed records and a try-lock, without
printing, allocating or reading user memory in the trap. Records carry PID,
process unique ID, exec generation, task identity and thread ID.

After the original critical-exit policy decides to panic, `PICASSO_CRITICAL`
reports the process, signal, reason and record status. A matching signal reason
2/code 5 enables `PICASSO_EL0` register output, followed by bounded instruction,
three-frame, image0/path and candidate x0-text reads. XNU may deliver a reasoned
SIGTRAP through SIGKILL, so the diagnostic reports the actual exit signal and
uses the reason code to identify the fault. Missing or mismatched records and
copy failures remain explicit; they never suppress the original panic.

`PICASSO_WATCHDOG match` reports the first eight actual single-service queries
for `IOProviderClass=IOWatchdog`, including PID/thread, result and whether a
service was found. It adds no extra matching or service-open call and preserves
the original matching dictionary, return value and lifetime rules.

The next log should correlate this result with watchdogd's EL0 snapshot. For a
PC in the main image, subtract the reported `image0` base and add its preferred
base `0x100000000` before comparing with the original binary. A genuine service
or PMU backend fix depends on that observation. This change fills a diagnostic
gap; it does not claim that watchdogd or the reboot is already fixed.

Snapshots are best effort: an overwritten record or a different exiting thread
can produce `missing`. A matching snapshot is the latest breakpoint on that
process/task/thread, not proof that it caused the later exit. User-memory reads
have size limits but can still fault or block; the register lines print first,
and memory-derived fields may be unavailable.

Host validation extracts the actual generated capture/report helpers, checks
the fixed XNU dyld structure layouts, and passes 51 cases in both normal MSVC and
ASan builds. Cases include the SIGKILL carrier, process/exec/thread identity,
ring overwrite and busy handling, copy failures, frame limits, output budgets
and printing registers before any user-memory read. Two complete patch passes
check all 61 patches and stable output for 12 source files. Removing the added
diagnostics restores the three affected files exactly; the other nine remain
unchanged. These are host checks, not Apple SDK or device execution of the new
diagnostic code.

On 2026-10-30, the resumed review verified that the current outer patch and all
12 generated-source hashes match that completed replay, then rebuilt and ran
the actual helpers: 51 capture/report cases in normal and ASan builds, plus 14
service-matching cases in normal, ASan and release configurations. The latter
cover the original return/release behavior, malformed dictionaries, query/output
limits and concurrent ticket allocation. Independent review also reran the
original watchdogd connection branches. No new CI build or phone boot has yet
validated this diagnostic patch.
