/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4KLive.h"
#define PAGE UINT64_C(4096)
#define PA_MASK UINT64_C(0x0000fffffffff000)

static int output_width(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
static int pages_valid(const ArmSmmu4KTablePages *p)
{return p && p->allocate && p->find && p->overlaps;}
/* The arena's shape: its root, MAIR and start level. table_pages is the state's own count (the tables a walk allocated). */
static ArmSmmu4KStatus layout(const ArmSmmu4KLiveArena *a,ArmSmmu4KResult *out)
{
    if(!a || !pages_valid(a->pages) || a->upper_region>1 ||
        a->config.input_bits<32 || a->config.input_bits>48 ||
        !output_width(a->config.output_bits) || !a->config.leaf_budget)return ArmSmmu4KArgument;
    uint64_t limit=UINT64_C(1)<<a->config.input_bits;
    uint64_t physical_limit=UINT64_C(1)<<a->config.output_bits;
    if(!a->bytes || ((a->iova|a->bytes|a->config.table_physical)&4095) ||
        (a->upper_region ? a->iova<UINT64_MAX-limit+1 : a->iova>=limit) ||
        a->bytes>limit-(a->iova&(limit-1)) ||
        a->config.table_physical>=physical_limit)return ArmSmmu4KBadRange;
    if(a->bytes/PAGE>a->config.leaf_budget)return ArmSmmu4KBudget;
    if(!a->pages->find(a->pages->cookie,a->config.table_physical))return ArmSmmu4KArgument;
    unsigned start=4-(a->config.input_bits-12+8)/9;
    *out=(ArmSmmu4KResult){a->config.table_physical,0xff44,0,0,start,a->upper_region};
    return ArmSmmu4KSuccess;
}
static uint64_t descriptor(const ArmSmmu4KRange *r,uint64_t iova)
{
    uint64_t attrs=(1u<<11)|(1u<<10)|3u|((uint64_t)r->shareability<<8);
    if(!(r->permissions&ArmSmmu4KExecute))attrs|=UINT64_C(3)<<53;
    if(!(r->permissions&ArmSmmu4KPrivileged))attrs|=1u<<6;
    if(!(r->permissions&ArmSmmu4KWrite))attrs|=1u<<7;
    if(r->writeback)attrs|=1u<<2;
    return (r->physical+(iova-r->iova))|attrs;
}
static const ArmSmmu4KRange *find(const ArmSmmu4KRange *ranges,size_t count,uint64_t iova)
{
    size_t low=0,high=count;
    while(low<high) {
        size_t mid=low+(high-low)/2;
        const ArmSmmu4KRange *r=&ranges[mid];
        if(iova<r->iova)high=mid;
        else if(iova-r->iova>=r->bytes)low=mid+1;
        else return r;
    }
    return 0;
}
/* The walk visits addresses in ascending order, so one cursor per range list replaces a binary search for every leaf: lower_bound once per
 * leaf page, then forward. A range is behind the address when the address lies past its end (written as a difference: native addresses
 * may end at 2^64). */
static size_t first_not_behind(const ArmSmmu4KRange *ranges,size_t count,uint64_t va)
{
    size_t low=0,high=count;
    while(low<high) {
        size_t mid=low+(high-low)/2;
        if(va>=ranges[mid].iova && va-ranges[mid].iova>=ranges[mid].bytes)low=mid+1;
        else high=mid;
    }
    return low;
}
static const ArmSmmu4KRange *scan(const ArmSmmu4KRange *ranges,size_t count,size_t *cursor,uint64_t va)
{
    while(*cursor<count && va>=ranges[*cursor].iova && va-ranges[*cursor].iova>=ranges[*cursor].bytes)++*cursor;
    if(*cursor<count && va>=ranges[*cursor].iova && va-ranges[*cursor].iova<ranges[*cursor].bytes)return &ranges[*cursor];
    return 0;
}
static uint64_t expected(const ArmSmmu4KRange *ranges,size_t count,uint64_t iova)
{
    const ArmSmmu4KRange *r=find(ranges,count,iova);
    return r?descriptor(r,iova):0;
}
static ArmSmmu4KStatus validate_range(const ArmSmmu4KLiveArena *a,const ArmSmmu4KRange *r)
{
    uint64_t physical_limit=UINT64_C(1)<<a->config.output_bits;
    if(!r || !r->bytes || ((r->iova|r->physical|r->bytes)&4095) ||
        !(r->permissions&ArmSmmu4KRead) || (r->permissions&~15u) ||
        r->writeback>1 || (r->shareability!=2 && r->shareability!=3) ||
        r->physical>=physical_limit || r->bytes>physical_limit-r->physical ||
        r->iova<a->iova || r->iova-a->iova>=a->bytes ||
        r->bytes>a->bytes-(r->iova-a->iova))return ArmSmmu4KBadRange;
    if(a->pages->overlaps(a->pages->cookie,r->physical,r->bytes))return ArmSmmu4KOverlap;
    return ArmSmmu4KSuccess;
}
static int conflicting_alias(const ArmSmmu4KRange *a,const ArmSmmu4KRange *b)
{
    return (a->writeback!=b->writeback || a->shareability!=b->shareability ||
        ((a->permissions^b->permissions)&ArmSmmu4KPrivileged)) &&
        a->physical<b->physical+b->bytes && b->physical<a->physical+a->bytes;
}
/* Only ranges of different cache/shareability/privilege classes can alias in conflict, and of two conflicting ranges at most one
 * is in the class most ranges of both sets have. So for every other class k, the members of class k in one set are compared with
 * the members of other classes in the other set, both ways. Each such comparison sorts the smaller side by physical address in
 * the arena's scratch and looks every range of the larger side up in it: a scattered buffer is one range per physical page, and
 * with coherent imports (write-back) beside the GPU's own non-cacheable buffers a pairwise loop over both was some 2.4 billion
 * comparisons per map (int-144: 368 ms per map on average, 8 s at worst). Without scratch, or a side larger than it, the
 * comparison is the pairwise loop. */
static unsigned alias_class(const ArmSmmu4KRange *r)
{return (r->writeback&1u)|((r->shareability&3u)<<1)|((r->permissions&ArmSmmu4KPrivileged)?8u:0u);}
typedef struct {
    const ArmSmmu4KRange *ranges;size_t count;
    unsigned cls;int same;   /* members: class == cls (same) or class != cls */
} AliasSide;
static int alias_member(const AliasSide *s,size_t i)
{return (alias_class(&s->ranges[i])==s->cls)==(s->same!=0);}
static void sift(const ArmSmmu4KRange *r,uint32_t *h,size_t root,size_t n)
{
    for(;;) {
        size_t child=2*root+1;
        if(child>=n)return;
        if(child+1<n && r[h[child+1]].physical>r[h[child]].physical)++child;
        if(r[h[root]].physical>=r[h[child]].physical)return;
        uint32_t t=h[root];h[root]=h[child];h[child]=t;root=child;
    }
}
/* np and nq: the members of each side, from the caller's one count per class (counting them here walked both sides again in every
 * pass, four extra walks of the whole range list per buffer map). */
static int alias_pass(const ArmSmmu4KLiveArena *arena,const AliasSide *p,const AliasSide *q,size_t np,size_t nq)
{
    if(!np || !nq)return 0;
    const AliasSide *sorted=np<=nq?p:q,*probe=np<=nq?q:p;
    const size_t n=np<=nq?np:nq;
    uint32_t *h=arena?arena->alias_scratch:0;
    if(!h || n>arena->alias_scratch_entries || sorted->count>UINT32_MAX) {
        for(size_t i=0;i<p->count;++i)if(alias_member(p,i))
            for(size_t j=0;j<q->count;++j)if(alias_member(q,j) && conflicting_alias(&p->ranges[i],&q->ranges[j]))return 1;
        return 0;
    }
    const ArmSmmu4KRange *r=sorted->ranges;
    size_t k=0;
    for(size_t i=0;i<sorted->count;++i)if(alias_member(sorted,i)) {
        h[k++]=(uint32_t)i;
    }
    for(size_t i=n/2;i-->0;)sift(r,h,i,n);
    for(size_t end=n;end>1;) {--end;uint32_t t=h[0];h[0]=h[end];h[end]=t;sift(r,h,0,end);}
    /* Drop intervals contained by an earlier interval. Both starts and ends of
     * the retained indices are now monotonic, even with nested/duplicate runs.
     * A distant long run cannot force gap queries to scan short runs backward.
     * Every compared pair belongs to different classes by AliasSide's filter. */
    size_t kept=0;
    for(size_t i=0;i<n;++i) {
        uint32_t index=h[i];
        if(!kept || r[index].physical+r[index].bytes>r[h[kept-1]].physical+r[h[kept-1]].bytes)h[kept++]=index;
    }
    for(size_t j=0;j<probe->count;++j) {
        if(!alias_member(probe,j))continue;
        const ArmSmmu4KRange *y=&probe->ranges[j];
        size_t lo=0,hi=kept;   /* first interval ending after y's start */
        while(lo<hi) {size_t mid=lo+(hi-lo)/2;if(r[h[mid]].physical+r[h[mid]].bytes<=y->physical)lo=mid+1;else hi=mid;}
        if(lo<kept && r[h[lo]].physical<y->physical+y->bytes)return 1;
    }
    return 0;
}
static int any_conflicting_alias(const ArmSmmu4KLiveArena *arena,const ArmSmmu4KRange *a,size_t na,
    const ArmSmmu4KRange *b,size_t nb)
{
    if(!na || !nb)return 0;
    size_t ma[16]={0},mb[16]={0},members[16];
    for(size_t i=0;i<na;++i)++ma[alias_class(&a[i])];
    const int self=a==b && na==nb;
    if(self)for(unsigned k=0;k<16;++k)mb[k]=ma[k];else for(size_t j=0;j<nb;++j)++mb[alias_class(&b[j])];
    for(unsigned k=0;k<16;++k)members[k]=self?ma[k]:ma[k]+mb[k];
    unsigned reference=0;
    for(unsigned k=1;k<16;++k)if(members[k]>members[reference])reference=k;
    for(unsigned k=0;k<16;++k) {
        if(k==reference || !members[k])continue;
        const AliasSide ak={a,na,k,1},bo={b,nb,k,0};
        if(alias_pass(arena,&ak,&bo,ma[k],nb-mb[k]))return 1;
        if(self)continue;
        const AliasSide bk={b,nb,k,1},ao={a,na,k,0};
        if(alias_pass(arena,&bk,&ao,mb[k],na-ma[k]))return 1;
    }
    return 0;
}
static ArmSmmu4KStatus validate_ranges_checked(const ArmSmmu4KLiveArena *a,
    const ArmSmmu4KRange *ranges,size_t count,size_t *leaves,int aliases);
static ArmSmmu4KStatus validate_ranges(const ArmSmmu4KLiveArena *a,
    const ArmSmmu4KRange *ranges,size_t count,size_t *leaves)
{return validate_ranges_checked(a,ranges,count,leaves,1);}
static ArmSmmu4KStatus validate_ranges_checked(const ArmSmmu4KLiveArena *a,
    const ArmSmmu4KRange *ranges,size_t count,size_t *leaves,int aliases)
{
    *leaves=0;
    if((count && !ranges) || count>a->config.leaf_budget ||
        count>SIZE_MAX/sizeof(*ranges))return ArmSmmu4KArgument;
    for(size_t i=0;i<count;++i) {
        ArmSmmu4KStatus status=validate_range(a,&ranges[i]);
        if(status!=ArmSmmu4KSuccess)return status;
        if(i && ranges[i].iova<=ranges[i-1].iova+(ranges[i-1].bytes-1))return ArmSmmu4KOverlap;
        size_t pages=(size_t)(ranges[i].bytes/PAGE);
        if(pages>a->config.leaf_budget-*leaves)return ArmSmmu4KBudget;
        *leaves+=pages;
    }
    return aliases && any_conflicting_alias(a,ranges,count,ranges,count)?ArmSmmu4KOverlap:ArmSmmu4KSuccess;
}
/* Whether a range of the list (and, mapping, a change) lies in [first, last]: what an absent table there must not have to hold. */
static int any_in(const ArmSmmu4KRange *ranges,size_t count,const ArmSmmu4KRange *changes,size_t changed,unsigned map,
    uint64_t first,uint64_t last)
{
    size_t i=first_not_behind(ranges,count,first);
    if(i<count && ranges[i].iova<=last)return 1;
    if(map && changed) {size_t j=first_not_behind(changes,changed,first);if(j<changed && changes[j].iova<=last)return 1;}
    return 0;
}
/* The tables as they are, against the ranges (and the changes, with map telling what they leave): every table descriptor names a page
 * of the arena's own; an absent table covers nothing mapped; a leaf holds its range's descriptor, or zero. Entries outside the
 * aperture are zero. Tables are kept once made, so a present table may cover nothing. *seen counts the tables visited. */
static ArmSmmu4KStatus walk(const ArmSmmu4KLiveArena *a,const ArmSmmu4KResult *shape,
    unsigned level,const volatile uint64_t *table,uint64_t prefix,size_t *seen,
    const ArmSmmu4KRange *ranges,size_t count,const ArmSmmu4KRange *changes,size_t changed,
    unsigned map,ArmSmmu4KLiveResult *r)
{
    uint64_t limit=UINT64_C(1)<<a->config.input_bits;
    uint64_t low=a->iova&(limit-1),high=low+a->bytes-1;
    uint64_t physical_limit=UINT64_C(1)<<a->config.output_bits;
    unsigned shift=39-9*level;
    unsigned width=level==shape->start_level?a->config.input_bits-shift:9;
    size_t range_cursor=0,change_cursor=0;
    ++*seen;
    if(level==3) {
        uint64_t first=a->upper_region ? prefix|~(limit-1) : prefix;
        range_cursor=first_not_behind(ranges,count,first);change_cursor=first_not_behind(changes,changed,first);
    }
    for(unsigned slot=0;slot<512;++slot) {
        uint64_t begin=prefix+((uint64_t)slot<<shift),span=UINT64_C(1)<<shift;
        int inside=slot<(1u<<width) && begin<=high && low<begin+span;
        uint64_t value=table[slot];++r->descriptor_reads;
        uint64_t va=a->upper_region ? begin|~(limit-1) : begin;
        if(!inside) {if(value)return ArmSmmu4KStateMismatch;continue;}
        if(level<3) {
            if(!value) {
                if(any_in(ranges,count,changes,changed,map,va,va+(span-1)))return ArmSmmu4KStateMismatch;
                continue;
            }
            uint64_t physical=value&PA_MASK;
            if(value!=(physical|3u) || physical>=physical_limit || physical==shape->root_physical)return ArmSmmu4KStateMismatch;
            const volatile uint64_t *child=a->pages->find(a->pages->cookie,physical);
            if(!child)return ArmSmmu4KStateMismatch;
            ArmSmmu4KStatus status=walk(a,shape,level+1,child,begin,seen,ranges,count,changes,changed,map,r);
            if(status!=ArmSmmu4KSuccess)return status;
            continue;
        }
        uint64_t want=0;
        const ArmSmmu4KRange *c=scan(changes,changed,&change_cursor,va);
        if(c)want=map?descriptor(c,va):0;
        else {
            const ArmSmmu4KRange *r0=scan(ranges,count,&range_cursor,va);
            want=r0?descriptor(r0,va):0;
        }
        if(value!=want)return ArmSmmu4KStateMismatch;
    }
    return ArmSmmu4KSuccess;
}
/* The level-3 descriptor of va: *entry its CPU address and *leaf its table, both NULL when a table on the path is absent and create
 * is 0. With create an absent table is allocated, checked zero, published, linked and its parent published, so the walker never meets
 * a table it cannot read: a linked empty table translates nothing, as the absent one did. */
static ArmSmmu4KStatus leaf_entry(const ArmSmmu4KLiveArena *a,ArmSmmu4KLiveState *state,const ArmSmmu4KLiveVisibility *visibility,
    uint64_t va,int create,volatile uint64_t **entry,volatile uint64_t **leaf,ArmSmmu4KLiveResult *r)
{
    const ArmSmmu4KResult *shape=&state->tables;
    uint64_t physical_limit=UINT64_C(1)<<a->config.output_bits;
    *entry=*leaf=0;
    volatile uint64_t *table=a->pages->find(a->pages->cookie,shape->root_physical);
    if(!table)return ArmSmmu4KStateMismatch;
    for(unsigned level=shape->start_level;level<3;++level) {
        unsigned shift=39-9*level,width=level==shape->start_level?a->config.input_bits-shift:9;
        size_t slot=(size_t)((va>>shift)&((UINT64_C(1)<<width)-1));
        uint64_t value=table[slot];++r->descriptor_reads;
        if(!value) {
            if(!create)return ArmSmmu4KSuccess;
            uint64_t physical=0;volatile uint64_t *page=0;
            if(a->pages->allocate(a->pages->cookie,&physical,&page))return ArmSmmu4KCapacity;
            if(!page || (physical&4095) || physical>=physical_limit || a->pages->find(a->pages->cookie,physical)!=page)
                return ArmSmmu4KStateMismatch;
            ++state->tables.table_pages;
            for(size_t i=0;i<512;++i) {++r->descriptor_reads;if(page[i])return ArmSmmu4KStateMismatch;}
            if(visibility->publish_table_page(visibility->cookie,page,4096))return ArmSmmu4KIoError;
            ++r->published_pages;
            value=physical|3u;table[slot]=value;++r->descriptor_writes;
            if(visibility->publish_table_page(visibility->cookie,table,4096))return ArmSmmu4KIoError;
            ++r->published_pages;
        }
        uint64_t physical=value&PA_MASK;
        if(value!=(physical|3u))return ArmSmmu4KStateMismatch;
        table=a->pages->find(a->pages->cookie,physical);
        if(!table)return ArmSmmu4KStateMismatch;
    }
    *leaf=table;*entry=&table[(va>>12)&511];
    return ArmSmmu4KSuccess;
}
/* One pass's last leaf table: the 512 pages of a 2 MiB block share it, so a pass over a change walks from the root once per block, not
 * once per page (i14aef633e: 2.2 million spans, each walked five times through a binary search of the table pages per level, 4.8 s
 * of batch maps under the driver's lock). Only a found table is kept; the walk counts its reads as before. */
struct leaf_cache {uint64_t block;volatile uint64_t *leaf;};
static ArmSmmu4KStatus leaf_entry_cached(const ArmSmmu4KLiveArena *a,ArmSmmu4KLiveState *state,const ArmSmmu4KLiveVisibility *visibility,
    uint64_t va,struct leaf_cache *cache,volatile uint64_t **entry,volatile uint64_t **leaf,ArmSmmu4KLiveResult *r)
{
    const uint64_t block=(va>>21)+1;
    if(cache->leaf && cache->block==block) {*leaf=cache->leaf;*entry=&cache->leaf[(va>>12)&511];return ArmSmmu4KSuccess;}
    ArmSmmu4KStatus status=leaf_entry(a,state,visibility,va,0,entry,leaf,r);
    if(status==ArmSmmu4KSuccess && *leaf) {cache->block=block;cache->leaf=*leaf;}
    return status;
}
/* The changed leaves alone, through the table path each one's address takes: before the store (after=0) each holds what the
 * operation replaces (nothing for a map, its range's descriptor for an unmap; an absent table holds nothing), after it what the
 * operation wrote. */
static ArmSmmu4KStatus verify_changed(const ArmSmmu4KLiveArena *a,ArmSmmu4KLiveState *state,const ArmSmmu4KLiveVisibility *visibility,
    const ArmSmmu4KRange *changes,size_t changed,unsigned map,unsigned after,ArmSmmu4KLiveResult *r)
{
    struct leaf_cache cache={0,0};
    for(size_t i=0;i<changed;++i)for(uint64_t offset=0;offset<changes[i].bytes;offset+=PAGE) {
        const uint64_t va=changes[i].iova+offset;volatile uint64_t *entry,*leaf;
        ArmSmmu4KStatus status=leaf_entry_cached(a,state,visibility,va,&cache,&entry,&leaf,r);if(status!=ArmSmmu4KSuccess)return status;
        const uint64_t want=(map==after)?descriptor(&changes[i],va):0;
        ++r->descriptor_reads;
        if((entry?*entry:0)!=want)return ArmSmmu4KStateMismatch;
    }
    return ArmSmmu4KSuccess;
}
static ArmSmmu4KStatus verify(const ArmSmmu4KLiveArena *a,const ArmSmmu4KResult *shape,
    const ArmSmmu4KRange *ranges,size_t count,const ArmSmmu4KRange *changes,size_t changed,
    unsigned map,ArmSmmu4KLiveResult *r)
{
    const volatile uint64_t *root=a->pages->find(a->pages->cookie,shape->root_physical);
    if(!root)return ArmSmmu4KStateMismatch;
    size_t seen=0;
    ArmSmmu4KStatus status=walk(a,shape,shape->start_level,root,0,&seen,ranges,count,changes,changed,map,r);
    return status==ArmSmmu4KSuccess && seen!=shape->table_pages?ArmSmmu4KStateMismatch:status;
}
ArmSmmu4KStatus ArmSmmu4KLivePrepare(const ArmSmmu4KLiveArena *a,ArmSmmu4KLiveState *state)
{
    if(!state || state->prepared || state->poisoned)return ArmSmmu4KArgument;
    ArmSmmu4KResult shape;
    ArmSmmu4KStatus status=layout(a,&shape);
    if(status!=ArmSmmu4KSuccess)return status;
    const volatile uint64_t *root=a->pages->find(a->pages->cookie,shape.root_physical);
    for(size_t i=0;i<512;++i)if(root[i])return ArmSmmu4KStateMismatch;
    shape.table_pages=1;
    state->tables=shape;state->generation=0;state->prepared=1;state->context_offset=0;
    return ArmSmmu4KSuccess;
}
static int same_config(const ArmSmmu4KConfig *a,const ArmSmmu4KConfig *b)
{
    return a && b && a->input_bits==b->input_bits && a->output_bits==b->output_bits &&
        a->table_physical==b->table_physical && a->leaf_budget==b->leaf_budget;
}
ArmSmmu4KStatus ArmSmmu4KLiveAudit(const ArmSmmu4KLiveArena *a,const ArmSmmu4KLiveState *state,
    const ArmSmmu4KRange *ranges,size_t count,ArmSmmu4KLiveResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KLiveResult){0};r->must_retain_resources=1;
    if(!state || !state->prepared || state->poisoned)return ArmSmmu4KArgument;
    ArmSmmu4KResult shape;
    ArmSmmu4KStatus status=layout(a,&shape);
    if(status!=ArmSmmu4KSuccess)return status;
    if(state->tables.root_physical!=shape.root_physical || state->tables.mair!=shape.mair ||
        state->tables.start_level!=shape.start_level ||
        state->tables.upper_region!=shape.upper_region)return ArmSmmu4KArgument;
    size_t leaves=0;
    status=validate_ranges(a,ranges,count,&leaves);if(status!=ArmSmmu4KSuccess)return status;
    if(leaves!=state->tables.leaf_pages)return ArmSmmu4KArgument;
    status=verify(a,&state->tables,ranges,count,0,0,0,r);
    r->complete=status==ArmSmmu4KSuccess;return status;
}
static int touches_pool(const ArmSmmu4KRange *r,const ArmSmmu4KLiveArena *a)
{return a->pages->overlaps(a->pages->cookie,r->physical,r->bytes)!=0;}
/* aliases=0 (an update that is not its region's whole-verification one) trusts the existing ranges: each was checked as a change when it
 * was added (valid, disjoint from the others by the empty leaves it replaced, clear of both pools, in budget) and only those updates
 * modified the list. Scanning every range in each of the update's five metadata passes cost about 1 ms per buffer map at 52609 ranges
 * (i567: 15831 maps spent 15.5 s in the update, and the compositor dropped to 5-25 fps while it mapped). */
