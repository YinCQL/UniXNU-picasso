/* Execute the extracted task helper AND the unchanged XNU backtrace_user.
 * Locks, queues, user maps and saved registers below are host stand-ins. */
#define _CRT_SECURE_NO_WARNINGS
#define __arm64__ 1
#define DEVELOPMENT 1
#define DEBUG 0
#define __unused __attribute__((unused))
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define TRUE 1
#define TH_WAIT 1
typedef int errno_t;
typedef uintptr_t user_addr_t;
typedef unsigned backtrace_info_t;
typedef unsigned backtrace_flags_t;
#define BTI_NONE 0u
#define BTI_64_BIT 1u
#define BTI_TRUNCATED 2u
#define BTF_NONE 0u
typedef errno_t (*backtrace_user_copy_fn)(void *,void *,user_addr_t,size_t);
struct backtrace_control {
 backtrace_flags_t btc_flags;uintptr_t btc_frame_addr;void *btc_user_thread;
 backtrace_user_copy_fn btc_user_copy;void *btc_user_copy_context;int64_t btc_addr_offset;
};
#define BTCTL_INIT ((struct backtrace_control){0})
struct backtrace_user_info {
 backtrace_info_t btui_info;errno_t btui_error;unsigned btui_async_start_index;
 uintptr_t btui_async_frame_addr,btui_next_frame_addr;
};
#define BTUINFO_INIT ((struct backtrace_user_info){0})
typedef struct arm_saved_state { int is64;uint32_t cpsr;uint64_t pc,lr,sp,fp; } arm_saved_state_t;
struct task;
typedef struct task *task_t;
struct thread { struct thread *next;task_t task;uint64_t tid;unsigned sched_lock,state,block_hint;arm_saved_state_t *regs; };
typedef struct thread *thread_t;
struct queue { thread_t first; };
struct task { struct queue threads;unsigned thread_count;uint64_t mach_header_vm_address;uint32_t task_shared_region_slide; };
typedef void *vm_map_t;
typedef int spl_t;
static int scenario,preempt,task_locked,sched_locked,irq_level;
static unsigned copies,lines,bytes,lock_attempts;
static char output[32768];
static task_t ct;
static thread_t self;
static task_t current_task(void) { return ct; }
static thread_t current_thread(void) { return self; }
static task_t get_threadtask(thread_t t) { return t->task; }
static uint64_t thread_tid(thread_t t) { return t->tid; }
static struct arm_saved_state *get_user_regs(thread_t t) { assert(t==self);return t->regs; }
static int is_saved_state64(arm_saved_state_t *s) { return s->is64; }
static uint32_t get_saved_state_cpsr(arm_saved_state_t *s) { return s->cpsr; }
#define PSR64_IS_USER64(x) ((x)==0)
static uint64_t get_saved_state_pc(arm_saved_state_t *s) { return s->pc; }
static uint64_t get_saved_state_lr(arm_saved_state_t *s) { return s->lr; }
static uint64_t get_saved_state_sp(arm_saved_state_t *s) { return s->sp; }
static uint64_t get_saved_state_fp(arm_saved_state_t *s) { return s->fp; }
static int ml_get_interrupts_enabled(void) { return !irq_level; }
static vm_map_t get_task_map_reference(task_t t) { (void)t;assert(0);return NULL; }
static vm_map_t vm_map_switch(vm_map_t m) { (void)m;assert(0);return NULL; }
static void vm_map_deallocate(vm_map_t m) { (void)m;assert(0); }
static void disable_preemption(void) { ++preempt; }
static void enable_preemption(void) { assert(preempt>0);--preempt; }
static int task_lock_try(task_t t) {
 assert(t==ct && !task_locked && !sched_locked);++lock_attempts;
 if(scenario==2)return 0;task_locked=1;return 1;
}
static void task_unlock(task_t t) { assert(t==ct && task_locked && !sched_locked);task_locked=0; }
static int thread_lck_grp;
static int simple_lock_try(unsigned *lock,int *grp) {
 assert(task_locked && !sched_locked && irq_level && grp==&thread_lck_grp);++lock_attempts;
 if(*lock)return 0;sched_locked=1;return 1;
}
static void thread_unlock(thread_t t) { (void)t;assert(sched_locked);sched_locked=0; }
static spl_t splsched(void) { int prev=irq_level;irq_level=1;return prev; }
static void splx(spl_t old) { assert(!sched_locked);irq_level=old; }
#define queue_iterate(head,t,type,link) for((t)=(head)->first;(t);(t)=(t)->next)
static int copyin(user_addr_t src,void *dst,size_t n) {
 assert(preempt>0 && !task_locked && !sched_locked && !irq_level);++copies;
 /* Model the ordinary copyin user-range check and a nonresident frame.
  * The production VM early-failure rule is separately checked in Python. */
 if(src<0x1000 || src>=0x1400 || (src&15) || n!=16 ||
    (scenario==8 && src==0x1020) || scenario==14 || scenario==19) {
  memset(dst,0xa5,n);return EFAULT;
 }
 uint64_t pair[2]={src+16,0x180000000ULL+(src-0x1000)*4};
 if(src==0x1020 && scenario!=9)pair[0]=0;
 if(scenario==11)pair[0]=src;
 if(scenario==12)pair[0]=src-16;
 if(scenario==15)pair[0]=UINT64_MAX-15;
 memcpy(dst,pair,16);return 0;
}
static int host_log(const char *fmt,...) {
 assert(!preempt && !task_locked && !sched_locked && !irq_level);
 va_list ap;va_start(ap,fmt);int n=vsnprintf(output+bytes,sizeof(output)-bytes,fmt,ap);va_end(ap);
 assert(n>0 && n<256 && (unsigned)n<sizeof(output)-bytes);bytes+=(unsigned)n;++lines;return n;
}
#define printf host_log
#undef __x86_64__ /* Select the native ARM64 walker after host headers are read. */
#pragma clang diagnostic push
/* The pinned native walker sets has_async without reading it. */
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "unwinder.inc"
#pragma clang diagnostic pop
#include "task.inc"
static void contains(const char *s) { if(!strstr(output,s)) { fprintf(stderr,"missing %s\n%s",s,output);abort(); } }
int main(int argc,char **argv) {
 assert(argc==2);scenario=atoi(argv[1]);
 struct task task={0},other={0};struct thread threads[64]={0};
 arm_saved_state_t regs={1,0,0x180001000ULL,0x180002000ULL,0xf000,0x1000};
 task.thread_count=scenario==4?64:4;task.threads.first=threads;
 task.mach_header_vm_address=UINT64_MAX;task.task_shared_region_slide=UINT32_MAX;
 for(unsigned i=0;i<task.thread_count;++i) {
  threads[i].next=i+1<task.thread_count?&threads[i+1]:NULL;
  threads[i].task=&task;threads[i].tid=UINT64_MAX-i;threads[i].state=TH_WAIT;
  threads[i].block_hint=6;threads[i].regs=&regs;
 }
 self=threads;ct=&task;
 if(scenario==1)ct=&other;
 if(scenario==3)threads[2].sched_lock=1;
 if(scenario==5)self->regs=NULL;
 if(scenario==6)regs.is64=0;
 if(scenario==7)regs.fp=0x1001;
 if(scenario==10)regs.fp=0;
 if(scenario==13)regs.cpsr=4;
 if(scenario==14)regs.fp=UINT64_MAX-15;
 if(scenario==16) { task.threads.first=NULL;task.thread_count=0; }
 if(scenario==17)threads[1].state=4; /* stale hint must not be reported as a wait */
 if(scenario==19) { uint64_t pair[2];preempt=3;assert(picasso_wait_copy(NULL,pair,0x1000,16)==EFAULT);assert(preempt==3);preempt=0; }
 picasso_service_wait_snapshot(scenario==18?NULL:&task,2147483647,UINT64_MAX,2);
 assert(!preempt && !task_locked && !sched_locked && !irq_level && lock_attempts<=17 && lines<=22);
 if(scenario==1 || scenario==18) { assert(!lines && !copies); }
 else if(scenario==5 || scenario==6 || scenario==13) { contains("unavailable=user-state");assert(!copies); }
 else {
  contains("scope=exiting-thread");
  if(scenario==2)contains("locked=0 total=0 sampled=0 main_header=0 cache_slide=0");
  if(scenario==3)contains("valid=0 state=0 hint=0");
  if(scenario==4)contains("total=64 sampled=16");
  if(scenario==7 || scenario==14 || scenario==19)contains("frames=1 error=");
  if(scenario==8) { contains("frames=3 error=");assert(!strstr(output,"a5a5a5a5")); }
  if(scenario==9) { contains("frames=12 error=0 flags=3");assert(copies==11); }
  if(scenario==10) { contains("frames=1 error=0");assert(!copies); }
  if(scenario==11 || scenario==12) { contains("frames=2 error=0");assert(copies==1); }
  if(scenario==15) { contains("frames=2 error=");assert(copies==2); }
  if(scenario==16)contains("total=0 sampled=0");
  if(scenario==17)contains("valid=1 state=4 hint=0");
  if(scenario==0) { contains("frames=4 error=0 flags=1");contains("180001000 180000000 180000040 180000080"); }
 }
 fprintf(stdout,"PASS task=%d copies=%u lines=%u\n",scenario,copies,lines);
 return 0;
}
