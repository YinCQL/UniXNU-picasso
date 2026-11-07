// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "PicassoFastrpc.h"
#include "PicassoRpcDma.h"
alignas(4096) static uint8_t ram[2][PicassoRpcDomainBytes];
static PicassoFastrpc f;
static unsigned published,acquired;
static int healthError,syncError;
static int sync(void *,unsigned n,unsigned s,bool pub) { assert(n<2 && s<2);if(pub)++published;else ++acquired;return syncError; }
static int healthy(void *) { return healthError; }
static void reset() {
    f={};memset(ram,0,sizeof ram);healthError=syncError=0;
    assert(picasso_rpc_bind(&f,{nullptr,sync,healthy},ram[0],ram[1])==0);
}
static void reply(unsigned n,unsigned slot,uint32_t ret=0,unsigned flags=0,bool strip=false) {
    auto &c=f.domain[n].call[slot];uint8_t wire[24]={};
    rpcPut64(wire,strip?c.ctx&~3ULL:c.ctx);rpcPut32(wire+8,ret);rpcPut32(wire+12,flags);rpcPut32(wire+20,2);
    picasso_rpc_receive(&f,wire,24);
}
static void sent(unsigned n,unsigned s) { assert(f.domain[n].call[s].queued);picasso_rpc_sent(&f,n*2+s); }
static uint8_t *output(unsigned n,unsigned slot,unsigned arg) { return ram[n]+slot*PicassoRpcSlotBytes+f.domain[n].call[slot].offset[arg]; }
static void initialization() {
    reset();picasso_rpc_tick(&f);uint32_t wire[10];
    assert(picasso_rpc_packet(&f,wire)==0 && wire[0]==0 && wire[4]==1 && wire[5]==0x10000);
    assert(wire[6]==0x80000000 && wire[7]==3 && wire[8]==16384);
    assert(rpc64(ram[0])==0x10000200 && rpc64(ram[0]+8)==4);
    assert(rpc32(ram[0]+16)==1 && rpc32(ram[0]+20)==0);
    assert(rpc64(ram[0]+24)==0x380000000ULL && rpc64(ram[0]+32)==4096);
    assert(rpc32(ram[0]+512)==0x188);
    for(unsigned n=0;n<2;++n) {
        sent(n,0);reply(n,0,0,0,true);picasso_rpc_tick(&f);
        assert(f.domain[n].stage==2 && f.domain[n].call[0].sc==0x3000000);
        sent(n,0);reply(n,0);picasso_rpc_tick(&f);
        assert(f.domain[n].initialized && f.domain[n].stage==3 && f.domain[n].call[1].queued);
        assert(f.domain[n].call[1].sc==0x04020200);
        assert(rpc32(output(n,1,0)+4)==0xffffffffU && rpc32(output(n,1,0)+12)==4096);
        sent(n,1); // blocks independently of control open/register
        assert(f.domain[n].call[0].sc==0x20200);
        assert(!strcmp(reinterpret_cast<char *>(output(n,0,1)),"adsp_default_listener"));
        sent(n,0);rpcPut32(output(n,0,2),5);rpcPut32(output(n,0,2)+4,0);
        reply(n,0);picasso_rpc_tick(&f);
        assert(f.domain[n].stage==4 && f.domain[n].defaultHandle==5);
        assert(!f.domain[n].call[0].sc && f.domain[n].call[0].handle==5);
        sent(n,0);reply(n,0);picasso_rpc_tick(&f);assert(f.domain[n].registered && f.domain[n].stage==5);
    }
    for(unsigned i=0;i<1000;++i)picasso_rpc_tick(&f);
    assert(!f.domain[0].error && !f.domain[1].error && f.domain[0].call[1].pending);
}
static unsigned input(uint8_t *p,unsigned at,const void *data,unsigned len) {
    rpcPut32(p+at,len);at+=4;if(len) { at=(at+7)&~7U;memcpy(p+at,data,len);at+=len; }return at;
}
static void reverseCalls() {
    initialization();unsigned n=1;auto &d=f.domain[n];uint8_t *p=output(n,1,3),prim[8];
    rpcPut32(prim,9);rpcPut32(prim+4,32);
    unsigned len=input(p,0,prim,8);len=input(p,len,"apps_std",9);
    rpcPut32(p+len,8);rpcPut32(p+len+4,32);len+=8;
    auto *meta=output(n,1,2);rpcPut32(meta,0x123);rpcPut32(meta+4,0);rpcPut32(meta+8,0x20200);rpcPut32(meta+12,len);
    reply(n,1);picasso_rpc_tick(&f);
    assert(d.opens==1 && d.stdOpen && !d.previousResult && !strcmp(d.name,"apps_std"));
    assert(d.call[1].queued && rpc32(output(n,1,0))==0x123);
    assert(rpc32(output(n,1,1))==8 && rpc32(output(n,1,1)+8)==0x100);
    sent(n,1);
    const char path[]="/vendor/etc/sensors/sns_reg_config";
    p=output(n,1,3);rpcPut32(prim,31);rpcPut32(prim+4,sizeof(path));
    len=input(p,0,prim,8);len=input(p,len,path,sizeof(path));rpcPut32(p+len,96);len+=4;
    meta=output(n,1,2);rpcPut32(meta,0x124);rpcPut32(meta+4,0x100);rpcPut32(meta+8,0x1f020100);rpcPut32(meta+12,len);
    reply(n,1);picasso_rpc_tick(&f);
    assert(d.previousResult==2 && d.replyBytes==0 && !strcmp(d.name,path));
    assert(rpc32(output(n,1,0))==0x124 && rpc32(output(n,1,0)+4)==2 && !rpc32(output(n,1,0)+8));
    sent(n,1);meta=output(n,1,2);rpcPut32(meta,0x125);rpcPut32(meta+12,4097);
    reply(n,1);picasso_rpc_tick(&f);assert(d.previousResult==77 && !d.replyBytes);
}
static void errors() {
    reset();picasso_rpc_tick(&f);sent(0,0);
    uint8_t bad[24]={};rpcPut64(bad,f.domain[0].call[0].ctx+0x1000);rpcPut32(bad+20,2);
    picasso_rpc_receive(&f,bad,24);assert(f.unclaimed==1 && f.domain[0].call[0].pending);
    rpcPut64(bad,f.domain[0].call[0].ctx);rpcPut32(bad+20,3);
    picasso_rpc_receive(&f,bad,24);assert(f.malformed==1);
    reply(0,0,0,2);assert(f.domain[0].call[0].pending);
    reply(0,0,0,3);assert(f.malformed==2 && f.domain[0].call[0].pending);
    reply(0,0,17,1);picasso_rpc_tick(&f);assert(f.domain[0].call[0].pending && !f.domain[0].error);
    reply(0,0,0,3);picasso_rpc_tick(&f);assert(f.domain[0].error==17);
    reset();picasso_rpc_tick(&f);sent(0,0);for(unsigned i=0;i<251;++i)picasso_rpc_tick(&f);
    assert(f.domain[0].error==-60 && f.domain[1].error==-60 && f.domain[0].call[0].pending);
    reset();picasso_rpc_tick(&f);sent(0,0);picasso_rpc_stop(&f,-104);reply(0,0);
    assert(f.stopped && f.unclaimed==1 && !f.completed && !f.domain[0].initialized);
    reset();healthError=-5;picasso_rpc_tick(&f);assert(f.stopped && !f.sent);
    reset();syncError=-5;picasso_rpc_tick(&f);assert(f.domain[0].error==-5 && !f.domain[0].call[0].queued);
    reset();uint8_t b=1;const uint8_t *in[1]={&b};uint32_t len=4097;
    assert(rpcBuild(&f,0,0,1,0x10000,in,&len)==-22 && !f.domain[0].call[0].queued);
    assert(picasso_rpc_bind(&f,{nullptr,sync,healthy},ram[0],ram[1])==-22);
    // deterministic malformed packet corpus under ASan/UBSan
    uint32_t rng=0x12345678;uint8_t p[100],out[4096];PicassoRpcArg a[8];unsigned olen;
    for(unsigned i=0;i<10000;++i) { for(auto &x:p) { rng=rng*1664525+1013904223;x=uint8_t(rng>>24); }
        picasso_rpc_receive(&f,p,i%101);(void)rpcUnpack(p,i%101,rng,out,a,&olen); }
}
static uint32_t regs[0x100000/4];
static unsigned writes;static bool stuck,drop;
static uint32_t rd(unsigned at) { assert(at<sizeof regs && !(at&3));return regs[at/4]; }
static void wr(unsigned at,uint32_t v) { ++writes;if(drop)return;if((at&4095)==0x58 && at>=0x80000)v=0;
    if((at&4095)==0x7f0 && at>=0x80000)regs[(at+4)/4]=stuck?1:0;regs[at/4]=v; }
