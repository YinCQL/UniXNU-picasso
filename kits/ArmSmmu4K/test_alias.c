/* SPDX-License-Identifier: MIT */
/* The sorted physical-alias check against the pairwise definition (conflicting_alias over every pair) on random sets: scattered
 * single pages and long runs, up to eight attribute classes, self and cross comparisons, with scratch, without it and with too
 * little of it; then one int-144-sized case (24k coherent import pages beside 82k non-cacheable pages) for its cost. */
#ifndef ARM_SMMU4K_ALIAS_SOURCE
#define ARM_SMMU4K_ALIAS_SOURCE "ArmSmmu4KLive.c"
#endif
#include ARM_SMMU4K_ALIAS_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef ARM_SMMU4K_TEST_COVERAGE
/* Compiler basic-block coverage counts, test-only. Production needs no hook or
 * counter. Volatile keeps the caller's reads intact before coverage insertion. */
static volatile uint64_t alias_steps;
__attribute__((no_sanitize("coverage"))) void __sanitizer_cov_trace_pc_guard(uint32_t *guard)
{(void)guard;++alias_steps;}
__attribute__((no_sanitize("coverage"))) void __sanitizer_cov_trace_pc_guard_init(uint32_t *start,uint32_t *end)
{if(start==end || *start)return;for(uint32_t *p=start;p<end;++p)*p=1;}
#endif
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static uint64_t seed=0x9e3779b97f4a7c15ull;
static uint64_t rnd(void){seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
static int pairwise(const ArmSmmu4KRange *a,size_t na,const ArmSmmu4KRange *b,size_t nb)
{
    for(size_t i=0;i<na;++i)for(size_t j=0;j<nb;++j) {
        const int different=a[i].writeback!=b[j].writeback || a[i].shareability!=b[j].shareability ||
            ((a[i].permissions^b[j].permissions)&ArmSmmu4KPrivileged);
        if(different && a[i].physical<b[j].physical+b[j].bytes && b[j].physical<a[i].physical+a[i].bytes)return 1;
    }
    return 0;
}
static void fill(ArmSmmu4KRange *r,size_t n,unsigned classes,uint64_t span)
{
    for(size_t i=0;i<n;++i) {
        const unsigned c=(unsigned)(rnd()%classes);
        const uint64_t pages=(rnd()%8)?1:1+rnd()%64;
        r[i]=(ArmSmmu4KRange){0x100000+i*0x100000,0x80000000+(rnd()%span)*4096,pages*4096,
            (c&2)?3u|ArmSmmu4KPrivileged:3u,c&1u,(c&4u)?3u:2u};
    }
}
static uint32_t scratch[1u<<18];
int main(void)
{
    static ArmSmmu4KRange a[4096],b[4096];
    ArmSmmu4KLiveArena with={0},without={0},small={0};
    with.alias_scratch=scratch;with.alias_scratch_entries=sizeof scratch/sizeof *scratch;
    small.alias_scratch=scratch;small.alias_scratch_entries=3;
    unsigned cases=0,conflicts=0;
    for(unsigned round=0;round<3000;++round) {
        const size_t na=1+rnd()%(round<2000?40:600),nb=1+rnd()%(round<2000?40:600);
        const unsigned classes=1+(unsigned)(rnd()%8);
        const uint64_t span=1+rnd()%(round&1?200000:4000);
        fill(a,na,classes,span);fill(b,nb,classes,span);
        const int cross=pairwise(a,na,b,nb),self=pairwise(a,na,a,na);
        CHECK(any_conflicting_alias(&with,a,na,b,nb)==cross && any_conflicting_alias(&with,b,nb,a,na)==cross);
        CHECK(any_conflicting_alias(&without,a,na,b,nb)==cross && any_conflicting_alias(&small,a,na,b,nb)==cross);
        CHECK(any_conflicting_alias(&with,a,na,a,na)==self && any_conflicting_alias(&small,a,na,a,na)==self);
        cases+=5;conflicts+=(unsigned)(cross+self);
    }
    /* int-144's shape: 23855 coherent import pages and 82000 non-cacheable pages, disjoint, then one aliased page. */
    enum {Imports=23855,Owned=82000};
    ArmSmmu4KRange *all=calloc(Imports+Owned,sizeof *all);CHECK(all);
    for(size_t i=0;i<Imports+Owned;++i) {
        const int coherent=i<Imports;
        all[i]=(ArmSmmu4KRange){0x100000+i*4096,(coherent?UINT64_C(0x800000000):UINT64_C(0x900000000))+(rnd()%0x100000)*4096*2+
            (coherent?0:4096),4096,3,coherent?1u:0u,coherent?3u:2u};
    }
    const clock_t began=clock();
    CHECK(!any_conflicting_alias(&with,all,Imports+Owned,all,Imports+Owned));
    CHECK(!any_conflicting_alias(&with,all,Imports,all+Imports,Owned));
    all[Imports+Owned-1].physical=all[17].physical;
    CHECK(any_conflicting_alias(&with,all,Imports+Owned,all,Imports+Owned));
    CHECK(any_conflicting_alias(&with,all+Imports+Owned-1,1,all,Imports));
    const double ms=1000.0*(double)(clock()-began)/CLOCKS_PER_SEC;
    CHECK(ms<2000);
    free(all);
    printf("PASS: %u sorted alias checks equal the pairwise definition (%u conflicting) with, without and with too little scratch; "
           "a 23855+82000-page mixed-class set checked 4 times in %.0f ms\n",cases,conflicts,ms);
    /* One long interval lies beyond every probe. Its length must not make each
     * gap probe scan all preceding small intervals. Equal interval lengths in
     * the earlier large case do not expose that degeneration. */
    enum {Mixed=8192};
    ArmSmmu4KRange *left=calloc(Mixed+1,sizeof *left),*right=calloc(Mixed+2,sizeof *right);CHECK(left && right);
    for(size_t i=0;i<Mixed+2;++i) {
        if(i<Mixed)left[i]=(ArmSmmu4KRange){0,0x40000000+i*8192,4096,3,0,2};
        right[i]=(ArmSmmu4KRange){0,0x40001000+i*8192,4096,3,1,3};
    }
    left[Mixed]=(ArmSmmu4KRange){0,UINT64_C(0x200000000),UINT64_C(0x10000000),3,0,2};
#ifdef ARM_SMMU4K_TEST_COVERAGE
    alias_steps=0;
#endif
    const clock_t mixed_began=clock();
    CHECK(!any_conflicting_alias(&with,left,Mixed+1,right,Mixed+2));
#ifdef ARM_SMMU4K_TEST_COVERAGE
    const uint64_t work=alias_steps;
    printf("mixed alias diagnostic: %u intervals, %llu coverage steps, %.1f host ms (not hardware timing)\n",2*Mixed+3,
        (unsigned long long)work,1000.0*(double)(clock()-mixed_began)/CLOCKS_PER_SEC);fflush(stdout);
    CHECK(work>0 && work<UINT64_C(512)*(2*Mixed+3));
#else
    (void)mixed_began;
#endif
    CHECK(!any_conflicting_alias(&with,right,Mixed+2,left,Mixed+1));
    right[Mixed+1].physical=left[Mixed].physical+4096;
    CHECK(any_conflicting_alias(&with,left,Mixed+1,right,Mixed+2));
    free(left);free(right);
#ifdef ARM_SMMU4K_TEST_COVERAGE
    puts("PASS: mixed-size alias gap/long-range controls and bounded compiler-coverage work");
#else
    puts("PASS: mixed-size alias gap/long-range controls; work counter not enabled");
#endif
    return 0;
}
