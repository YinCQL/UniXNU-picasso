/* SPDX-License-Identifier: MIT */
#include "ArmSmmu4KContext.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#define PAGES 32
static uint64_t low[PAGES*512],high[PAGES*512],before[PAGES*512];
/* Built separately from the pinned original arm-smmu.h inline helpers. */
extern uint32_t source_tcr(unsigned,unsigned,unsigned);
extern uint32_t source_tcr2(unsigned);
static uint64_t walk(const uint64_t *tables,const ArmSmmu4KConfig *c,
    const ArmSmmu4KResult *r,uint64_t va)
{
    unsigned level=4-(c->input_bits-12+8)/9;size_t page=0;
    for(;level<=3;++level) {
        unsigned shift=39-9*level;
        unsigned bits=level==r->start_level?c->input_bits-shift:9;
        uint64_t entry=tables[page*512+((va>>shift)&((1u<<bits)-1))];
        if(!entry)return 0;
        CHECK((entry&3)==3); /* Never block descriptors. */
        if(level==3)return entry;
        uint64_t physical=entry&UINT64_C(0xfffffffff000);
        CHECK(physical>=r->root_physical && physical-r->root_physical<r->table_pages*4096);
        page=(physical-r->root_physical)/4096;
    }
    exit(1);
}
static void upper_tables(void)
{
    for(unsigned bits=32;bits<=48;++bits) {
        ArmSmmu4KConfig c={bits,48,0x11000000,64};
        uint64_t base=UINT64_MAX-(UINT64_C(1)<<bits)+1;
        ArmSmmu4KRange ranges[]={
            {base,0x20000000,4096,1,0,2},
            {base+0x1ff000,0x21000000,12288,3,1,3},
            {base+0x3ffff000,0x22000000,12288,1,1,3},
            {base+UINT64_C(0x7ffffff000),0x23000000,12288,3,0,2},
            {UINT64_MAX-4095,0x24000000,4096,1,0,2}};
        if(bits<40)ranges[3]=ranges[4];
        size_t count=bits<40?4:5;ArmSmmu4KResult r;
        memset(high,0xa5,sizeof(high));memcpy(before,high,sizeof(high));
        CHECK(ArmSmmu4KBuildUpper(&c,ranges,count,high,PAGES,&r)==ArmSmmu4KSuccess);
        CHECK(r.upper_region==1);
        unsigned shift=39-9*r.start_level,root_entries=1u<<(bits-shift);
        for(unsigned i=root_entries;i<512;++i)CHECK(!high[i]);
        size_t leaves=0;
        for(size_t i=0;i<count;++i)for(uint64_t off=0;off<ranges[i].bytes;off+=4096) {
            uint64_t entry=walk(high,&c,&r,ranges[i].iova+off);
            CHECK((entry&UINT64_C(0xfffffffff000))==ranges[i].physical+off);
            CHECK(((entry>>53)&3)==3 && ((entry>>2)&7)==ranges[i].writeback);
            CHECK(((entry>>8)&3)==ranges[i].shareability && ((entry>>10)&3)==3);
            CHECK(((entry>>6)&3)==(ranges[i].permissions==1?3:1));++leaves;
        }
        CHECK(leaves==r.leaf_pages && !walk(high,&c,&r,base+4096));
        CHECK(!memcmp(high+r.table_pages*512,before+r.table_pages*512,(PAGES-r.table_pages)*4096));
        memcpy(before,high,sizeof(high));
        ArmSmmu4KRange bad=ranges[count-1];bad.bytes=8192;
        CHECK(ArmSmmu4KBuildUpper(&c,&bad,1,high,PAGES,&r)==ArmSmmu4KBadRange);
        CHECK(!memcmp(high,before,sizeof(high)) && !r.table_pages);
        bad=ranges[0];bad.iova=0;
        CHECK(ArmSmmu4KBuildUpper(&c,&bad,1,high,PAGES,&r)==ArmSmmu4KBadRange);
        bad.iova=base-4096;
        CHECK(ArmSmmu4KBuildUpper(&c,&bad,1,high,PAGES,&r)==ArmSmmu4KBadRange);
        bad=ranges[0];
        CHECK(ArmSmmu4KBuild(&c,&bad,1,high,PAGES,&r)==ArmSmmu4KBadRange);
        CHECK(!memcmp(high,before,sizeof(high)));
    }
}
static void refusal(ArmSmmu4KContextConfig c,ArmSmmu4KStatus status)
{
    ArmSmmu4KContextImage image;memset(&image,0xa5,sizeof(image));
    CHECK(ArmSmmu4KEncodeContext(&c,&image)==status);
    unsigned char zero[sizeof(image)]={0};CHECK(!memcmp(&image,zero,sizeof(image)));
}
int main(void)
{
    upper_tables();unsigned widths[]={32,36,40,42,44,48};size_t images=0,mismatches=0,tbi_accepted=0;
    for(unsigned input=32;input<=48;++input)for(unsigned output=0;output<6;++output) {
        ArmSmmu4KConfig a={input,widths[output],0x10000000,16},b=a;b.table_physical=0x11000000;
        ArmSmmu4KRange ra={0x1000,0x20000000,4096,3,1,3},rb=ra;rb.iova=UINT64_MAX-4095;
        ArmSmmu4KResult ar,br;
        CHECK(ArmSmmu4KBuild(&a,&ra,1,low,PAGES,&ar)==ArmSmmu4KSuccess);
        CHECK(ArmSmmu4KBuildUpper(&b,&rb,1,high,PAGES,&br)==ArmSmmu4KSuccess);
        for(unsigned mode=1;mode<=3;++mode)for(unsigned coherent=0;coherent<2;++coherent)
        for(unsigned fault=0;fault<4;++fault)for(unsigned ai=0;ai<3;++ai) {
            unsigned asids[]={0,1,65535};
            ArmSmmu4KContextConfig c={mode&1?&a:NULL,mode&2?&b:NULL,
                mode&1?&ar:NULL,mode&2?&br:NULL,1u<<30,0x1555,asids[ai],coherent,fault&1,fault>>1,0};
            ArmSmmu4KContextImage v;
            CHECK(ArmSmmu4KEncodeContext(&c,&v)==ArmSmmu4KSuccess);
            uint32_t expected=UINT32_C(2)<<30;
            if(mode&1)expected|=source_tcr(input,0,coherent);
            if(mode&2)expected|=source_tcr(input,1,coherent);
            if(mode==3)expected&=~((1u<<7)|(1u<<23));
            CHECK(v.tcr==expected && v.tcr2==(source_tcr2(output)|(1u<<4)));
            CHECK(v.cbar==0x1f300 && v.cba2r==1 && v.mair0==0xff44 && !v.mair1 && !v.reserved_zero);
            CHECK(v.sctlr==(0x1067|((fault&1)<<7)|((fault>>1)<<8)));
            CHECK(v.ttbr0==((uint64_t)c.asid<<48|(mode&1?a.table_physical:0)));
            CHECK(v.ttbr1==((uint64_t)c.asid<<48|(mode&2?b.table_physical:0)));
            CHECK(ArmSmmu4KValidateContextReadback(&c,&v)==ArmSmmu4KSuccess);++images;
            /* Every stored bit, including each granule, ASID, root address,
             * enable/stage/format/cache/attribute field, must match. */
            /* Except TCR2.TBI0/TBI1 reading 1 where 0 was written (int-008 hardware), accepted alone or together,
             * and SCTLR.ASIDPNE reading 0 where 1 was written (int-009 hardware). */
            for(size_t byte=0;byte<sizeof(v);++byte)for(unsigned bit=0;bit<8;++bit) {
                ArmSmmu4KContextImage changed=v;((unsigned char *)&changed)[byte]^=1u<<bit;
                const int tbi=(byte==offsetof(ArmSmmu4KContextImage,tcr2) && (bit==5 || bit==6)) ||
                    (byte==offsetof(ArmSmmu4KContextImage,sctlr)+1 && bit==4);
                CHECK(ArmSmmu4KValidateContextReadback(&c,&changed)==(tbi?ArmSmmu4KSuccess:ArmSmmu4KStateMismatch));
                if(tbi)++tbi_accepted;else ++mismatches;
            }
            {
                ArmSmmu4KContextImage changed=v;changed.tcr2|=0x60;
                CHECK(ArmSmmu4KValidateContextReadback(&c,&changed)==ArmSmmu4KSuccess);++tbi_accepted;
                changed.sctlr&=~0x1000u;   /* the int-009 image: both tolerances together */
                CHECK(ArmSmmu4KValidateContextReadback(&c,&changed)==ArmSmmu4KSuccess);++tbi_accepted;
                changed.tcr2|=0x80;   /* any other extra bit still refuses */
                CHECK(ArmSmmu4KValidateContextReadback(&c,&changed)==ArmSmmu4KStateMismatch);++mismatches;
            }
            ArmSmmu4KContextConfig bad=c;bad.id0=0;refusal(bad,ArmSmmu4KUnsupported);
            bad=c;bad.id2&=~(1u<<12);refusal(bad,ArmSmmu4KUnsupported);
            bad=c;bad.id2=(bad.id2&~0xf00)|0x600;refusal(bad,ArmSmmu4KUnsupported);
            bad=c;bad.id2=(bad.id2&~0xf0)|0x60;refusal(bad,ArmSmmu4KUnsupported);
            bad=c;bad.asid=65536;refusal(bad,ArmSmmu4KArgument);
            bad=c;bad.coherent_walk=2;refusal(bad,ArmSmmu4KArgument);
            bad=c;bad.stall_on_fault=2;refusal(bad,ArmSmmu4KArgument);
        }
        ArmSmmu4KContextConfig c={&a,&b,&ar,&br,1u<<30,0x1555,7,1,1,1,0};
        ArmSmmu4KContextConfig bad=c;bad.upper=NULL;refusal(bad,ArmSmmu4KArgument);
        bad=c;bad.lower_tables=NULL;refusal(bad,ArmSmmu4KArgument);
        bad=c;bad.continue_after_fault=2;refusal(bad,ArmSmmu4KArgument);
        bad=c;bad.id2=0x1000;if(input>32 || widths[output]>32)refusal(bad,ArmSmmu4KUnsupported);
        uint64_t saved=br.root_physical;br.root_physical++;refusal(c,ArmSmmu4KArgument);br.root_physical=saved;
        saved=br.mair;br.mair=0;refusal(c,ArmSmmu4KArgument);br.mair=saved;
        unsigned level=br.start_level;br.start_level=3;refusal(c,ArmSmmu4KArgument);br.start_level=level;
        br.upper_region=0;refusal(c,ArmSmmu4KArgument);br.upper_region=1;
        saved=b.table_physical;b.table_physical=a.table_physical;br.root_physical=b.table_physical;
        refusal(c,ArmSmmu4KOverlap);b.table_physical=saved;br.root_physical=saved;
    }
    ArmSmmu4KContextConfig none={0};refusal(none,ArmSmmu4KArgument);
    CHECK(ArmSmmu4KEncodeContext(NULL,NULL)==ArmSmmu4KArgument);
    CHECK(ArmSmmu4KValidateContextReadback(NULL,NULL)==ArmSmmu4KArgument);
    printf("PASS: native4K TTBR1 high/partial-root/upper-bound tables; %zu context images versus pinned original helpers; %zu one-bit readback refusals; %zu TCR2.TBI/SCTLR.ASIDPNE readback tolerances\n",images,mismatches,tbi_accepted);
}
