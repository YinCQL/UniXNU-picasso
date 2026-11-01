/* SPDX-License-Identifier: MIT */
/* Host memory and register fixtures only; no physical device is accessed. */
#include "ArmSmmu4KLive.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define POOL_PAGES 16
#define MMIO_BYTES 0x40000
extern uint64_t source_permission_pte(unsigned,unsigned);
static _Alignas(4096) uint64_t tables[POOL_PAGES*512],snapshot[POOL_PAGES*512];
static _Alignas(4096) uint64_t peer_tables[POOL_PAGES*512];
static ArmSmmu4KLiveArena peer_arena;
static ArmSmmu4KLiveState peer_state;
static unsigned pair_mode;
/* Table pages handed out one at a time from a fixture pool, as a driver allocates them: zeroed when given, found by their physical
 * address, untouched (0xa5) until then. refuse_after: allocations left before the provider refuses (SIZE_MAX: never). */
typedef struct {uint64_t *pool;uint64_t physical;size_t used,refuse_after;} Pages;
static Pages main_pages,peer_pages;
static int page_allocate(void *cookie,uint64_t *physical,volatile uint64_t **mapping)
{
    Pages *p=cookie;
    if(p->used>=POOL_PAGES || !p->refuse_after)return 1;
    if(p->refuse_after!=SIZE_MAX)--p->refuse_after;
    memset(p->pool+p->used*512,0,4096);
    *physical=p->physical+p->used*4096;*mapping=p->pool+p->used*512;++p->used;return 0;
}
static volatile uint64_t *page_find(void *cookie,uint64_t physical)
{
    Pages *p=cookie;
    if(physical<p->physical || (physical&4095) || (physical-p->physical)/4096>=p->used)return 0;
    return p->pool+(physical-p->physical)/8;
}
static int page_overlaps(void *cookie,uint64_t physical,uint64_t bytes)
{Pages *p=cookie;return physical<p->physical+p->used*4096 && p->physical<physical+bytes;}
static const ArmSmmu4KTablePages main_provider={&main_pages,page_allocate,page_find,page_overlaps};
static const ArmSmmu4KTablePages peer_provider={&peer_pages,page_allocate,page_find,page_overlaps};
/* A fresh provider over pool at physical, its root allocated. */
static uint64_t pages_reset(Pages *p,uint64_t *pool,uint64_t physical)
{
    *p=(Pages){pool,physical,0,SIZE_MAX};
    uint64_t root;volatile uint64_t *mapping;
    CHECK(!page_allocate(p,&root,&mapping) && root==physical);
    return root;
}
typedef struct {
    uint64_t registers[MMIO_BYTES/8];
    size_t calls,fail_call,publication_calls,fail_publication,tlbi_calls,sync_calls,page_tlbi_calls;
    size_t writes,barriers,cb;
    unsigned after_write,timeout,dirty,require_published,mutate_on_publish;
    unsigned synced,clean_observed;
} Fixture;
static Fixture f;
static ArmSmmu4KLiveArena arena;
static uint32_t alias_scratch[64];   /* the main arena's alias checks sort; the peer's compare pairwise */
static ArmSmmu4KLiveState state;
static ArmSmmu4KContextConfig context;
static ArmSmmu4KIo io;
static ArmSmmu4KLiveVisibility visibility;
static ArmSmmu4KLiveResult result;
static uint32_t get32(size_t at)
{uint32_t value;memcpy(&value,(char *)f.registers+at,4);return value;}
static uint64_t get64(size_t at)
{uint64_t value;memcpy(&value,(char *)f.registers+at,8);return value;}
static void put32(size_t at,uint32_t value){memcpy((char *)f.registers+at,&value,4);}
static void put64(size_t at,uint64_t value){memcpy((char *)f.registers+at,&value,8);}
static int call(void *cookie,size_t at,size_t bytes)
{CHECK(cookie==&f && !(at&(bytes-1)) && at<=MMIO_BYTES-bytes);return ++f.calls==f.fail_call;}
static int read32(void *cookie,size_t at,uint32_t *value)
{
    if(call(cookie,at,4))return 1;
    *value=at==f.cb+0x7f4 && f.timeout?1:get32(at);
    if(at==f.cb+0x7f4 && !(*value&1))f.synced=1;
    return 0;
}
static int read64(void *cookie,size_t at,uint64_t *value)
{if(call(cookie,at,8))return 1;*value=get64(at);return 0;}
static int write32(void *cookie,size_t at,uint32_t value)
{
    int fail=call(cookie,at,4);++f.writes;
    if(fail && !f.after_write)return 1;
    if(f.require_published) {
        /* Live updates must not disable translation or change another bank,
         * roots, granules, stream routes or interrupt/fault registers. */
        CHECK(at==f.cb+0x610 || at==f.cb+0x7f0);
        CHECK(f.publication_calls && f.clean_observed && !f.dirty);
    }
    if(at==f.cb+0x610) {CHECK(value==context.asid);++f.tlbi_calls;f.synced=0;}
    if(at==f.cb+0x7f0) {CHECK((f.tlbi_calls || f.page_tlbi_calls) && value==UINT32_MAX);++f.sync_calls;}
    if(at==f.cb+0x58)put32(at,get32(at)&~value);
    else put32(at,value);
    return fail;
}
static int write64(void *cookie,size_t at,uint64_t value)
{
    int fail=call(cookie,at,8);++f.writes;
    /* The one 64-bit write of a live update: TLBIVAL of a removed page, tagged with the context's ASID. */
    if(at==f.cb+0x620) {CHECK((value>>48)==context.asid);++f.page_tlbi_calls;f.synced=0;}
    else CHECK(!f.require_published);
    if(fail && !f.after_write)return 1;put64(at,value);return fail;
}
static int barrier(void *cookie)
{CHECK(cookie==&f);++f.barriers;return ++f.calls==f.fail_call;}
static int publish(void *cookie,volatile uint64_t *page,size_t bytes)
{
    CHECK(cookie==&f && bytes==4096 && !((uintptr_t)page&4095));
    uintptr_t at=(uintptr_t)page;
    CHECK((at>=(uintptr_t)tables && at-(uintptr_t)tables<main_pages.used*4096) ||
        (pair_mode && at>=(uintptr_t)peer_tables && at-(uintptr_t)peer_tables<peer_pages.used*4096));
    ++f.publication_calls;
    CHECK(!f.tlbi_calls && !f.sync_calls);
    if(f.publication_calls==f.fail_publication)return 1;
    if(f.publication_calls==1) {
        if(f.mutate_on_publish==1)put64(f.cb+0x28,get64(f.cb+0x28)^4096);
        /* A root slot no update writes (the first publication is of a table about to be linked into slot 0). */
        if(f.mutate_on_publish==2)tables[1]^=1;
        if(f.mutate_on_publish==3)put32(f.cb+0x58,2);
        if(f.mutate_on_publish==4)peer_tables[0]^=1;
    }
    f.dirty=0;f.clean_observed=1;return 0;
}
static void reset_counters(void)
{
    f.calls=f.fail_call=f.publication_calls=f.fail_publication=0;
    f.tlbi_calls=f.sync_calls=f.page_tlbi_calls=f.writes=f.barriers=0;
    f.after_write=f.timeout=f.mutate_on_publish=f.synced=f.clean_observed=0;
    f.require_published=f.dirty=1;
}
static void setup(unsigned bits,unsigned upper,unsigned output,unsigned spacing,uint64_t low)
{
    memset(&state,0,sizeof(state));memset(&f,0,sizeof(f));memset(tables,0xa5,sizeof(tables));
    pair_mode=0;
    const uint64_t root=pages_reset(&main_pages,tables,0x10000000);
    arena=(ArmSmmu4KLiveArena){{bits,output,root,16},
        (upper?UINT64_MAX-(UINT64_C(1)<<bits)+1:0)+low,4*4096,upper,&main_provider,alias_scratch,64};
    CHECK(ArmSmmu4KLivePrepare(&arena,&state)==ArmSmmu4KSuccess);
    /* Only the root exists: every other table is made by the first map that needs it. */
    CHECK(state.prepared && !state.poisoned && !state.tables.leaf_pages && state.tables.table_pages==1 && main_pages.used==1);
    ArmSmmu4KLiveResult audit;
    CHECK(ArmSmmu4KLiveAudit(&arena,&state,0,0,&audit)==ArmSmmu4KSuccess);
    CHECK(audit.complete && audit.must_retain_resources && audit.descriptor_reads==512);
    for(size_t i=512;i<POOL_PAGES*512;++i)CHECK(tables[i]==UINT64_C(0xa5a5a5a5a5a5a5a5));
    context=(ArmSmmu4KContextConfig){upper?0:&arena.config,upper?&arena.config:0,
        upper?0:&state.tables,upper?&state.tables:0,0,0,0x1234,1,1,1,0};
    put32(0x20,1u<<30);put32(0x24,spacing?0x80000002:0x40000010);put32(0x28,0x1555);
    f.cb=0x20000;
    io=(ArmSmmu4KIo){&f,MMIO_BYTES,read32,read64,write32,write64,barrier};
    visibility=(ArmSmmu4KLiveVisibility){&f,publish};
    ArmSmmu4KProgramResult r;
    CHECK(ArmSmmu4KProgramDisabledContext(&io,0,&context,4,&r)==ArmSmmu4KSuccess);
    reset_counters();
}
static ArmSmmu4KRange resource(unsigned pages,unsigned permissions,unsigned cache)
{return (ArmSmmu4KRange){arena.iova,0x30000000,pages*4096,permissions,cache,cache?3:2};}
/* Deliberately different page-walk arithmetic and original Linux permission
 * oracle, not the live updater's traversal/descriptor helpers. */
