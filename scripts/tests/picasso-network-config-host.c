/* Replayed boot configuration with failing filesystem/mount calls. No device I/O. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef void *proc_t;typedef uintptr_t user_addr_t;typedef void *vfs_context_t;
enum {Z_WAITOK=1,UIO_SYSSPACE=1,UIO_READ=1,VNODE_LOOKUP_NOFOLLOW=1,VREG=1,VDIR=2};
struct vnode_attr {unsigned va_mode,va_uid,va_gid;};
struct vnode {int type,exists;struct vnode_attr attr;};typedef struct vnode *vnode_t;
static struct vnode nodes[3];
#define NULLVP NULL
#define VATTR_INIT(va) memset(va,0,sizeof(*(va)))
#define VATTR_SET(va,name,value) ((va)->name=(value))
#define VATTR_WANTED(va,name) ((void)(va))
#define VATTR_IS_SUPPORTED(va,name) (1)
#define CAST_USER_ADDR_T(p) ((uintptr_t)(p))
struct dirent {uint64_t ino;uint16_t d_reclen,d_namlen;char d_name[256];};
struct uio {uintptr_t buffer;size_t residual;uint64_t offset;};typedef struct uio *uio_t;
static unsigned allocs,refs,reads,mounts,writes,mkdirs;
static int scenario,badDigest,dirMode;
static char copied[192],upper[2048];static size_t upperSize;
static void *kalloc_data(size_t n,int flag){assert(flag==Z_WAITOK);if(scenario==13)return NULL;++allocs;return calloc(1,n);}
static void kfree_data(void *p,size_t n){assert(n==4096);--allocs;free(p);}
static uio_t uio_create(int n,int off,int seg,int direction){assert(n==1 && !off && seg==1 && direction==1);++allocs;return calloc(1,sizeof(struct uio));}
static void uio_free(uio_t u){--allocs;free(u);}
static uint64_t uio_offset(uio_t u){return u->offset;}
static size_t uio_resid(uio_t u){return u->residual;}
static void uio_reset(uio_t u,uint64_t off,int seg,int dir){assert(seg==1 && dir==1);u->offset=off;}
static int uio_addiov(uio_t u,uintptr_t p,size_t n){u->buffer=p;u->residual=n;return 0;}
static vfs_context_t vfs_context_current(void){return (void *)1;}
static unsigned pathIndex(const char *p){if(!strcmp(p,"/private/var/picasso"))return 0;if(!strcmp(p,"/private/var/picasso/ifnamer"))return 1;assert(!strcmp(p,"/private/var/picasso/ifnamer/Info.plist"));return 2;}
static int vnode_lookup(const char *p,int flags,vnode_t *out,vfs_context_t ctx){
    assert(ctx==(void *)1 && flags==VNODE_LOOKUP_NOFOLLOW);unsigned n=pathIndex(p);
    if(!nodes[n].exists)return ENOENT;*out=&nodes[n];++refs;return 0;
}
static void vnode_put(vnode_t v){assert(v);--refs;}
static int vnode_vtype(vnode_t v){return v->type;}
static int vnode_setattr(vnode_t v,struct vnode_attr *a,vfs_context_t ctx){assert(ctx==(void *)1);if(scenario==9)return EPERM;v->attr=*a;return 0;}
static int vnode_getattr(vnode_t v,struct vnode_attr *a,vfs_context_t ctx){assert(ctx==(void *)1);if(scenario==10)return EIO;*a=v->attr;if(scenario==11)a->va_uid=501;return 0;}
static size_t add(char *p,const char *name){struct dirent *d=(struct dirent *)p;size_t n=strlen(name);d->d_namlen=(uint16_t)n;d->d_reclen=(uint16_t)((offsetof(struct dirent,d_name)+n+8)&~7U);memcpy(d->d_name,name,n+1);return d->d_reclen;}
static int VNOP_READDIR(vnode_t v,uio_t u,int flags,int *eof,int *entries,vfs_context_t ctx){
    assert(v==&nodes[1] && !flags && ctx==(void *)1);++reads;if(scenario==12)return EIO;
    char *p=(char *)u->buffer;size_t used=0;*eof=dirMode!=5;*entries=3;
    if(dirMode!=3){used+=add(p+used,".");used+=add(p+used,"..");}
    if(dirMode!=1 && dirMode!=3)used+=add(p+used,"Info.plist");
    if(dirMode==2)used+=add(p+used,"InterfaceNamer");
    if(dirMode==4)((struct dirent *)p)->d_reclen=4095;
    u->residual-=used;u->offset+=used;return 0;
}
struct mkdir_args {user_addr_t path;unsigned mode;};
static int copyout(const void *data,user_addr_t addr,size_t n){assert(addr==0x1000 && n<sizeof copied);if(scenario==7)return EFAULT;memcpy(copied,data,n);return 0;}
static int mkdir(proc_t p,struct mkdir_args *a,int32_t *result){assert(p==(void *)2 && a->mode==0755 && result);++mkdirs;if(scenario==8)return EACCES;unsigned n=pathIndex(copied);nodes[n].type=VDIR;nodes[n].exists=1;return 0;}
static int native_gpu_addon_write(const char *p,const uint8_t *data,uint64_t size,uint32_t mode,vfs_context_t ctx){
    assert(pathIndex(p)==2 && mode==0644 && ctx==(void *)1 && size<sizeof upper);++writes;
    if(scenario==14)return EIO;nodes[2].exists=1;nodes[2].type=VREG;memcpy(upper,data,(size_t)size);upperSize=(size_t)size;return scenario==1?-1:0;
}
static int picasso_ifnamer_read(const char *p,uint8_t *data,size_t cap,uint64_t *size,vfs_context_t ctx){
    assert(strstr(p,"/System/Library/SystemConfiguration/InterfaceNamer.bundle/Info.plist") && cap==2048 && ctx==(void *)1);
    if(scenario==2 || (mounts && scenario==16))return EIO;
    if(mounts){*size=upperSize;memcpy(data,upper,upperSize);if(scenario==17)data[0]^=1;}
    else {*size=scenario==3?842:843;memset(data,0,(size_t)*size);}return 0;
}
typedef int SHA256_CTX;
static void SHA256_Init(SHA256_CTX *c){*c=1;}
static void SHA256_Update(SHA256_CTX *c,const void *b,size_t n){assert(*c==1 && b && (n==843 || n==842));}
static void SHA256_Final(uint8_t *out,SHA256_CTX *c){assert(*c==1);static const uint8_t digest[32]={0x06,0x65,0x4c,0xe2,0x0e,0xd8,0x9d,0x9c,0xcd,0x7a,0xa8,0xe6,0x8f,0xce,0x05,0x0b,0xd7,0xd2,0xac,0x0f,0x9e,0x4d,0xed,0xcd,0xb6,0x15,0xa6,0x2f,0xa0,0xa9,0xc7,0xa9};memcpy(out,digest,32);if(badDigest)out[0]^=1;}
static void native_gpu_addon_mount(proc_t p,user_addr_t scratch,user_addr_t offset,const char *source,size_t ss,const char *point,size_t ps,const char *visible,const char *own,volatile int *stage,volatile int *status){
    assert(p==(void *)2 && scratch==0x1000 && offset==0xc00 && pathIndex(source)==1 && ss==strlen(source)+1);
    assert(!strcmp(point,"/System/Library/SystemConfiguration/InterfaceNamer.bundle") && ps==strlen(point)+1);
    assert(strstr(visible,"/_CodeSignature/CodeResources") && strstr(own,"/Info.plist"));
    ++mounts;*stage=scenario==15?2:5;*status=scenario==15?EPERM:0;
}
#include "network-config.inc"
int main(void){
    for(scenario=0;scenario<23;++scenario){
        assert(!refs && !allocs);reads=mounts=writes=mkdirs=0;badDigest=scenario==4;dirMode=scenario>=18?scenario-17:0;memset(nodes,0,sizeof nodes);
        if(scenario==5){nodes[0].exists=1;nodes[0].type=9;}
        if(scenario==6){nodes[0].exists=1;nodes[0].type=VDIR;nodes[1].exists=1;nodes[1].type=9;}
        picasso_ifnamer_mount((void *)2,0x1000);
        assert(mounts==(scenario<=1 || (scenario>=15 && scenario<=17)));
        if(scenario<=1)assert(writes==1 && mkdirs==2 && nodes[2].attr.va_mode==0644 && !nodes[2].attr.va_uid);
        assert(!refs && !allocs && reads<=4);
    }
    puts("PASS InterfaceNamer config: 23 original-identity, metadata, directory, write, mount and readback cases");
}
