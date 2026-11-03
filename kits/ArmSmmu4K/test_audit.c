/* SPDX-License-Identifier: MIT */
/* Synthetic owned physical memory only; no hardware observation. */
#include "ArmSmmu4KTableMemory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define CAP 16
_Alignas(4096) static uint64_t tables[CAP*512];
static unsigned char visited[CAP];
static ArmSmmu4KConfig c;
static ArmSmmu4KResult built;
static ArmSmmu4KRange ranges[3];
static ArmSmmu4KAuditRegion a;
static ArmSmmu4KTableAuditResult result;
static size_t calls,fail_at;
static int read_descriptor(void *cookie,uint64_t pa,uint64_t *out)
{
    CHECK(cookie==tables && !(pa&7) && pa>=c.table_physical && pa-c.table_physical<sizeof(tables));
    if(++calls==fail_at)return 1;
    *out=tables[(pa-c.table_physical)/8];return 0;
}
static ArmSmmu4KTableIo io={tables,read_descriptor};
static ArmSmmu4KStatus audit(void)
{calls=0;return ArmSmmu4KAuditTables(&io,&c,&built,&a,c.table_physical,&result);}
static void setup(unsigned bits,unsigned oas,unsigned upper,unsigned permissions)
{
    c=(ArmSmmu4KConfig){bits,oas,0x1000000,8};
    uint64_t span=UINT64_C(1)<<bits,base=upper?UINT64_MAX-span+1:0;
    ranges[0]=(ArmSmmu4KRange){base,0x2000000,8192,permissions,0,2};
    ranges[1]=(ArmSmmu4KRange){base+0x1ff000,0x3000000,8192,permissions,1,3};
    ranges[2]=(ArmSmmu4KRange){base+(span-4096),0x4000000,4096,permissions,0,3};
    CHECK(!(upper?ArmSmmu4KBuildUpper(&c,ranges,3,tables,CAP,&built):ArmSmmu4KBuild(&c,ranges,3,tables,CAP,&built)));
    a=(ArmSmmu4KAuditRegion){ranges,3,CAP,CAP*512,visited,sizeof(visited)};fail_at=0;
}
int main(void)
{
    unsigned widths[]={32,36,40,42,44,48},profiles=0,mutations=0,failures=0;
    for(unsigned bits=32;bits<=48;++bits)for(unsigned o=0;o<6;++o)
    for(unsigned upper=0;upper<2;++upper)for(unsigned p=1;p<16;p+=2) {
        setup(bits,widths[o],upper,p);CHECK(!audit() && result.complete && result.leaf_pages==5);
        CHECK(result.table_pages==built.table_pages && calls==built.table_pages*512);++profiles;
    }
    for(unsigned upper=0;upper<2;++upper) {
        setup(40,40,upper,15);size_t reads=built.table_pages*512;
        for(size_t n=1;n<=reads;++n){fail_at=n;CHECK(audit()==ArmSmmu4KIoError && !result.complete && calls==n);++failures;}
        fail_at=0;
        /* Every reachable nonzero descriptor bit, plus EVERY unused slot.
         * Includes reserved high bits, AP/XN, AF/nG, SH, type and PA bits. */
        for(size_t n=0;n<reads;++n) {
            uint64_t old=tables[n];
            for(unsigned bit=0;bit<(old?64u:1u);++bit) {
                tables[n]=old^(UINT64_C(1)<<bit);CHECK(audit()!=ArmSmmu4KSuccess && !result.complete);++mutations;
            }
            tables[n]=old;
        }
        CHECK(!audit());
        a.read_budget=reads-1;CHECK(audit()==ArmSmmu4KBudget && !calls);a.read_budget=CAP*512;
        a.visited_bytes=CAP-1;CHECK(audit()==ArmSmmu4KArgument && !calls);a.visited_bytes=CAP;
        built.root_physical+=4096;CHECK(audit()==ArmSmmu4KStateMismatch && !calls);built.root_physical-=4096;
        uint64_t root=tables[0];tables[0]=c.table_physical|3;CHECK(audit()==ArmSmmu4KOverlap);tables[0]=root;
        tables[0]=(c.table_physical+CAP*4096)|3;CHECK(audit()==ArmSmmu4KBadRange);tables[0]=root;
        /* A legitimate graph need not have builder allocation order: swap
         * root's first child into a spare page and redirect its actual pointer. */
        size_t child=(size_t)((root&UINT64_C(0x0000fffffffff000))-c.table_physical)/4096;
        memcpy(tables+(CAP-1)*512,tables+child*512,4096);
        memset(tables+child*512,0xcc,4096);tables[0]=(c.table_physical+(CAP-1)*4096)|3;
        CHECK(!audit());
    }
    /* Cross-region reservations must be checked before any physical read. */
    setup(40,40,0,3);
    ArmSmmu4KConfig uc=c;uc.table_physical+=4096;
    ArmSmmu4KResult ub=built;ub.root_physical=uc.table_physical;ub.upper_region=1;
    ArmSmmu4KRange ur[3];for(unsigned i=0;i<3;++i){ur[i]=ranges[i];ur[i].iova|=UINT64_MAX-((UINT64_C(1)<<40)-1);}
    unsigned char uv[CAP];ArmSmmu4KAuditRegion ua=a;ua.ranges=ur;ua.visited=uv;
    ArmSmmu4KContextConfig cc={.lower=&c,.upper=&uc,.lower_tables=&built,.upper_tables=&ub};
    ArmSmmu4KAuditConfig ac={io,&a,&ua};ArmSmmu4KAuditResult ar;
    calls=0;CHECK(ArmSmmu4KAuditDeclaredContext(&cc,&ac,&ar)==ArmSmmu4KOverlap && !calls);
    uc.table_physical=0x1800000;ub.root_physical=uc.table_physical;
    ur[0].physical=c.table_physical+(CAP-1)*4096;
    CHECK(ArmSmmu4KAuditDeclaredContext(&cc,&ac,&ar)==ArmSmmu4KOverlap && !calls);
    ur[0].physical=ranges[0].physical;ur[0].permissions|=ArmSmmu4KPrivileged;
    CHECK(ArmSmmu4KAuditDeclaredContext(&cc,&ac,&ar)==ArmSmmu4KOverlap && !calls);
    /* Exercise the concrete production volatile-load adapter on real host
     * buffers with SYNTHETIC physical labels. It does not prove VA->PA. */
    setup(40,40,1,15);
    ArmSmmu4KTableMemory memory={1,{{tables,c.table_physical,CAP},{0}}};ArmSmmu4KTableIo direct;
    CHECK(!ArmSmmu4KBindTableMemory(&memory,&direct));
    CHECK(!ArmSmmu4KAuditTables(&direct,&c,&built,&a,c.table_physical,&result));
    uint64_t value;
    CHECK(direct.read_descriptor(direct.cookie,c.table_physical-8,&value));
    CHECK(direct.read_descriptor(direct.cookie,c.table_physical+CAP*4096,&value));
    CHECK(direct.read_descriptor(direct.cookie,c.table_physical+1,&value));
    CHECK(direct.read_descriptor(direct.cookie,c.table_physical,NULL));
    memory.count=2;memory.pools[1]=memory.pools[0];
    CHECK(ArmSmmu4KBindTableMemory(&memory,&direct)==ArmSmmu4KOverlap && !direct.read_descriptor);
    memory.count=1;memory.pools[0].mapping=tables+1;
    CHECK(ArmSmmu4KBindTableMemory(&memory,&direct)==ArmSmmu4KBadRange && !direct.read_descriptor);
    memory.pools[0].mapping=(const volatile uint64_t *)(UINTPTR_MAX-4095);
    CHECK(ArmSmmu4KBindTableMemory(&memory,&direct)==ArmSmmu4KBadRange);
    memory.pools[0].mapping=tables;memory.pools[0].physical=(UINT64_C(1)<<48)-4096;
    CHECK(ArmSmmu4KBindTableMemory(&memory,&direct)==ArmSmmu4KBadRange);
    printf("PASS: %u physical-graph profiles, %u descriptor mutations, %u read failures; upper final page, relocated tables, cycles, full-pool and cross-region isolation; synthetic physical memory only\n",profiles,mutations,failures);
}
