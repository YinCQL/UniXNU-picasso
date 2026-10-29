/* Host-only API stand-ins for helpers extracted from the generated XNU tree. */
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#undef EFAULT
#undef ENOENT
#undef ENAMETOOLONG
#define EFAULT 14
#define ENOENT 2
#define ENAMETOOLONG 63 /* Fixed XNU errno in the copyinstr stand-in. */
#ifdef _MSC_VER
#include <intrin.h>
#endif
#define __arm64__ 1
#define DEVELOPMENT 1
#define DEBUG 0
#define SIGTRAP 5
#define OS_REASON_SIGNAL 2
#define WTERMSIG(x) ((x) & 0177)
#define WIFSIGNALED(x) (WTERMSIG(x) != 0177 && WTERMSIG(x) != 0)
#define WEXITSTATUS(x) (((x) >> 8) & 0xff)
#ifdef _MSC_VER
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __atomic_exchange_n(p,v,o) ((unsigned)_InterlockedExchange((volatile long *)(p),(long)(v)))
#define __atomic_store_n(p,v,o) ((void)_InterlockedExchange((volatile long *)(p),(long)(v)))
#define os_atomic_load(p,o) ((unsigned)_InterlockedCompareExchange((volatile long *)(p),0,0))
#define os_atomic_cmpxchg(p,e,d,o) ((unsigned)_InterlockedCompareExchange((volatile long *)(p),(long)(d),(long)(e)) == (unsigned)(e))
#else
#define os_atomic_load(p,o) __atomic_load_n((p),__ATOMIC_RELAXED)
static int host_atomic_compare(unsigned *p, unsigned expected, unsigned desired) {
 return __atomic_compare_exchange_n(p,&expected,desired,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED);
}
#define os_atomic_cmpxchg(p,e,d,o) host_atomic_compare((p),(e),(d))
#endif
typedef uintptr_t user_addr_t;
typedef uint64_t user64_addr_t;
typedef uint64_t user64_ulong_t;
typedef uint64_t user64_size_t;
typedef uint32_t mach_port_name_t;
typedef uint8_t dyld_bool;
#define DYLD_MAX_PROCESS_INFO_NOTIFY_COUNT 8
#include "dyld-types.h"
/* Deliberately opaque, as task_t is in the BSD translation unit. */
struct task;
typedef struct task *task_t;
struct host_task { uint32_t cached_slide; uint64_t image_info, main_header; };
struct reason { unsigned osr_namespace; uint64_t osr_code; };
struct proc { int pid, pidversion; uint64_t uniqueid; struct reason *p_exit_reason; task_t task; char p_name[33]; };
typedef struct proc *proc_t;
struct thread { uint64_t tid; };
typedef struct thread *thread_t;
typedef struct state { int is64; uint32_t cpsr; uint64_t pc,lr,sp,fp,x[29]; } arm_saved_state_t;
static proc_t cp;
static task_t ct;
static struct thread thread;
static proc_t current_proc(void) { return cp; }
static task_t current_task(void) { return ct; }
static thread_t current_thread(void) { return &thread; }
static int proc_getpid(proc_t p) { return p->pid; }
static int proc_pidversion(proc_t p) { return p->pidversion; }
static uint64_t proc_uniqueid(proc_t p) { return p->uniqueid; }
static task_t proc_task(proc_t p) { return p->task; }
static char host_exit_description[200];
static const char *exit_reason_get_string_desc(struct reason *r) { return r ? host_exit_description : NULL; }
static uint64_t thread_tid(thread_t t) { return t->tid; }
static int is_saved_state64(arm_saved_state_t *s) { return s->is64; }
static uint32_t get_saved_state_cpsr(arm_saved_state_t *s) { return s->cpsr; }
#define PSR64_IS_USER64(x) ((x) == 0)
static uint64_t get_saved_state_pc(arm_saved_state_t *s) { return s->pc; }
static uint64_t get_saved_state_lr(arm_saved_state_t *s) { return s->lr; }
static uint64_t get_saved_state_sp(arm_saved_state_t *s) { return s->sp; }
static uint64_t get_saved_state_fp(arm_saved_state_t *s) { return s->fp; }
static uint64_t get_saved_state_reg(arm_saved_state_t *s,unsigned i) { assert(i<29);return s->x[i]; }
static user_addr_t task_get_all_image_info_addr(task_t t) { return ((struct host_task *)t)->image_info; }
struct mapping { user_addr_t address; void *data; size_t size; };
static struct mapping maps[16];
static unsigned map_count, reads, lines, bytes;
static int scenario;
static char output[32768];
static int copyin(user_addr_t,void *,size_t);
static int copyinstr(user_addr_t,char *,size_t,size_t *);
static int host_log(const char *format,...) {
 va_list args;va_start(args,format);
 int n=vsnprintf(output+bytes,sizeof(output)-bytes,format,args);
 va_end(args);assert(n>0 && (unsigned)n<sizeof(output)-bytes);
 bytes+=(unsigned)n;++lines;return n;
}
#define printf host_log
#include "actual-helper.inc"
static void map(user_addr_t address,void *data,size_t n) {
 assert(map_count<16); maps[map_count++]=(struct mapping){address,data,n};
}
static struct mapping *find(user_addr_t address) {
 for(unsigned i=0;i<map_count;++i) if(maps[i].address==address) return &maps[i];
 return NULL;
}
static int copyin(user_addr_t address,void *destination,size_t n) {
 assert(picasso_fault_lock==0 && (scenario>=56 ? lines%7==4 : lines%6==3));++reads;
 if((scenario==25 && address==0x1000) ||
    (scenario==28 && address==UINT64_C(0x10000ab0c)) ||
    (scenario==29 && address==0x9000) ||
    (scenario==30 && address==0xa000)) return EFAULT;
 if((scenario==43 && address==UINT64_C(0x10000ab0c)) ||
    (scenario==44 && address==0x9000) ||
    (scenario==45 && address==0x1000)) { memset(destination,0xa5,n);return EFAULT; }
 struct mapping *m=find(address);
 if(!m || m->size<n) return EFAULT;
 memcpy(destination,m->data,n); return 0;
}
static int copyinstr(user_addr_t address,char *destination,size_t capacity,size_t *copied) {
 assert(picasso_fault_lock==0 && (scenario>=56 ? lines%7==4 : lines%6==3));++reads;
 if((scenario==33 && address==0xb000) || (scenario==34 && address==0x8000)) return EFAULT;
 struct mapping *m=find(address);
 if(!m) return EFAULT;
 for(size_t i=0;i<capacity;++i) {
  if(i>=m->size) return EFAULT;
  destination[i]=((char*)m->data)[i];
  if(!destination[i]) { *copied=i+1;return 0; }
 }
 return ENAMETOOLONG;
}
static void contains(const char *needle) {
 if(!strstr(output,needle)) { fprintf(stderr,"scenario %d missing: %s\n%s",scenario,needle,output);abort(); }
}
static void reset_output(void) { bytes=lines=0; output[0]=0; }
int main(int argc,char **argv) {
 assert(argc==2); scenario=atoi(argv[1]);
 struct host_task task={0xabc,0x9000,UINT64_C(0x100000000)},other_task={0xdef,0x9000,0};
 if(scenario==51) task.cached_slide=0;
 if(scenario==52) task.cached_slide=UINT32_MAX;
 if(scenario==53) task.main_header=UINT64_C(0x100847000);
 if(scenario==54) task.main_header=0;
 if(scenario==55) task.main_header=UINT64_C(0xfedcba9876543210);
 const uint64_t captured_main_header=task.main_header;
 struct reason reason={OS_REASON_SIGNAL,SIGTRAP};
 struct proc proc={95,3,98765,&reason,(task_t)&task,"watchdogd"},other=proc;
 arm_saved_state_t state={0}; state.is64=1;state.pc=UINT64_C(0x10000ab0c);state.lr=0x1234;state.sp=0x4560;state.fp=0x1000;
 for(unsigned i=0;i<29;++i) state.x[i]=0x3000+i;
 state.x[0]=0x8000;
 thread.tid=42;cp=&proc;ct=(task_t)&task;
 uint32_t instruction=0xd4200000;
 struct user64_dyld_all_image_infos info={0};info.version=17;info.infoArrayCount=1;info.infoArray=0xa000;
 struct user64_dyld_image_info image={UINT64_C(0x100000000),0xb000,0};
 uint64_t frames[4][2]={{0x1100,0x5100},{0x1200,0x5200},{0x1300,0x5300},{0,0x5400}};
 char message[256]="IOWatchdog is missing",path[128]="/usr/libexec/watchdogd";
 if(scenario==53) {
  image.imageLoadAddress=UINT64_C(0x19022b000);
  strcpy(path,"/System/Library/Frameworks/IOKit.framework/IOKit");
 }
 map(state.pc,&instruction,sizeof(instruction));
 map(UINT64_C(0x10000ab54),&instruction,sizeof(instruction));
 map(0x9000,&info,sizeof(info));map(0xa000,&image,sizeof(image));
 map(0xb000,path,sizeof(path));map(0x8000,message,sizeof(message));
 for(unsigned i=0;i<4;++i) map(0x1000+i*0x100,frames[i],sizeof(frames[i]));
 int rv=scenario==1 ? 9 : SIGTRAP;
 if(scenario==3) state.is64=0;
 if(scenario==4) state.cpsr=4;
 if(scenario==5) cp=NULL;
 if(scenario==6) picasso_fault_lock=1;
 if(scenario==22) state.fp=0x1001;
 if(scenario==23) frames[0][0]=0x1000;
 if(scenario==24) frames[0][0]=0x101010;
 if(scenario==26) frames[0][0]=0;
 if(scenario==31) info.infoArrayCount=0;
 if(scenario==32) info.infoArray=0;
 if(scenario==35) memset(message,'A',sizeof(message));
 if(scenario==36) memset(path,'B',sizeof(path));
 if(scenario==37) { strcpy(message,"line1\nline2\t\200");strcpy(path,"bad\rpath");strcpy(proc.p_name,"watch\ndog\377"); }
 if(scenario==40) state.fp=0;
 if(scenario==41) state.pc=0;
 if(scenario==42) {frames[0][0]=0x101000;frames[1][0]=0;map(0x101000,frames[1],sizeof(frames[1]));}
 if(scenario==39) picasso_user_breakpoint(NULL,0xf2000000,task.cached_slide,task.main_header);
 else if(scenario!=2) picasso_user_breakpoint(&state,0xf2000000,task.cached_slide,task.main_header);
 assert(!reads && !bytes && !lines);
 if(scenario!=2 && scenario!=3 && scenario!=4 && scenario!=5 && scenario!=6 && scenario!=39) {
  assert(picasso_faults[0].valid && picasso_faults[0].pc==state.pc && picasso_faults[0].lr==state.lr && picasso_faults[0].sp==state.sp && picasso_faults[0].fp==state.fp);
  assert(picasso_faults[0].slide==(uint64_t)task.cached_slide);
  assert(picasso_faults[0].main_header==captured_main_header);
  for(unsigned i=0;i<29;++i) assert(picasso_faults[0].x[i]==state.x[i]);
 }
 cp=&proc;picasso_fault_lock=0;
 if(scenario>=56) {
  strcpy(proc.p_name,"backboardd");
  strcpy(host_exit_description,"reason\nline\t\200");
  if(scenario==56) {
   picasso_report_service_exit(&proc,rv);
   contains("PICASSO_SERVICE exit sample=1 pid=95 name=backboardd");
   contains("description=reason?line??");contains("record=breakpoint-snapshot");
   assert(lines==7 && reads<=8);goto pass;
  }
  if(scenario==57) {strcpy(proc.p_name,"backboardd-helper");picasso_report_service_exit(&proc,rv);assert(!lines && !reads);goto pass;}
  if(scenario==58) {rv=78<<8;picasso_report_service_exit(&proc,rv);contains("status=78");contains("record=not-breakpoint-exit");assert(lines==2 && !reads);goto pass;}
  if(scenario==59) {reason.osr_code=9;picasso_report_service_exit(&proc,9);contains("exit_signal=9 namespace=2 code=9");contains("record=not-breakpoint-exit");assert(lines==2 && !reads);goto pass;}
  if(scenario==60) {++thread.tid;picasso_report_service_exit(&proc,rv);contains("record=missing");assert(lines==2 && !reads);goto pass;}
  if(scenario==61) {proc.p_exit_reason=NULL;picasso_report_service_exit(&proc,rv);contains("description=\n");contains("namespace=0 code=0");assert(lines==2 && !reads);goto pass;}
  if(scenario==62) {
   const char *names[]={"backboardd","SpringBoard","mediaserverd","audiomxd","CommCenter","powerd","mobiletimerd"};
   for(unsigned n=0;n<7;++n) {
    strcpy(proc.p_name,names[n]);
    for(unsigned i=0;i<20;++i) {++proc.pid;picasso_report_service_exit(&proc,0);}
   }
   assert(lines==56 && !reads);contains("sample=4");assert(!strstr(output,"sample=5"));
   /* Service quotas must not consume the critical-exit quota. */
   picasso_report_critical_fault(&proc,0);assert(lines==57);contains("PICASSO_CRITICAL sample=1");goto pass;
  }
  if(scenario==63) {memset(host_exit_description,'A',sizeof(host_exit_description));host_exit_description[199]=0;picasso_report_service_exit(&proc,0);assert(lines==2 && !reads);char *s=strstr(output,"description=")+12;assert(strchr(s,'\n')-s==160);goto pass;}
 }
 if(scenario==7) picasso_fault_lock=1;
 if(scenario==8) ++proc.pid;
 if(scenario==9) ++proc.pidversion;
 if(scenario==10) ++proc.uniqueid;
 if(scenario==11) {ct=(task_t)&other_task;proc.task=ct;}
 if(scenario==12) ++thread.tid;
 if(scenario==13) cp=&other;
 if(scenario==14) proc.task=(task_t)&other_task;
 if(scenario==15) rv=0;
 if(scenario==47) rv=127;
 if(scenario==48) rv=0x100;
 if(scenario==49) rv=0x85;
 if(scenario==50) rv=0x89;
 if(scenario==16) reason.osr_namespace=4;
 if(scenario==17) reason.osr_code=6;
 if(scenario==18) proc.p_exit_reason=NULL;
 if(scenario==20) {
  for(unsigned i=0;i<8;++i) {++proc.pidversion;picasso_user_breakpoint(&state,0xf2000000,task.cached_slide,task.main_header);}
  proc.pidversion=3;assert(picasso_fault_filled==8);
 }
 if(scenario==21) {state.pc=UINT64_C(0x10000ab54);picasso_user_breakpoint(&state,0xf2000000,task.cached_slide,task.main_header);}
 if(scenario==38) {
  for(unsigned i=0;i<8;++i) {picasso_user_breakpoint(&state,0xf2000000,task.cached_slide,task.main_header);picasso_report_critical_fault(&proc,SIGTRAP);}
  assert(lines==48);unsigned prior_reads=reads,prior_bytes=bytes;
  picasso_user_breakpoint(&state,0xf2000000,task.cached_slide,task.main_header);picasso_report_critical_fault(&proc,SIGTRAP);
  assert(reads==prior_reads && bytes==prior_bytes && lines==48);
  contains("sample=8");goto pass;
 }
 state.pc=0xdeadbeef;state.lr=0xfefefefe;state.x[0]=0;task.cached_slide=0xffff;
 task.main_header=UINT64_C(0x123456789abcdef0);
 picasso_report_critical_fault(&proc,rv);
 if((scenario>=2 && scenario<=6) || (scenario>=8 && scenario<=12) || scenario==20 || scenario==39) {
  contains("record=missing");assert(lines==1 && reads==0);goto pass;
 }
 if((scenario>=13 && scenario<=18) || scenario==47 || scenario==48) {contains("record=not-breakpoint-exit");assert(lines==1 && reads==0);goto pass;}
 if(scenario==7) {contains("record=busy");assert(picasso_fault_lock==1 && lines==1 && reads==0);goto pass;}
 contains("record=breakpoint-snapshot");assert(lines==6);
 if(scenario==51) contains("cache_slide=0\n");
 else if(scenario==52) contains("cache_slide=ffffffff\n");
 else contains("cache_slide=abc\n");
 if(scenario==53) {
  contains("main_header=100847000 cache_slide=");
  contains("image0_kind=dyld-first image0=19022b000 ");
  contains("path=/System/Library/Frameworks/IOKit.framework/IOKit\n");
 } else if(scenario==54) {
  contains("main_header=0 cache_slide=");
  contains("image0_kind=dyld-first image0=100000000 ");
 } else if(scenario==55) {
  contains("main_header=fedcba9876543210 cache_slide=");
  contains("image0_kind=dyld-first image0=100000000 ");
 } else contains("main_header=100000000 cache_slide=");
 assert(!strstr(output,"123456789abcdef0"));
 contains("LR=1234");contains("x0=8000");assert(!strstr(output,"deadbeef"));
 if(scenario==1) contains("exit_signal=9 namespace=2 code=5");
 if(scenario==19) {unsigned r=reads;reset_output();picasso_report_critical_fault(&proc,rv);contains("record=missing");assert(reads==r && lines==1);goto pass;}
 if(scenario==21) contains("PC=10000ab54");
 else if(scenario==41) contains("PC=0 ");
 else contains("PC=10000ab0c");
 if(scenario==22) contains("frames=0 error=0 stop=unaligned");
 if(scenario==23 || scenario==24) contains("frames=1 error=0 stop=range");
 if(scenario==25 || scenario==45) contains("frames=0 error=14 stop=copy-error 0/0");
 if(scenario==26) contains("frames=1 error=0 stop=end");
 if(scenario==27 || scenario==0) contains("frames=3 error=0 stop=limit");
 if(scenario==28 || scenario==41 || scenario==43) contains("insn=0 insn_error=14");
 if(scenario==29 || scenario==30 || scenario==44) contains("image0=0 image_error=14 path_error=-1");
 if(scenario==31 || scenario==32) contains("image0=0 image_error=2 path_error=-1");
 if(scenario==33) contains("path_error=14 path=\n");
 if(scenario==34) contains("x0str_error=14 candidate_text=\n");
 if(scenario==35) contains("x0str_error=63 candidate_text=\n");
 if(scenario==36) contains("path_error=63 path=\n");
 if(scenario==37) {contains("name=watch?dog?");contains("path=bad?path");contains("candidate_text=line1?line2??");}
 if(scenario==40) contains("frames=0 error=0 stop=end");
 if(scenario==42) contains("frames=2 error=0 stop=end");
 assert(reads<=8);
pass:
 assert(bytes<16000);
 unsigned newline_count=0;
 for(unsigned i=0;i<bytes;++i) if(output[i]=='\n') ++newline_count;
 assert(newline_count==lines);
 fprintf(stdout,"PASS scenario=%d reads=%u lines=%u bytes=%u\n",scenario,reads,lines,bytes);
 return 0;
}
