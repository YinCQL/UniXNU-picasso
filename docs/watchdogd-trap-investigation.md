# Watchdogd trap after early boot

## Build 40951382277: service found, open denied

[40951382277](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40951382277)
at 70bfbfdcb8d8c04f208868f45cf2a78b3ddaf132 completed successfully. Artifact
12932766022 / picasso-bootimg-72 is retained in
out/watchdog-open-20261030/build72/. Its 54,169,600-byte kernel SHA-256 is
d9293a8318d89f6bddc86e5603e594728e3e97c8ec2452d0d8eb2e9367593e13,
matching kernel.json. LC_UUID 4A215F1C-E7F3-3ADE-A70F-1AC5D5C00171 matches
the supplied picasso-windows-20261030-045518-105.log (150,584 bytes, SHA-256
06bb1f749a976c2b9b8a6d3a465d01a8d38be73cdb449fd83d016dbfe40b6e5e).
The user confirms this newest image and automatic reboot. The phone's actual
boot.img has not independently been hashed.

The log proves publication of the boot-stage API, original watchdog policy,
first policy check/hardware pet, and native instance matching. Data and early
boot complete again. PID 95 / thread 417 finds IOWatchdog (`kr=0 found=1`),
then traps at PC 0x1007d7b54, main_header 0x1007cd000. Subtracting the actual
main slide gives original BRK 0x10000ab54: IOServiceOpen failure. x20 holds
0xe00002e2, kIOReturnNotPermitted. The matching frame returns map to
0x10000682c and 0x100008958. This is beyond the prior missing-service trap.
Original critical-exit panic, flush and PSHOLD reset follow; core-file ENOENT
is secondary.

There is no original newUserClient diagnostic. In the fixed IOUserClient.cpp,
mac_iokit_check_open_service runs before newUserClient and returns that error
directly. The retained Sandbox hook is 0xfffffff0069fb914, operation 64;
post-client open is operation 63 at 0xfffffff0069ffadc. The actual original
daemon's embedded entitlements include both IOWatchdogUserClient and
com.apple.private.iowatchdog.user-access. Its service-class rule in the
retained Sandbox data still names AppleARMWatchdogTimer, whereas the native
service is a real IOWatchdog subclass with its own class name.

### Board policy adaptation

The native class is now named **PicassoWatchdogDriver**. Its 21-byte name fits
the existing service-class literal exactly. This deliberately adapts one
board-specific Sandbox rule; it does not claim the Sandbox data is unchanged.
The daemon executable/entitlements, original client rule and original
root/type/entitlement/exclusive-open checks remain unchanged.

board.toml selects scripts/prelink-picasso.py. It invokes the fixed original
emitter in memory, qualifies the actual linked native class/superclass and
metaclass/client dispatch slots, then replaces only the 21 literal bytes in
the 32-byte atom at original VA 0xfffffff005838818 (candidate 0xfffffff004838818).
Encoded length, matching operators, alignment and offsets stay identical.
The neighboring watchdogd path, IOWatchdogUserClient and boot-stage rules are
checked against the pinned original. An exact original-image hash, slide,
selected Sandbox extent, unique atom, candidate hash and unmodified context
are mandatory. Existing candidate/report files and repeated adaptation are
rejected. Source kernelcache bytes are never modified.

The report records both candidate hashes and both atoms. Verification reverses
the literal replacement and reproduces the entire result/report. Both workflows
run that check against the linked driver; the normal payload check binds the
same kernel into FAT. No IOClassNameOverride, fabricated Apple superclass,
NativeSandboxGrantProcess, MAC bypass or watchdogd-disable option is added.
The hardware backend and lifecycle are unchanged; the C++ change is only the
real class rename. Its source filename/static initializer name remain stable,
and the 171-slot object audit uses the new mangled class symbols.

Local reports under out/watchdog-open-20261030/:

