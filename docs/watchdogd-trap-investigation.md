# Watchdogd trap after early boot

## Confirmed missing service in build 40922207086

The user confirms automatic reboot with
[40922207086](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40922207086),
head b67e8481813a4c780f1a13e0641670ed65b97445, artifact 12919601876 /
picasso-bootimg-69. The downloaded kernel is 54,136,832 bytes, SHA-256
3514d87b9fe2ab4e0256ee1847f8e8b8e0baa34d22c33fc1b94fad5248f6a4e1.
Its LC_UUID AE74B65C-E92A-3B8E-8748-96F633B955AC matches the phone's panic.
The actual boot.img used on the phone has not been independently hashed.

The capture picasso-windows-20261030-014603-884.log is 150,643 bytes, SHA-256
d12ecabcf1568b853f277b7c2851c495b539781b5f1f788ad99aa12e167b47fc.
Data reaches `done stage=4 error=0 readonly=0`; launchd reports
**Early boot complete. Continuing system boot.** The failure follows:

- PID 95 / thread 409 queries IOWatchdog: `kr=e00002f0 found=0`.
- The same thread's breakpoint snapshot reports PC/LR 0x100851b0c,
  ESR 0xf2000001 and instruction 0xd4200020 (BRK #1).
- The critical process exits with OS_REASON_SIGNAL / SIGTRAP.
- The original critical-process panic, panic flush and PSHOLD reset follow.

One executable slide, 0x847000, aligns four locations in unchanged watchdogd:

| Runtime location | Original address | Meaning |
| --- | --- | --- |
| PC/LR 0x100851b0c | 0x10000ab0c | Missing IOWatchdog service BRK |
| Frame return 0x10084d830 | 0x100006830 | Return from missing-service cold path |
| Frame return 0x10084f958 | 0x100008958 | Return from watchdog connection in main |
| x20 0x10084f90c | 0x10000890c | LC_MAIN |

This confirms **missing IOWatchdog**, before IOServiceOpen, PMU lookup or
external methods. It is not an observed hardware bite or PMU failure.
The original 21H461 daemon SHA-256 is
ded4c688e1c2add74acf3e7f9eea5fe2c2c569180ce81492221877944d913a01.

The previous diagnostic's image0 is IOKit.framework. Dyld image order does not
guarantee that image0 is the executable. The new diagnostic passes the actual
task mach_header_vm_address from OSFMK into the BSD snapshot and labels image0
as `dyld-first`. Use `main_header` for future main-image addresses.

## Original policy with the SM7250 watchdog

PicassoWatchdog derives from the original 21H461 IOWatchdog (size 0xf8).
The original AppleARMWatchdogTimer kext remains in the prelink closure: APFS
depends on its metaclass, global and event handler. Loading that module registers
the parent and entitled UserClient without attaching the Apple hardware driver
to fabricated DT/MMIO.

The native service retains original policy start/check, stop, newUserClient,
root/entitlement checks, APFS defang and panic behavior. Its three hardware slots
use the actual SM7250 APSS watchdog at 0x17c10000. The board currently boots one
CPU; this does not implement Apple's per-core checker for future SMP.

The service prepares a workloop and high-priority timer before claiming hardware.
Claim permanently retires USB's independent boot pets. Each 1-second callback
executes the original policy check before petting the 15-second hardware
watchdog. The original first deadline (600 seconds), subsequent checkin deadline
(90 seconds) and daemon's normal target checkin interval (10 seconds) remain
distinct. A policy panic cannot reach the subsequent pet.

All hardware writers share the IRQ-protected, nonblocking ownership lock.
Readbacks check enable/bark/bite; the write-only reset pulse has no invented
acknowledgement. Panic, reset and forced-bite paths permanently reject ordinary
operations. Ownership is never released back to USB. Before claim, resource
failure cleans up; after claim, a failed start, enable, pet, timeout scheduling
or attempted free panics.

The original registerService call is asynchronous. After starting the timer,
bootstrap waits up to 5 seconds for real IOWatchdog matching and checks instance
identity. It loads the original module before explicitly registering the native
metaclass with preModLoad/postModLoad.

## Volatile boot-stage interface

Original main connects to IOWatchdog, then matches
`IOService + IOPropertyMatch={IOPMUPrimary=1}`, then calls selectors 0, 1 and 9.
Missing PMU would hit a different BRK, 0x10000b160. That is a binary dependency
established by analysis; it has not yet been observed on this phone. Returning
a disabled watchdog does not skip this lookup.

PicassoBootState implements only the known boot-stage property API. It publishes
IOPMUPrimary=1 and initial IOPMUBootStage=0, with explicit `volatile-software`
and HardwarePMUImplemented=false properties. Administrator-only setProperties
accepts exactly one OSNumber boot stage in 0..255, samples it once, and stores
an independent number under the registry property lock. Unknown operations,
extra keys, invalid types/ranges and allocation failures return errors.
Repeated stage-255 writes produce one log.

It reuses an existing primary service when present and verifies property
matching after synchronous publication. The value lasts for this boot only.
This is not a PMIC, battery, persistent boot history or power-control
implementation. Other system clients may require additional real capabilities.

## Build integration and ABI checks

scripts/build-picasso-pmgr.py replays the actual IOPlatformExpert C++ compile
record for Pmgr, Watchdog and BootState, preserving kernel macros, SDK and target
flags. It redirects dependency/diagnostic outputs to protect the original
record. All objects must be ordinary ARM64, iOS 17 Mach-O objects.

The board pins the original kernelcache SHA and prelink slide.
prepare-picasso-watchdog-bindings.py checks the complete image, class sizes,
171 virtual slots, function fingerprints and APFS references. The compiled
Watchdog object's inherited slots, permitted overrides, original imports and
parent metaclass are audited before and after late conversion. The alias object
must contain exactly the checked original targets.

Watchdog and BootState static constructors must run late. LLVM objcopy removes
their automatic __mod_init_func section. LLVM 18 does not support
--globalize-symbol for Mach-O, so the builder exports the exact initializer by
updating N_EXT, nlist groups, LC_DYSYMTAB, relocation indices and indirect-symbol
indices. Unsupported auxiliary tables fail closed. Every executable section
must retain identical bytes. The final object must have no automatic
initializer and must export the required initializer.

Only Pmgr.o, Watchdog.late.o, BootState.late.o and watchdog-bindings.o enter
the existing libShengPlatformPower.a force-load path. A new archive is checked
for exact membership before installation; the previous archive is moved to a
unique backup directory.

Both manual workflows fetch KernelCache before archive building and export
UNIXNU_KERNELCACHE. The fixed upstream cmd_ipsw special-cases KernelCache and
calls its decompressor, rather than merely extracting the compressed member.
Both workflows upload platform JSON reports, compiler logs, objects and archive.

Late-registration ordering was informed by the tutorial-linked
[watchdog service notes](https://github.com/mouse233/unixnu-fuxi/blob/feat/ramdisk-test/diagnostics/ram-root/watchdog-service.md)
and sheng's platform archive builder. This board uses its real SM7250 backend.

## Verification and next checkpoint

Local reports are under out/watchdog-integration-20261030/:

- 16 builder/ABI checks use real LLVM ARM64 Mach-O schema objects, including a
  real C++ initializer, a separate-object link with no unresolved symbols,
  virtual-slot negatives, complete archive assembly and old-archive retention.
  The schema inputs are not an SDK compilation of the production drivers.
- BootState: 12 startup scenarios in ordinary and ASan builds, plus property
  privilege/shape/bounds, single sampling, independent storage, failed-store
  preservation, publication and diagnostic-limit checks.
- Watchdog lifecycle: 10 scenarios in ordinary and ASan builds, covering cleanup
  before claim, permanent ownership after claim, policy-before-pet ordering,
  stop/enable/timer behavior and original client result preservation.
  IOKit, scheduling and original policy are host stand-ins.
- The unchanged backend and fixture match the prior 25+25 passing groups.
  The diagnostic source and fixture match the prior 56+56 passing cases.
- Both patch hashes and all 12 generated sources match the saved successful
  two-pass 61-patch replay. No identical replay was repeated.
- The downloaded build-69 kernel hash matches metadata and its UUID matches
  the device log. Python, TOML, YAML and workflow input ordering were checked.

There is no new production-driver iPhoneOS SDK compile or device qualification
in these results. No CI was dispatched and no phone operation was performed.
The latest successful build remains 40922207086/b67e848, which predates this
service implementation. A new manual SDK build must pass the real-object guards.

The next device log should show original IOWatchdog matching, original
newUserClient type 1 succeeding, the first original policy check/hardware pet,
and subsequent boot-stage progress. If another critical process fails, use its
new main_header/PC and original panic evidence to identify the next stop.

Reproduction from the device repository, using new output directories:

~~~sh
python scripts/check-picasso-platform-build.py --output out/platform-build-NEW \
  --kernelcache /path/to/original/kernelcache.macho \
  --native-prelink-dir /path/to/fixed/NativePrelink --llvm-bin /path/to/llvm/bin
python scripts/check-picasso-boot-state.py --output out/boot-state-NEW
python scripts/check-picasso-watchdog-service.py --output out/watchdog-service-NEW
python scripts/check-picasso-watchdog-backend.py --output out/watchdog-backend-NEW
python scripts/check-picasso-user-fault.py --xnu /path/to/replayed/xnu --output out/user-fault-NEW
~~~

The earlier build 40917708279 failed because BSD read an opaque struct task
field. Commit b67e848 moved the read to OSFMK and passed the cached slide into
BSD. Build 40922207086 subsequently compiled successfully. The new main_header
follows the same boundary; BSD still does not dereference task internals, and
the host regression retains an opaque task_t negative case.
