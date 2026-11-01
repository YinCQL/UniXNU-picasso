/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4K.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1); } } while (0)
#define CAPACITY 1024
static uint64_t tables[CAPACITY * 512], before[CAPACITY * 512];
static unsigned visited[CAPACITY];
static uint64_t walk_at(const uint64_t *base, const ArmSmmu4KResult *result, uint64_t va)
{
    size_t page = 0;
    for (unsigned level = result->start_level; level <= 3; ++level) {
        uint64_t entry = base[page * 512 + ((va >> (39 - 9 * level)) & 511)];
        if (!entry) return 0;
        CHECK((entry & 3) == 3); /* No block leaves anywhere. */
        if (level == 3) return entry;
        uint64_t address = entry & UINT64_C(0xfffffffff000);
        CHECK(address >= result->root_physical);
        page = (address - result->root_physical) >> 12;
        CHECK(page < result->table_pages);
    }
    exit(1);
}
static size_t graph(size_t page, unsigned level, const ArmSmmu4KResult *result, size_t *leaves)
{
    CHECK(page < result->table_pages && !visited[page]);
    visited[page] = 1;
    size_t count = 1;
    for (size_t slot = 0; slot < 512; ++slot) {
        uint64_t entry = tables[page * 512 + slot];
        if (!entry) continue;
        CHECK((entry & 3) == 3);
        if (level == 3) { ++*leaves; continue; }
        CHECK(!(entry & ~UINT64_C(0xfffffffff003)));
        CHECK((entry & UINT64_C(0xfffffffff000)) >= result->root_physical);
        size_t child = ((entry & UINT64_C(0xfffffffff000)) - result->root_physical) >> 12;
        count += graph(child, level + 1, result, leaves);
    }
    return count;
}
static void verify(const ArmSmmu4KConfig *config, const ArmSmmu4KRange *ranges, size_t n, const ArmSmmu4KResult *result)
{
    CHECK(result->root_physical == config->table_physical && result->mair == 0xff44 && !result->upper_region);
    size_t leaves = 0;
    memset(visited, 0, sizeof(visited));
    CHECK(graph(0, result->start_level, result, &leaves) == result->table_pages);
    CHECK(leaves == result->leaf_pages);
    size_t checked = 0;
    for (size_t i = 0; i < n; ++i) {
        for (uint64_t off = 0; off < ranges[i].bytes; off += 4096) {
            uint64_t entry = walk_at(tables,result, ranges[i].iova + off);
            CHECK((entry & UINT64_C(0xfffffffff000)) == ranges[i].physical + off);
            CHECK(((entry >> 53) & 3) == (ranges[i].permissions & ArmSmmu4KExecute ? 0 : 3));
            CHECK(((entry >> 10) & 1) && ((entry >> 11) & 1));
            CHECK(((entry >> 6) & 1) == !(ranges[i].permissions & ArmSmmu4KPrivileged));
            CHECK(((entry >> 7) & 1) == !(ranges[i].permissions & ArmSmmu4KWrite));
            CHECK(((entry >> 2) & 7) == ranges[i].writeback);
            CHECK(((entry >> 8) & 3) == ranges[i].shareability);
            ++checked;
        }
        if (i + 1 < n && ranges[i].iova + ranges[i].bytes < ranges[i + 1].iova)
            CHECK(!walk_at(tables,result, ranges[i].iova + ranges[i].bytes));
    }
    CHECK(checked == result->leaf_pages);
    /* Successful building touches only the actually used table pages. */
    CHECK(!memcmp(tables + result->table_pages * 512, before + result->table_pages * 512,
                  (CAPACITY - result->table_pages) * 4096));
}
static void refusal(ArmSmmu4KConfig c, const ArmSmmu4KRange *r, size_t n, size_t capacity, ArmSmmu4KStatus status)
{
    ArmSmmu4KResult result;
    memset(tables, 0xa5, sizeof(tables));memcpy(before,tables,sizeof(tables));
    memset(&result,0xa5,sizeof(result));
    CHECK(ArmSmmu4KBuild(&c,r,n,tables,capacity,&result) == status);
    CHECK(!result.root_physical && !result.mair && !result.table_pages && !result.leaf_pages && !result.start_level && !result.upper_region);
    CHECK(!memcmp(tables,before,sizeof(tables)));
}
static void success(ArmSmmu4KConfig c, const ArmSmmu4KRange *r, size_t n)
{
    ArmSmmu4KResult result;
    memset(tables,0xa5,sizeof(tables));memcpy(before,tables,sizeof(tables));
    CHECK(ArmSmmu4KBuild(&c,r,n,tables,CAPACITY,&result) == ArmSmmu4KSuccess);
    verify(&c,r,n,&result);
}
int main(void)
{
    ArmSmmu4KConfig c = {48,48,0x10000000,4096};
    ArmSmmu4KRange r[] = {
        {0,0x20000000,0x3000,1,0,2}, {0x1ff000,0x21000000,0x3000,3,1,3},
        {0x3ffff000,0x22000000,0x3000,1,1,3}, {UINT64_C(0x7ffffff000),0x23000000,0x3000,3,0,2},
        {UINT64_C(0xfffffffff000),UINT64_C(0xfffffffff000),0x1000,1,0,2}
    };
    success(c,r,5);
    refusal(c,r,5,1,ArmSmmu4KCapacity);
    c.leaf_budget = 12;refusal(c,r,5,CAPACITY,ArmSmmu4KBudget);c.leaf_budget=4096;
    ArmSmmu4KRange bad[5];memcpy(bad,r,sizeof(r));
    bad[4].bytes=8192;refusal(c,bad,5,CAPACITY,ArmSmmu4KBadRange);
    memcpy(bad,r,sizeof(r));bad[4].physical += 4096;refusal(c,bad,5,CAPACITY,ArmSmmu4KBadRange);
    memcpy(bad,r,sizeof(r));bad[3].iova=0;refusal(c,bad,5,CAPACITY,ArmSmmu4KOverlap);
    memcpy(bad,r,sizeof(r));bad[3].physical=c.table_physical;refusal(c,bad,5,CAPACITY,ArmSmmu4KOverlap);
    memcpy(bad,r,sizeof(r));bad[3].physical=r[1].physical;refusal(c,bad,5,CAPACITY,ArmSmmu4KOverlap);
    bad[3].writeback=1;bad[3].shareability=3;success(c,bad,5); /* Consistent physical aliases allowed. */
    memcpy(bad,r,sizeof(r));bad[3].permissions=2;refusal(c,bad,5,CAPACITY,ArmSmmu4KBadRange);
    memcpy(bad,r,sizeof(r));bad[3].iova++;refusal(c,bad,5,CAPACITY,ArmSmmu4KBadRange);
    memcpy(bad,r,sizeof(r));bad[3].writeback=2;refusal(c,bad,5,CAPACITY,ArmSmmu4KBadRange);
    memcpy(bad,r,sizeof(r));bad[3].shareability=1;refusal(c,bad,5,CAPACITY,ArmSmmu4KBadRange);
    memcpy(bad,r,sizeof(r));bad[3].physical=r[1].physical;bad[3].writeback=1;
    refusal(c,bad,5,CAPACITY,ArmSmmu4KOverlap); /* Same MAIR, different SH. */
    c.table_physical++;refusal(c,r,5,CAPACITY,ArmSmmu4KBadRange);c.table_physical--;
    c.input_bits=49;refusal(c,r,5,CAPACITY,ArmSmmu4KArgument);c.input_bits=48;
    c.output_bits=52;refusal(c,r,5,CAPACITY,ArmSmmu4KArgument);c.output_bits=48;
    ArmSmmu4KRange many[256];
    for (unsigned bits=32;bits<=48;++bits) {
        c.input_bits=bits;
        uint64_t limit=UINT64_C(1)<<bits;
        for (size_t i=0;i<256;++i) many[i]=(ArmSmmu4KRange){(limit/256*i)&~UINT64_C(4095),0x30000000+i*4096,4096,i&1?1:3,(unsigned)(i&1),2u+(unsigned)(i&1)};
        many[255].iova=limit-4096;
        success(c,many,256);
    }
    c.input_bits=32;
    unsigned output_bits[]={32,36,40,42,44,48};
    for (size_t i=0;i<6;++i) {
        c.output_bits=output_bits[i];
        ArmSmmu4KRange edge={0x1000,(UINT64_C(1)<<c.output_bits)-4096,4096,1,0,2};
        success(c,&edge,1);
        edge.bytes=8192;refusal(c,&edge,1,CAPACITY,ArmSmmu4KBadRange);
    }
    refusal(c,r,0,CAPACITY,ArmSmmu4KArgument);
    CHECK(ArmSmmu4KBuild(&c,r,1,tables,CAPACITY,NULL)==ArmSmmu4KArgument);
    refusal(c,r,SIZE_MAX,CAPACITY,ArmSmmu4KArgument);
    refusal(c,r,1,SIZE_MAX,ArmSmmu4KArgument);
    refusal(c,r,1,0,ArmSmmu4KArgument);
    c.table_physical=(UINT64_C(1)<<c.output_bits)-4096;
    refusal(c,r,1,CAPACITY,ArmSmmu4KBadRange);
    c.table_physical=0x10000000;c.input_bits=48;
    ArmSmmu4KRange one={0x1000,0x40000000,4096,1,0,2};
    for (unsigned cache=0;cache<=1;++cache) {
        for (unsigned sh=2;sh<=3;++sh) {
            one.writeback=cache;one.shareability=sh;
            success(c,&one,1);
        }
    }
    for(unsigned permissions=0;permissions<32;++permissions) {
        one.permissions=permissions;
        if((permissions&1) && !(permissions&~15u))success(c,&one,1);
        else refusal(c,&one,1,CAPACITY,ArmSmmu4KBadRange);
    }
    ArmSmmu4KRange aliases[2]={{0x1000,0x40000000,4096,3,0,2},{0x2000,0x40000000,4096,11,0,2}};
    refusal(c,aliases,2,CAPACITY,ArmSmmu4KOverlap);
    one.permissions=1;one.writeback=0;one.shareability=2;
    ArmSmmu4KResult first,second_result;
    CHECK(ArmSmmu4KBuild(&c,&one,1,tables,CAPACITY,&first)==ArmSmmu4KSuccess);
    memcpy(before,tables,sizeof(tables));
    static uint64_t second[CAPACITY*512];
    c.table_physical=0x11000000;one.physical=0x50000000;
    CHECK(ArmSmmu4KBuild(&c,&one,1,second,CAPACITY,&second_result)==ArmSmmu4KSuccess);
    CHECK(!memcmp(tables,before,sizeof(tables)));
    CHECK((walk_at(tables,&first,one.iova)&UINT64_C(0xfffffffff000))==0x40000000);
    CHECK((walk_at(second,&second_result,one.iova)&UINT64_C(0xfffffffff000))==0x50000000);
    puts("PASS: native 4K leaves, independent table graph/walk, 2M/1G/512G boundaries, IAS32..48, six OAS widths, transactional refusals");
}
