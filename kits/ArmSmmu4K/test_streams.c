/* SPDX-License-Identifier: MIT */
/* Software SMMUv2 register peer only: no device or active hardware proof. */
#include "ArmSmmu4KStreams.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
#define BYTES 0x40000
#define CB 0x22000
static uint32_t memory[BYTES/4];
static struct {unsigned calls,fail_call,after,writes,events,corrupt,cleanup_fail;size_t at[1024];uint32_t value[1024];} f;
static ArmSmmu4KIo io;
static ArmSmmu4KStreamState state;
static ArmSmmu4KStream routes[2]={{2,0},{4,1}};
static ArmSmmu4KStreamResult result;
static uint64_t tables[2048];
static ArmSmmu4KConfig lower={32,40,0x10000000,1};
static ArmSmmu4KResult built;
static ArmSmmu4KContextConfig config;
#ifdef SOURCE_ORACLE
extern void source_stream(unsigned,unsigned,unsigned,unsigned,unsigned,uint32_t *,uint32_t *);
extern uint32_t source_stream_register(unsigned);
#endif
static int event(void){return ++f.calls==f.fail_call;}
static int read32(void *cookie,size_t at,uint32_t *v)
{CHECK(cookie==&f && at%4==0 && at<=BYTES-4);if(event())return 1;*v=memory[at/4];return 0;}
static int read64(void *cookie,size_t at,uint64_t *v)
{CHECK(cookie==&f && at%8==0 && at<=BYTES-8);if(event())return 1;memcpy(v,memory+at/4,8);return 0;}
static int write32(void *cookie,size_t at,uint32_t v)
{
    CHECK(cookie==&f && at%4==0 && at<=BYTES-4 && f.events<1024);
    ++f.writes;f.at[f.events]=at;f.value[f.events++]=v;
    int fail=event();if(fail && !f.after)return 1;
    if(f.cleanup_fail && v==0x20000)return 1;
    memory[at/4]=v;
    if(f.corrupt && at==0x808)memory[at/4]^=2;
    return fail;
}
static int write64(void *cookie,size_t at,uint64_t v)
{(void)cookie;(void)at;(void)v;CHECK(0);return 1;}
static int barrier(void *cookie){CHECK(cookie==&f);return event();}
static void reset(unsigned exid)
{
    memset(memory,0,sizeof(memory));memset(&f,0,sizeof(f));state=(ArmSmmu4KStreamState){0};
    io=(ArmSmmu4KIo){&f,BYTES,read32,read64,write32,write64,barrier};
    memory[0x20/4]=(1u<<30)|(1u<<27)|(4u<<9)|8|(exid<<8);
    memory[0x24/4]=0x40000010;memory[0x28/4]=0x1433;memory[0]=0x436|(exid<<3);
    config=(ArmSmmu4KContextConfig){.lower=&lower,.lower_tables=&built,.asid=1,.coherent_walk=1,.continue_after_fault=1};
    ArmSmmu4KContextConfig actual=config;actual.id0=memory[0x20/4];actual.id2=memory[0x28/4];
    ArmSmmu4KContextImage image;CHECK(!ArmSmmu4KEncodeContext(&actual,&image));
    memory[0x1008/4]=image.cbar;memory[0x1808/4]=image.cba2r;
    memory[CB/4]=image.sctlr;memory[(CB+0x10)/4]=image.tcr2;memory[(CB+0x30)/4]=image.tcr;
    memcpy(memory+(CB+0x20)/4,&image.ttbr0,8);memcpy(memory+(CB+0x28)/4,&image.ttbr1,8);
    memory[(CB+0x38)/4]=image.mair0;
    for(unsigned i=0;i<8;++i)memory[(0xc00+4*i)/4]=0x20000;
    /* An unrelated live route, never to be altered by attachment/recovery. */
    memory[0x800/4]=(exid?0:1u<<31)|9;memory[0xc00/4]=(exid?1u<<10:0)|3;
}
static ArmSmmu4KStatus attach(void)
{return ArmSmmu4KAttachStreams(&io,&state,routes,2,2,&config,&result);}
static void isolated(unsigned exid)
{
    CHECK(memory[0x800/4]==((exid?0:1u<<31)|9) && memory[0xc00/4]==((exid?1u<<10:0)|3));
    for(unsigned i=0;i<f.events;++i) {
        CHECK(f.at[i]==0x808 || f.at[i]==0x810 || f.at[i]==0xc08 || f.at[i]==0xc10);
        if(f.at[i]>=0xc00)CHECK(((f.value[i]>>16)&3)!=1); /* Never BYPASS. */
    }
}
int main(void)
{
    ArmSmmu4KRange range={0,0x20000000,4096,3,0,2};
    CHECK(!ArmSmmu4KBuild(&lower,&range,1,tables,4,&built));
#ifdef SOURCE_ORACLE
    const uint32_t offsets[]={0,0x20,0x48,0x808,0xc08,0x80000000,0x400,0x400};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);++i)CHECK(source_stream_register(i)==offsets[i]);
