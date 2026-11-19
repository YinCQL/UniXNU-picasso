/* Actual pre-launchd migration with injected I/O failures and crash remnants.
 * SHA uses the Python standard library oracle, including reordered policy bytes.
 */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
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
    char path[1024], command[2300];
    int n=snprintf(path,sizeof path,"%s.input",JOBS_SHA_SCRIPT);assert(n>0 && (size_t)n<sizeof path);
    FILE *f=fopen(path,"wb");assert(f && fwrite(s->data,1,s->size,f)==s->size);assert(!fclose(f));
    n=snprintf(command,sizeof command,"python3 \"%s\" \"%s\"",JOBS_SHA_SCRIPT,path);assert(n>0 && (size_t)n<sizeof command);
    assert(!system(command));strcat(path,".sha");f=fopen(path,"rb");assert(f && fread(out,1,32,f)==32 && fgetc(f)==EOF);assert(!fclose(f));
}

static void setup(void){assert(!refs && !allocs);memset(nodes,0,sizeof nodes);for(unsigned i=0;i<4;++i){nodes[i].exists=1;nodes[i].type=i==3?VREG:VDIR;nodes[i].attr.va_mode=i==3?0644:0755;nodes[i].attr.va_nlink=1;}nodes[3].attr.va_data_size=sizeof jobs_before;memcpy(nodes[3].data,jobs_before,sizeof jobs_before);ops=renames=writes=failAt=0;shortWrite=corruptRead=0;}
static int is_after(void){return nodes[3].attr.va_data_size==sizeof jobs_after && !memcmp(nodes[3].data,jobs_after,sizeof jobs_after);}
static int is_before(void){if(nodes[3].attr.va_data_size==sizeof jobs_bluetooth && !memcmp(nodes[3].data,jobs_bluetooth,sizeof jobs_bluetooth))return 1;if(nodes[3].attr.va_data_size==sizeof jobs_legacy && !memcmp(nodes[3].data,jobs_legacy,sizeof jobs_legacy))return 1;return nodes[3].attr.va_data_size==sizeof jobs_before && !memcmp(nodes[3].data,jobs_before,sizeof jobs_before);}
static int run(void){int e=picasso_jobs_enable((void *)2,0x1000);assert(!refs && !allocs);return e;}
static void parser_tests(void){
    uint8_t data[8192],original[8192];size_t size;unsigned removed;
    /* Reorder two entries, add an unrelated false override, then preserve it. */
    const char *head="<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>";
    const char *tail="\n</dict>\n</plist>\n";
    const char *keep="\n <key>com.example.retained</key>\r\n <false/>";
    const char *drop="\n <key>com.apple.bluetoothd</key>\r\n <true/>";
    int n=snprintf((char *)data,sizeof data,"%s%s%s%s",head,drop,keep,tail);assert(n>0);size=(size_t)n;
    assert(!picasso_jobs_transform(data,&size,&removed) && removed==1);
    n=snprintf((char *)original,sizeof original,"%s%s%s",head,keep,tail);assert(size==(size_t)n && !memcmp(data,original,size));
    assert(!picasso_jobs_transform(data,&size,&removed) && !removed);
    const char *near="\n <key>com.apple.mobile.lockdown.extra</key>\r\n <true/>";
    const char *owner="\n <key>com.apple.mobile.lockdown</key>\r\n <false/>";
    n=snprintf((char *)data,sizeof data,"%s%s%s%s%s",head,keep,owner,near,tail);size=(size_t)n;
    assert(!picasso_jobs_transform(data,&size,&removed) && removed==1);
    n=snprintf((char *)original,sizeof original,"%s%s%s%s",head,keep,near,tail);assert(size==(size_t)n && !memcmp(data,original,size));
    const char *bad[]={"<key>x</key><true/><key>x</key><false/>","<key>x</key><integer>1</integer>","<key></key><true/>","<key>x&amp;y</key><true/>","<key>x</key>","<key>com.apple.bluetoothd</key><true/><key>x</key><dict/>"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];++i){n=snprintf((char *)data,sizeof data,"%s%s%s",head,bad[i],tail);size=(size_t)n;memcpy(original,data,size);assert(picasso_jobs_transform(data,&size,&removed)==EINVAL && size==(size_t)n && !memcmp(data,original,size));}
    memcpy(data,jobs_before,sizeof jobs_before);
    for(size_t cut=0;cut<sizeof jobs_before-1;++cut){size=cut;memcpy(original,data,size);assert(picasso_jobs_transform(data,&size,&removed)==EINVAL && size==cut && !memcmp(data,original,size));}
}
int main(void){
    parser_tests();
    setup();assert(!run() && is_after() && renames==1);unsigned max=ops;ops=0;assert(!run() && renames==1);
    for(unsigned at=1;at<=max;++at){setup();failAt=at;assert(run()!=0);assert(is_before() || is_after());failAt=0;assert(!run() && is_after());}
    setup();nodes[3].attr.va_data_size=sizeof jobs_legacy;memcpy(nodes[3].data,jobs_legacy,sizeof jobs_legacy);assert(!run() && is_after());
    setup();nodes[3].attr.va_data_size=sizeof jobs_bluetooth;memcpy(nodes[3].data,jobs_bluetooth,sizeof jobs_bluetooth);assert(!run() && is_after());
    unsigned bluetoothMax=ops;
    for(unsigned at=1;at<=bluetoothMax;++at){setup();nodes[3].attr.va_data_size=sizeof jobs_bluetooth;memcpy(nodes[3].data,jobs_bluetooth,sizeof jobs_bluetooth);failAt=at;assert(run()!=0 && (is_before() || is_after()));failAt=0;assert(!run() && is_after());}
    setup();nodes[3].attr.va_data_size=sizeof jobs_legacy;memcpy(nodes[3].data,jobs_legacy,sizeof jobs_legacy);assert(!run() && is_after());
    unsigned legacyMax=ops;
    for(unsigned at=1;at<=legacyMax;++at){setup();nodes[3].attr.va_data_size=sizeof jobs_legacy;memcpy(nodes[3].data,jobs_legacy,sizeof jobs_legacy);failAt=at;assert(run()!=0 && (is_before() || is_after()));failAt=0;assert(!run() && is_after());}
    setup();shortWrite=1;assert(run()==EIO && is_before());shortWrite=0;assert(!run() && is_after());
    setup();corruptRead=1;assert(run()==EIO && is_before());corruptRead=0;assert(!run() && is_after());
    for(unsigned n=0;n<5;++n){for(unsigned bad=0;bad<4;++bad){setup();if(n==4){nodes[4]=nodes[3];nodes[4].attr.va_data_size=1;}if(bad==0)nodes[n].type=9;if(bad==1)nodes[n].attr.va_uid=501;if(bad==2)nodes[n].attr.va_gid=501;if(bad==3)nodes[n].attr.va_mode|=2;assert(run()!=0 && is_before() && !renames);}}
    setup();nodes[3].attr.va_nlink=2;assert(run()==EPERM && is_before());
    setup();nodes[3].data[0]^=1;assert(run()==EINVAL && !writes && !renames);
    setup();nodes[4]=nodes[3];nodes[4].attr.va_data_size=1;assert(!run() && is_after());
    setup();nodes[3].attr.va_data_size=sizeof jobs_device;memcpy(nodes[3].data,jobs_device,sizeof jobs_device);
    assert(!run() && nodes[3].attr.va_data_size==sizeof jobs_device_after && !memcmp(nodes[3].data,jobs_device_after,sizeof jobs_device_after));
    unsigned deviceMax=ops;assert(!run() && renames==1);
    for(unsigned at=1;at<=deviceMax;++at){
        setup();nodes[3].attr.va_data_size=sizeof jobs_device;memcpy(nodes[3].data,jobs_device,sizeof jobs_device);failAt=at;
        assert(run()!=0);
        assert((nodes[3].attr.va_data_size==sizeof jobs_device && !memcmp(nodes[3].data,jobs_device,sizeof jobs_device)) ||
            (nodes[3].attr.va_data_size==sizeof jobs_device_after && !memcmp(nodes[3].data,jobs_device_after,sizeof jobs_device_after)));
        failAt=0;assert(!run() && nodes[3].attr.va_data_size==sizeof jobs_device_after && !memcmp(nodes[3].data,jobs_device_after,sizeof jobs_device_after));
    }
    printf("PASS runtime jobs: %u injected operation failures/recovery, partial temp, short writes, readback, ownership/types, unknown policy and idempotence\n",max);
}