static uint64_t pte_from(const ArmSmmu4KLiveArena *a,const ArmSmmu4KLiveState *s,const uint64_t *pool,uint64_t va)
{
    size_t at=0;unsigned level=s->tables.start_level;
    for(;;) {
        unsigned shift=12+9*(3-level),width=level==s->tables.start_level?a->config.input_bits-shift:9;
        uint64_t entry=pool[at+((va>>shift)&((1u<<width)-1))];
        if(!entry)return 0;
        CHECK((entry&3)==3);
        if(level==3)return entry;
        uint64_t physical=entry&UINT64_C(0x0000fffffffff000);
        CHECK(physical>=a->config.table_physical && physical-a->config.table_physical<s->tables.table_pages*4096);
        at=(size_t)((physical-a->config.table_physical)/8);++level;
    }
}
static uint64_t pte(uint64_t va) {return pte_from(&arena,&state,tables,va);}
static ArmSmmu4KStatus update(const ArmSmmu4KRange *before,size_t count,const ArmSmmu4KRange *change,unsigned map)
{return ArmSmmu4KLiveUpdate(&arena,&state,&io,0,&context,&visibility,before,count,change,map,4,&result);}
static void complete(unsigned removed,size_t pages)
{
    CHECK(result.complete && result.translation_removed==removed && !result.must_retain_resources);
    CHECK(!state.poisoned && state.tables.leaf_pages==pages);
    /* A whole-verification update observes the context and synchronizes its ASID; between them an unmap synchronizes it alone and a
     * map of invalid leaves needs neither. */
    if(result.whole_verification)CHECK(result.synchronization.tlb_synchronized && f.tlbi_calls==1 && f.sync_calls==1 && f.synced && f.barriers>=4);
    else if(removed)CHECK(result.synchronization.tlb_synchronized && (f.page_tlbi_calls?!f.tlbi_calls:f.tlbi_calls==1) &&
        f.sync_calls==1 && f.synced && f.barriers>=2);
    else CHECK(!result.synchronization.tlb_synchronized && !f.tlbi_calls && !f.page_tlbi_calls && !f.sync_calls);
    /* A whole-region update reads every descriptor of every table; the others read each changed leaf's path before and after the store. */
    CHECK(result.whole_verification?result.descriptor_reads>=state.tables.table_pages*512:result.descriptor_reads>=2);
    CHECK(state.tables.table_pages==main_pages.used);
}
static unsigned profiles(void)
{
    unsigned count=0,outputs[]={32,36,40,42,44,48};
    for(unsigned bits=32;bits<=48;++bits)for(unsigned upper=0;upper<2;++upper)
    for(unsigned output=0;output<6;++output)for(unsigned permissions=1;permissions<16;permissions+=2)
    for(unsigned cache=0;cache<2;++cache) {
        uint64_t boundary=bits>=40?UINT64_C(1)<<39:UINT64_C(1)<<30;
        setup(bits,upper,outputs[output],cache,boundary-4096);
        ArmSmmu4KRange r=resource(2,permissions,cache);
        CHECK(update(0,0,&r,1)==ArmSmmu4KSuccess);complete(0,2);
        /* The two pages straddle a table boundary: two leaf tables. Each table made took one link write and two publications (itself
         * and its parent); each leaf table is published once more after its leaves. */
        const size_t made=state.tables.table_pages-1;
        CHECK(result.descriptor_writes==2+made && result.published_pages==2*made+2 && state.generation==1);
        for(unsigned i=0;i<2;++i)CHECK(pte(r.iova+4096*i)==((r.physical+4096*i)|source_permission_pte(permissions,cache)|3));
        CHECK(!pte(r.iova+8192));
        reset_counters();CHECK(update(&r,1,&r,0)==ArmSmmu4KSuccess);complete(1,0);
        CHECK(!pte(r.iova) && !pte(r.iova+4096) && state.generation==2);
        /* Reuse after a completed native invalidation, with different PA. */
        r.physical+=0x10000;reset_counters();
        /* The unmap kept the tables: the map again makes none and writes the two leaves alone. */
        const size_t tables_before=state.tables.table_pages;
        CHECK(update(0,0,&r,1)==ArmSmmu4KSuccess);complete(0,2);
        CHECK(state.tables.table_pages==tables_before && result.descriptor_writes==2 && result.published_pages==2);
        CHECK(pte(r.iova)==(r.physical|source_permission_pte(permissions,cache)|3));
        ++count;
    }
    return count;
}
static unsigned failures(void)
{
    ArmSmmu4KRange r;
    unsigned count=0;
    for(unsigned unmap=0;unmap<2;++unmap)for(unsigned after=0;after<2;++after) {
    /* The device accesses of this update without a failure: the first map's (whole verification), the following unmap's (not). */
    setup(48,1,48,0,0x1ff000);r=resource(2,3,1);
    CHECK(update(0,0,&r,1)==ArmSmmu4KSuccess);
    if(unmap) {reset_counters();CHECK(update(&r,1,&r,0)==ArmSmmu4KSuccess);}
    const size_t calls=f.calls;CHECK(calls);
    for(size_t failed=1;failed<=calls;++failed) {
        setup(48,1,48,0,0x1ff000);r=resource(2,3,1);
        if(unmap) {CHECK(update(0,0,&r,1)==ArmSmmu4KSuccess);reset_counters();}
        f.fail_call=failed;f.after_write=after;
        CHECK(update(unmap?&r:0,unmap?1:0,&r,!unmap)==ArmSmmu4KIoError);
        CHECK(state.poisoned && result.must_retain_resources && !result.complete && !result.translation_removed);
        size_t old_calls=f.calls,old_writes=f.writes;
        CHECK(update(unmap?&r:0,unmap?1:0,&r,!unmap)==ArmSmmu4KArgument);
        CHECK(f.calls==old_calls && f.writes==old_writes);++count;
    }
    }
    for(unsigned unmap=0;unmap<2;++unmap)for(unsigned fail=1;fail<=2;++fail) {
        setup(48,1,48,0,0x1ff000);r=resource(2,3,1);
        if(unmap) {CHECK(update(0,0,&r,1)==ArmSmmu4KSuccess);reset_counters();}
        f.fail_publication=fail;
        CHECK(update(unmap?&r:0,unmap?1:0,&r,!unmap)==ArmSmmu4KIoError);
        CHECK(state.poisoned && result.must_retain_resources && !result.complete && !f.writes);++count;
    }
    for(unsigned unmap=0;unmap<2;++unmap) {
        setup(48,1,48,0,0x1ff000);r=resource(2,3,1);
        if(unmap) {CHECK(update(0,0,&r,1)==ArmSmmu4KSuccess);reset_counters();}
        f.timeout=1;
        CHECK(update(unmap?&r:0,unmap?1:0,&r,!unmap)==ArmSmmu4KTimeout);
        CHECK(state.poisoned && !result.complete && result.must_retain_resources);
        CHECK(result.synchronization.poll_reads==4 && !result.synchronization.tlb_synchronized);++count;
    }
    for(unsigned mutation=1;mutation<=3;++mutation) {
        setup(48,1,48,0,0x1ff000);r=resource(2,3,1);f.mutate_on_publish=mutation;
        ArmSmmu4KStatus status=update(0,0,&r,1);
        CHECK(status==(mutation==3?ArmSmmu4KFault:ArmSmmu4KStateMismatch));
        CHECK(state.poisoned && result.must_retain_resources && !result.complete);++count;
    }
    return count;
}
static unsigned mutations(void)
{
    setup(48,1,48,0,0x1ff000);size_t slots=state.tables.table_pages*512;
    memcpy(snapshot,tables,sizeof(tables));ArmSmmu4KRange r=resource(2,3,1);
    unsigned count=0;
    for(size_t index=0;index<slots;++index)for(unsigned bit=0;bit<2;++bit) {
        memset(&state,0,sizeof(state));main_pages.used=slots/512;
        state.tables=(ArmSmmu4KResult){arena.config.table_physical,0xff44,slots/512,0,0,1};state.prepared=1;
        memcpy(tables,snapshot,sizeof(tables));tables[index]^=UINT64_C(1)<<(bit?53:0);reset_counters();
        CHECK(update(0,0,&r,1)==ArmSmmu4KStateMismatch);
        CHECK(state.poisoned && !result.descriptor_writes && !f.writes && !result.complete);++count;
    }
    return count;
}
static void refusals_and_partial(void)
{
    setup(48,1,48,0,0x1ff000);ArmSmmu4KRange r=resource(2,3,1),bad=r;
    memcpy(snapshot,tables,sizeof(tables));
    bad.bytes=4097;CHECK(update(0,0,&bad,1)==ArmSmmu4KBadRange);
    bad=r;bad.physical=arena.config.table_physical;CHECK(update(0,0,&bad,1)==ArmSmmu4KOverlap);
    bad=r;bad.iova=0;CHECK(update(0,0,&bad,1)==ArmSmmu4KBadRange);
    bad=r;bad.permissions=2;CHECK(update(0,0,&bad,1)==ArmSmmu4KBadRange);
    bad=r;bad.shareability=0;CHECK(update(0,0,&bad,1)==ArmSmmu4KBadRange);
    bad=r;bad.iova+=3*4096;CHECK(update(0,0,&bad,1)==ArmSmmu4KBadRange);
    CHECK(!f.calls && !state.poisoned && !memcmp(snapshot,tables,sizeof(tables)));
    CHECK(update(0,0,&r,0)==ArmSmmu4KStateMismatch);CHECK(!f.calls && !state.poisoned);
    CHECK(update(0,0,&r,1)==ArmSmmu4KSuccess);reset_counters();
    CHECK(update(&r,1,&r,1)==ArmSmmu4KStateMismatch);CHECK(!f.calls && !f.writes && !state.poisoned);
    bad=r;bad.physical+=4096;
    CHECK(update(&r,1,&bad,0)==ArmSmmu4KStateMismatch);CHECK(!f.calls && !state.poisoned);
    ArmSmmu4KRange first=r;first.bytes=4096;
    CHECK(update(&r,1,&first,0)==ArmSmmu4KSuccess);complete(1,1);CHECK(!pte(first.iova));
    ArmSmmu4KRange second=r;second.iova+=4096;second.physical+=4096;second.bytes=4096;
    reset_counters();first.physical+=0x10000;
    CHECK(update(&second,1,&first,1)==ArmSmmu4KSuccess);complete(0,2);
    CHECK(pte(first.iova)==(first.physical|source_permission_pte(3,1)|3));
    ArmSmmu4KRange both[]={first,second};reset_counters();
    CHECK(update(both,2,&first,0)==ArmSmmu4KSuccess);complete(1,1);
    reset_counters();CHECK(update(&second,1,&second,0)==ArmSmmu4KSuccess);complete(1,0);
    /* Final canonical page ending at 2^64 must remain representable. */
    setup(48,1,48,0,(UINT64_C(1)<<48)-4*4096);
    r=resource(1,3,1);r.iova=UINT64_MAX-4095;
    CHECK(update(0,0,&r,1)==ArmSmmu4KSuccess);complete(0,1);CHECK(pte(r.iova));
    reset_counters();CHECK(update(&r,1,&r,0)==ArmSmmu4KSuccess);complete(1,0);
    /* Explicit preparation bounds and no writes on malformed input: no provider, a root it never gave, a root that is not empty. */
    setup(48,0,48,0,0x1ff000);memcpy(snapshot,tables,sizeof(tables));
    ArmSmmu4KLiveState s={0};ArmSmmu4KLiveArena a=arena;
    a.config.leaf_budget=1;CHECK(ArmSmmu4KLivePrepare(&a,&s)==ArmSmmu4KBudget);
    a=arena;a.pages=0;CHECK(ArmSmmu4KLivePrepare(&a,&s)==ArmSmmu4KArgument);
    a=arena;a.config.table_physical+=4096;CHECK(ArmSmmu4KLivePrepare(&a,&s)==ArmSmmu4KArgument);
    tables[3]=1;CHECK(ArmSmmu4KLivePrepare(&arena,&s)==ArmSmmu4KStateMismatch && !s.prepared);tables[3]=snapshot[3];
    CHECK(!memcmp(snapshot,tables,sizeof(tables)));
}
static void setup_pair(unsigned bits,unsigned output,unsigned spacing)
{
    setup(bits,0,output,spacing,0x1ff000);pair_mode=1;
    memset(&peer_state,0,sizeof(peer_state));
    unsigned peer_bits=80-bits;
    const uint64_t peer_root=pages_reset(&peer_pages,peer_tables,0x11000000);
    peer_arena=(ArmSmmu4KLiveArena){{peer_bits,output,peer_root,16},
        UINT64_MAX-(UINT64_C(1)<<peer_bits)+1+0x1ff000,4*4096,1,&peer_provider,0,0};   /* pairwise alias checks */
    CHECK(!ArmSmmu4KLivePrepare(&peer_arena,&peer_state));
    context.upper=&peer_arena.config;context.upper_tables=&peer_state.tables;
    /* Explicitly reset the HOST REGISTER fixture, never a live hardware bank. */
    f.require_published=0;put32(f.cb,0);ArmSmmu4KProgramResult programmed;
    CHECK(!ArmSmmu4KProgramDisabledContext(&io,0,&context,4,&programmed));
    reset_counters();
}
static ArmSmmu4KStatus pair_update(ArmSmmu4KLiveRegion *lo,ArmSmmu4KLiveRegion *hi,
    const ArmSmmu4KRange *change,unsigned upper,unsigned map)
{return ArmSmmu4KLiveUpdateContext(&io,0,&context,lo,hi,upper,&visibility,change,map,4,&result);}
static unsigned pair_profiles(void)
{
    unsigned profiles=0,outputs[]={32,36,40,42,44,48};
    for(unsigned bits=32;bits<=48;++bits)for(unsigned oi=0;oi<6;++oi)for(unsigned permission=1;permission<16;permission+=2)
    for(unsigned cache=0;cache<2;++cache) {
        setup_pair(bits,outputs[oi],cache);
        ArmSmmu4KLiveRegion lo={&arena,&state,0,0},hi={&peer_arena,&peer_state,0,0};
        ArmSmmu4KRange l=resource(2,permission,cache),h=l;h.iova=peer_arena.iova;
        CHECK(!pair_update(&lo,&hi,&l,0,1));lo.ranges=&l;lo.count=1;
        CHECK(state.tables.leaf_pages==2 && !peer_state.tables.leaf_pages && result.complete);
        reset_counters();CHECK(!pair_update(&lo,&hi,&h,1,1));hi.ranges=&h;hi.count=1;
        CHECK(peer_state.tables.leaf_pages==2 && state.tables.leaf_pages==2 && result.complete);
        CHECK(f.tlbi_calls==1 && f.sync_calls==1 && f.synced);
        for(unsigned i=0;i<2;++i) {
            uint64_t wanted=(l.physical+4096*i)|source_permission_pte(permission,cache)|3;
            CHECK(pte(l.iova+4096*i)==wanted);
            CHECK(pte_from(&peer_arena,&peer_state,peer_tables,h.iova+4096*i)==wanted);
        }
        ArmSmmu4KLiveResult audit;
        CHECK(!ArmSmmu4KLiveAuditContext(&context,&lo,&hi,&audit) && audit.complete);
        CHECK(audit.descriptor_reads==(state.tables.table_pages+peer_state.tables.table_pages)*512);
        reset_counters();CHECK(!pair_update(&lo,&hi,&l,0,0));lo.ranges=0;lo.count=0;
        CHECK(result.translation_removed && !state.tables.leaf_pages && peer_state.tables.leaf_pages==2);
        reset_counters();CHECK(!pair_update(&lo,&hi,&h,1,0));hi.ranges=0;hi.count=0;
        CHECK(result.translation_removed && !peer_state.tables.leaf_pages);
        CHECK(!ArmSmmu4KLiveAuditContext(&context,&lo,&hi,&audit));++profiles;
    }
    return profiles;
}
static unsigned pair_failures(void)
{
    setup_pair(48,48,0);ArmSmmu4KLiveRegion lo={&arena,&state,0,0},hi={&peer_arena,&peer_state,0,0};
    ArmSmmu4KRange l=resource(2,3,1);
    CHECK(!pair_update(&lo,&hi,&l,0,1));unsigned controls=0;
    for(unsigned upper=0;upper<2;++upper)for(unsigned unmap=0;unmap<2;++unmap)
    for(unsigned after=0;after<2;++after) {
    /* The device accesses of the update without a failure: a region's first map (whole verification) or the unmap after it (not). */
    size_t calls;
    {
        setup_pair(48,48,0);
        lo=(ArmSmmu4KLiveRegion){&arena,&state,0,0};hi=(ArmSmmu4KLiveRegion){&peer_arena,&peer_state,0,0};
        ArmSmmu4KRange r=resource(2,3,1);if(upper)r.iova=peer_arena.iova;
        if(unmap) {
            CHECK(!pair_update(&lo,&hi,&r,upper,1));
            if(upper){hi.ranges=&r;hi.count=1;}else{lo.ranges=&r;lo.count=1;}
        }
        reset_counters();CHECK(!pair_update(&lo,&hi,&r,upper,!unmap));calls=f.calls;CHECK(calls);
    }
    for(size_t failed=1;failed<=calls;++failed) {
        setup_pair(48,48,0);
        lo=(ArmSmmu4KLiveRegion){&arena,&state,0,0};hi=(ArmSmmu4KLiveRegion){&peer_arena,&peer_state,0,0};
        ArmSmmu4KRange r=resource(2,3,1);if(upper)r.iova=peer_arena.iova;
        if(unmap) {
            CHECK(!pair_update(&lo,&hi,&r,upper,1));
            if(upper){hi.ranges=&r;hi.count=1;}else{lo.ranges=&r;lo.count=1;}
            reset_counters();
        }
        f.fail_call=failed;f.after_write=after;
        CHECK(pair_update(&lo,&hi,&r,upper,!unmap)==ArmSmmu4KIoError);
        CHECK(state.poisoned && peer_state.poisoned && result.must_retain_resources && !result.complete);
        size_t previous=f.calls;
        CHECK(pair_update(&lo,&hi,&r,upper,!unmap)==ArmSmmu4KArgument && f.calls==previous);++controls;
    }
    }
    for(unsigned mutation=1;mutation<=4;++mutation) {
        setup_pair(48,48,0);lo=(ArmSmmu4KLiveRegion){&arena,&state,0,0};hi=(ArmSmmu4KLiveRegion){&peer_arena,&peer_state,0,0};
        l=resource(2,3,1);f.mutate_on_publish=mutation;
        ArmSmmu4KStatus status=pair_update(&lo,&hi,&l,0,1);
        CHECK(status==(mutation==3?ArmSmmu4KFault:ArmSmmu4KStateMismatch));
        CHECK(state.poisoned && peer_state.poisoned && result.must_retain_resources && !result.complete);++controls;
    }
    setup_pair(48,48,0);lo=(ArmSmmu4KLiveRegion){&arena,&state,0,0};hi=(ArmSmmu4KLiveRegion){&peer_arena,&peer_state,0,0};
    /* A range over the other region's table page, and two regions sharing their tables, refuse before any access. */
    l=resource(2,3,1);ArmSmmu4KRange bad=l;bad.physical=peer_arena.config.table_physical;
    CHECK(pair_update(&lo,&hi,&bad,0,1)==ArmSmmu4KOverlap && !f.calls);++controls;
    const ArmSmmu4KTablePages *own=peer_arena.pages;uint64_t old=peer_arena.config.table_physical;
    peer_arena.pages=&main_provider;peer_arena.config.table_physical=arena.config.table_physical;
    peer_state.tables.root_physical=arena.config.table_physical;
    CHECK(pair_update(&lo,&hi,&l,0,1)==ArmSmmu4KOverlap && !f.calls);++controls;
    peer_arena.pages=own;peer_arena.config.table_physical=old;peer_state.tables.root_physical=old;
    CHECK(!pair_update(&lo,&hi,&l,0,1));lo.ranges=&l;lo.count=1;reset_counters();
    bad=l;bad.iova=peer_arena.iova;bad.shareability=2;
    CHECK(pair_update(&lo,&hi,&bad,1,1)==ArmSmmu4KOverlap && !f.calls);++controls;
    bad=l;bad.iova=peer_arena.iova;bad.writeback=0;
    CHECK(pair_update(&lo,&hi,&bad,1,1)==ArmSmmu4KOverlap && !f.calls);++controls;
    bad=l;bad.iova=peer_arena.iova;bad.permissions|=ArmSmmu4KPrivileged;
    CHECK(pair_update(&lo,&hi,&bad,1,1)==ArmSmmu4KOverlap && !f.calls);++controls;
    CHECK(pair_update(&lo,0,&l,0,1)==ArmSmmu4KArgument && !f.calls);++controls;
    CHECK(!state.poisoned && !peer_state.poisoned);
    return controls;
}
static ArmSmmu4KStatus batch(const ArmSmmu4KRange *before,size_t count,const ArmSmmu4KRange *changes,size_t n,unsigned map)
{
    ArmSmmu4KLiveRegion lo={&arena,&state,before,count};
    return ArmSmmu4KLiveUpdateContextBatch(&io,0,&context,&lo,0,0,&visibility,changes,n,map,4,&result);
}
/* Four scattered physical pages (descending addresses, never adjacent) over the arena's 2 MiB boundary. */
static void scattered(ArmSmmu4KRange pages[4])
{for(unsigned i=0;i<4;++i)pages[i]=(ArmSmmu4KRange){arena.iova+4096*i,0x30000000+0x20000*(3-i),4096,3,0,2};}
static unsigned batches(void)
{
    unsigned controls=0;
    setup(48,0,48,0,0x1ff000);ArmSmmu4KRange pages[4];scattered(pages);
    memcpy(snapshot,tables,sizeof(tables));
    ArmSmmu4KRange swapped[2]={pages[1],pages[0]};
    CHECK(batch(0,0,swapped,2,1)==ArmSmmu4KOverlap);++controls;
    ArmSmmu4KRange alias[2]={pages[0],pages[1]};alias[1].physical=alias[0].physical;alias[1].writeback=1;alias[1].shareability=3;
    CHECK(batch(0,0,alias,2,1)==ArmSmmu4KOverlap);++controls;
    alias[1].writeback=0;alias[1].shareability=2;alias[1].permissions|=ArmSmmu4KPrivileged;
    CHECK(batch(0,0,alias,2,1)==ArmSmmu4KOverlap);++controls;
    CHECK(batch(0,0,pages,0,1)==ArmSmmu4KArgument);++controls;
    CHECK(batch(0,0,0,1,1)==ArmSmmu4KBadRange);++controls;
    CHECK(!f.calls && !state.poisoned && !memcmp(snapshot,tables,sizeof(tables)));
    /* One transaction: four leaves written, two leaf pages published once each, one TLB synchronization. */
    CHECK(batch(0,0,pages,4,1)==ArmSmmu4KSuccess);complete(0,4);
    /* The four leaves span two leaf tables; every table on their paths was made by this map. */
    {const size_t made=state.tables.table_pages-1;
    CHECK(result.descriptor_writes==4+made && result.published_pages==2*made+2 && state.generation==1);}
    for(unsigned i=0;i<4;++i)CHECK(pte(pages[i].iova)==(pages[i].physical|source_permission_pte(3,0)|3));
    ++controls;
    /* Any already mapped page refuses the whole batch before MMIO or a store. */
    reset_counters();memcpy(snapshot,tables,sizeof(tables));
    CHECK(batch(pages,4,pages+2,2,1)==ArmSmmu4KStateMismatch && !f.calls && !memcmp(snapshot,tables,sizeof(tables)));++controls;
    /* Unmap a sub-batch given as part of the region's own ranges. */
    reset_counters();CHECK(batch(pages,4,pages+1,2,0)==ArmSmmu4KSuccess);complete(1,2);
    CHECK(f.page_tlbi_calls==2 && !f.tlbi_calls);   /* the two removed pages, each by its own TLBIVAL */
    CHECK(pte(pages[0].iova) && !pte(pages[1].iova) && !pte(pages[2].iova) && pte(pages[3].iova));++controls;
    ArmSmmu4KRange rest[2]={pages[0],pages[3]},wrong[2]={pages[0],pages[3]};wrong[1].physical+=4096;
    reset_counters();CHECK(batch(rest,2,wrong,2,0)==ArmSmmu4KStateMismatch && !f.calls);++controls;
    reset_counters();CHECK(batch(rest,2,rest,2,0)==ArmSmmu4KSuccess);complete(1,0);++controls;
    /* An exclusive map skips the alias check against the existing ranges only: a page of another class over a mapped one's physical
     * page refuses as a plain map and is taken as an exclusive one; aliases among its own changes still refuse, and so does a mapped page. */
    {
        ArmSmmu4KLiveRegion lo={&arena,&state,0,0};
        reset_counters();CHECK(batch(0,0,pages,1,1)==ArmSmmu4KSuccess);
        ArmSmmu4KRange other=pages[1];other.physical=pages[0].physical;other.writeback=1;other.shareability=3;
        CHECK(batch(pages,1,&other,1,1)==ArmSmmu4KOverlap);++controls;
        lo.ranges=pages;lo.count=1;
        ArmSmmu4KRange pair[2]={pages[1],pages[2]};pair[1].physical=pair[0].physical;pair[1].writeback=1;pair[1].shareability=3;
        CHECK(ArmSmmu4KLiveMapContextBatchExclusive(&io,0,&context,&lo,0,0,&visibility,pair,2,4,&result)==ArmSmmu4KOverlap);++controls;
        CHECK(ArmSmmu4KLiveMapContextBatchExclusive(&io,0,&context,&lo,0,0,&visibility,pages,1,4,&result)==ArmSmmu4KStateMismatch);++controls;
        CHECK(!state.poisoned);
        reset_counters();CHECK(ArmSmmu4KLiveMapContextBatchExclusive(&io,0,&context,&lo,0,0,&visibility,&other,1,4,&result)==ArmSmmu4KSuccess);
        CHECK(pte(other.iova));++controls;
        ArmSmmu4KRange both[2]={pages[0],other};
        reset_counters();CHECK(batch(both,2,both,2,0)==ArmSmmu4KSuccess);CHECK(!pte(pages[0].iova) && !pte(other.iova));++controls;
    }
    /* Every register access of a batch failing, before or after its write, poisons and retains. */
    for(unsigned unmap=0;unmap<2;++unmap)for(unsigned after=0;after<2;++after) {
    /* The accesses without a failure: the first map's (whole verification) or the unmap's after it (not). */
    setup(48,0,48,0,0x1ff000);scattered(pages);
    CHECK(batch(0,0,pages,4,1)==ArmSmmu4KSuccess);
    if(unmap) {reset_counters();CHECK(batch(pages,4,pages,4,0)==ArmSmmu4KSuccess);}
    const size_t calls=f.calls;CHECK(calls);
    for(size_t failed=1;failed<=calls;++failed) {
        setup(48,0,48,0,0x1ff000);scattered(pages);
        if(unmap) {CHECK(batch(0,0,pages,4,1)==ArmSmmu4KSuccess);reset_counters();}
        f.fail_call=failed;f.after_write=after;
        CHECK(batch(unmap?pages:0,unmap?4:0,pages,4,!unmap)==ArmSmmu4KIoError);
        CHECK(state.poisoned && result.must_retain_resources && !result.complete);++controls;
    }
    }
    return controls;
}
/* The leaf slot of va in the fixture's pool, by the oracle's own walk. */
static uint64_t *leaf_of(uint64_t va)
{
    size_t at=0;unsigned level=state.tables.start_level;
    for(;;) {
        unsigned shift=12+9*(3-level),width=level==state.tables.start_level?arena.config.input_bits-shift:9;
        uint64_t *entry=&tables[at+((va>>shift)&((1u<<width)-1))];
        if(level==3)return entry;
        CHECK((*entry&3)==3);
        at=(size_t)(((*entry&UINT64_C(0x0000fffffffff000))-arena.config.table_physical)/8);++level;
    }
}
/* After a region's first, whole-region update an update proves the leaves it changes: a changed leaf holding something else refuses
 * before any store; a descriptor elsewhere is not read again (ArmSmmu4KLiveAudit proves the whole region on request). */
