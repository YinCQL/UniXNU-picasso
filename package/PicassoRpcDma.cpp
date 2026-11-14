// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "PicassoRpcDma.h"
#include "PicassoFastrpc.h"
#include "PicassoPcm.h"
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
void diagnose(int error,unsigned n) {
    const auto &p=memory.plan;const auto &d=p.diagnostic;
    IOLog("PICASSO_ADSP rpc-dma check error=%d pd=%u phase=%s reg=%x actual=%x expected=%x mask=%x\n",
        error,n?2:0,d.phase?d.phase:"none",d.offset,d.actual,d.expected,d.mask);
    if(n<2 && p.bank[n]<p.banks) {
        unsigned cb=(p.pages+p.bank[n])*4096;
        IOLog("PICASSO_ADSP rpc-dma context pd=%u bank=%u sctlr=%x actlr=%x inherited=%x/%x cbar=%x cba2r=%x fsr=%x far=%08x%08x syndrome=%x\n",
            n?2:0,p.bank[n],read(cb),read(cb+4),p.sctlr[n],p.actlr[n],read(0x1000+4*p.bank[n]),
            read(0x1800+4*p.bank[n]),read(cb+0x58),read(cb+0x64),read(cb+0x60),read(cb+0x68));
    }
    if(d.phase && (d.offset>=0x800 && d.offset<0xa00)) {
        unsigned slot=(d.offset-0x800)/4;uint32_t s2cr=read(0xc00+4*slot);
        IOLog("PICASSO_ADSP rpc-dma stream slot=%u smr=%x s2cr=%x\n",slot,read(d.offset),s2cr);
        unsigned bank=s2cr&255;
        if(((s2cr>>16)&3)==0 && bank<p.banks) {
            unsigned cb=(p.pages+bank)*4096;
            IOLog("PICASSO_ADSP rpc-dma owner bank=%u sctlr=%x actlr=%x cbar=%x cba2r=%x\n",
                bank,read(cb),read(cb+4),read(0x1000+4*bank),read(0x1800+4*bank));
        }
    }
}
int healthy(void *) {
    for(unsigned n=0;n<2;++n) {
        int error=picasso_rpc_dma_check(memory.plan,n,memory.physical[n],true,true,read);
        if(error) { diagnose(error,n);return error; }
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
        error=picasso_rpc_dma_program(memory.plan,n,memory.physical[n],read,write,write64);if(error) { diagnose(error,n);break; }
    }
    if(!error)error=healthy(nullptr);
    if(!error)error=picasso_rpc_bind(f,{nullptr,sync,healthy},memory.data[0],memory.data[1]);
    if(error)picasso_rpc_stop(f,error);
    const auto &p=memory.plan;
    if(p.error)diagnose(p.error,0);
    IOLog("PICASSO_ADSP rpc-dma survey streams=%u banks=%u pages=%u used=%u enabled=%u idle=%u nonzero_actlr=%u phase=%s\n",
        p.count,p.banks,p.pages,p.usedBanks,p.enabledBanks,p.idleBanks,p.nonzeroActlr,p.diagnostic.phase);
    if(!error)for(unsigned n=0;n<2;++n)
        IOLog("PICASSO_ADSP rpc-dma inherited pd=%u bank=%u sctlr=%x actlr=%x preserved=1\n",n?2:0,p.bank[n],p.sctlr[n],p.actlr[n]);
    IOLog("PICASSO_ADSP rpc-dma prepared=%u error=%d id=%x/%x/%x slots=%u/%u banks=%u/%u asids=%u/%u sid=1403/1404 iova=80000000 bytes=32768 retained=1\n",
        !error,error,p.id[0],p.id[1],p.id[2],p.slot[0],p.slot[1],p.bank[0],p.bank[1],p.asid[0],p.asid[1]);
    // One boot, bounded 88KiB. Retain allocations/mappings even on failures:
    // timeout or partial programming does not prove the DSP has released them.
}

