#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <setjmp.h>
#define DEVELOPMENT 1
#define DEBUG 0
#define DTYPE_VNODE 1
typedef void *proc_t;
typedef void *vnode_t;
typedef unsigned u_int;
struct ucred {int unused;};
struct fileglob {int type;void *data;};
#define FILEGLOB_DTYPE(fg) ((fg)->type)
static void *fg_get_data(struct fileglob *fg){return fg->data;}
static proc_t current_proc(void){return (void *)1;}
static int proc_selfpid(void){return 26;}
static unsigned diagnosticTicket,logs,calls,busy,unbusy,callTrace,resultTrace;
unsigned picasso_inject_load_ticket(proc_t p,vnode_t v){assert(p==(void *)1 && v==(void *)2);return diagnosticTicket;}
static int trace(const char *fmt,...){assert(strstr(fmt,"mmap_policy"));++logs;return 0;}
#define printf trace
static jmp_buf panicJump;
static void panic(const char *why){assert(strstr(why,"increased"));longjmp(panicJump,1);}
struct mac_policy_ops {int (*mpo_file_check_mmap)(struct ucred *,struct fileglob *,void *,int,int,uint64_t,int *);};
struct mac_policy_conf {struct mac_policy_ops *mpc_ops;const char *mpc_name;};
static struct {unsigned staticmax,maxindex;struct {struct mac_policy_conf *mpc;} entries[5];} mac_policy_list;
static int mac_policy_list_conditional_busy(void){return busy;}
static void mac_policy_list_unbusy(void){++unbusy;}
#define DTRACE_MACF3(...) (++callTrace)
#define DTRACE_MACF2(...) (++resultTrace)
#include "ui-mac.inc"
#undef printf
static int errors[3],masks[3],sequence[3];
static int hook(unsigned index,struct ucred *cred,struct fileglob *fg,void *label,int prot,int flags,uint64_t offset,int *maxp){
    assert(cred && fg && !label && prot==5 && flags==2 && offset==4096);
    sequence[calls++]=index;*maxp&=masks[index];return errors[index];
}
#define HOOK(n) static int hook##n(struct ucred *c,struct fileglob *f,void *l,int p,int g,uint64_t o,int *m){return hook(n,c,f,l,p,g,o,m);}
HOOK(0) HOOK(1) HOOK(2)
static int increase(struct ucred *c,struct fileglob *f,void *l,int p,int g,uint64_t o,int *m){(void)c;(void)f;(void)l;(void)p;(void)g;(void)o;*m=15;return 0;}
int main(void){
    struct mac_policy_ops ops[4]={{hook0},{hook1},{hook2},{0}};
    struct mac_policy_conf conf[4]={{&ops[0],"first"},{&ops[1],"second"},{&ops[2],"dynamic"},{&ops[3],"nohook"}};
    mac_policy_list.staticmax=3;mac_policy_list.maxindex=4;
    mac_policy_list.entries[0].mpc=&conf[0];mac_policy_list.entries[1].mpc=&conf[3];
    mac_policy_list.entries[2].mpc=&conf[1];mac_policy_list.entries[4].mpc=&conf[2];
    struct ucred cred={0};struct fileglob fg={DTYPE_VNODE,(void *)2};
    const int values[]={0,EPERM,EACCES,ENOENT,ESRCH,EINVAL,EDEADLK,EIO};unsigned scenarios=0;
    for(unsigned dynamic=0;dynamic<2;++dynamic)for(unsigned a=0;a<8;++a)for(unsigned b=0;b<8;++b)for(unsigned c=0;c<8;++c){
        errors[0]=values[a];errors[1]=values[b];errors[2]=values[c];masks[0]=7;masks[1]=5;masks[2]=1;busy=dynamic;
        int expected=0,expectedMax=7;
        for(unsigned ticket=0;ticket<2;++ticket){
            calls=logs=unbusy=callTrace=resultTrace=0;diagnosticTicket=ticket;int maxp=7;
            int error=mac_file_check_mmap(&cred,&fg,5,2,4096,&maxp);
            assert(calls==2+dynamic && sequence[0]==0 && sequence[1]==1);
            assert(callTrace==calls && resultTrace==calls && logs==(ticket?calls:0) && unbusy==dynamic);
            if(!ticket){expected=error;expectedMax=maxp;}else assert(error==expected && maxp==expectedMax);
        }++scenarios;
    }
    ops[0].mpo_file_check_mmap=increase;masks[1]=masks[2]=15;
    for(unsigned ticket=0;ticket<2;++ticket){diagnosticTicket=ticket;calls=0;int maxp=7;
        if(!setjmp(panicJump)){mac_file_check_mmap(&cred,&fg,5,2,4096,&maxp);assert(0);} }
    printf("PASS MAC: %u static/dynamic/error-precedence cases, same DTrace and maxprot rejection\n",scenarios);
}