static ArmSmmu4KStatus region_metadata(const ArmSmmu4KLiveRegion *region,
    const ArmSmmu4KConfig *config,const ArmSmmu4KResult *root,unsigned upper,int aliases)
{
    if(!region)return (!config && !root)?ArmSmmu4KSuccess:ArmSmmu4KArgument;
    if(!region->arena || !region->state || region->arena->upper_region!=upper ||
        root!=&region->state->tables || !same_config(config,&region->arena->config) ||
        !region->state->prepared || region->state->poisoned)return ArmSmmu4KArgument;
    ArmSmmu4KResult shape;
    ArmSmmu4KStatus status=layout(region->arena,&shape);if(status)return status;
    const ArmSmmu4KResult *r=&region->state->tables;
    if(r->root_physical!=shape.root_physical || r->mair!=shape.mair ||
        r->start_level!=shape.start_level || r->upper_region!=shape.upper_region)return ArmSmmu4KArgument;
    if(!aliases)return (region->count && !region->ranges) || region->count>r->leaf_pages?ArmSmmu4KArgument:ArmSmmu4KSuccess;
    size_t leaves=0;
    status=validate_ranges_checked(region->arena,region->ranges,region->count,&leaves,aliases);if(status)return status;
    return leaves==r->leaf_pages?ArmSmmu4KSuccess:ArmSmmu4KArgument;
}
/* aliases=0 (an update that is not its region's whole-verification one) skips the existing ranges' alias and pool checks against each
 * other and across the regions: each range was checked against all of them when it was mapped. */
