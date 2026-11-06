// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define DEVELOPMENT 1
#define DEBUG 0
#define OS_REASON_COREANIMATION 17
typedef uintptr_t user_addr_t;
typedef struct proc {char p_name[32];} *proc_t;
static char logline[2048];static unsigned logs, reads;static int failed;
static int proc_getpid(proc_t p) {(void)p;return 42;}
static int copyin(user_addr_t p,void *out,size_t n) {++reads;if(failed)return -1;memcpy(out,(void *)p,n);return 0;}
static int hostprintf(const char *f,...) {++logs;va_list a;va_start(a,f);int n=vsnprintf(logline,sizeof logline,f,a);va_end(a);return n;}
#define printf hostprintf
// ABORT_HELPER
#undef printf
int main(void) {
    struct proc p={"SpringBoard"};char payload[600];memset(payload,'A',sizeof payload);payload[0]='\n';
    picasso_abort_payload_text(&p,1,0,(user_addr_t)payload,sizeof payload);assert(!logs&&!reads);
    memcpy(p.p_name,"other",sizeof "other");picasso_abort_payload_text(&p,17,0,(user_addr_t)payload,sizeof payload);assert(!logs&&!reads);
    memcpy(p.p_name,"backboardd",sizeof "backboardd");
    for(unsigned n=0;n<4;++n) {
        const unsigned sizes[]={0,511,512,600};
        picasso_abort_payload_text(&p,17,123,(user_addr_t)payload,sizes[n]);
        assert(logs==n+1 && strlen(logline)<sizeof logline);
        assert(n ? !strstr(logline,"text=\n") : strstr(logline,"text=\n")!=NULL);
    }
    failed=1;picasso_abort_payload_text(&p,17,1,(user_addr_t)1,20);assert(logs==5&&strstr(logline,"text=\n"));
    for(unsigned n=0;n<10;++n)picasso_abort_payload_text(&p,17,1,0,0);
    assert(logs==8 && reads==4);
    puts("PASS abort payload bounds, filtering, copy failure and eight-record limit");
}