- policy-v2: 25 checks on actual build-72 policy bytes, wrong/stale/changed
  inputs, a real linked ARM64 class schema and wrapper output/error behavior.
  Only the 21 literal bytes differ in the policy test image. Wrapper emission
  and qualification are explicit stand-ins in the file-integration case;
  the linked-class gate is tested separately. Sandbox itself is not executed.
- builder-v1: 29 LLVM Mach-O builder/ABI checks with the new class symbols.
  These schema objects are not a new production SDK compile.
- service-v1: ordinary and ASan each pass 10 lifecycle scenarios, including
  preservation of original client errors; actual workloop declarations and
  the protected-access negative compile still pass.
- Python/TOML/YAML checks pass. Reversing the class rename exactly recovers
  the prior production C++. XNU/USB patch hashes still match the saved passing
  replay/backend reports, so those unchanged tests were not repeated.

New SDK/complete prelink and phone behavior remain unverified. The next log
must show original newUserClient type 1 success and further watchdogd/boot-stage
progress. Static policy correspondence is not a runtime Sandbox acceptance
result. The assistant has not dispatched CI or operated the phone.

### Tutorial and reference comparison, 2026-10-30

Read the local UniXNU docs at 066c8d8315f83749d28bcae2faf3595718646be5:
zh/porting/platform.md, drivers.md, concepts/security.md and prelink.md,
plus the watchdog sections linked from devices/fuxi-mouse233.md. The current
tutorial describes newer layouts; this board still uses its fixed 75fba878
upstream and kits/NativePrelink path.

| Reference | Inspected behavior and applicability |
| --- | --- |
| UniXNU-sheng, 08cde20e | board.toml and BootWatchdogPolicy.c disable watchdogd; its GPU/audio process-name grants bypass MAC checks. These are not this board's retained-watchdog solution. |
| UniXNU-sirius, 10f440b6 | Same daemon-disable bring-up policy, with RAM Data improvements. No equivalent original-client open fix found. |
| UniXNU-guacamole, 6269d315 | mkadt.py adds wdt/no-pmu for the later boot-stage dependency, not the current open-service denial. |
| UniXNU-hummer, 1322c2ad | Different SM8750/platform configuration; no IOWatchdog implementation or matching policy fix found in the searched tracked sources. |
| UniXNU-OnePlus-Ace5, aaa100d1 | Selects the original kext in first-entry configuration; no native watchdog archive is selected. Inclusion alone does not publish a working service. |
| Tutorial-linked unixnu-fuxi, 1d4bfaf1 | RamWatchdog.cpp and watchdog-service.md retain the original base/client with a software backend and late registration. v47 records matching fixes, not qualification of this open-service boundary. |

Reference files were only read. The Picasso SM7250 backend, original-client
contract and evidence requirements govern this implementation.

## Build 40948813010: Darwin archive symbol index

[40948813010](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40948813010)
at 0450ea008ba0e18485e6ad9facef9757dc2df6d6 failed in the archive membership
check. Artifact 12931725696 / picasso-bootimg-71 contains all three successfully
SDK-compiled service objects, both converted late objects, and successful
Watchdog raw/late ABI reports. The workloop access fix therefore passed the SDK
compiler, as did BootState. The final kernel link was not reached.

Apple ar lists its generated symbol index as the first member,
`__.SYMDEF SORTED`, followed by the exact four intended objects. The builder
incorrectly compared this listing directly with the four object names.
The earlier LLVM-based test missed the difference because llvm-ar t hides the
index. This was a false rejection by the builder, not an extra driver object.

The check now accepts one leading, exact Darwin index name (`__.SYMDEF`,
`__.SYMDEF SORTED`, `__.SYMDEF_64` or `__.SYMDEF_64 SORTED`) or a listing where
the tool hides the index. It still requires the same four object names in order,
and rejects other entries, duplicate indexes, missing/duplicate objects and
reordering. Build reports retain both the raw listing and the classified index.

