/* SPDX-License-Identifier: MIT */
#ifndef ARM_SMMU_4K_LIVE_H
#define ARM_SMMU_4K_LIVE_H
#include "ArmSmmu4KProgram.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Where an arena's table pages come from: one 4 KiB page at a time, as a walk first needs it (Linux io-pgtable-arm allocates its
 * tables so, and Apple's pmap its page-table pages): no table of the aperture exists before a mapping needs it. */
typedef struct {
    void *cookie;
    /* A zeroed 4 KiB table page the caller owns for this arena, cacheable as every table page of it: its physical address and a
     * writable CPU mapping of it. Nonzero: none could be had (nothing is changed). */
    int (*allocate)(void *cookie,uint64_t *physical,volatile uint64_t **mapping);
    /* The CPU mapping of a page allocate() gave this arena; NULL for any other physical address. */
    volatile uint64_t *(*find)(void *cookie,uint64_t physical);
    /* Nonzero when [physical, physical+bytes) overlaps a page allocate() gave this arena. */
    int (*overlaps)(void *cookie,uint64_t physical,uint64_t bytes);
} ArmSmmu4KTablePages;
typedef struct {
    ArmSmmu4KConfig config;           /* table_physical: the root table, a page of pages */
    uint64_t iova,bytes;              /* Bounded native VA aperture, no conversion. */
    unsigned upper_region;
    const ArmSmmu4KTablePages *pages;
    /* Optional caller-owned scratch for the physical-alias checks (one entry per range of the smaller compared side), used by one
     * call at a time; absent or too small, those checks compare pairwise. Never read as state. */
    uint32_t *alias_scratch;
    size_t alias_scratch_entries;
} ArmSmmu4KLiveArena;
typedef struct {
    ArmSmmu4KResult tables;
    uint64_t generation;
    unsigned prepared,poisoned;
    /* The context bank's register offset the last whole-verification update observed its image at; 0 before the first. */
    uint64_t context_offset;
    /* 1: a table the bank does not run while it is updated (one of several switched into TTBR0, ArmSmmu4KContextConfig.ttbr0_switched).
     * Its updates never observe the bank's image, which shows another table: the caller sets context_offset from the bank's owned
     * table (whose updates observed it) before the first update. The table graphs are still proved as for an installed table, and an
     * unmap still invalidates the table's own ASID, which also drops what it left cached while it ran. */
    unsigned detached;
} ArmSmmu4KLiveState;
typedef struct {
    void *cookie;
    /* Clean/publish the actual mapped table page for device walks. Required
     * even for coherent walks (where the platform may use ordering alone).
     * Return zero only after visibility is established. No fake success. */
    int (*publish_table_page)(void *,volatile uint64_t *,size_t bytes);
} ArmSmmu4KLiveVisibility;
typedef struct {
    const ArmSmmu4KLiveArena *arena;
    ArmSmmu4KLiveState *state;
    const ArmSmmu4KRange *ranges;
    size_t count;
} ArmSmmu4KLiveRegion;
typedef struct {
    size_t descriptor_reads,descriptor_writes,published_pages;
    unsigned complete,translation_removed,must_retain_resources;
    unsigned whole_verification;   /* this update proved the whole region (a region's first) */
    ArmSmmu4KProgramResult synchronization;
} ArmSmmu4KLiveResult;
/* Prepare an INACTIVE empty native4K arena over its root table (config.table_physical, a zeroed page of arena->pages). No other
 * table exists yet: a map allocates the tables its walk needs, publishes each before linking it and keeps it for the arena's life
 * (an unmap clears leaves only, as Linux's io-pgtable does for a page unmap). No MMIO, TLB or cache operation here; the caller
 * publishes the root before enabling a context with &state->tables. state must be zero initialized and never reset while a context
 * can walk the tables. The leaf budget bounds the aperture's pages. This is not activation. */
ArmSmmu4KStatus ArmSmmu4KLivePrepare(const ArmSmmu4KLiveArena *,ArmSmmu4KLiveState *);
/* Read-only exact graph check, including reserved empty intermediate tables.
 * Use before context activation and while masters are quiesced. This neither
 * observes a context/streams nor clears poison or authorizes resource release. */
ArmSmmu4KStatus ArmSmmu4KLiveAudit(const ArmSmmu4KLiveArena *,const ArmSmmu4KLiveState *,
    const ArmSmmu4KRange *ranges,size_t count,ArmSmmu4KLiveResult *);
