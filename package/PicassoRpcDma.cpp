// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "PicassoRpcDma.h"
#include "PicassoFastrpc.h"
namespace {
struct RpcMemory {
    IOMemoryMap *map;
    IOBufferMemoryDescriptor *allocation[2];
    uint8_t *data[2];
    uint64_t physical[2];
    PicassoRpcDmaPlan plan;
    bool attempted;
};
static RpcMemory memory;
void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
uint32_t read(unsigned at) { return *reinterpret_cast<volatile uint32_t *>(memory.map->getVirtualAddress()+at); }
void write(unsigned at,uint32_t v) { barrier();*reinterpret_cast<volatile uint32_t *>(memory.map->getVirtualAddress()+at)=v;barrier(); }
void write64(unsigned at,uint64_t v) { barrier();*reinterpret_cast<volatile uint64_t *>(memory.map->getVirtualAddress()+at)=v;barrier(); }
void cache(uint8_t *p,unsigned bytes,bool publish) {
    barrier();for(uintptr_t a=uintptr_t(p);a<uintptr_t(p)+bytes;a+=64) {
        if(publish)__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }barrier();
}
int healthy(void *) {
    if(read(0)&1)return -16;
    for(unsigned n=0;n<2;++n) {
        const auto &p=memory.plan;unsigned cb=(p.pages+p.bank[n])*4096;
        uint32_t fsr=read(cb+0x58);
        if(fsr&0xc00001feU) {
            IOLog("PICASSO_ADSP rpc-dma fault pd=%u fsr=%x far=%08x%08x syndrome=%x\n",
                n?2:0,fsr,read(cb+0x64),read(cb+0x60),read(cb+0x68));return -5;
        }
        uint64_t ttbr=(uint64_t(p.asid[n])<<48)|(memory.physical[n]+32768);
        if(read(0x800+4*p.slot[n])!=0x80001403U+n || read(0xc00+4*p.slot[n])!=p.bank[n] ||
            read(0x1800+4*p.bank[n])!=1 || read(0x1000+4*p.bank[n])!=0x1f300 ||
            (read(cb+0x10)&~0x60U)!=0x38011 || read(cb+0x30)!=0x80802020 || read(cb+4) ||
            (read(cb)&~0x1000U)!=0x127 || read(cb+0x20)!=uint32_t(ttbr) || read(cb+0x24)!=uint32_t(ttbr>>32) ||
            read(cb+0x28) || read(cb+0x2c)!=(p.asid[n]<<16) || read(cb+0x38)!=0xff44 || read(cb+0x3c))return -16;
    }barrier();return 0;
}
int sync(void *,unsigned n,unsigned slot,bool publish) {
    if(n>1 || slot>1)return -22;
    int error=healthy(nullptr);if(error)return error;
    cache(memory.data[n]+slot*PicassoRpcSlotBytes,PicassoRpcSlotBytes,publish);return 0;
}
}
extern "C" void picasso_rpc_device_prepare(PicassoFastrpc *f) {
    if(memory.attempted || !f || f->bound || f->stopped)return;
    memory.attempted=true;int error=-12;
    auto *desc=IODeviceMemory::withRange(0x15000000,0x100000);
    if(desc) { memory.map=desc->map(kIOMapAnywhere|kIOMapInhibitCache);desc->release(); }
    if(!memory.map || !memory.map->getVirtualAddress()) { picasso_rpc_stop(f,error);return; }
    memory.plan=picasso_rpc_dma_plan(read);error=memory.plan.error;
    if(!error)for(unsigned n=0;n<2;++n) {
        auto *a=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous)|
            static_cast<IOOptionBits>(kIOMemoryMapperNone),PicassoRpcAllocation,4096);
        memory.allocation[n]=a;if(!a) { error=-12;break; }
        IOByteCount bytes=0;memory.physical[n]=a->getPhysicalSegment(0,&bytes,kIOMemoryMapperNone);
        memory.data[n]=static_cast<uint8_t *>(a->getBytesNoCopy());
        if(!memory.data[n] || (uintptr_t(memory.data[n])&4095) || bytes<PicassoRpcAllocation) { error=-22;break; }
        rpcZero(memory.data[n],PicassoRpcAllocation);
        error=picasso_rpc_dma_tables(reinterpret_cast<uint64_t *>(memory.data[n]+32768),memory.physical[n]);
        if(error)break;cache(memory.data[n],PicassoRpcAllocation,true);
    }
    if(!error)for(unsigned n=0;n<2;++n) {
        error=picasso_rpc_dma_program(memory.plan,n,memory.physical[n],read,write,write64);if(error)break;
    }
    if(!error)error=healthy(nullptr);
    if(!error)error=picasso_rpc_bind(f,{nullptr,sync,healthy},memory.data[0],memory.data[1]);
    if(error)picasso_rpc_stop(f,error);
    const auto &p=memory.plan;
    IOLog("PICASSO_ADSP rpc-dma prepared=%u error=%d id=%x/%x/%x slots=%u/%u banks=%u/%u asids=%u/%u sid=1403/1404 iova=80000000 bytes=32768 retained=1\n",
        !error,error,p.id[0],p.id[1],p.id[2],p.slot[0],p.slot[1],p.bank[0],p.bank[1],p.asid[0],p.asid[1]);
    // One boot, bounded 88KiB. Retain allocations/mappings even on failures:
    // timeout or partial programming does not prove the DSP has released them.
}
