// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso lito.dtsi: ADSP streams 0x1403/0x1404. ARM SMMUv2 native4K,
// isolated 32KiB per stream; no bypass, global reset, or inherited bank takeover.
#ifndef PICASSO_RPC_DMA_H
#define PICASSO_RPC_DMA_H
#include <stdint.h>
enum { PicassoRpcDmaBytes=32768, PicassoRpcAllocation=45056 };
struct PicassoRpcDmaDiagnostic {
    const char *phase;
    uint32_t offset,actual,expected,mask;
};
struct PicassoRpcDmaPlan {
    uint32_t id[3],pages,count,banks,slot[2],bank[2],asid[2],sctlr[2],actlr[2];
    uint32_t usedBanks,enabledBanks,idleBanks,nonzeroActlr,firstSid,streamCount;
    PicassoRpcDmaDiagnostic diagnostic;
    int error;
};
template<class Read> static bool rpcDmaCheck(PicassoRpcDmaPlan &p,Read read,
    const char *phase,unsigned offset,uint32_t expected,uint32_t mask=0xffffffffU) {
    uint32_t actual=read(offset);
    if((actual&mask)==(expected&mask))return true;
    p.diagnostic={phase,offset,actual,expected,mask};return false;
}
static inline void rpcDmaConflict(PicassoRpcDmaPlan &p,const char *phase,unsigned at,uint32_t value) {
    p.diagnostic={phase,at,value,0,0xffffffffU};
}
static inline bool rpcDmaMatches(uint32_t smr,unsigned sid) {
    return (smr>>31) && !((smr^sid)&0x7fff&~((smr>>16)&0x7fff));
}
// ASIDs are shared across banks, including AArch32 contexts using CONTEXTIDR.
// Reserve against routed disabled banks too, since their owner may enable them.
// Compare the low eight bits as well: an inherited AS=0 context has an 8-bit ASID.
template<class Read> static bool rpcDmaAsidBusy(const PicassoRpcDmaPlan &p,unsigned candidate,Read read) {
    for(unsigned i=(p.id[1]>>16)&255;i<p.banks;++i) {
        if(i==p.bank[0] || i==p.bank[1])continue;
        unsigned cb=(p.pages+i)*4096;
        if(((read(cb+0x24)>>16)&255)==(candidate&255) ||
            ((read(cb+0x2c)>>16)&255)==(candidate&255) ||
            (read(cb+0x34)&255)==(candidate&255))return true;
    }return false;
}
template<class Read> static PicassoRpcDmaPlan picasso_rpc_dma_plan(Read read,unsigned firstSid=0x1403,unsigned streams=2) {
    PicassoRpcDmaPlan p={};p.error=-95;p.diagnostic.phase="geometry";
    p.slot[0]=p.slot[1]=p.bank[0]=p.bank[1]=0xffffffffU;
    if(!((firstSid==0x1403 && streams==2) || (firstSid==0x1401 && streams==1)))return p;
    p.firstSid=firstSid;p.streamCount=streams;
    for(unsigned i=0;i<3;++i)p.id[i]=read(0x20+4*i);
    p.count=p.id[0]&255;p.banks=p.id[1]&255;p.pages=1U<<(((p.id[1]>>28)&7)+1);
    if(!(p.id[0]&(1U<<30)) || !(p.id[0]&(1U<<27)) || (p.id[0]&(1U<<8)) || !(p.id[2]&(1U<<12)) ||
        ((p.id[2]>>4)&15)<1 || ((p.id[2]>>4)&15)>5 || ((p.id[2]>>8)&15)>5 ||
        (p.id[1]>>31) || !p.count || p.count>128 || p.banks<3 || p.banks>128 ||
        p.pages<p.banks || (p.pages+p.banks)*4096>0x100000 || ((p.id[1]>>16)&255)>=p.banks)return p;
    if(!rpcDmaCheck(p,read,"client-disabled",0,0,1)) { p.error=-16;return p; }
    bool used[128]={};unsigned slots=0;
    for(unsigned i=0;i<p.count;++i) {
        uint32_t smr=read(0x800+4*i),s2cr=read(0xc00+4*i);
        if(!(smr>>31)) { if(slots<streams)p.slot[slots++]=i;continue; }
        if(rpcDmaMatches(smr,firstSid) || (streams==2 && rpcDmaMatches(smr,firstSid+1))) {
            rpcDmaConflict(p,"stream-conflict",0x800+4*i,smr);p.error=-16;return p;
        }
        if(((s2cr>>16)&3)==0 && (s2cr&255)<128)used[s2cr&255]=true;
    }
    if(slots!=streams) { p.diagnostic.phase="no-slots";p.error=-28;return p; }
    unsigned found=0,s2=(p.id[1]>>16)&255;
    for(unsigned i=p.banks;i>s2;) {
        --i;unsigned cb=(p.pages+i)*4096;
        uint32_t sctlr=read(cb),actlr=read(cb+4);
        p.usedBanks+=used[i];p.enabledBanks+=bool(sctlr&1);p.nonzeroActlr+=bool(actlr);
        // Linux arm_smmu_handoff_cbs reserves valid translated streams;
        // SCTLR.M is the enable bit. Neither fault-control bits nor ACTLR
        // indicate ownership. qcom,skip-init leaves those values inherited.
        if(used[i] || (sctlr&1))continue;
        ++p.idleBanks;
        if(found<streams) { p.bank[found]=i;p.sctlr[found]=sctlr;p.actlr[found++]=actlr; }
    }
    if(found!=streams) { p.diagnostic.phase="no-idle-banks";p.error=-16;return p; }
    // Preserve ACTLR: lito's ADSP SIDs have no qcom,actlr override, matching
    // qsmmuv500_init_cb's !has_actlr path. Revalidate before any write.
    found=0;
    for(unsigned candidate=0x188;candidate<0x200 && found<streams;++candidate)
        if(!rpcDmaAsidBusy(p,candidate,read))p.asid[found++]=candidate;
    p.error=found==streams?0:-16;p.diagnostic.phase=found==streams?"planned":"no-asids";return p;
}
// Dedicated contiguous pool: first eight pages are data, last three are tables.
// IOVA 0x80000000 is L1[2]/L2[0]/L3[0..7]; nothing else, including the tables,
// is mapped for the DSP. NC, outer-shareable, RW, PXN/UXN, non-global
// data leaves: both PDs share an IOVA, so TLB entries must be ASID-tagged.
static inline int picasso_rpc_dma_tables(uint64_t *tables,uint64_t physical) {
    if(!tables || (uintptr_t(tables)&7) || !physical || (physical&4095) ||
        physical>=(1ULL<<36) || PicassoRpcAllocation>(1ULL<<36)-physical)return -22;
    for(unsigned i=0;i<1536;++i)tables[i]=0;
    tables[2]=(physical+36864)|3;tables[512]=(physical+40960)|3;
    for(unsigned i=0;i<8;++i)tables[1024+i]=(physical+i*4096)|3|(1U<<6)|(2U<<8)|(1U<<10)|(1U<<11)|(3ULL<<53);
    return 0;
}
// Verify the exact configuration before publishing a stream and during use.
// ACTLR is inherited, never guessed or reset; firmware changes are fatal.
template<class Read> static int picasso_rpc_dma_check(PicassoRpcDmaPlan &p,unsigned n,uint64_t physical,
    bool enabled,bool routed,Read read) {
    if(p.error || !p.streamCount || n>=p.streamCount)return -22;
    unsigned bank=p.bank[n],slot=p.slot[n],cb=(p.pages+bank)*4096;
    uint64_t ttbr=(uint64_t(p.asid[n])<<48)|(physical+32768);
    if(!rpcDmaCheck(p,read,"client-disabled",0,0,1))return -16;
    if(!rpcDmaCheck(p,read,"context-fault",cb+0x58,0,0xc00001feU))return -5;
    if(!rpcDmaCheck(p,read,"actlr-drift",cb+4,p.actlr[n]) ||
        !rpcDmaCheck(p,read,"cba2r",0x1800+4*bank,1) ||
        !rpcDmaCheck(p,read,"cbar",0x1000+4*bank,0x1f300) ||
        !rpcDmaCheck(p,read,"tcr2",cb+0x10,0x38011,~0x60U) ||
        !rpcDmaCheck(p,read,"tcr",cb+0x30,0x80802020) ||
        !rpcDmaCheck(p,read,"ttbr0-low",cb+0x20,uint32_t(ttbr)) ||
        !rpcDmaCheck(p,read,"ttbr0-high",cb+0x24,uint32_t(ttbr>>32)) ||
        !rpcDmaCheck(p,read,"ttbr1-low",cb+0x28,0) ||
        !rpcDmaCheck(p,read,"ttbr1-high",cb+0x2c,p.asid[n]<<16) ||
        !rpcDmaCheck(p,read,"mair0",cb+0x38,0xff44) ||
        !rpcDmaCheck(p,read,"mair1",cb+0x3c,0) ||
        !rpcDmaCheck(p,read,"sctlr",cb,enabled?0x127:0,~0x1000U))return -5;
    if(routed && (!rpcDmaCheck(p,read,"s2cr",0xc00+4*slot,bank) ||
        !rpcDmaCheck(p,read,"smr",0x800+4*slot,0x80000000U+p.firstSid+n)))return -16;
    return 0;
}
template<class Read,class Write,class Write64> static int picasso_rpc_dma_program(
    PicassoRpcDmaPlan &p,unsigned n,uint64_t physical,Read read,Write write,Write64 write64) {
    if(p.error || !p.streamCount || n>=p.streamCount || !physical || (physical&4095) || physical>=(1ULL<<36) ||
        PicassoRpcAllocation>(1ULL<<36)-physical)return -22;
    unsigned bank=p.bank[n],slot=p.slot[n],cb=(p.pages+bank)*4096;
    // Revalidate the lease immediately before the first write. A bank used
    // even by a disabled/bypass stream belongs to that stream's owner.
    if(!rpcDmaCheck(p,read,"client-disabled",0,0,1) ||
        !rpcDmaCheck(p,read,"slot-raced",0x800+4*slot,0,0x80000000U) ||
        !rpcDmaCheck(p,read,"sctlr-raced",cb,p.sctlr[n]) ||
        !rpcDmaCheck(p,read,"actlr-raced",cb+4,p.actlr[n]))return -16;
    for(unsigned i=0;i<p.count;++i) {
        auto smr=read(0x800+4*i),s2cr=read(0xc00+4*i);if(!(smr>>31))continue;
        if(rpcDmaMatches(smr,p.firstSid+n)) {
            rpcDmaConflict(p,"stream-raced",0x800+4*i,smr);return -16;
        }
        if(((s2cr>>16)&3)==0 && (s2cr&255)==bank) {
            rpcDmaConflict(p,"bank-raced",0xc00+4*i,s2cr);return -16;
        }
    }
    if(rpcDmaAsidBusy(p,p.asid[n],read)) { p.diagnostic.phase="asid-raced";return -16; }
    // Only this unrouted, translation-disabled bank is normalized. Check
    // accessibility here before installing page-table addresses or routing.
    write(cb,0);
    if(!rpcDmaCheck(p,read,"disable-readback",cb,0))return -5;
    write(0x1800+4*bank,1);write(0x1000+4*bank,0x1f300);
    write(cb+0x10,0x38011);write(cb+0x30,0x80802020); // 32-bit TTBR0, TTBR1 disabled, NC walks
    uint64_t ttbr=(uint64_t(p.asid[n])<<48)|(physical+32768);
    write64(cb+0x20,ttbr);write64(cb+0x28,uint64_t(p.asid[n])<<48);
    write(cb+0x38,0xff44);write(cb+0x3c,0);write(cb+0x58,0xffffffffU);
    write(cb+0x610,p.asid[n]);write(cb+0x7f0,0xffffffffU);
    unsigned polls=0;while(read(cb+0x7f4)&1)if(++polls==1000) {
        rpcDmaConflict(p,"tlb-timeout",cb+0x7f4,read(cb+0x7f4));return -60;
    }
    int error=picasso_rpc_dma_check(p,n,physical,false,false,read);if(error)return error;
    // Fault reporting without CFIE: this owner polls FSR before every RPC.
    // Non-stalling faults terminate; never leave an unhandled level IRQ set.
    write(cb,0x1127);
    error=picasso_rpc_dma_check(p,n,physical,true,false,read);if(error)return error;
    write(0xc00+4*slot,bank);
    if(!rpcDmaCheck(p,read,"s2cr-readback",0xc00+4*slot,bank))return -5;
    write(0x800+4*slot,0x80000000U+p.firstSid+n);
    return picasso_rpc_dma_check(p,n,physical,true,true,read);
}
struct PicassoFastrpc;
extern "C" void picasso_rpc_device_prepare(PicassoFastrpc *);
#endif
