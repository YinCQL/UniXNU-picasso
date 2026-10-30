/* Deliberately opaque BSD task_t; no task layout is visible to the gate. */
#define _CRT_SECURE_NO_WARNINGS
#define __arm64__ 1
#define DEVELOPMENT 1
#define DEBUG 0
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define SIGKILL 9
#define OS_REASON_WATCHDOG 20
#define WTERMSIG(x) ((x) & 0177)
#define WIFSIGNALED(x) (WTERMSIG(x) != 0177 && WTERMSIG(x) != 0)
#define os_atomic_load(p,o) __atomic_load_n(p,__ATOMIC_RELAXED)
static int cmp(unsigned *p,unsigned e,unsigned d) { return __atomic_compare_exchange_n(p,&e,d,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED); }
#define os_atomic_cmpxchg(p,e,d,o) cmp(p,e,d)
typedef struct task *task_t;
struct reason { unsigned osr_namespace; uint64_t osr_code; };
struct proc { task_t task; int pid; uint64_t unique; char p_name[33]; struct reason *p_exit_reason; };
typedef struct proc *proc_t;
static proc_t cp;
static task_t ct;
static proc_t current_proc(void) { return cp; }
static task_t current_task(void) { return ct; }
static task_t proc_task(proc_t p) { return p->task; }
static int proc_getpid(proc_t p) { return p->pid; }
static uint64_t proc_uniqueid(proc_t p) { return p->unique; }
static unsigned calls,last_sample;
void picasso_service_wait_snapshot(task_t,int,uint64_t,unsigned);
void picasso_service_wait_snapshot(task_t t,int pid,uint64_t unique,unsigned sample) {
 assert(t==ct && pid==cp->pid && unique==cp->unique && sample<=2);
 ++calls;last_sample=sample;
}
#include "gate.inc"
int main(int argc,char **argv) {
 assert(argc==2);int c=atoi(argv[1]);
 struct reason reason={20,1};
 struct proc p={(task_t)(uintptr_t)1,58,UINT64_MAX,"backboardd",&reason}, other=p;
 cp=&p;ct=p.task;int rv=9;
 switch(c) {
 case 1: cp=&other;break;
 case 2: ct=(task_t)(uintptr_t)2;break;
 case 3: rv=0;break;
 case 4: rv=5;break;
 case 5: p.p_exit_reason=NULL;break;
 case 6: reason.osr_namespace=1;break;
 case 7: reason.osr_code=2;break;
 case 8: strcpy(p.p_name,"backboardd-extra");break;
 case 9: strcpy(p.p_name,"CommCenter");break;
 case 10: strcpy(p.p_name,"SpringBoard");break;
 case 11: strcpy(p.p_name,"mediaserverd");break;
 case 12: strcpy(p.p_name,"audiomxd");break;
 }
 if(c==13) {
  const char *names[]={"backboardd","SpringBoard","mediaserverd","audiomxd"};
  for(unsigned i=0;i<4;++i) {
   strcpy(p.p_name,names[i]);
   for(unsigned j=0;j<10;++j) { ++p.pid;++p.unique;picasso_report_service_wait(&p,9); }
   assert(calls==2*(i+1) && last_sample==2);
  }
 } else {
  picasso_report_service_wait(&p,rv);
  assert(calls==(unsigned)(c==0 || c>=10));
  /* Failed gates cannot consume the valid victim's budget. */
  cp=&p;ct=p.task;reason.osr_namespace=20;reason.osr_code=1;p.p_exit_reason=&reason;
  strcpy(p.p_name,"backboardd");picasso_report_service_wait(&p,9);
  if(c>0 && c<10) assert(last_sample==1 && calls==1);
 }
 printf("PASS gate=%d\n",c);
 return 0;
}