namespace {
static RpcMemory pcmMemory;
uint32_t pcmRead(unsigned at) { return *reinterpret_cast<volatile uint32_t *>(pcmMemory.map->getVirtualAddress()+at); }
void pcmWrite(unsigned at,uint32_t v) { barrier();*reinterpret_cast<volatile uint32_t *>(pcmMemory.map->getVirtualAddress()+at)=v;barrier(); }
void pcmWrite64(unsigned at,uint64_t v) { barrier();*reinterpret_cast<volatile uint64_t *>(pcmMemory.map->getVirtualAddress()+at)=v;barrier(); }
int pcmCheck(PicassoPcm &o) {
    int e=picasso_rpc_dma_check(pcmMemory.plan,0,pcmMemory.physical[0],true,true,pcmRead);
    auto &p=pcmMemory.plan;unsigned cb=(p.pages+p.bank[0])*4096;
    o.fsr=pcmRead(cb+0x58);o.farLow=pcmRead(cb+0x60);o.farHigh=pcmRead(cb+0x64);o.syndrome=pcmRead(cb+0x68);
    if(e) {
        const auto &d=p.diagnostic;
        IOLog("PICASSO_ADSP pcm-dma check error=%d phase=%s reg=%x actual=%x expected=%x mask=%x\n",e,d.phase,d.offset,d.actual,d.expected,d.mask);
    }
    barrier();return e;
}
}
static int pcmSilence(PicassoPcm &o) {
    if(!o.prepared || o.address!=0x180000000ULL || o.bytes!=32768 || !pcmMemory.data[0])return -22;
    if(o.busyMask)return -16;
    int e=pcmCheck(o);if(e)return e;
    rpcZero(pcmMemory.data[0],4096);cache(pcmMemory.data[0],4096,true);return pcmCheck(o);
}
static int pcmTone(PicassoPcm &o,unsigned slot,unsigned sequence) {
    if(!o.prepared || o.address!=0x180000000ULL || o.bytes!=32768 || !pcmMemory.data[0] || slot>=2)return -22;
    unsigned bytes=picasso_pcm_chunk_bytes(sequence);if(!bytes)return -22;
    if(o.busyMask&(1U<<slot))return -16;
    int e=pcmCheck(o);if(e)return e;
    auto *data=pcmMemory.data[0]+slot*PicassoPcmSlotBytes;
    e=picasso_pcm_tone(data,bytes,sequence*PicassoPcmChunkFrames);if(e)return e;
    cache(data,bytes,true);return pcmCheck(o);
}
static int pcmCopy(PicassoPcm &o,unsigned slot,const uint8_t *source,unsigned bytes) {
    if(!o.prepared || o.address!=0x180000000ULL || o.bytes!=32768 || !pcmMemory.data[0] ||
        slot>=2 || !source || !bytes || bytes>PicassoPcmSlotBytes || (bytes&3))return -22;
    if(o.busyMask&(1U<<slot))return -16;
    int e=pcmCheck(o);if(e)return e;
    auto *out=pcmMemory.data[0]+slot*PicassoPcmSlotBytes;
    for(unsigned i=0;i<bytes;++i)out[i]=source[i];
    cache(out,bytes,true);return pcmCheck(o);
}
extern "C" void picasso_pcm_device_prepare(PicassoPcm &o) {
    if(pcmMemory.attempted) { o.dmaError=-16;return; }
    pcmMemory.attempted=true;int e=-12;
    IOLog("PICASSO_ADSP pcm-dma stage=map sid=1401 bytes=32768\n");
    auto *desc=IODeviceMemory::withRange(0x15000000,0x100000);
    if(desc) { pcmMemory.map=desc->map(kIOMapAnywhere|kIOMapInhibitCache);desc->release(); }
    if(!pcmMemory.map || !pcmMemory.map->getVirtualAddress()) { o.dmaError=e;return; }
    auto &p=pcmMemory.plan;p=picasso_rpc_dma_plan(pcmRead,0x1401,1);e=p.error;
    if(!e) {
        auto *a=IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
            static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous)|
            static_cast<IOOptionBits>(kIOMemoryMapperNone),PicassoRpcAllocation,4096);
        pcmMemory.allocation[0]=a;o.retained=a!=nullptr;
        if(!a)e=-12;
        else {
            IOByteCount bytes=0;pcmMemory.physical[0]=a->getPhysicalSegment(0,&bytes,kIOMemoryMapperNone);
            pcmMemory.data[0]=static_cast<uint8_t *>(a->getBytesNoCopy());
            if(!pcmMemory.data[0] || (uintptr_t(pcmMemory.data[0])&4095) || bytes<PicassoRpcAllocation)e=-22;
            else {
                rpcZero(pcmMemory.data[0],PicassoRpcAllocation);
                e=picasso_rpc_dma_tables(reinterpret_cast<uint64_t *>(pcmMemory.data[0]+32768),pcmMemory.physical[0]);
                if(!e)cache(pcmMemory.data[0],PicassoRpcAllocation,true);
            }
        }
    }
    if(!e)e=picasso_rpc_dma_program(p,0,pcmMemory.physical[0],pcmRead,pcmWrite,pcmWrite64);
    if(!e)e=pcmCheck(o);
    o.dmaError=e;o.prepared=!e;o.physical=pcmMemory.physical[0];o.sid=0x1401;
    o.slot=p.slot[0];o.bank=p.bank[0];o.asid=p.asid[0];
    if(!e) { o.address=0x180000000ULL;o.bytes=PicassoRpcDmaBytes;o.check=pcmCheck;o.silence=pcmSilence;o.tone=pcmTone;o.copy=pcmCopy; }
    const auto &d=p.diagnostic;
    IOLog("PICASSO_ADSP pcm-dma prepared=%u error=%d slot=%u bank=%u asid=%u sid=1401 address=%08x%08x bytes=%u retained=%u\n",
        o.prepared,e,o.slot,o.bank,o.asid,uint32_t(o.address>>32),uint32_t(o.address),o.bytes,o.retained);
    if(e)IOLog("PICASSO_ADSP pcm-dma prepare-error phase=%s reg=%x actual=%x expected=%x mask=%x\n",d.phase?d.phase:"allocation",d.offset,d.actual,d.expected,d.mask);
    // One boot, 44 KiB pinned. No reuse/free based on a timeout or BASIC ACK.
}
