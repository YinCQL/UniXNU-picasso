# Memory shortage after the union root fix

Build [77](https://github.com/YinCQL/UniXNU-picasso/actions/runs/40974341838)
at `726e4b6b1620df9e605365126f0db5edc4601998` reaches the authenticated union
root and writable Data, then kills system services for VM-page shortage.
The next change requests 768 MiB of actual loader-owned memory, up from
256 MiB. Its device allocation and subsequent service progress remain untested.

## Captured evidence

The user confirms the latest image and automatic reboot. The log is
`picasso-windows-20261030-075201-502.log`, 117,526 bytes, SHA-256
`41aab2573b21a00e7a60a1498fb935834ab9779ade807fecda485a14c09d244e`.
Its kernel UUID `D649FCD2-FB43-3F9E-8CDB-6944C559BB45` matches the downloaded
build77 kernel. Independent Android header and FAT-chain checks verified the
embedded kernel, packed Data, trust cache, ADT and loader configuration.

| Downloaded artifact | SHA-256 |
| --- | --- |
| kernel.macho | `ed8bf32777ace24b8c86731592dd9f335f19e8d039c7bc2a5e5aacfc71fb83d6` |
| boot.img | `b7554763374148a223268144d70596867d49fa4a4ff0b41dcae00dbfd5ff70d2` |

All three `NATIVE_OWNER_UNION` paths return `error=0 source_match=1`.
Cache finishes at stage 6 and Data at stage 4, both with error 0; Data is
writable. AES known-answer/interface checks and the original watchdog client
open also pass. The previous libobjc-trampolines missing-library SIGABRTs
are absent. This does not establish working display or healthy userspace.

SpringBoard exits three times, mediaserverd twice, mobiletimerd and audiomxd
once each with SIGKILL and `namespace=1 code=4`. In the pinned XNU,
`bsd/sys/reason.h:88` defines namespace 1 as `OS_REASON_JETSAM`, and
`bsd/sys/kern_memorystatus.h:294` defines code 4 as
`JETSAM_REASON_MEMORY_VMPAGESHORTAGE`. These are system page-shortage kills,
not a missing-library exception or a per-process memory-limit code.

The final panic is `watchdog timeout: no checkins from watchdogd in 90 seconds
(2 total checkins since monitoring last enabled)`, from kernel_task, followed
by panic flush and PSHOLD reset. The log proves memory pressure and the final
timeout; it does not contain watchdogd's blocked stack or prove why its
checkins stopped. No `PICASSO_IOKIT` lines were captured.

The actual build77 symbol table resolves the allocation stack as:

```text
kalloc_ext + 0x138
IOMalloc_internal + 0x58
ramSparseIO<...> + 0xc0                  (0xfffffff007bc7308)
NativeRamBlock::doAsyncReadWrite + 0x1a8 (0xfffffff007bc6110)
original IOStorageFamily / APFS
spec_strategy / buf_strategy / VNOP_STRATEGY / cluster_io
```

The 83 MiB figure describes the entire `kalloc.type.var4.4096` zone. The top
sample records 8,948 allocations / 36,651,008 bytes. Neither is an exact
RAM Data resident count or proof of a leak. The pinned NativeRamBlock
`d4d22d9` allocates one page per previously unwritten logical page, reuses
that page on subsequent writes, and releases its pages when freed. These
volatile disk contents must stay resident while mounted; reducing a reported
capacity or discarding occupied pages would not preserve storage semantics.

## Actual memory allocation

The payload used `darwinboot-memory-size=0x10000000`. The fixed DarwinBoot
`d099354` consumes that request, allocates a conventional-memory run, and
only then sets both `boot_args.memSize` and `memSizeActual` to the allocated
size. `dram_size` is a separate descriptive property; increasing it alone
does not allocate memory for XNU.

The committed FD SHA-256 remains
`037e28ec5d69fa24d472d518798c5397a8b950c3585814e1b23538742ac7c946`.
Inspection of its decompressed ARM64 PE modules confirms the active paths:

- DarwinBoot RVA `0x2bcc` references the memory-request property; `0x2c90`
  checks its location, size and alignment. RVAs `0x2000..0x2100` select
  type-7 conventional memory, check the complete range, call
  `AllocatePages(AllocateAddress, EfiLoaderCode, requested_pages, ...)`, and
  accept only success. RVAs `0x2330..0x2338` store that same size into both
  boot argument fields. Failure retains the firmware and its `NO WINDOW`
  diagnostic; it does not silently fall back to a smaller window.
- PicassoBootMemoryDxe adds at most `0x40000000` bytes (1 GiB) and only
  ranges ending at or below `0x100000000`. The actual instructions at RVAs
  `0x1624`, `0x1648`, `0x165c` and `0x16f4` agree with the source limits.
  It derives banks and exclusions from the supplied DT and existing GCD map.

The board now requests `0x30000000` (768 MiB), leaving headroom below that
firmware pool limit for other allocations and alignment. Runtime fragmentation
is still unknown: this is a request, not a claim that 768 MiB has been obtained.
If no suitable run exists, firmware allocation must fail.

Sheng's `0x130000000` means **4.75 GiB**. It is valid for the generic loader
parser but cannot be satisfied by Picasso's current firmware memory map.
Using it requires separately extending and validating Picasso's firmware RAM
mapping against its real DRAM and reserved regions. This change keeps the FD,
FAT payload size, Data image capacity and physical `dram_size` unchanged.

## Verification and next capture

The ADT check generates three trees, validates the exact little-endian 64-bit
request, and rejects six missing, old, malformed or misplaced handoffs. The
build's Data artifact check now repeats this check on the **FAT-extracted ADT**.

Host tests execute the extracted original loader parser and allocator with
synthetic UEFI services: eight allocation scenarios cover contiguous/adjacent
runs, fragmentation, reserved memory, alignment, allocation errors and the
unsatisfiable 4.75 GiB request. Both ordinary and ASan runs pass. Two boot-log
scenarios check missing arguments, values above 4 GiB and no writes to the
arguments. An ARM64/iOS17 compile of the extracted real boot types and logging
function verifies their offsets and format types; it is not a full SDK build.
Local evidence is retained in `out/memory-pressure-20261030/`.

`PicassoPmgr` now emits one `PICASSO_MEMORY boot` line after console startup,
reporting the actual physical base, `memSize`, `memSizeActual` and data end.
The next successful allocation should show `bytes=30000000 actual=30000000`.
Then check for page-shortage exits and renewed watchdog checkins. No new
device result is available yet. No watchdog, Jetsam, authentication or system
service policy is relaxed, and no phone operation or CI dispatch was performed.
