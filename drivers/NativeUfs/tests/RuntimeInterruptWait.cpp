// SPDX-License-Identifier: BSD-2-Clause-Patent
// Production IRQ and wait adapters; only kernel scheduling/MMIO primitives are replaced.
#include <cstdint>
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../RuntimeMetrics.h"
constexpr unsigned kSlots=4, NSEC_PER_USEC=1000;
constexpr int THREAD_UNINT=0, THREAD_WAITING=1, THREAD_AWAKENED=2, THREAD_CONTINUE_NULL=0;
using event_t=uintptr_t;
static uint32_t gSlotGen[kSlots],gOutstanding,regs[64];static uint64_t gSlotDoneAt[kSlots];
static bool gIrqMode=true,allow=true,armed=false,woken=false,reject=false;
static uint64_t gSpinTicks=200,gIrqs=0,gIrqSleeps=0,gRuntimeTicks=0,clockTick=300;
static int8_t gEntrySlot[32];static struct {uintptr_t hc=1;}gHost;
static unsigned stage=0,armTimeout=0,cancels=0,blocks=0;
static void runtimeInterrupt(void*);
static uint64_t mach_absolute_time(){return clockTick;}
static void absolutetime_to_nanoseconds(uint64_t n,uint64_t*out){*out=n*1000;}
static bool preemption_enabled(){return allow;}
static void hostCpuRelax(){++clockTick;}
static void ufsPollPause(void(*pause)()){pause();}
static uint32_t read32(uintptr_t,unsigned at){return regs[at/4];}
static void hostWrite(unsigned at,uint32_t value){assert(at==0x20&&value==1);regs[at/4]&=~value;}
static uintptr_t current_thread(){return 1;}
static void thread_wakeup(event_t){if(armed)woken=true;}
static void finish(){regs[0x58/4]=0;regs[0x20/4]|=1;runtimeInterrupt(nullptr);clockTick+=7;}
static int assert_wait_timeout(event_t,int,uint32_t us,unsigned){
  armTimeout=us;if(stage==1)finish();armed=!reject;return armed?THREAD_WAITING:0;
}
static void clear_wait(uintptr_t,int){assert(armed);++cancels;armed=false;}
static void thread_block(int){assert(armed);if(stage==2)finish();++blocks;
  if(stage==2)assert(woken);clockTick+=3;armed=false;}
#include "../RuntimeInterruptWait.h"
static void reset(){memset(gSlotGen,0,sizeof(gSlotGen));memset(gSlotDoneAt,0,sizeof(gSlotDoneAt));
  memset(regs,0,sizeof(regs));memset(gEntrySlot,-1,sizeof(gEntrySlot));gEntrySlot[0]=0;
  gOutstanding=1;regs[0x58/4]=1;clockTick=300;stage=armTimeout=cancels=blocks=0;
  armed=woken=reject=false;gIrqMode=allow=true;gIrqs=gIrqSleeps=0;gIrqWakes=gIrqWakeTicks=gIrqWakeMax=gIrqTimeouts=0;
  gWaitPauses=gWaitPauseTicks=0;
}
int main(){
  {reset();stage=2;rtWaitCompletion(0,0,0,10000);assert(blocks==1&&gIrqSleeps==1&&gIrqWakes==1&&gIrqTimeouts==0);
    assert(gIrqWakeTicks==10&&gIrqWakeMax==10&&gSlotDoneAt[0]==300&&armTimeout==1000);}
  {reset();stage=1;rtWaitCompletion(0,0,0,10000);assert(cancels==1&&!blocks&&!gIrqSleeps&&!gIrqTimeouts);}
  {reset();rtWaitCompletion(0,0,0,350);assert(blocks==1&&gIrqTimeouts==1&&!gIrqWakes&&armTimeout==50);}
  {reset();reject=true;rtWaitCompletion(0,0,0,10000);assert(!blocks&&!cancels&&!gIrqSleeps);}
  {reset();allow=false;rtWaitCompletion(0,0,0,10000);assert(gWaitPauses==1&&gWaitPauseTicks==1&&!blocks);}
  {reset();regs[0x58/4]=0;regs[0x20/4]=0x801;runtimeInterrupt(nullptr);clockTick=900;runtimeInterrupt(nullptr);
    assert(regs[0x20/4]==0x800&&gSlotDoneAt[0]==300&&gSlotGen[0]==2&&gIrqs==2);}
  {reset();clockTick=350;rtWaitDone(300);assert(gWaitRanges[1]==1);}
  puts("PASS production IRQ wait adapters: completion before block, completion during arm, timer timeout, rejected arm, masked context, first IRQ timestamp and preserved error status");
}