#endif
    unsigned fault_cases=0;
    for(unsigned exid=0;exid<2;++exid) {
        reset(exid);CHECK(!attach() && state.attached && result.streams_verified==2 && result.context.enabled && result.must_retain_resources);
        unsigned normal_calls=f.calls;
        CHECK(f.at[0]==0xc08 && f.at[1]==0x808);
        for(unsigned i=0;i<2;++i) {
            uint32_t smr=(exid?0:1u<<31)|routes[i].sid,s2cr=2|(exid?1u<<10:0);
#ifdef SOURCE_ORACLE
            source_stream(exid,routes[i].sid,0,1,2,&smr,&s2cr);
#endif
            CHECK(memory[(0x800+4*routes[i].slot)/4]==smr && memory[(0xc00+4*routes[i].slot)/4]==s2cr);
        }
        unsigned writes=f.writes;CHECK(!ArmSmmu4KObserveStreams(&io,&state,&config,&result) && f.writes==writes);
        const unsigned full_reads=result.reads;
        CHECK(!ArmSmmu4KObserveOwnedStreams(&io,&state,&config,&result) && f.writes==writes && result.streams_verified==2);
        CHECK(result.reads==3+2*2 && result.reads<full_reads && result.context.enabled);
        /* Every read or barrier failing is an I/O error, never a pass. */
        for(unsigned at=1;;++at) {
            f.calls=0;f.fail_call=at;
            const ArmSmmu4KStatus st=ArmSmmu4KObserveOwnedStreams(&io,&state,&config,&result);
            if(!st){CHECK(at>1);break;}
            CHECK(st==ArmSmmu4KIoError && f.writes==writes);++fault_cases;
        }
        f.fail_call=0;f.calls=0;
        CHECK(attach()==ArmSmmu4KBusy && f.writes==writes);
        CHECK(!ArmSmmu4KFaultStreams(&io,&state,&result) && !state.attached && !state.poisoned && result.streams_faulted==2 && result.must_retain_resources);
        isolated(exid);
        for(unsigned i=0;i<2;++i)CHECK(memory[(0xc00+4*routes[i].slot)/4]==(0x20000|(exid?1u<<10:0)));
        for(unsigned at=1;at<=normal_calls;++at)for(unsigned after=0;after<2;++after) {
            reset(exid);f.fail_call=at;f.after=after;
            CHECK(attach()==ArmSmmu4KIoError && result.must_retain_resources && !state.attached);
            if(state.attempted){CHECK(state.poisoned && result.streams_faulted);CHECK(attach()==ArmSmmu4KBusy);}
            else CHECK(!f.writes);
            isolated(exid);++fault_cases;
        }
        reset(exid);f.corrupt=1;
        CHECK(attach()==ArmSmmu4KStateMismatch && state.poisoned && !state.attached && result.must_retain_resources);
        isolated(exid);
        reset(exid);f.fail_call=45;f.cleanup_fail=1;
        CHECK(attach()!=ArmSmmu4KSuccess && result.must_retain_resources);isolated(exid);
        /* Every global/context/route refusal must precede any register write. */
        for(unsigned control=0;control<16;++control) {
            reset(exid);
            switch(control) {
            case 0:memory[0]|=1;break;case 1:memory[0]&=~0x400;break;case 2:memory[0]^=8;break;
            case 3:memory[0x48/4]=2;break;case 4:memory[(CB+0x30)/4]^=1u<<14;break;
            case 5:memory[CB/4]&=~1;break;case 6:memory[(CB+0x58)/4]=2;break;
            case 7:memory[0xc08/4]=0x10000;break;
            case 8:memory[0x800/4]=(exid?0:1u<<31)|0;break; /* exact collision */
            case 9:memory[0x800/4]=(exid?0:1u<<31)|(15u<<16)|8;break; /* masked collision */
            case 10:memory[0x20/4]&=~(1u<<27);break;
            case 11:memory[0x808/4]=(exid?0:1u<<31)|7;memory[0xc08/4]|=exid?1u<<10:0;break;
            case 12:memory[0x20/4]&=~255u;break;
            case 13:memory[0xc00/4]=(exid?1u<<10:0)|2;break;
            case 14:memory[0]&=~2u;break;   /* no global fault reporting */
            case 15:memory[0]&=~4u;break;   /* no global fault interrupt */
            }
            CHECK(attach()!=ArmSmmu4KSuccess && !f.writes && !state.attempted && result.must_retain_resources);
        }
        /* Configuration-access fault reporting clear, as the sheng Adreno SMMU firmware leaves it (0x002f0406). */
        reset(exid);memory[0]&=~0x30u;
        CHECK(!attach() && state.attached);
        reset(exid);
        CHECK(!ArmSmmu4KCheckFaultedStreams(&io,routes,2,2,&result) && !f.writes);
        CHECK(!attach());writes=f.writes;memory[0xc08/4]^=1;
        CHECK(ArmSmmu4KObserveStreams(&io,&state,&config,&result)==ArmSmmu4KStateMismatch && f.writes==writes);
        CHECK(ArmSmmu4KObserveOwnedStreams(&io,&state,&config,&result)==ArmSmmu4KStateMismatch && f.writes==writes);
        memory[0xc08/4]^=1;memory[0x810/4]^=1;
        CHECK(ArmSmmu4KObserveOwnedStreams(&io,&state,&config,&result)==ArmSmmu4KStateMismatch && f.writes==writes);
        memory[0x810/4]^=1;memory[0]|=1;
        CHECK(ArmSmmu4KObserveOwnedStreams(&io,&state,&config,&result)==ArmSmmu4KStateMismatch && f.writes==writes);
        memory[0]&=~1u;memory[CB/4]&=~1u;
        CHECK(ArmSmmu4KObserveOwnedStreams(&io,&state,&config,&result)!=ArmSmmu4KSuccess && f.writes==writes);
        memory[CB/4]|=1u;CHECK(!ArmSmmu4KObserveOwnedStreams(&io,&state,&config,&result));
        memory[0xc08/4]^=1;
        CHECK(ArmSmmu4KFaultStreams(&io,&state,&result)==ArmSmmu4KStateMismatch && f.writes==writes);
    }
    printf("PASS: legacy/EXIDS stream attachment and quarantine, %u per-call I/O failure cases, 32 pre-write refusals, 2 attachments without GCFGFRE/GCFGFIE, actual context readback and unrelated-route isolation (software registers only)\n",fault_cases);
}
