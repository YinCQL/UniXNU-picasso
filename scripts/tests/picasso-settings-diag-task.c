/* Host lock/map/lifetime model for the extracted production helper. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#define __arm64__ 1
#define DEVELOPMENT 1
#define DEBUG 0
#define TH_WAIT 1
typedef struct map { unsigned refs; } *vm_map_t;
#define VM_MAP_NULL NULL
typedef struct arm_saved_state { unsigned is64,cpsr; uint64_t pc,lr,fp; } arm_saved_state_t;
typedef struct thread { struct thread *next; unsigned sched_lock,state,block_hint; uint64_t tid; arm_saved_state_t *regs; } *thread_t;
struct queue { thread_t first; };
typedef struct task { unsigned active,thread_count; uint64_t mach_header_vm_address,task_shared_region_slide; struct queue threads; vm_map_t map; } *task_t;
typedef int spl_t;
static unsigned scenario,task_locked,sched_locked,irq,preempt,copies,lines,attempts,switches;
static vm_map_t current_map_model;
static int thread_lck_grp;
static int task_lock_try(task_t t) { (void)t; assert(!task_locked&&!sched_locked); ++attempts; if(scenario==1)return 0; task_locked=1;return 1; }
static void task_unlock(task_t t) { (void)t;assert(task_locked&&!sched_locked);task_locked=0; }
static uint64_t thread_tid(thread_t t) { assert(task_locked);return t->tid; }
static spl_t splsched(void) { assert(!irq);irq=1;return 0; }
static int simple_lock_try(unsigned *lock,int *grp) { assert(irq&&task_locked&&!sched_locked&&grp==&thread_lck_grp); if(*lock)return 0; sched_locked=1;return 1; }
static void thread_unlock(thread_t t) { (void)t;assert(sched_locked);sched_locked=0; }
static void splx(spl_t s) { assert(!s&&!sched_locked);irq=0; }
static arm_saved_state_t *get_user_regs(thread_t t) { assert(task_locked&&sched_locked&&irq);return t->regs; }
static int is_saved_state64(arm_saved_state_t *s) { return s->is64; }
static unsigned get_saved_state_cpsr(arm_saved_state_t *s) { return s->cpsr; }
#define PSR64_IS_USER64(x) ((x)==0)
static uint64_t get_saved_state_pc(arm_saved_state_t *s) { return s->pc; }
static uint64_t get_saved_state_lr(arm_saved_state_t *s) { return s->lr; }
static uint64_t get_saved_state_fp(arm_saved_state_t *s) { return s->fp; }
#define queue_iterate(q,t,type,link) for((t)=(q)->first;(t);(t)=(t)->next)
static void vm_map_reference(vm_map_t m) { assert(task_locked&&m);++m->refs; }
static void vm_map_deallocate(vm_map_t m) { assert(!task_locked&&!sched_locked&&!preempt&&m&&m->refs==2);--m->refs; }
static vm_map_t vm_map_switch(vm_map_t m) { assert(!task_locked&&!sched_locked&&!preempt&&!irq);++switches;vm_map_t old=current_map_model;current_map_model=m;return old; }
static void disable_preemption(void) { assert(!preempt);preempt=1; }
static void enable_preemption(void) { assert(preempt==1);preempt=0; }
static int copyin(uint64_t src,void *dst,size_t size) {
 assert(current_map_model&&current_map_model->refs==2&&preempt==1&&!task_locked&&!sched_locked&&!irq);
 assert(size==16&&src>=0x1000&&src<=0x7fffffffffULL-15&&!(src&15));++copies;
 if(scenario==8 || (scenario==9&&src==0x1020))return EFAULT;
 uint64_t pair[2]={src+16,0x180000000ULL+src};
 if(scenario!=15&&src==0x1020)pair[0]=0;
 if(scenario==10)pair[0]=src;
 if(scenario==11)pair[0]=src-16;
 if(scenario==12)pair[1]=0;
 if(scenario==13)pair[0]=0x7ffffffff0ULL;
 if(scenario==14)pair[0]=0xfffffe0000000000ULL;
 memcpy(dst,pair,sizeof(pair));return 0;
}
static int model_log(const char *fmt,...) { assert(!task_locked&&!sched_locked&&!irq&&!preempt&&current_map_model==NULL);++lines;va_list a;va_start(a,fmt);int n=vprintf(fmt,a);va_end(a);return n; }
#define printf model_log
#include "task.inc"
#undef printf
int main(int argc,char **argv) {
 assert(argc==2);scenario=(unsigned)atoi(argv[1]);
 struct map map={1};struct thread threads[12]={0};arm_saved_state_t regs={1,0,0x180001000ULL,0x180002000ULL,0x1000};
 for(unsigned i=0;i<12;++i){threads[i].next=i+1<12?&threads[i+1]:NULL;threads[i].state=TH_WAIT;threads[i].block_hint=4;threads[i].tid=i+1;threads[i].regs=&regs;}
 struct task task={1,12,0x100000000ULL,0x1234000,{threads},&map};
 switch(scenario) {
 case 2:task.active=0;break;
 case 3:task.map=NULL;break;
 case 4:for(unsigned i=0;i<12;++i)threads[i].sched_lock=1;break;
 case 5:regs.is64=0;break;
 case 6:regs.cpsr=4;break;
 case 7:regs.fp=0x1001;break;
 case 16:regs.fp=0;break;
 case 17:task.threads.first=NULL;task.thread_count=0;break;
 case 18:for(unsigned i=0;i<12;++i)threads[i].regs=NULL;break;
 case 19:regs.fp=UINT64_MAX;break;
 }
 picasso_settings_task_snapshot(&task,7,123,1,"Preferences");
 assert(map.refs==1&&attempts==1&&!task_locked&&!sched_locked&&!irq&&!preempt&&!current_map_model&&switches==copies*2);
 assert(copies<=80&&lines<=41);
 if(scenario==0)assert(copies==24&&lines==33);
 if(scenario==1||scenario==2||scenario==17)assert(lines==1&&copies==0);
 if(scenario==3||scenario==4||scenario==5||scenario==6||scenario==18)assert(copies==0&&lines==9);
 if(scenario==7||scenario==16||scenario==19)assert(copies==0);
 if(scenario==8||scenario==10||scenario==11||scenario==12)assert(copies==8);
 if(scenario==15)assert(copies==80&&lines==41);
 puts("PASS settings task");
}
