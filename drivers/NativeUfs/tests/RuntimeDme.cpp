// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <array>
#include <vector>
#include <utility>
#include "../RuntimeDme.h"
struct Port {
  bool allowed=true,locked=false,submitLocked=false,uicAvailable=true,submitAvailable=true;
  bool ready=true,idleState=true,poisoned=false,complete=true,boundary=false,fatal=false;
  uint64_t clock=0,finish=4;
  unsigned pauses=0,issued=0;
  uint32_t code=0,answer=3;
  std::array<uint32_t,64> regs{};
  std::vector<std::pair<unsigned,uint32_t>> writes;
  Port(){regs[0x30/4]=8;regs[0x20/4]=1;}
  bool canQuery(){return allowed;} bool enter(){if(!uicAvailable)return false;assert(!locked);locked=true;return true;}
  void leave(){assert(locked);locked=false;} bool usable(){return ready&&!poisoned;}
  bool submit(){if(!submitAvailable)return false;submitLocked=true;return true;}
  void unsubmit(){assert(submitLocked);submitLocked=false;}
  bool idle(){return idleState;}
  uint32_t read(unsigned at){return regs[at/4];}
  void write(unsigned at,uint32_t value){
    writes.emplace_back(at,value);
    if(at==0x20)regs[at/4]&=~value;else regs[at/4]=value;
    if(at==0x90){assert(locked&&submitLocked&&value==1);++issued;}
  }
  uint64_t now(){if(boundary&&issued&&clock>=10){regs[0x20/4]|=1U<<10;regs[0x9c/4]=answer;boundary=false;}return clock;}
  uint64_t deadline(){return clock+10;}
  void pause(){assert(locked&&!submitLocked);++pauses;++clock;
    if(issued&&clock==finish){if(fatal)regs[0x20/4]|=0x20000;
      else if(complete){regs[0x20/4]|=1U<<10;regs[0x98/4]=code;regs[0x9c/4]=answer;}}}
  void poison(){assert(locked&&!submitLocked);poisoned=true;}
};
int main(){
  {Port p;uint32_t v;assert(ufsDmeGet(p,0x1568,2,&v)==UfsDmeResult::Success&&v==3);
    assert(!p.locked&&!p.submitLocked&&!p.poisoned&&p.issued==1&&p.pauses==4);
    assert(p.writes.size()==6&&p.writes[0]==std::make_pair(0x20U,1U<<10));
    assert(p.writes[1]==std::make_pair(0x94U,0x15680002U));assert(p.regs[0x20/4]==1);}
  {Port p;uint32_t v;assert(ufsDmeGet(p,1,0,nullptr)==UfsDmeResult::Invalid&&!p.issued);
    assert(ufsDmeGet(p,0x10000,0,&v)==UfsDmeResult::Invalid&&v==~0U);
    assert(ufsDmeGet(p,1,0x10000,&v)==UfsDmeResult::Invalid&&!p.issued);}
  for(unsigned mode=0;mode<7;++mode){Port p;uint32_t v;
    if(mode==0)p.allowed=false;if(mode==1)p.uicAvailable=false;if(mode==2)p.submitAvailable=false;
    if(mode==3)p.ready=false;if(mode==4)p.idleState=false;if(mode==5)p.regs[0x30/4]=0;if(mode==6)p.poisoned=true;
    assert(ufsDmeGet(p,0x1568,0,&v)!=UfsDmeResult::Success&&!p.issued&&p.writes.empty());assert(!p.locked&&!p.submitLocked);}
  {Port p;p.code=7;uint32_t v;assert(ufsDmeGet(p,1,0,&v)==UfsDmeResult::DeviceError&&v==0xffff0007&&!p.poisoned);}
  {Port p;p.complete=false;uint32_t v;assert(ufsDmeGet(p,1,0,&v)==UfsDmeResult::Timeout&&p.poisoned&&v==~0U);
    auto n=p.writes.size();p.regs[0x20/4]|=1U<<10;assert(ufsDmeGet(p,2,0,&v)==UfsDmeResult::Unavailable&&p.writes.size()==n&&p.issued==1);}
  {Port p;p.fatal=true;uint32_t v;assert(ufsDmeGet(p,1,0,&v)==UfsDmeResult::Fatal&&p.poisoned&&p.regs[0x20/4]&0x20000);}
  {Port p;p.regs[0x20/4]=0x20001;uint32_t v;assert(ufsDmeGet(p,1,0,&v)==UfsDmeResult::Fatal&&!p.issued&&p.writes.empty());}
  {Port p;p.complete=false;p.boundary=true;uint32_t v;
    // The clock callback publishes completion between status and final sample.
    assert(ufsDmeGet(p,1,0,&v)==UfsDmeResult::Success&&v==3&&!p.poisoned);}
  for(uint32_t bit: {0x80U,0x800U,0x10000U,0x20000U,0x40000U}) {Port p;uint32_t v;p.regs[0x20/4]|=bit;
    assert(ufsDmeGet(p,1,0,&v)==UfsDmeResult::Fatal&&!p.issued&&p.regs[0x20/4]&bit);}
  puts("PASS production DME_GET: register order, 16-bit bounds, lock/IRQ/idle refusals, result error, preserved IS, timeout quarantine and final completion sample");
}
