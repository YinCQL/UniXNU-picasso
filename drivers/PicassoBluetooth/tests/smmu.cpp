#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "PicassoBtSmmu.h"
struct Model {
    uint32_t regs[0xd0000/4]{};
    std::vector<unsigned> writes;
    bool reject=false;
    void reset(){for(auto &r:regs)r=0;writes.clear();reject=false;regs[0x20/4]=(1u<<30)|128;regs[0x24/4]=64;regs[0x28/4]=(1u<<12)|0x222;regs[0x1000/4+63]=0x10000;regs[0x1800/4+63]=1;regs[0x800/4]=0x80000000|0x60;regs[0xc00/4]=63;}
};
static uint32_t rd(void *p,unsigned at){assert(at<0xd0000 && !(at&3));return static_cast<Model *>(p)->regs[at/4];}
static void wr(void *p,unsigned at,uint32_t value){assert(at<0xd0000 && !(at&3));auto &m=*static_cast<Model *>(p);m.writes.push_back(at);if(!m.reject)m.regs[at/4]=value;}
int main(){Model m; m.reset();smmu_io io{&m,rd,wr};auto p=smmu_plan_bypass(&io,0x4e3,0x80000000,32768);assert(!p.error && p.bank_ready && p.bank==63 && p.slot==1 && !p.reuse);assert(!smmu_configure_bypass(&io,&p,0x4e3));assert((m.writes==std::vector<unsigned>{0xc04,0x804}));assert(m.regs[0x800/4]==(0x80000000|0x60));
 m.writes.clear();p=smmu_plan_bypass(&io,0x4e3,0x80000000,32768);assert(!p.error && p.reuse && !smmu_configure_bypass(&io,&p,0x4e3) && m.writes.empty());
 m.reset();m.regs[0x800/4+1]=0x80000000|0x4e3;m.regs[0xc00/4+1]=12;p=smmu_plan_bypass(&io,0x4e3,0x80000000,32768);assert(p.error && smmu_configure_bypass(&io,&p,0x4e3) && m.writes.empty());
 m.reset();m.regs[(2+63)*4096/4]=1;p=smmu_plan_bypass(&io,0x4e3,0x80000000,32768);assert(p.error && m.writes.empty());
 m.reset();p=smmu_plan_bypass(&io,0x4e3,1ull<<40,32768);assert(p.error);p=smmu_plan_bypass(&io,0x4e3,(1ull<<40)-16384,32768);assert(p.error);
 m.reset();m.reject=true;p=smmu_plan_bypass(&io,0x4e3,0x80000000,32768);assert(!p.error && smmu_configure_bypass(&io,&p,0x4e3) && m.writes.size()==1 && m.writes[0]==0xc04);
 m.reset();m.regs[0xc00/4]=59;m.regs[0x1000/4+59]=0x10000;m.regs[0x1800/4+59]=1;m.regs[(2+63)*4096/4]=1;
 p=smmu_plan_bypass(&io,0x4e3,0x80000000,32768);assert(!p.error && p.bank==59 && p.bank_ready);assert(!smmu_configure_bypass(&io,&p,0x4e3));assert((m.writes==std::vector<unsigned>{0xc04,0x804}));assert(m.regs[(2+63)*4096/4]==1);
 m.reset();m.regs[0x800/4]=0;p=smmu_plan_bypass(&io,0x4e3,0x80000000,32768);assert(p.error && m.writes.empty());
 m.reset();m.regs[0x800/4+1]=0x80000000|0x60;m.regs[0xc00/4+1]=63;p=smmu_plan_bypass(&io,0x4e3,0x80000000,32768);assert(p.error && m.writes.empty());
 m.reset();m.regs[0x800/4]|=1u<<16;p=smmu_plan_bypass(&io,0x4e3,0x80000000,32768);assert(p.error && m.writes.empty());
 m.reset();m.regs[0x1800/4+63]=0;p=smmu_plan_bypass(&io,0x4e3,0x80000000,32768);assert(p.error && m.writes.empty());
 puts("PASS shared UFS bypass bank preserved, isolated SID order/readback, existing mapping reuse, conflict/width/active-bank refusal");
}