static unsigned incremental(void)
{
    unsigned count=0;
    setup(48,1,48,0,0x1ff000);
    /* Both pages in one leaf table (the aperture's first page is the last of the 2 MiB below), which r's map makes. */
    ArmSmmu4KRange r=resource(1,3,1);r.iova+=4096;r.physical+=4096;
    ArmSmmu4KRange r2=r;r2.iova+=4096;r2.physical+=0x10000;
    CHECK(update(0,0,&r,1)==ArmSmmu4KSuccess && result.whole_verification);
    *leaf_of(r2.iova)=(r2.physical|0x703);reset_counters();
    CHECK(update(&r,1,&r2,1)==ArmSmmu4KStateMismatch && !result.whole_verification);
    CHECK(state.poisoned && !result.descriptor_writes && !f.writes && !result.complete);++count;
    setup(48,1,48,0,0x1ff000);
    CHECK(update(0,0,&r,1)==ArmSmmu4KSuccess);
    const ArmSmmu4KRange both[2]={r,r2};
    while(state.generation<63) {
        reset_counters();CHECK(update(&r,1,&r2,1)==ArmSmmu4KSuccess && !result.whole_verification);complete(0,2);
        CHECK(pte(r2.iova)==(r2.physical|source_permission_pte(3,1)|3));
        reset_counters();CHECK(update(both,2,&r2,0)==ArmSmmu4KSuccess && !result.whole_verification);complete(1,1);
        CHECK(!pte(r2.iova));
    }
    reset_counters();CHECK(update(&r,1,&r2,1)==ArmSmmu4KSuccess && !result.whole_verification && state.generation==64);
    *leaf_of(r.iova+8*4096)=UINT64_C(0x40000703);reset_counters();
    CHECK(update(both,2,&r2,0)==ArmSmmu4KSuccess && !result.whole_verification && !state.poisoned);++count;
    return count;
}
/* Tables on demand: a sparse map far apart makes only the tables of its two paths; a provider that refuses ends a map before any leaf
 * is written, with the region consistent and usable; an unmap keeps the tables and a map again reuses them. */
