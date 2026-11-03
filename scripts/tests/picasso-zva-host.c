// SPDX-License-Identifier: BSD-2-Clause-Patent

#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
typedef uint64_t user_addr_t;
typedef struct {uint64_t pc,x[4];bool is64,user;} arm_saved_state_t;
static uint64_t code_pc=0x2156521f4ULL;
static bool fail_zva,fail_stores;
static unsigned reads,observed;
static const char *name="backboardd";
static bool is_saved_state64(arm_saved_state_t *s){return s->is64;}
static bool get_saved_state_cpsr(arm_saved_state_t *s){return s->user;}
#define PSR64_IS_USER64(x) (x)
static uint64_t get_saved_state_pc(arm_saved_state_t *s){return s->pc;}
static uint64_t get_saved_state_reg(arm_saved_state_t *s,unsigned r){assert(r<4);return s->x[r];}
static void set_saved_state_pc(arm_saved_state_t *s,uint64_t v){s->pc=v;}
struct process { const char *p_name; };
typedef struct process *proc_t;
static struct process process;
static bool no_process;
static int test_pid=50;
static proc_t current_proc(void){process.p_name=name;return no_process ? NULL : &process;}
static int proc_getpid(proc_t p){assert(p==&process);return test_pid;}
#include "process.inc"
static uint32_t zva_bytes[4]={0xd50b7423,0x91010063,0xf1010042,0x54ffffa8};
static uint32_t store_bytes[13]={0xa8000461,0xa8010461,0xa8020461,0xa8030461,0x91010063,0xf1010042,0x54ffff48,0x8b020063,0xa8000461,0xa8010461,0xa8020461,0xa8030461,0xd65f03c0};
static int copyin(user_addr_t addr,void *out,size_t size){
 ++reads;
 if(addr==code_pc){assert(size==sizeof(zva_bytes));memcpy(out,zva_bytes,size);return fail_zva;}
 assert(addr==code_pc+0x78 && size==sizeof(store_bytes));memcpy(out,store_bytes,size);return fail_stores;
}
static void picasso_zva_observe(uint64_t a,uint64_t pc,uint64_t target,int pid){assert(a==0x11128f040 && pc==code_pc && target==code_pc+0x78 && pid==test_pid);++observed;}
#include "zva.inc"
int main(void){
 unsigned cases=0;
 arm_saved_state_t valid={0x2156521f4,{0x11128f000,0,0x27e420,0x11128f040},true,true};
 for(unsigned i=0;i<10;++i){
  arm_saved_state_t s=valid;
  if(i==0)s.is64=false;if(i==1)s.user=false;if(i==2)s.pc+=4;if(i==3)s.x[1]=1;
  if(i==4)s.x[2]=0;if(i==5)s.x[3]+=1;if(i==6)name="other";
  if(i==7)fail_zva=true;if(i==8)fail_stores=true;if(i==9)zva_bytes[0]^=1;
  arm_saved_state_t before=s;
  assert(!picasso_user_zva(&s,0x92000061,s.x[3],0x43b50000));
  assert(!memcmp(&s,&before,sizeof(s)) && observed==0);
  name="backboardd";fail_zva=fail_stores=false;if(i==9)zva_bytes[0]^=1;++cases;
 }
 for(unsigned i=0;i<13;++i){arm_saved_state_t s=valid;store_bytes[i]^=1;
  assert(!picasso_user_zva(&s,0x92000061,s.x[3],0x43b50000));assert(s.pc==valid.pc);
  assert(!memcmp(&s,&valid,sizeof(s)));store_bytes[i]^=1;++cases;}
 for(unsigned i=0;i<32;++i){arm_saved_state_t s=valid;
  assert(!picasso_user_zva(&s,0x92000061^(1U<<i),s.x[3],0x43b50000));assert(!memcmp(&s,&valid,sizeof(s)));++cases;}
 arm_saved_state_t s=valid;assert(!picasso_user_zva(&s,0x92000061,s.x[3]+64,0x43b50000));
 assert(!picasso_user_zva(&s,0x92000061,s.x[3],0));cases+=2;
 for(unsigned i=0;i<3;++i){s=valid;no_process=(i==0);test_pid=(i==2 ? -1 : 0);
  unsigned before_reads=reads,before_observed=observed;
  assert(!picasso_user_zva(&s,0x92000061,s.x[3],0x43b50000));
  assert(!memcmp(&s,&valid,sizeof(s)) && reads==before_reads && observed==before_observed);++cases;}
 no_process=false;test_pid=50;
 const char *names[]={"backboardd","SpringBoard","Preferences","mediaserverd"};
 for(unsigned i=0;i<4;++i){s=valid;name=names[i];assert(picasso_user_zva(&s,0x92000061,s.x[3],0x43b50000));
  assert(s.pc==valid.pc+0x78 && !memcmp(s.x,valid.x,sizeof(s.x)));++cases;}
 assert(observed==4);printf("PASS ZVA qualification: %u cases\n",cases);
}