static ArmSmmu4KStatus context_metadata(const ArmSmmu4KContextConfig *context,
    const ArmSmmu4KLiveRegion *lower,const ArmSmmu4KLiveRegion *upper,int aliases)
{
    if(!context || (!lower && !upper))return ArmSmmu4KArgument;
    ArmSmmu4KStatus status=region_metadata(lower,context->lower,context->lower_tables,0,aliases);if(status)return status;
    status=region_metadata(upper,context->upper,context->upper_tables,1,aliases);if(status)return status;
    if(lower && upper) {
        const ArmSmmu4KLiveArena *a=lower->arena,*b=upper->arena;
        /* Each region's tables are its own pages: neither root may be a table of the other. */
        if(lower->state==upper->state || a->config.output_bits!=b->config.output_bits || a->pages==b->pages ||
            a->config.table_physical==b->config.table_physical ||
            a->pages->overlaps(a->pages->cookie,b->config.table_physical,PAGE) ||
            b->pages->overlaps(b->pages->cookie,a->config.table_physical,PAGE))
            return ArmSmmu4KOverlap;
        if(!aliases)return ArmSmmu4KSuccess;
        for(size_t i=0;i<lower->count;++i)if(touches_pool(&lower->ranges[i],b))return ArmSmmu4KOverlap;
        for(size_t i=0;i<upper->count;++i)if(touches_pool(&upper->ranges[i],a))return ArmSmmu4KOverlap;
        if(any_conflicting_alias(lower->arena,lower->ranges,lower->count,upper->ranges,upper->count))return ArmSmmu4KOverlap;
    }
    return ArmSmmu4KSuccess;
}
ArmSmmu4KStatus ArmSmmu4KLiveAuditContext(const ArmSmmu4KContextConfig *context,
    const ArmSmmu4KLiveRegion *lower,const ArmSmmu4KLiveRegion *upper,ArmSmmu4KLiveResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KLiveResult){0};r->must_retain_resources=1;
    ArmSmmu4KStatus status=context_metadata(context,lower,upper,1);if(status)return status;
    const ArmSmmu4KLiveRegion *regions[]={lower,upper};
    for(unsigned i=0;i<2;++i)if(regions[i]) {
        const ArmSmmu4KLiveRegion *v=regions[i];
        status=verify(v->arena,&v->state->tables,v->ranges,v->count,0,0,0,r);if(status)return status;
    }
    r->complete=1;return ArmSmmu4KSuccess;
}
static ArmSmmu4KStatus update(const ArmSmmu4KLiveArena *a,ArmSmmu4KLiveState *state,
    const ArmSmmu4KIo *io,unsigned bank,const ArmSmmu4KContextConfig *context,
    const ArmSmmu4KLiveVisibility *visibility,const ArmSmmu4KRange *before,size_t count,
    const ArmSmmu4KLiveRegion *peer,const ArmSmmu4KRange *changes,size_t change_count,unsigned map,unsigned exclusive,unsigned budget,
    ArmSmmu4KLiveResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KLiveResult){0};r->must_retain_resources=1;
    if(!state || !state->prepared || state->poisoned || !context || (state->detached && (!state->context_offset || !context->ttbr0_switched)) ||
        !visibility || !visibility->publish_table_page || map>1 || exclusive>map || !change_count ||
        !budget || budget>1048576 || state->generation==UINT64_MAX)return ArmSmmu4KArgument;
    ArmSmmu4KResult layout_shape;
    ArmSmmu4KStatus status=layout(a,&layout_shape);
    if(status!=ArmSmmu4KSuccess)return status;
    const ArmSmmu4KResult *root=a->upper_region?context->upper_tables:context->lower_tables;
    if((!peer && (a->upper_region?(context->lower || context->lower_tables):(context->upper || context->upper_tables))) ||
        !same_config(a->upper_region?context->upper:context->lower,&a->config) ||
        root!=&state->tables || state->tables.root_physical!=layout_shape.root_physical ||
        state->tables.mair!=layout_shape.mair || !state->tables.table_pages ||
        state->tables.start_level!=layout_shape.start_level || state->tables.upper_region!=layout_shape.upper_region)
        return ArmSmmu4KArgument;
    /* The state's own tables: the walks below count what this update allocates. */
    const ArmSmmu4KResult *shape=&state->tables;
    /* A region's first update proves the whole region: the existing ranges against each other and every table descriptor before and
     * after. The others prove what they change: each changed leaf holds its old descriptor before the store and its new one after it,
     * the existing ranges are still sorted, disjoint and in budget, and the changes conflict with none of them. The ranges were each
     * checked as changes when they were added, so the tables stay what the first proof and the proved changes made them.
     * A whole-region walk read every descriptor of a 2 GiB aperture four times per update (int-143: 10 ms per buffer map); repeated
     * every 64th update it still stalled the caller 37 ms on a map and 79 ms on an unmap (i557u, 4.8 GiB apertures). */
    const int whole=state->generation==0;
    r->whole_verification=whole?1u:0u;
    /* The others take the leaf count the proved updates kept and the existing ranges as region_metadata() trusts them: a changed page
     * that an existing range covers still refuses below, through the binary search of the sorted list. */
    size_t leaves=state->tables.leaf_pages;
    if(whole) {
        status=validate_ranges_checked(a,before,count,&leaves,1);if(status!=ArmSmmu4KSuccess)return status;
        if(leaves!=state->tables.leaf_pages)return ArmSmmu4KArgument;
    } else if((count && !before) || count>leaves)return ArmSmmu4KArgument;
    /* The changes: sorted, disjoint, each valid; together within the leaf budget. */
    size_t changed=0;
    if(!changes)return ArmSmmu4KBadRange;
    if(change_count>a->config.leaf_budget)return ArmSmmu4KArgument;
    for(size_t i=0;i<change_count;++i) {
        status=validate_range(a,&changes[i]);if(status!=ArmSmmu4KSuccess)return status;
        if(i && changes[i].iova<=changes[i-1].iova+(changes[i-1].bytes-1))return ArmSmmu4KOverlap;
        size_t pages=(size_t)(changes[i].bytes/PAGE);
        if(pages>a->config.leaf_budget-changed)return ArmSmmu4KBudget;
        changed+=pages;
    }
    if(peer) {
        ArmSmmu4KLiveRegion target={a,state,before,count};
        status=context_metadata(context,a->upper_region?peer:&target,a->upper_region?&target:peer,whole);
        if(status)return status;
        for(size_t i=0;i<change_count;++i)if(touches_pool(&changes[i],peer->arena))return ArmSmmu4KOverlap;
        if(map && !exclusive && any_conflicting_alias(a,changes,change_count,peer->ranges,peer->count))return ArmSmmu4KOverlap;
    }
    if(map && changed>a->config.leaf_budget-leaves)return ArmSmmu4KBudget;
    if(map && (any_conflicting_alias(a,changes,change_count,changes,change_count) ||
        (!exclusive && any_conflicting_alias(a,changes,change_count,before,count))))return ArmSmmu4KOverlap;
    for(size_t i=0;i<change_count;++i)for(uint64_t offset=0;offset<changes[i].bytes;offset+=PAGE) {
        uint64_t va=changes[i].iova+offset,old=expected(before,count,va);
        if(map?old!=0:old!=descriptor(&changes[i],va))return ArmSmmu4KStateMismatch;
    }
    /* The context image and faults are observed around the stores of the whole-verification update only, and remembered there; the
     * caller observes its routes on its own schedule. After it the bank stays the caller's exclusively owned one; each of these
     * reads is a device register access, and an update observed it five times (int-159: 1.1 ms of a 1.7 ms buffer map). */
    const int observed=!state->detached && (whole || !state->context_offset);
    if(observed) {
        status=ArmSmmu4KObserveOwnedContext(io,bank,context,&r->synchronization);
        if(status!=ArmSmmu4KSuccess)goto poisoned;
    }
    if(whole) {
        status=verify(a,shape,before,count,0,0,0,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
        if(peer) {
            status=verify(peer->arena,&peer->state->tables,peer->ranges,peer->count,0,0,0,r);
            if(status)goto poisoned;
        }
    } else {
        status=verify_changed(a,state,visibility,changes,change_count,map,0,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
    }
    if(observed) {
        ArmSmmu4KProgramResult initial=r->synchronization;
        status=ArmSmmu4KObserveOwnedContext(io,bank,context,&r->synchronization);
        if(status!=ArmSmmu4KSuccess)goto poisoned;
        if(initial.id0!=r->synchronization.id0 || initial.id1!=r->synchronization.id1 ||
            initial.id2!=r->synchronization.id2 || initial.context_offset!=r->synchronization.context_offset)
            {status=ArmSmmu4KStateMismatch;goto poisoned;}
    }
    /* A map first links every table its leaves need: empty tables, published before they are linked, change no translation. One that
     * cannot be had ends the update here, with no leaf written and the tables made so far kept (they are the state's, counted). The
     * whole-verification update proves them with the rest below. */
    if(map) {
        for(size_t i=0;i<change_count;++i)for(uint64_t offset=0;offset<changes[i].bytes;offset+=PAGE) {
            volatile uint64_t *entry,*leaf;
            status=leaf_entry(a,state,visibility,changes[i].iova+offset,1,&entry,&leaf,r);
            if(status==ArmSmmu4KCapacity)return status;
            if(status!=ArmSmmu4KSuccess)goto poisoned;
            /* Each leaf table holds 512 pages: skip to the next one. */
            const uint64_t next=((changes[i].iova+offset)|(UINT64_C(512)*PAGE-1))+1-changes[i].iova;
            if(next>offset+PAGE)offset=next-PAGE;
        }
    }
    /* All validation precedes the first leaf store. Caller keeps every master
     * quiesced and retains both old/new object references through this call. */
    struct leaf_cache stores={0,0},publishes={0,0};
    for(size_t i=0;i<change_count;++i)for(uint64_t offset=0;offset<changes[i].bytes;offset+=PAGE) {
        uint64_t va=changes[i].iova+offset;volatile uint64_t *entry,*leaf;
        status=leaf_entry_cached(a,state,visibility,va,&stores,&entry,&leaf,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
        if(!entry) {status=ArmSmmu4KStateMismatch;goto poisoned;}
        *entry=map?descriptor(&changes[i],va):0;++r->descriptor_writes;
    }
    /* Sorted changes reach leaf tables in order, so each changed table is published once. */
    const volatile uint64_t *last_leaf=0;
    for(size_t i=0;i<change_count;++i)for(uint64_t offset=0;offset<changes[i].bytes;offset+=PAGE) {
        volatile uint64_t *entry,*leaf;
        status=leaf_entry_cached(a,state,visibility,changes[i].iova+offset,&publishes,&entry,&leaf,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
        if(!leaf) {status=ArmSmmu4KStateMismatch;goto poisoned;}
        if(leaf!=last_leaf) {
            if(visibility->publish_table_page(visibility->cookie,leaf,4096)) {status=ArmSmmu4KIoError;goto poisoned;}
            ++r->published_pages;last_leaf=leaf;
        }
    }
    /* A map of leaves that were invalid needs no TLB invalidation: the TLBs hold no translation of an invalid descriptor (as Linux's
     * io-pgtable maps without one). The unmap invalidates the ASID before it reports the translation removed. */
    if(observed) {
        status=ArmSmmu4KSynchronizeOwnedContext(io,bank,context,budget,&r->synchronization);
        if(status!=ArmSmmu4KSuccess)goto poisoned;
    } else if(!map) {
        /* Up to ArmSmmu4KTlbPageLimit pages are dropped one by one and the rest of the ASID's translations stay cached; a bigger
         * unmap invalidates the ASID (i568: 3622 unmaps each emptied the GPU's TLB). */
        status=changed<=ArmSmmu4KTlbPageLimit?
            ArmSmmu4KSyncContextTlbPages(io,state->context_offset,context->asid,changes,change_count,budget,&r->synchronization):
            ArmSmmu4KSyncContextTlb(io,state->context_offset,context->asid,0,budget,&r->synchronization);
        if(status!=ArmSmmu4KSuccess)goto poisoned;
    }
    if(whole) {
        status=verify(a,shape,before,count,changes,change_count,map,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
        if(peer) {
            status=verify(peer->arena,&peer->state->tables,peer->ranges,peer->count,0,0,0,r);
            if(status)goto poisoned;
        }
    } else {
        status=verify_changed(a,state,visibility,changes,change_count,map,1,r);if(status!=ArmSmmu4KSuccess)goto poisoned;
    }
    if(observed) {
        ArmSmmu4KProgramResult after;
        status=ArmSmmu4KObserveOwnedContext(io,bank,context,&after);if(status!=ArmSmmu4KSuccess)goto poisoned;
        if(after.id0!=r->synchronization.id0 || after.id1!=r->synchronization.id1 ||
            after.id2!=r->synchronization.id2 || after.context_offset!=r->synchronization.context_offset)
            {status=ArmSmmu4KStateMismatch;goto poisoned;}
        state->context_offset=r->synchronization.context_offset;
    }
    state->tables.leaf_pages=map?leaves+changed:leaves-changed;++state->generation;
    r->complete=1;r->translation_removed=!map;r->must_retain_resources=0;
    return ArmSmmu4KSuccess;
poisoned:
    state->poisoned=1;if(peer)peer->state->poisoned=1;return status;
}
ArmSmmu4KStatus ArmSmmu4KLiveUpdate(const ArmSmmu4KLiveArena *a,ArmSmmu4KLiveState *state,
    const ArmSmmu4KIo *io,unsigned bank,const ArmSmmu4KContextConfig *context,
    const ArmSmmu4KLiveVisibility *visibility,const ArmSmmu4KRange *before,size_t count,
    const ArmSmmu4KRange *change,unsigned map,unsigned budget,ArmSmmu4KLiveResult *r)
{return update(a,state,io,bank,context,visibility,before,count,0,change,1,map,0,budget,r);}
static ArmSmmu4KStatus update_context(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *context,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,const ArmSmmu4KLiveVisibility *visibility,
    const ArmSmmu4KRange *changes,size_t change_count,unsigned map,unsigned exclusive,unsigned budget,ArmSmmu4KLiveResult *r)
{
    if(!r)return ArmSmmu4KArgument;
    *r=(ArmSmmu4KLiveResult){0};r->must_retain_resources=1;
    if(change_upper>1)return ArmSmmu4KArgument;
    const ArmSmmu4KLiveRegion *target=change_upper?upper:lower,*peer=change_upper?lower:upper;
    if(!target || !target->state)return ArmSmmu4KArgument;
    ArmSmmu4KStatus status=context_metadata(context,lower,upper,target->state->generation==0);if(status)return status;
    return update(target->arena,target->state,io,bank,context,visibility,target->ranges,target->count,
        peer,changes,change_count,map,exclusive,budget,r);
}
ArmSmmu4KStatus ArmSmmu4KLiveUpdateContextBatch(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *context,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,const ArmSmmu4KLiveVisibility *visibility,
    const ArmSmmu4KRange *changes,size_t change_count,unsigned map,unsigned budget,ArmSmmu4KLiveResult *r)
{return update_context(io,bank,context,lower,upper,change_upper,visibility,changes,change_count,map,0,budget,r);}
ArmSmmu4KStatus ArmSmmu4KLiveMapContextBatchExclusive(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *context,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,const ArmSmmu4KLiveVisibility *visibility,
    const ArmSmmu4KRange *changes,size_t change_count,unsigned budget,ArmSmmu4KLiveResult *r)
{return update_context(io,bank,context,lower,upper,change_upper,visibility,changes,change_count,1,1,budget,r);}
ArmSmmu4KStatus ArmSmmu4KLiveUpdateContext(const ArmSmmu4KIo *io,unsigned bank,
    const ArmSmmu4KContextConfig *context,const ArmSmmu4KLiveRegion *lower,
    const ArmSmmu4KLiveRegion *upper,unsigned change_upper,const ArmSmmu4KLiveVisibility *visibility,
    const ArmSmmu4KRange *change,unsigned map,unsigned budget,ArmSmmu4KLiveResult *r)
{return ArmSmmu4KLiveUpdateContextBatch(io,bank,context,lower,upper,change_upper,visibility,change,1,map,budget,r);}
