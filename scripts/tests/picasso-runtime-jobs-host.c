/* Actual pre-launchd migration with injected I/O failures and crash remnants.
 * SHA API is an exact fixture preimage map; Python verifies production SHA constants.
 */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef void *proc_t;typedef uintptr_t user_addr_t;typedef void *vfs_context_t;typedef char *caddr_t;
enum {Z_WAITOK=1,VNODE_LOOKUP_NOFOLLOW=1,VREG=1,VDIR=2,FREAD=1,FWRITE=2,O_CREAT=4,O_EXCL=8,O_NOFOLLOW=16,UIO_READ=1,UIO_WRITE=2,UIO_SYSSPACE=1,IO_NOAUTH=2,MNT_WAIT=1};
struct vnode_attr {unsigned va_uid,va_gid,va_mode,va_nlink;uint64_t va_data_size;};
struct vnode {int type,exists;struct vnode_attr attr;uint8_t data[8192];};typedef struct vnode *vnode_t;
static struct vnode nodes[5];static unsigned refs,allocs,ops,failAt,renames,writes;static int shortWrite,corruptRead;
#define NULLVP NULL
#define VATTR_INIT(v) memset(v,0,sizeof(*(v)))
#define VATTR_WANTED(v,f) ((void)(v))
#define VATTR_IS_SUPPORTED(v,f) (1)
static int fail(void){return ++ops==failAt;}
static void *kalloc_data(size_t n,int flag){assert(n==8192 && flag==1);if(fail())return NULL;++allocs;return malloc(n);}
static void kfree_data(void *p,size_t n){assert(n==8192);--allocs;free(p);}
static vfs_context_t vfs_context_current(void){return (void *)1;}
static void *vfs_context_ucred(vfs_context_t c){return c;}
static void *vfs_context_proc(vfs_context_t c){(void)c;return (void *)2;}
static unsigned indexof(const char *p){
    if(!strcmp(p,"/private/var"))return 0;if(!strcmp(p,"/private/var/db"))return 1;
    if(!strcmp(p,"/private/var/db/com.apple.xpc.launchd"))return 2;
    if(!strcmp(p,"/private/var/db/com.apple.xpc.launchd/disabled.plist"))return 3;
    assert(!strcmp(p,"/private/var/db/com.apple.xpc.launchd/.picasso-disabled.tmp"));return 4;
}
static int vnode_lookup(const char *p,int flags,vnode_t *v,vfs_context_t c){assert(c==(void *)1 && flags==VNODE_LOOKUP_NOFOLLOW);if(fail())return EIO;unsigned n=indexof(p);if(!nodes[n].exists)return ENOENT;*v=&nodes[n];++refs;return 0;}
static void vnode_put(vnode_t v){assert(v && refs);--refs;}
static int vnode_vtype(vnode_t v){return v->type;}
static int vnode_getattr(vnode_t v,struct vnode_attr *a,vfs_context_t c){assert(c==(void *)1);if(fail())return EIO;*a=v->attr;return 0;}
static int vnode_open(const char *p,int flags,int mode,int context,vnode_t *v,vfs_context_t c){
    assert(c==(void *)1 && !context);if(fail())return EIO;unsigned n=indexof(p);
    if(flags&FWRITE){assert(flags==(FWRITE|O_CREAT|O_EXCL|O_NOFOLLOW) && mode==0600 && n==4);if(nodes[n].exists)return EEXIST;memset(&nodes[n],0,sizeof nodes[n]);nodes[n].exists=1;nodes[n].type=VREG;nodes[n].attr.va_nlink=1;nodes[n].attr.va_mode=0600;}
    else {assert(flags==(FREAD|O_NOFOLLOW));if(!nodes[n].exists)return ENOENT;}
    *v=&nodes[n];++refs;return 0;
}
static int vnode_close(vnode_t v,int flags,vfs_context_t c){assert(v && c==(void *)1 && (flags==FREAD || flags==FWRITE));--refs;return fail()?EIO:0;}
static int vn_rdwr(int dir,vnode_t v,caddr_t data,int size,int off,int seg,int flags,void *cred,int *resid,proc_t p){
    assert(size<=8192 && !off && seg==UIO_SYSSPACE && flags==IO_NOAUTH && cred==(void *)1 && p==(void *)2);if(fail())return EIO;
    *resid=dir==UIO_WRITE && shortWrite ? 1:0;
    if(dir==UIO_WRITE){++writes;v->attr.va_data_size=(unsigned)(size-*resid);memcpy(v->data,data,(size_t)(size-*resid));}
    else {assert(dir==UIO_READ && (uint64_t)size==v->attr.va_data_size);memcpy(data,v->data,(size_t)size);if(corruptRead && v==&nodes[4])data[0]^=1;}
    return 0;
}
static int VNOP_FSYNC(vnode_t v,int flags,vfs_context_t c){assert(v && flags==MNT_WAIT && c==(void *)1);return fail()?EIO:0;}
struct unlink_args {user_addr_t path;};struct rename_args {user_addr_t from,to;};
static char copied[2][512];
static int copyout(const void *p,user_addr_t a,size_t n){assert((a==0x1000 || a==0x1200) && n<512);if(fail())return EFAULT;memcpy(copied[(a-0x1000)/512],p,n);return 0;}
static int host_unlink(proc_t p,struct unlink_args *a,int32_t *r){assert(p==(void *)2 && a->path==0x1000 && r && indexof(copied[0])==4);if(fail())return EIO;nodes[4].exists=0;return 0;}
static int host_rename(proc_t p,struct rename_args *a,int32_t *r){assert(p==(void *)2 && r && a->from==0x1000 && a->to==0x1200 && indexof(copied[0])==4 && indexof(copied[1])==3);if(fail())return EIO;assert(nodes[4].exists);nodes[3]=nodes[4];nodes[4].exists=0;++renames;return 0;}
#define unlink host_unlink
#define rename host_rename
#include "jobs-data.inc"
typedef struct {const uint8_t *data;size_t size;} SHA256_CTX;
static void SHA256_Init(SHA256_CTX *s){memset(s,0,sizeof *s);}
static void SHA256_Update(SHA256_CTX *s,const void *p,size_t n){s->data=p;s->size=n;}
static void SHA256_Final(uint8_t out[32],SHA256_CTX *s);
#include "network-reader.inc"
#include "runtime-jobs.inc"
static void SHA256_Final(uint8_t out[32],SHA256_CTX *s){
    memset(out,0,32);
    if(s->size==sizeof jobs_before && !memcmp(s->data,jobs_before,s->size))memcpy(out,picasso_jobs_before_sha,32);
    if(s->size==sizeof jobs_after && !memcmp(s->data,jobs_after,s->size))memcpy(out,picasso_jobs_after_sha,32);
}
static void setup(void){assert(!refs && !allocs);memset(nodes,0,sizeof nodes);for(unsigned i=0;i<4;++i){nodes[i].exists=1;nodes[i].type=i==3?VREG:VDIR;nodes[i].attr.va_mode=i==3?0644:0755;nodes[i].attr.va_nlink=1;}nodes[3].attr.va_data_size=sizeof jobs_before;memcpy(nodes[3].data,jobs_before,sizeof jobs_before);ops=renames=writes=failAt=0;shortWrite=corruptRead=0;}
static int is_after(void){return nodes[3].attr.va_data_size==sizeof jobs_after && !memcmp(nodes[3].data,jobs_after,sizeof jobs_after);}
static int is_before(void){return nodes[3].attr.va_data_size==sizeof jobs_before && !memcmp(nodes[3].data,jobs_before,sizeof jobs_before);}
static int run(void){int e=picasso_jobs_enable((void *)2,0x1000);assert(!refs && !allocs);return e;}
int main(void){
    setup();assert(!run() && is_after() && renames==1);unsigned max=ops;ops=0;assert(!run() && renames==1);
    for(unsigned at=1;at<=max;++at){setup();failAt=at;assert(run()!=0);assert(is_before() || is_after());failAt=0;assert(!run() && is_after());}
    setup();shortWrite=1;assert(run()==EIO && is_before());shortWrite=0;assert(!run() && is_after());
    setup();corruptRead=1;assert(run()==EIO && is_before());corruptRead=0;assert(!run() && is_after());
    for(unsigned n=0;n<5;++n){for(unsigned bad=0;bad<4;++bad){setup();if(n==4){nodes[4]=nodes[3];nodes[4].attr.va_data_size=1;}if(bad==0)nodes[n].type=9;if(bad==1)nodes[n].attr.va_uid=501;if(bad==2)nodes[n].attr.va_gid=501;if(bad==3)nodes[n].attr.va_mode|=2;assert(run()!=0 && is_before() && !renames);}}
    setup();nodes[3].attr.va_nlink=2;assert(run()==EPERM && is_before());
    setup();nodes[3].data[0]^=1;assert(run()==EINVAL && !writes && !renames);
    setup();nodes[4]=nodes[3];nodes[4].attr.va_data_size=1;assert(!run() && is_after());
    printf("PASS runtime jobs: %u injected operation failures/recovery, partial temp, short writes, readback, ownership/types, unknown policy and idempotence\n",max);
}
