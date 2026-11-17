// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cstdint>
#include <cassert>
#include <cstdio>
#include "../RuntimeCompletionWait.h"
struct Port {
  uint64_t clock=300, budget=200, scale=1000;
  bool irq=true, allowed=true, armed=false, woke=false, reject=false;
  unsigned atArm=0, atGeneration=0, atBlock=0;
  uint32_t gen=7, timeout=0;
  unsigned pauses=0, relaxes=0, blocks=0, cancels=0;
  uint64_t now(){return clock;} bool interruptMode(){return irq;} bool canBlock(){return allowed;}
  uint64_t spinTicks(){return budget;} uint64_t nanoseconds(uint64_t n){return n*scale;}
  void pause(){++pauses;} void relax(){++relaxes;}
  void complete(){++gen;if(armed)woke=true;}
  bool arm(uint32_t us){if(atArm)complete();timeout=us;armed=!reject;return armed;}
  uint32_t generation(){if(atGeneration)complete();return gen;}
  void cancel(){assert(armed);armed=false;++cancels;}
  void block(){if(atBlock)complete();assert(armed);++blocks;assert(!atBlock||woke);armed=false;}
};
int main(){
  {Port p;p.irq=false;ufsRuntimeWaitCompletion(p,7,0,10000);assert(p.pauses==1&&!p.timeout);}
  {Port p;p.allowed=false;ufsRuntimeWaitCompletion(p,7,0,10000);assert(p.pauses==1&&!p.timeout);}
  {Port p;p.clock=199;ufsRuntimeWaitCompletion(p,7,0,10000);assert(p.pauses==1&&!p.timeout);}
  {Port p;p.clock=10000;ufsRuntimeWaitCompletion(p,7,0,10000);assert(p.pauses==1&&!p.timeout);}
  {Port p;ufsRuntimeWaitCompletion(p,7,0,10000);assert(p.timeout==1000&&p.blocks==1);}
  {Port p;ufsRuntimeWaitCompletion(p,7,0,350);assert(p.timeout==50&&p.blocks==1);}
  {Port p;p.scale=1;ufsRuntimeWaitCompletion(p,7,0,350);assert(p.relaxes==1&&!p.timeout);}
  {Port p;p.gen=8;ufsRuntimeWaitCompletion(p,7,0,10000);assert(p.cancels==1&&!p.blocks);}
  {Port p;p.atArm=1;ufsRuntimeWaitCompletion(p,7,0,10000);assert(p.cancels==1&&!p.blocks);}
  {Port p;p.atGeneration=1;ufsRuntimeWaitCompletion(p,7,0,10000);assert(p.cancels==1&&!p.blocks);}
  {Port p;p.atBlock=1;ufsRuntimeWaitCompletion(p,7,0,10000);assert(p.blocks==1&&p.woke);}
  {Port p;p.reject=true;ufsRuntimeWaitCompletion(p,7,0,10000);assert(!p.blocks&&!p.cancels);}
  puts("PASS 12 production completion-wait cases: IRQ/masked/spin/deadline, bounded timeout and every lost-wakeup boundary");
}
