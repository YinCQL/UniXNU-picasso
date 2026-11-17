#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#define __arm64__ 1
#define DEVELOPMENT 1
#define DEBUG 0
#define NSEC_PER_SEC 1000000000ULL
typedef struct task {unsigned refs;} *task_t;
typedef struct proc {struct proc *next;char p_name[33];uint8_t p_uuid[16];task_t task;int pid;uint64_t unique;} *proc_t;
typedef void *thread_call_param_t;
struct call {void(*fn)(void*,void*);};
typedef struct call *thread_call_t;
static struct call call;
static proc_t first;
static unsigned scenario,list_locked,entered,snapshots,worker,allocations,at_exec;
static uint64_t now=1000000000ULL;
static void proc_list_lock(void){assert(!list_locked&&worker);list_locked=1;}
static void proc_list_unlock(void){assert(list_locked);list_locked=0;}
#define ALLPROC_FOREACH(p) for((p)=first;(p);(p)=(p)->next)
static task_t proc_task(proc_t p){assert(list_locked);return p->task;}
static int proc_getpid(proc_t p){assert(list_locked||at_exec);return p->pid;}
static uint64_t proc_uniqueid(proc_t p){assert(list_locked||at_exec);return p->unique;}
static void task_reference(task_t t){assert(list_locked&&t);++t->refs;}
static void task_deallocate(task_t t){assert(!list_locked&&worker&&t->refs==2);--t->refs;}
void picasso_settings_task_snapshot(task_t t,int pid,uint64_t u,unsigned s,const char *n){assert(!list_locked&&worker&&t->refs==2&&pid>0&&u>0&&s<=8);assert(!strcmp(n,"Preferences")||!strcmp(n,"configd")||!strcmp(n,"lockdownd")||!strcmp(n,"fairplayd.H2"));++snapshots;}
static thread_call_t thread_call_allocate(void(*fn)(void*,void*),void *arg){assert(!worker&&!arg&&!list_locked);++allocations;if(scenario==2)return NULL;call.fn=fn;return &call;}
static int thread_call_enter(thread_call_t c){assert(c==&call&&!worker&&!list_locked&&!entered);entered=1;return 0;}
static uint64_t mach_absolute_time(void){return now;}
static void nanoseconds_to_absolutetime(uint64_t n,uint64_t *p){*p=n;}
#define os_atomic_store(p,v,o) __atomic_store_n(p,v,__ATOMIC_RELEASE)
static int cmp(unsigned *p,unsigned expected,unsigned desired){return __atomic_compare_exchange_n(p,&expected,desired,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED);}
#define os_atomic_cmpxchg(p,e,d,o) cmp(p,e,d)
static int model_log(const char *fmt,...){assert(!list_locked);va_list a;va_start(a,fmt);int n=vprintf(fmt,a);va_end(a);return n;}
#define printf model_log
#include "bsd.inc"
#undef printf
static void fire(void){assert(entered);entered=0;worker=1;call.fn(NULL,NULL);worker=0;assert(!settings_diag_busy);}
int main(int argc,char **argv){
 assert(argc==2);scenario=(unsigned)atoi(argv[1]);struct task tasks[5]={{1},{1},{1},{1},{1}};struct proc procs[5]={0};
 const char *names[]={"Preferences","configd","lockdownd","fairplayd.H2","Preferences"};
 for(unsigned i=0;i<5;++i){procs[i].next=i+1<5?&procs[i+1]:NULL;strcpy(procs[i].p_name,names[i]);procs[i].task=&tasks[i];procs[i].pid=(int)i+7;procs[i].unique=i+123;}
 first=procs;
 const uint8_t stock_uuid[16]={ 0x30, 0x68, 0x88, 0xad, 0xfb, 0xc4, 0x39, 0xc4, 0x96, 0x1c, 0x72, 0x1c, 0x7f, 0xf2, 0x30, 0xd1 };
 memcpy(procs[3].p_uuid,stock_uuid,16);
 if(scenario==1)settings_diag_busy=1;
 if(scenario==3)settings_diag_sample=8;
 if(scenario==4){settings_diag_sample=1;settings_diag_last=now;}
 if(scenario==5)first=NULL;
 if(scenario==6)procs[0].task=NULL;
 if(scenario==7){for(unsigned i=0;i<5;++i)strcpy(procs[i].p_name,"other");memset(procs[3].p_uuid,0,16);}
 if(scenario==8)procs[0].next=NULL;
 if(scenario==10)first=&procs[3];
 if(scenario==11)procs[3].task=NULL;
 if(scenario==12)strcpy(procs[3].p_name,"renamed-stock-service");
 if(scenario==13)procs[3].p_uuid[0]^=1;
 if(scenario>=14){
  memset(procs[3].p_uuid,0,16);strcpy(procs[3].p_name,"changed-service-name");
  at_exec=1;
  picasso_settings_service_exec(&procs[3]);assert(picasso_settings_service_match(&procs[3]));
  if(scenario==15){++procs[3].unique;assert(!picasso_settings_service_match(&procs[3]));}
  if(scenario==16){++procs[3].pid;assert(!picasso_settings_service_match(&procs[3]));}
  if(scenario==17){settings_service_ids[0].ready=0;assert(!picasso_settings_service_match(&procs[3]));}
  if(scenario==18){procs[3].task=NULL;}
  if(scenario==19){for(unsigned n=1;n<4;++n){++procs[3].unique;picasso_settings_service_exec(&procs[3]);}++procs[3].unique;picasso_settings_service_exec(&procs[3]);assert(settings_service_count==4&&!picasso_settings_service_match(&procs[3]));}
  at_exec=0;
 }
 picasso_settings_diag_request();
 if(scenario>=1&&scenario<=4){assert(!entered&&!snapshots);if(scenario!=1)assert(!settings_diag_busy);}
 else {assert(entered&&!snapshots&&settings_diag_busy);picasso_settings_diag_request();assert(entered&&!snapshots);fire();
  assert(snapshots==(scenario==5||scenario==7?0:scenario==8?1:scenario==10?2:scenario==11||scenario==13||(scenario>=15&&scenario<=19)?3:4));
  unsigned s=settings_diag_sample;picasso_settings_diag_request();assert(!entered&&settings_diag_sample==s);
  if(scenario==9){while(settings_diag_sample<8){now+=5000000000ULL;picasso_settings_diag_request();assert(entered);fire();}now+=5000000000ULL;picasso_settings_diag_request();assert(!entered&&settings_diag_sample==8&&allocations==1);}
 }
 for(unsigned i=0;i<5;++i)assert(tasks[i].refs==1);
 assert(!list_locked);puts("PASS settings worker");
}
