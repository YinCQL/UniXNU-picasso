# NativeUfs

Native UFS storage driver for Redmi K30 5G (picasso, SM7250). It owns the host
controller, SMMU, command queues, authenticated OS/SystemOS reads and the
backing-store APIs used by persistent Data and records.

Linked into XNU by `unixnu build kernel`. Status below reflects the
`feat/fix-v3` port and hardware checks through 2026-11-18.

## Upstream port baseline

NativeUfs and ShengUfs share the original driver lineage; upstream renamed
NativeUfs to ShengUfs. This port retains the NativeUfs source/runtime names,
Picasso board configuration and Darwin 23 ABI, and adapts selected upstream
changes rather than replacing the driver wholesale.

| Component | Upstream revision used for this port |
| --- | --- |
| [ShengUfs](https://github.com/UniXNU/ShengUfs) | `main` at [`f020bd693c34790c04e97c52720af3cd842336fb`](https://github.com/UniXNU/ShengUfs/tree/f020bd693c34790c04e97c52720af3cd842336fb) |
| [ArmCpu](https://github.com/UniXNU/ArmCpu), supporting IRQ fixes | `main` at [`73c2360509690986d0d44b4da7236a653b91d0d7`](https://github.com/UniXNU/ArmCpu/tree/73c2360509690986d0d44b4da7236a653b91d0d7) |

Both upstream `main` revisions were checked on 2026-11-18. These are source
commit baselines, not a claim of complete feature or platform parity.

## Implemented in this port

| Area | Current implementation |
| --- | --- |
| Runtime queue | Separate bootstrap/runtime command paths; per-slot UTRD/UCD/data areas and PRDT scratch. Default: four allocated slots, three usable for normal commands, with one descriptor left unused. Hardware logs observed three concurrent commands (`peak=3`). |
| Completion waiting | Real UFS completion IRQ through ArmCpu SPI 297, a default 200 us polling budget, generation-based wakeup checks and sleeps bounded to at most 1 ms. Bounded polling/yield remains the fallback when IRQ setup or sleeping is unavailable. |
| DMA and failures | Explicit transfer direction, cache synchronization, capacity/address checks and bounded SCSI BUSY retries. Timeout, fatal and protocol failures quarantine the transport; descriptors and DMA buffers potentially still owned by hardware are retained until reboot, without reuse or resubmission. |
| Paged verification table | Default 8 MiB cache with 2,048 4 KiB pages, filled from the protected embedded verification table. The approximately 1.95 MiB complete embedded table is retained; this is not a header-only disk-format migration or a demonstrated memory saving. |
| Authentication snapshots | Eight 128 KiB driver-owned record snapshots per authenticated window, with concurrent reservation and verification before data is copied to a requester. These are separate from the verification-table cache. |
| Direct DMA | Enabled by default only for a complete 128 KiB authentication snapshot owned by the driver. Every page is checked against physical-address, controller CAP and SMMU limits, with direction-specific DMA synchronization. Snapshot allocation failure falls back to staging; non-owned destinations, LUN4 firmware reads and writes remain staged. |
| LUN4 firmware access | GPT/header and partition-array CRC, capacity, name and overlap checks, plus bounded modem-firmware reads. The existing strict Picasso partition path is retained. |
| Persistence | Runtime store/record integration preserves FUA writes, flushes and record ordering for the existing persistent Data consumers. |
| Media properties | Authenticated UFS-backed media report SSD and Internal properties. |
| GCC mapping | Corrected SM7250 UFS clock-register mappings for AXI/AHB/UniPro/ICE/AUX/TX/RX and related diagnostics. GCC means Qualcomm Global Clock Controller, based at `0x00100000`; the update only reads these registers and does not change clock rates. |
| Link diagnostics | Read-only local `DME_GET` for TX/RX gear, active lanes, HS series and power mode. A dedicated UIC try-lock, short submission lock and 500 ms deadline per query preserve transfer-completion/error bits. Timeout/fatal after dispatch disables subsequent UIC diagnostics until reboot; no `DME_SET` or link reconfiguration is performed. |
| Statistics | Atomic completion-wait buckets, polling time, IRQ-observation-to-thread-resume latency, short sleep timeouts, slot contention, oldest in-flight command and first quarantine reason. Store read/write/sync, root-window calls, snapshot waits and table-cache counters include counts, bytes, timing and failures where applicable. A separate thread emits at most eight samples, 15 seconds apart. Pool/table snapshots use try-lock and report `snapshot=0` when unavailable. |

The supporting ArmCpu port restores 17 Darwin multi-instruction assembly blocks,
fixes SGI operands, serializes SPI registration and limits SGI self-tests to
owned SGI 0. IRQ qualification preserves the original IGROUPR bits when the
non-secure register view is read-as-zero/write-ignored. Picasso's routing,
affinity conversion, redistributor stride, 6E/2P topology and frequency policy
are retained.

Relevant source entry points:

- [RuntimeSlots.h](RuntimeSlots.h), [RuntimeQueue.h](RuntimeQueue.h) and
  [RuntimePlatform.h](RuntimePlatform.h): commands, direct DMA and board/IRQ setup.
- [RuntimeInterruptWait.h](RuntimeInterruptWait.h) and
  [RuntimeDme.h](RuntimeDme.h): bounded waits and read-only UIC commands.
- [VerifiedTableCache.h](VerifiedTableCache.h),
  [ConcurrentRecords.h](ConcurrentRecords.h) and
  [NativeUfs.cpp](NativeUfs.cpp): verification caches, firmware and persistence.
- [RuntimeMetrics.h](RuntimeMetrics.h) and
  [ArmCpu.cpp](../ArmCpu/ArmCpu.cpp): metrics and supporting CPU/interrupt changes.

## Not ported or not enabled

| Upstream feature or experiment | Decision and reason |
| --- | --- |
| Cached store writes without FUA | Not enabled for current consumers. Data and record persistence depend on FUA completion semantics; there is no current consumer requiring the cached-write interface. |
| Generic partition find/read interfaces | Not substituted for the stricter local LUN4 validation and bounded firmware path. |
| Dedicated panic writer / takeover of normal slots | One descriptor remains unused, but no dedicated takeover writer is implemented. A normal slot or buffer may still be owned by DMA after a timeout and must not be reused. |
| WFE waiting / runtime wait-mode setter | Not enabled. Per-CPU eventstream and deadline wakeups have not been qualified; bounded polling/yield and IRQ sleeps are retained. |
| Header-only paged-table disk format | Deferred. It requires an independent image, policy and authentication migration; the complete protected embedded table remains in use. |
| APFS `root=2` / version 3 experiment | Not adopted. Upstream withdrew this experiment; the existing authenticated OS/SystemOS/Data layout is retained. |
| SM8550 DPU/SMMU registers and additional streams | Not applicable to Picasso's SM7250 hardware. Board register addresses and stream IDs are not copied from that platform. |
| Larger command queues | The source can configure more slots, but the accepted default is four allocated / three usable. A 32-slot configuration has not passed hardware qualification. |
| ArmCpu shared SPI dispatch, MMIO accessor replacement, platform suspend/idle and frequency-policy changes | Deferred to separate board/CPU work. The supporting fixes do not qualify these additional upstream features. |

## Validation and limits

The port was checked in stages: single-slot polling, real single-slot IRQ,
four-slot/three-command concurrency, owned-snapshot direct DMA, then DME and
statistics. Local commits record these stages:

| Local commit | Scope |
| --- | --- |
| `2599f9a` | Runtime, caches, GCC mapping, media properties and strict firmware/persistence handling in polling mode |
| `b8fa843` | Supporting ArmCpu assembly, registration and owned-SGI fixes |
| `f60e919` | Qualified single-slot IRQ completion and non-secure GIC readback handling |
| `2953bb8` | Four-slot queue with three usable normal slots |
| `3c1bc87` | Direct DMA to driver-owned authentication snapshots |
| `2205875` / `d873ec8` | Read-only DME diagnostics and completion/persistence statistics; qualified together in the final combined image |

Production-path host checks, ASan/UBSan checks, real SDK builds, object/archive
and final-link verification, and boot-image checks passed for the accepted
images. Hardware logs showed real IRQs, `peak=3`, direct and staged transfers,
all six link attributes valid, and successful authenticated-volume and
20 GiB Data/NVRAM restoration. The observed link was TX/RX gear 3, one lane in
each direction, HS series 2 and power mode `0x11`, with no UIC quarantine.
The user confirmed desktop entry without a grey screen or automatic restart
during the accepted checks; later ADSP images also passed speaker checks.

This does not establish controlled throughput/power gains, Data endurance or
long-term stability. Short sleep timeouts are scheduling-wait expirations,
not UFS command timeouts. Counters are approximate across concurrent updates;
missing or interleaved serial fields must not be interpreted as zero.
