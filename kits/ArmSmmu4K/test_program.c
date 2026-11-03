/* SPDX-License-Identifier: MIT */
/* Software I/O fixture only. No device, hardware mapping or GPU is opened. */
#include "ArmSmmu4KMmio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
#define BYTES 0x40000
#ifdef SOURCE_ORACLE
extern uint32_t source_register(unsigned);
static void source_definitions(void)
{
    const uint32_t expected[]={0x20,0x24,0x28,0,0x10,0x20,0x28,0x30,0x38,0x3c,
        0x58,0x610,0x7f0,0x7f4,0xc00001fe,0x800,UINT32_MAX};
    for(unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);++i)CHECK(source_register(i)==expected[i]);
}
#endif
typedef struct {unsigned kind;size_t at;uint64_t value;} Event;
typedef struct {
    uint64_t memory[BYTES/8];Event events[512];size_t event_count,calls,fail_call;
    size_t cb,gr1;unsigned bank,enabled_seen,timeout,corrupt_tcr,corrupt_enable;
    unsigned fault_on_enable,fail_disable,fail_after_write,sheng_bits;
} Fixture;
static Fixture f;
static ArmSmmu4KConfig lower={48,48,0x10000000,16},upper={48,48,0x11000000,16};
static ArmSmmu4KResult lr,ur;static uint64_t lower_tables[8*512],upper_tables[8*512];
static ArmSmmu4KContextConfig config;
static uint32_t get32(size_t at){uint32_t v;memcpy(&v,(unsigned char *)f.memory+at,4);return v;}
static uint64_t get64(size_t at){uint64_t v;memcpy(&v,(unsigned char *)f.memory+at,8);return v;}
static void put32(size_t at,uint32_t v){memcpy((unsigned char *)f.memory+at,&v,4);}
static void put64(size_t at,uint64_t v){memcpy((unsigned char *)f.memory+at,&v,8);}
static int event(unsigned kind,size_t at,uint64_t value)
{
    CHECK(f.event_count<512 && at<BYTES);f.events[f.event_count++]=(Event){kind,at,value};
    return ++f.calls==f.fail_call;
}
static int read32(void *cookie,size_t at,uint32_t *out)
{
    CHECK(cookie==&f && !(at&3) && at<=BYTES-4);
    if(event(0,at,0))return 1;
    *out=(at==f.cb+0x7f4 && f.timeout)?1:get32(at);return 0;
}
static int read64(void *cookie,size_t at,uint64_t *out)
{
    CHECK(cookie==&f && !(at&7) && at<=BYTES-8);
    if(event(1,at,0))return 1;*out=get64(at);return 0;
}
static int write32(void *cookie,size_t at,uint32_t value)
{
    CHECK(cookie==&f && !(at&3) && at<=BYTES-4);int fail=event(2,at,value);
    if((fail && !f.fail_after_write) || (at==f.cb && value==0 && f.enabled_seen && f.fail_disable))return 1;
    if(at==f.cb+0x58)put32(at,get32(at)&~value); /* actual W1C semantics */
    else if(at==f.cb+0x30 && f.corrupt_tcr)put32(at,value^(1u<<14));
    else if(at==f.cb+0x10 && f.sheng_bits)put32(at,value|0x60);   /* int-008: TCR2 TBI0/TBI1 read 1 */
    else if(at==f.cb && value&1) {
        if(f.sheng_bits)value&=~0x1000u;   /* int-009: SCTLR.ASIDPNE reads 0 */
        f.enabled_seen=1;put32(at,f.corrupt_enable?value^1:value);
        if(f.fault_on_enable)put32(f.cb+0x58,2);
    } else put32(at,value);
    return fail;
}
static int write64(void *cookie,size_t at,uint64_t value)
{
    CHECK(cookie==&f && !(at&7) && at<=BYTES-8);int fail=event(3,at,value);
    if(fail && !f.fail_after_write)return 1;put64(at,value);return fail;
}
static int barrier(void *cookie){CHECK(cookie==&f);return event(4,0,0);}
static ArmSmmu4KIo reset(unsigned register_64k)
{
    memset(&f,0,sizeof(f));f.bank=register_64k?1:2;f.gr1=register_64k?65536:4096;
    size_t pages=register_64k?2:32;f.cb=(pages+f.bank)*f.gr1;
    put32(0x20,1u<<30);put32(0x24,register_64k?0x80000002:0x40020010);put32(0x28,0x1555);
    /* Deliberately wrong supplied IDs: only the real I/O reads may authorize. */
    config=(ArmSmmu4KContextConfig){&lower,&upper,&lr,&ur,0,0,0xabcd,1,1,1,0};
    ArmSmmu4KIo io={&f,BYTES,read32,read64,write32,write64,barrier};return io;
}
static size_t first(unsigned kind,size_t at)
{for(size_t i=0;i<f.event_count;++i)if(f.events[i].kind==kind && f.events[i].at==at)return i;exit(1);}
static void no_writes(void)
{for(size_t i=0;i<f.event_count;++i)CHECK(f.events[i].kind!=2 && f.events[i].kind!=3);}
static void never_enabled(void)
{for(size_t i=0;i<f.event_count;++i)CHECK(!(f.events[i].kind==2 && f.events[i].at==f.cb && (f.events[i].value&1)));}
static void direct_adapter(void)
{
    /* Explicit host-memory unit buffer, never described as an MMIO device. */
    uint64_t storage[8]={0};ArmSmmu4KMmio mapping={(volatile unsigned char *)storage,sizeof(storage)};ArmSmmu4KIo io;
    CHECK(ArmSmmu4KBindMmio(&mapping,&io)==ArmSmmu4KSuccess);
    uint32_t a=0;uint64_t b=0;
    CHECK(!io.write32(io.cookie,0,0xaabbccdd) && !io.read32(io.cookie,0,&a) && a==0xaabbccdd);
    CHECK(!io.write64(io.cookie,8,UINT64_C(0x123456789abcdef0)) && !io.read64(io.cookie,8,&b) && b==UINT64_C(0x123456789abcdef0));
    CHECK(!io.barrier(io.cookie));
    CHECK(io.read32(io.cookie,1,&a) && io.read64(io.cookie,4,&b));
    CHECK(io.write32(io.cookie,sizeof(storage),1) && io.write64(io.cookie,sizeof(storage)-4,1));
    CHECK(io.read32(io.cookie,SIZE_MAX,&a) && io.read64(io.cookie,SIZE_MAX-7,&b));
    CHECK(io.read32(io.cookie,0,NULL) && io.read64(io.cookie,8,NULL));
    mapping.mapping++;CHECK(ArmSmmu4KBindMmio(&mapping,&io)==ArmSmmu4KArgument);
    CHECK(!io.read32 && !io.write64);
    mapping.mapping=(volatile unsigned char *)(UINTPTR_MAX-7);mapping.bytes=64;
    CHECK(ArmSmmu4KBindMmio(&mapping,&io)==ArmSmmu4KArgument);
}
int main(void)
{
#ifdef SOURCE_ORACLE
    source_definitions();
#endif
    ArmSmmu4KRange a={0x1000,0x20000000,4096,3,1,3},b=a;b.iova=UINT64_MAX-4095;
    CHECK(ArmSmmu4KBuild(&lower,&a,1,lower_tables,8,&lr)==ArmSmmu4KSuccess);
    CHECK(ArmSmmu4KBuildUpper(&upper,&b,1,upper_tables,8,&ur)==ArmSmmu4KSuccess);
    ArmSmmu4KProgramResult r;size_t successful_calls=0;
    for(unsigned spacing=0;spacing<2;++spacing) {
        ArmSmmu4KIo io=reset(spacing);
        CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KSuccess);
        CHECK(r.phase==5 && r.enabled && r.readback_valid && r.tlb_synchronized && r.must_retain_tables && !r.disabled_verified);
        CHECK(r.id0==(1u<<30) && r.id2==0x1555 && r.context_offset==f.cb);
        CHECK((r.readback.tcr>>14&3)==0 && (r.readback.tcr>>30&3)==2);
        CHECK(first(2,f.gr1+0x800+f.bank*4)<first(2,f.gr1+f.bank*4));
        CHECK(first(2,f.cb+0x10)<first(2,f.cb+0x30) && first(2,f.cb+0x30)<first(3,f.cb+0x20));
        CHECK(first(3,f.cb+0x20)<first(3,f.cb+0x28) && first(2,f.cb+0x610)<first(2,f.cb+0x7f0));
        for(size_t i=0;i<f.event_count;++i)if(f.events[i].kind==2 || f.events[i].kind==3) {
            size_t at=f.events[i].at;
            CHECK((at>=f.cb && at<f.cb+0x800) || at==f.gr1+f.bank*4 || at==f.gr1+0x800+f.bank*4);
            if(at==f.cb && f.events[i].value&1)CHECK(i>first(2,f.cb+0x7f0));
        }
        successful_calls=f.calls;
        /* Synchronize uses TLBIASID with the ASID; InvalidateAll uses TLBIALL; both sync and change nothing else. */
        f.event_count=f.calls=0;
        CHECK(ArmSmmu4KSynchronizeOwnedContext(&io,f.bank,&config,4,&r)==ArmSmmu4KSuccess && r.tlb_synchronized);
        CHECK(f.events[first(2,f.cb+0x610)].value==0xabcd && first(2,f.cb+0x610)<first(2,f.cb+0x7f0));
        for(size_t i=0;i<f.event_count;++i)if(f.events[i].kind==2)CHECK(f.events[i].at==f.cb+0x610 || f.events[i].at==f.cb+0x7f0);
        f.event_count=f.calls=0;
        CHECK(ArmSmmu4KInvalidateAllOwnedContext(&io,f.bank,&config,4,&r)==ArmSmmu4KSuccess && r.tlb_synchronized && get32(f.cb)&1);
        CHECK(first(2,f.cb+0x618)<first(2,f.cb+0x7f0));
        for(size_t i=0;i<f.event_count;++i)if(f.events[i].kind==2)CHECK(f.events[i].at==f.cb+0x618 || f.events[i].at==f.cb+0x7f0);
        ArmSmmu4KContextConfig other=config;other.asid++;f.event_count=f.calls=0;
        CHECK(ArmSmmu4KInvalidateAllOwnedContext(&io,f.bank,&other,4,&r)!=ArmSmmu4KSuccess);no_writes();
        /* A different ASID/root must never disable an unrelated context. */
        ArmSmmu4KContextConfig wrong=config;wrong.asid++;
        f.event_count=f.calls=0;
        CHECK(ArmSmmu4KDisableOwnedContext(&io,f.bank,&wrong,4,&r)==ArmSmmu4KStateMismatch);no_writes();CHECK(r.must_retain_tables);
        CHECK(ArmSmmu4KDisableOwnedContext(&io,f.bank,&config,4,&r)==ArmSmmu4KSuccess);
        CHECK(r.phase==7 && !r.enabled && r.disabled_verified && r.tlb_synchronized && !r.must_retain_tables && !get32(f.cb));
        CHECK(ArmSmmu4KDisableOwnedContext(&io,f.bank,&config,4,&r)==ArmSmmu4KSuccess);
    }
    /* The sheng Adreno SMMU's read-only bits: program, observe and disable all succeed. */
    {
        ArmSmmu4KIo io=reset(0);f.sheng_bits=1;
        CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KSuccess && r.enabled);
        CHECK(get32(f.cb+0x10)&0x60 && !(get32(f.cb)&0x1000));
        CHECK(ArmSmmu4KObserveOwnedContext(&io,f.bank,&config,&r)==ArmSmmu4KSuccess);
        CHECK(ArmSmmu4KDisableOwnedContext(&io,f.bank,&config,4,&r)==ArmSmmu4KSuccess && !get32(f.cb));
        f.sheng_bits=0;
    }
    size_t faults=0;
    for(unsigned after=0;after<2;++after)for(size_t call=1;call<=successful_calls;++call) {
        ArmSmmu4KIo io=reset(0);f.fail_call=call;f.fail_after_write=after;
        CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KIoError);
        if(r.writes)CHECK(!get32(f.cb) && r.disabled_verified && r.tlb_synchronized && !r.must_retain_tables);
        else {no_writes();CHECK(r.must_retain_tables);}++faults;
    }
    ArmSmmu4KIo io=reset(0);put32(f.cb,1);
    CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KBusy);no_writes();CHECK(r.enabled && r.must_retain_tables);
    io=reset(0);put32(0x28,0x555);config.id0=1u<<30;config.id2=0x1555;
    CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KUnsupported);no_writes();
    io=reset(0);CHECK(ArmSmmu4KProgramDisabledContext(&io,1,&config,4,&r)==ArmSmmu4KUnsupported);no_writes();
    io=reset(0);io.register_bytes=BYTES-1;
    CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KUnsupported);no_writes();
    io=reset(0);CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,0,&r)==ArmSmmu4KArgument);CHECK(!f.calls);
    io=reset(0);f.corrupt_tcr=1;
    CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KStateMismatch);never_enabled();CHECK(r.disabled_verified && !r.must_retain_tables);
    io=reset(0);f.corrupt_enable=1;
    CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KStateMismatch);CHECK(r.disabled_verified && !r.must_retain_tables);
    io=reset(0);f.timeout=1;
    CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KTimeout);never_enabled();
    CHECK(r.poll_reads==8 && r.disabled_verified && !r.tlb_synchronized && r.must_retain_tables);
    io=reset(0);f.fault_on_enable=1;
    CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KFault);
    CHECK(r.fault_status==2 && r.disabled_verified && !r.must_retain_tables && !r.enabled);
    io=reset(0);f.fault_on_enable=1;f.fail_disable=1;
    CHECK(ArmSmmu4KProgramDisabledContext(&io,f.bank,&config,4,&r)==ArmSmmu4KFault);
    CHECK(r.enabled && !r.disabled_verified && r.must_retain_tables && get32(f.cb)&1);
    direct_adapter();
    printf("PASS: real programming/disable code on software I/O fixture, TCR-before-TTBR/enable ordering, %zu I/O failure controls, bounded timeout/quarantine, fault/refusal, sheng read-only TBI/ASIDPNE bits and direct AArch64 adapter memory checks\n",faults);
    puts("No hardware MMIO, active GPU translation or GPU execution observed");
}
