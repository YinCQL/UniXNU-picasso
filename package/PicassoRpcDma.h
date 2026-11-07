// SPDX-License-Identifier: BSD-2-Clause-Patent
// Picasso lito.dtsi: ADSP streams 0x1403/0x1404. ARM SMMUv2 native4K,
// isolated 32KiB per stream; no bypass, global reset, or inherited bank takeover.
#ifndef PICASSO_RPC_DMA_H
#define PICASSO_RPC_DMA_H
#include <stdint.h>
enum { PicassoRpcDmaBytes=32768, PicassoRpcAllocation=45056 };
struct PicassoRpcDmaPlan {
    uint32_t id[3],pages,count,banks,slot[2],bank[2],asid[2];
    int error;
};
template<class Read> static PicassoRpcDmaPlan picasso_rpc_dma_plan(Read read) {
    PicassoRpcDmaPlan p={};p.error=-95;
    for(unsigned i=0;i<3;++i)p.id[i]=read(0x20+4*i);
    p.count=p.id[0]&255;p.banks=p.id[1]&255;p.pages=1U<<(((p.id[1]>>28)&7)+1);
    if(!(p.id[0]&(1U<<30)) || !(p.id[0]&(1U<<27)) || (p.id[0]&(1U<<8)) || !(p.id[2]&(1U<<12)) ||
        ((p.id[2]>>4)&15)<1 || ((p.id[2]>>4)&15)>5 || ((p.id[2]>>8)&15)>5 ||
        (p.id[1]>>31) || !p.count || p.count>128 || p.banks<3 || p.banks>128 ||
        p.pages<p.banks || (p.pages+p.banks)*4096>0x100000)return p;
    if(read(0)&1) { p.error=-16;return p; } // CLIENTPD disables all translation
    bool used[128]={};unsigned slots=0;
    for(unsigned i=0;i<p.count;++i) {
        uint32_t smr=read(0x800+4*i),s2cr=read(0xc00+4*i);
        if(!(smr>>31)) { if(slots<2)p.slot[slots++]=i;continue; }
        unsigned mask=(smr>>16)&0x7fff;
        if(!((smr^0x1403)&0x7fff&~mask) || !((smr^0x1404)&0x7fff&~mask)) { p.error=-16;return p; }
        if(((s2cr>>16)&3)==0 && (s2cr&255)<128)used[s2cr&255]=true;
    }
    if(slots!=2) { p.error=-28;return p; }
    unsigned found=0,s2=(p.id[1]>>16)&255;
    for(unsigned i=p.banks;i>s2 && found<2;) {
        --i;unsigned cb=(p.pages+i)*4096;
        if(!used[i] && !read(cb) && !read(cb+4))p.bank[found++]=i;
    }
    if(found!=2) { p.error=-16;return p; }
    // ASIDs are shared by banks. Inspect every active bank before reserving two.
    found=0;
    for(unsigned candidate=0x188;candidate<0x200 && found<2;++candidate) {
        bool busy=false;
        for(unsigned i=s2;i<p.banks;++i) { unsigned cb=(p.pages+i)*4096;
            if(read(cb)&1)busy|=((read(cb+0x24)>>16)==candidate || (read(cb+0x2c)>>16)==candidate); }
        if(!busy)p.asid[found++]=candidate;
    }
    p.error=found==2?0:-16;return p;
}
// Dedicated contiguous pool: first eight pages are data, last three are tables.
// IOVA 0x80000000 is L1[2]/L2[0]/L3[0..7]; nothing else, including the tables,
// is mapped for the DSP. NC, outer-shareable, RW, PXN/UXN data leaves.
static inline int picasso_rpc_dma_tables(uint64_t *tables,uint64_t physical) {
    if(!tables || (uintptr_t(tables)&7) || !physical || (physical&4095) ||
        physical>=(1ULL<<36) || PicassoRpcAllocation>(1ULL<<36)-physical)return -22;
    for(unsigned i=0;i<1536;++i)tables[i]=0;
    tables[2]=(physical+36864)|3;tables[512]=(physical+40960)|3;
    for(unsigned i=0;i<8;++i)tables[1024+i]=(physical+i*4096)|3|(1U<<6)|(2U<<8)|(1U<<10)|(3ULL<<53);
    return 0;
}
template<class Read,class Write,class Write64> static int picasso_rpc_dma_program(
    const PicassoRpcDmaPlan &p,unsigned n,uint64_t physical,Read read,Write write,Write64 write64) {
    if(p.error || n>1 || !physical || (physical&4095) || physical>=(1ULL<<36) ||
        PicassoRpcAllocation>(1ULL<<36)-physical)return -22;
    unsigned bank=p.bank[n],slot=p.slot[n],cb=(p.pages+bank)*4096;
    // Revalidate the lease immediately before the first write.
    if(read(0x800+4*slot)&0x80000000U || read(cb) || read(cb+4))return -16;
    for(unsigned i=0;i<p.count;++i) {
        auto smr=read(0x800+4*i),s2cr=read(0xc00+4*i);if(!(smr>>31))continue;
        unsigned mask=(smr>>16)&0x7fff;
        if(!((smr^(0x1403+n))&0x7fff&~mask) || (((s2cr>>16)&3)==0 && (s2cr&255)==bank))return -16;
    }
    write(0x1800+4*bank,1);write(0x1000+4*bank,0x1f300);
    write(cb+0x10,0x38011);write(cb+0x30,0x80802020); // 32-bit TTBR0, TTBR1 disabled, NC walks
    uint64_t ttbr=(uint64_t(p.asid[n])<<48)|(physical+32768);
    write64(cb+0x20,ttbr);write64(cb+0x28,uint64_t(p.asid[n])<<48);
    write(cb+0x38,0xff44);write(cb+0x3c,0);write(cb+0x58,0xffffffffU);
    write(cb+0x610,p.asid[n]);write(cb+0x7f0,0xffffffffU);
    unsigned polls=0;while(read(cb+0x7f4)&1)if(++polls==1000)return -60;
    if(read(0x1800+4*bank)!=1 || read(0x1000+4*bank)!=0x1f300 ||
        (read(cb+0x10)&~0x60U)!=0x38011 || read(cb+0x30)!=0x80802020 ||
        read(cb+0x20)!=uint32_t(ttbr) || read(cb+0x24)!=uint32_t(ttbr>>32) ||
        read(cb+0x28) || read(cb+0x2c)!=(p.asid[n]<<16) ||
        read(cb+0x38)!=0xff44 || read(cb+0x3c) || read(cb))return -5;
    // Fault reporting without CFIE: this owner polls FSR before every RPC.
    // Non-stalling faults terminate; never leave an unhandled level IRQ set.
    write(cb,0x1127);
    if((read(cb)&~0x1000U)!=0x127 || (read(cb+0x58)&0xc00001feU))return -5;
    write(0xc00+4*slot,bank);
    if(read(0xc00+4*slot)!=bank)return -5;
    write(0x800+4*slot,0x80001403U+n);
    return read(0x800+4*slot)==0x80001403U+n?0:-5;
}
struct PicassoFastrpc;
extern "C" void picasso_rpc_device_prepare(PicassoFastrpc *);
#endif