/* Incrementally map (map=1) an entirely invalid range, or unmap (map=0) an
 * exactly matching mapped range. Replacement requires unmap then map; valid
 * descriptors are never overwritten with different valid descriptors.
 * before describes ALL currently mapped resources, sorted by native IOVA.
 * Exact physical graph checks precede/follow changes to the actual pool.
 * A map first links the tables its leaves need (empty tables: no translation changes); a table that cannot be had refuses with
 * ArmSmmu4KCapacity before any leaf is written, with the arena still consistent. No block mappings or table retirement.
 *
 * Requires a SINGLE enabled region rooted in this arena, an exclusive powered
 * bank/ASID/stream lease, installed fault handling, and ALL masters quiesced
 * until successful completion. Caller establishes/retains actual CPU->PA
 * ownership, attributes, object references, data-cache ordering and queue
 * serialization. Metadata, arrays, state/results and table storage are disjoint
 * and immutable during each call except the explicit outputs/table writes.
 * These conditions are platform responsibilities, not caller boolean proofs.
 * On any uncertain context/table/cache/TLB failure the state is poisoned and
 * all old/new resources and tables must remain owned until an independently
 * verified quiesce/fault/disable/reset. No automatic rollback/replay occurs.
 * Successful unmap removes translation only; it does not generate a GPU fence
 * or establish completion of previous DMA. No hardware ownership is inferred
 * from declared addresses. The software tests are not hardware qualification. */
ArmSmmu4KStatus ArmSmmu4KLiveUpdate(const ArmSmmu4KLiveArena *,ArmSmmu4KLiveState *,
    const ArmSmmu4KIo *,unsigned bank,const ArmSmmu4KContextConfig *,
    const ArmSmmu4KLiveVisibility *,const ArmSmmu4KRange *before,size_t count,
    const ArmSmmu4KRange *change,unsigned map,unsigned poll_budget,ArmSmmu4KLiveResult *);
/* Full enabled-context variant: supply exactly its enabled lower/upper regions.
 * Both actual pool graphs are audited; cross-pool resource access, pool CPU/PA
 * aliases and inconsistent cross-region physical aliases refuse before writes.
 * Context roots/configs must match the actual supplied arena/state pairs.
 * AuditContext is read-only and does not observe hardware or release anything.
 * UpdateContext edits only the selected region, publishes its changed pages,
 * synchronizes the actual shared ASID and rechecks BOTH graphs. Any uncertainty
 * poisons both states and retains all resources. All ownership/quiescence/cache
 * preconditions of LiveUpdate apply to the entire context. No context switch,
 * TTBR change, copied/converted/shadow mapping or GPU fence is implemented. */
ArmSmmu4KStatus ArmSmmu4KLiveAuditContext(const ArmSmmu4KContextConfig *,
    const ArmSmmu4KLiveRegion *lower,const ArmSmmu4KLiveRegion *upper,ArmSmmu4KLiveResult *);
ArmSmmu4KStatus ArmSmmu4KLiveUpdateContext(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,
    const ArmSmmu4KLiveVisibility *,const ArmSmmu4KRange *change,unsigned map,
    unsigned poll_budget,ArmSmmu4KLiveResult *);
/* UpdateContext for several ranges of one region in one transaction: changes are
 * sorted by native IOVA and disjoint; map wants each entirely invalid, unmap
 * each exactly mapped. Every check of UpdateContext covers all of them before
 * the first store; their leaf pages are published once, the ASID synchronized
 * once and both graphs rechecked once. A buffer of scattered physical pages is
 * one change per page, so its mapping costs one transaction, not one per page.
 * The changes may lie inside the target region's ranges (an unmap). */
ArmSmmu4KStatus ArmSmmu4KLiveUpdateContextBatch(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,
    const ArmSmmu4KLiveVisibility *,const ArmSmmu4KRange *changes,size_t change_count,
    unsigned map,unsigned poll_budget,ArmSmmu4KLiveResult *);
/* UpdateContextBatch for a map whose pages the caller has cleared of cache-attribute aliases with every existing range of both
 * regions: it owns them exclusively, or it checked them against those ranges itself. The changes are not checked against the
 * existing ranges again (they still are against each other). Every other check stays. Those alias passes walked every existing
 * range per class on each map (i568: 52609 ranges, 0.4 ms a map). */
ArmSmmu4KStatus ArmSmmu4KLiveMapContextBatchExclusive(const ArmSmmu4KIo *,unsigned bank,
    const ArmSmmu4KContextConfig *,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,
    const ArmSmmu4KLiveVisibility *,const ArmSmmu4KRange *changes,size_t change_count,
    unsigned poll_budget,ArmSmmu4KLiveResult *);
#ifdef __cplusplus
}
#endif
#endif