static unsigned on_demand(void)
{
    unsigned count=0;
    setup(48,0,48,0,0x1ff000);
    arena.bytes=(UINT64_C(1)<<47)-0x1ff000;arena.config.leaf_budget=(size_t)(arena.bytes/4096);
    ArmSmmu4KRange low=resource(1,3,1),high=low;high.iova=(UINT64_C(1)<<46)+0x40000000;high.physical+=0x100000;
    CHECK(update(0,0,&low,1)==ArmSmmu4KSuccess);complete(0,1);
    CHECK(state.tables.table_pages==4);++count;   /* root, then one table per level on the path */
    reset_counters();CHECK(update(&low,1,&high,1)==ArmSmmu4KSuccess);complete(0,2);
    CHECK(state.tables.table_pages==7 && pte(high.iova) && pte(low.iova));++count;   /* the far path shares only the root */
    const ArmSmmu4KRange both[2]={low,high};ArmSmmu4KLiveResult audit;
    CHECK(ArmSmmu4KLiveAudit(&arena,&state,both,2,&audit)==ArmSmmu4KSuccess && audit.descriptor_reads==7*512);++count;
    /* Refused allocation: no leaf is written, nothing is poisoned, and the map succeeds once tables can be had. */
    ArmSmmu4KRange third=low;third.iova=(UINT64_C(1)<<45)+0x200000;third.physical+=0x200000;
    main_pages.refuse_after=1;reset_counters();memcpy(snapshot,tables,sizeof(tables));
    CHECK(update(both,2,&third,1)==ArmSmmu4KCapacity && !state.poisoned && !result.complete);
    CHECK(state.tables.table_pages==8 && main_pages.used==8 && !pte(third.iova));
    CHECK(ArmSmmu4KLiveAudit(&arena,&state,both,2,&audit)==ArmSmmu4KSuccess);++count;
    main_pages.refuse_after=SIZE_MAX;reset_counters();
    CHECK(update(both,2,&third,1)==ArmSmmu4KSuccess);complete(0,3);CHECK(pte(third.iova));++count;
    /* An unmap keeps the tables; the next map over them makes none. */
    const ArmSmmu4KRange all[3]={low,third,high};
    reset_counters();CHECK(update(all,3,&third,0)==ArmSmmu4KSuccess);complete(1,2);
    const size_t kept=state.tables.table_pages;
    reset_counters();CHECK(update(both,2,&third,1)==ArmSmmu4KSuccess);complete(0,3);
    CHECK(state.tables.table_pages==kept && result.descriptor_writes==1 && result.published_pages==1);++count;
    return count;
}
int main(void)
{
    unsigned p=profiles(),fcount=failures(),m=mutations();refusals_and_partial();
    printf("PASS: %u native4K lower/upper mapping profiles, %u I/O/cache/timeout/mutation failures, %u physical graph mutations; exact original LPAE permissions, partial unmaps, VA reuse and quarantine\n",p,fcount,m);
    puts("Host fixtures only: no physical table provenance, cache-coherence, hardware TLB, GPU execution or fence qualification");
    unsigned pairs=pair_profiles(),pair_faults=pair_failures();
    printf("PASS: %u dual-region native4K profiles and %u dual-region failure/alias/isolation controls, both graphs audited and retained on uncertainty\n",pairs,pair_faults);
    printf("PASS: %u batched scattered-page map/unmap transactions and refusals: one publication per leaf page, a TLB synchronization for each unmap and whole verification\n",batches());
    printf("PASS: %u incremental-verification controls: a changed leaf is proved before and after its store, the whole region on the first update\n",incremental());
    printf("PASS: %u on-demand table controls: sparse paths across a 48-bit range, refused allocation without a leaf store, tables kept by unmap\n",on_demand());
}