31 checks pass in out/watchdog-archive-index-20261030/host-v1/. They include the
actual failed Apple listing, index/extra-object negatives, a full builder run
with Apple-style listing output, and preservation of the previous archive.
The downloaded SDK objects were independently checked against source/report
hashes, original ABI and late-init guards, then archived with LLVM's Darwin
writer. Each archived object's bytes equal the corresponding downloaded object.
No production C++ or XNU patch changed for this fix. A new CI run must still
verify final kernel linking, prelink and boot-image composition.

## Build 40945353230: public workloop gate API

[40945353230](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40945353230)
at 8c62b6fbd7b64751bfa4f0f9a02f92cf1763c9d4 failed. Its artifact
12929876207 / picasso-bootimg-70 contains the compiler command and nine errors
from direct calls to IOWorkLoop::closeGate/openGate in PicassoWatchdog.cpp.
Those methods are protected; the native service is not an event-source friend.
The earlier host fixture incorrectly exposed them as public and missed this
compile boundary. Bootstrap's missing archive symbols are expected before
archive construction and are not a separate final-link regression.

The service now uses the public IOWorkLoop::runAction API for start and all
three hardware operations. The fixed XNU implementation calls closeGate,
executes the action synchronously, then openGate on the same recursive lock.
Original policy start can therefore call the enable hook recursively, and the
timer callback can call the pet hook while already holding the workloop gate.
Resource preparation still precedes permanent claim; returning claim failure
opens the gate and permits cleanup. Post-claim failures retain the original
panic behavior. No virtual slots, object fields or hardware rules changed.

The regression extracts the actual Action typedef, runAction declaration and
access specifiers from the fixed XNU IOWorkLoop.h. Ordinary and ASan builds pass
all ten lifecycle scenarios with those declarations. A direct-access negative
compile fails with C2248 for both protected methods. Reports are retained in
out/watchdog-gate-api-20261030/service-v1/. This checks the actual access contract
with host IOKit/policy stand-ins. Build 40948813010 subsequently passed this
source compilation and the real-object ABI checks.

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

PicassoWatchdogDriver derives from the original 21H461 IOWatchdog (size 0xf8).
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

The SDK attempt in build 40945353230 found the protected-method error above.
Build 40948813010 compiled the services before the archive-listing rejection.
Build 40951382277/70bfbfd subsequently completed the entire build and booted
through service matching and hardware ownership. Its later open denial and
the new, still unqualified policy adaptation are described at the top.

The next device log should show original IOWatchdog matching, original
newUserClient type 1 succeeding, the first original policy check/hardware pet,
and subsequent boot-stage progress. If another critical process fails, use its
new main_header/PC and original panic evidence to identify the next stop.

Reproduction from the device repository, using new output directories:

~~~sh
python scripts/check-picasso-platform-build.py --output out/platform-build-NEW \
  --kernelcache /path/to/original/kernelcache.macho \
  --native-prelink-dir /path/to/fixed/NativePrelink --llvm-bin /path/to/llvm/bin
# Add --sdk-objects /path/to/downloaded/tree/build/sheng-platform-power
# to independently recheck and archive a CI run's real compiled objects.
python scripts/check-picasso-boot-state.py --output out/boot-state-NEW
python scripts/check-picasso-watchdog-service.py --xnu /path/to/replayed/xnu --output out/watchdog-service-NEW
python scripts/check-picasso-watchdog-backend.py --output out/watchdog-backend-NEW
python scripts/check-picasso-user-fault.py --xnu /path/to/replayed/xnu --output out/user-fault-NEW
python scripts/check-picasso-watchdog-policy.py --output out/watchdog-policy-NEW \
  --kernelcache /path/to/original/kernelcache.macho --candidate /path/to/build72/kernel.macho \
  --native-prelink-dir /path/to/fixed/NativePrelink --llvm-bin /path/to/llvm/bin
~~~

The earlier build 40917708279 failed because BSD read an opaque struct task
field. Commit b67e848 moved the read to OSFMK and passed the cached slide into
BSD. Build 40922207086 subsequently compiled successfully. The new main_header
follows the same boundary; BSD still does not dereference task internals, and
the host regression retains an opaque task_t negative case.