static void wr64(unsigned at,uint64_t v) { assert(!(at&7));wr(at,uint32_t(v));wr(at+4,uint32_t(v>>32)); }
static void dma() {
    memset(regs,0,sizeof regs);regs[8]=0x48000080;regs[9]=0x60000040;regs[10]=0x5511;
    // UFS/USB banks must stay untouched.
    regs[(0x800+4*80)/4]=0x80000060;regs[(0xc00+4*80)/4]=63;
    regs[(0x800+4*81)/4]=0x800000a1;regs[(0xc00+4*81)/4]=62;
    auto p=picasso_rpc_dma_plan(rd);assert(!p.error && p.bank[0]==61 && p.bank[1]==60);
    alignas(8) uint64_t t[1536];assert(!picasso_rpc_dma_tables(t,0x123450000ULL));
    assert(t[2]==0x123459003ULL && t[512]==0x12345a003ULL);
    for(unsigned i=0;i<1536;++i)if(i!=2 && i!=512 && (i<1024 || i>=1032))assert(!t[i]);
    assert((t[1024]&~0xfffULL)==(0x123450000ULL|(3ULL<<53)) && !(t[1024]&(1ULL<<7)));
    assert(!picasso_rpc_dma_program(p,0,0x123450000ULL,rd,wr,wr64));
    assert(!picasso_rpc_dma_program(p,1,0x123470000ULL,rd,wr,wr64));
    assert(rd(0x800)==0x80001403 && rd(0x804)==0x80001404 && rd((128+61)*4096)==0x1127);
    assert(rd(0xc00+4*80)==63 && rd(0xc00+4*81)==62);
    assert(picasso_rpc_dma_plan(rd).error==-16);
    unsigned before=writes;assert(picasso_rpc_dma_program(p,0,0x123450000ULL,rd,wr,wr64)==-16 && writes==before);
    regs[0x800/4]=0;regs[0x804/4]=0;regs[((128+61)*4096)/4]=0;regs[((128+60)*4096)/4]=0;
    p=picasso_rpc_dma_plan(rd);stuck=true;
    assert(picasso_rpc_dma_program(p,0,0x123450000ULL,rd,wr,wr64)==-60 && !rd(0x800));stuck=false;
    drop=true;assert(picasso_rpc_dma_program(p,1,0x123470000ULL,rd,wr,wr64)!=0 && !rd(0x804));drop=false;
    regs[0x800/4]=0x9fff1400;assert(picasso_rpc_dma_plan(rd).error==-16);
    assert(picasso_rpc_dma_tables(t,0xfffff8000ULL)==-22);
    puts("PASS bounded RPC lifecycle, reverse registry request, malformed wire, isolated SMMU tables and refusal paths");
}
int main() { initialization();reverseCalls();errors();dma();assert(published && acquired); }
