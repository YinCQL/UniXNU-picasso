/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4K.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
extern uint64_t source_permission_pte(unsigned,unsigned);
static uint64_t tables[8*512];
static uint64_t walk(const ArmSmmu4KConfig *c,const ArmSmmu4KResult *r,uint64_t va)
{
    size_t page=0;
    for(unsigned level=r->start_level;level<=3;++level) {
        unsigned width=level==r->start_level?c->input_bits-12-9*(3-level):9;
        size_t index=(va>>(12+9*(3-level)))&((1u<<width)-1);
        uint64_t entry=tables[page*512+index];CHECK((entry&3)==3);
        if(level==3)return entry;
        uint64_t physical=entry&UINT64_C(0xfffffffff000);
        CHECK(physical>=r->root_physical);page=(physical-r->root_physical)/4096;CHECK(page<r->table_pages);
    }
    CHECK(0);return 0;
}
int main(void)
{
    unsigned count=0;
    for(unsigned bits=32;bits<=48;++bits)for(unsigned upper=0;upper<2;++upper)
    for(unsigned permission=1;permission<16;permission+=2)for(unsigned cache=0;cache<2;++cache) {
        ArmSmmu4KConfig c={bits,40,0x10000000,2};ArmSmmu4KResult r;
        uint64_t iova=upper?UINT64_MAX-(UINT64_C(1)<<bits)+1:0;iova+=0x1ff000;
        ArmSmmu4KRange range={iova,0x30000000,8192,permission,cache,cache?3:2};
        CHECK((upper?ArmSmmu4KBuildUpper:ArmSmmu4KBuild)(&c,&range,1,tables,8,&r)==ArmSmmu4KSuccess);
        for(unsigned page=0;page<2;++page) {
            uint64_t expected=(range.physical+4096*page)|source_permission_pte(permission,cache)|3;
            CHECK(walk(&c,&r,iova+4096*page)==expected);
        }
        ++count;
    }
    printf("PASS: %u lower/upper native4K permission profiles match original LPAE source (Read/Write/Execute/Privileged, NC/WB, IAS32..48); no CPU or live GPU permission claim\n",count);
}
