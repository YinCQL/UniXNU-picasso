#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef void *proc_t;typedef uintptr_t user_addr_t;typedef void *vfs_context_t;
enum {Z_WAITOK=1,UIO_SYSSPACE=1,UIO_READ=1,VNODE_LOOKUP_NOFOLLOW=1,VREG=1,VDIR=2};
struct vnode_attr {unsigned va_mode,va_uid,va_gid;};
struct vnode {int type;struct vnode_attr attr;};typedef struct vnode *vnode_t;
static struct vnode nodes[3];
#define NULLVP NULL
#define VATTR_INIT(va) memset(va,0,sizeof(*(va)))
#define VATTR_SET(va,name,value) ((va)->name=(value))
#define VATTR_WANTED(va,name) ((void)(va))
#define VATTR_IS_SUPPORTED(va,name) (1)
#define CAST_USER_ADDR_T(p) ((uintptr_t)(p))
struct dirent {uint64_t ino;uint16_t d_reclen,d_namlen;char d_name[256];};
struct uio {uintptr_t buffer;size_t residual;uint64_t offset;};typedef struct uio *uio_t;
static unsigned allocs,refs,reads,mounts,setattrs,lookups;
static bool runtime_test;
static int native_gpu_addon_files, native_gpu_addon_failed, native_gpu_addon_error;
static int failLookup=-1,failSet=-1,failGet=-1,badReadback,failAlloc,dirMode,readError;
static char requested[128]="/usr/lib/libpicasso-sb.dylib";
static void *kalloc_data(size_t n,int flag){assert(flag==Z_WAITOK);if(failAlloc)return NULL;++allocs;return calloc(1,n);}
static void kfree_data(void *p,size_t n){assert(n==4096);--allocs;free(p);}
static uio_t uio_create(int n,int off,int seg,int direction){assert(n==1 && !off && seg==1 && direction==1);++allocs;return calloc(1,sizeof(struct uio));}
static void uio_free(uio_t u){--allocs;free(u);}
static uint64_t uio_offset(uio_t u){return u->offset;}
static size_t uio_resid(uio_t u){return u->residual;}
static void uio_reset(uio_t u,uint64_t off,int seg,int dir){assert(seg==1 && dir==1);u->offset=off;}
static int uio_addiov(uio_t u,uintptr_t p,size_t n){u->buffer=p;u->residual=n;return 0;}
static vfs_context_t vfs_context_current(void){return (void *)1;}
static int vnode_lookup(const char *p,int flags,vnode_t *out,vfs_context_t ctx){
    assert(ctx==(void *)1 && flags==VNODE_LOOKUP_NOFOLLOW);unsigned n=lookups++;
    assert(n<3 && strstr(p,runtime_test ? "/private/var/ngpu" : "/private/var/picasso"));if((int)n==failLookup)return ENOENT;*out=&nodes[n];++refs;return 0;
}
static void vnode_put(vnode_t v){assert(v);--refs;}
static int vnode_vtype(vnode_t v){return v->type;}
static int vnode_setattr(vnode_t v,struct vnode_attr *a,vfs_context_t ctx){assert(ctx==(void *)1);++setattrs;if(v-nodes==failSet)return EPERM;v->attr=*a;return 0;}
static int vnode_getattr(vnode_t v,struct vnode_attr *a,vfs_context_t ctx){assert(ctx==(void *)1);if(v-nodes==failGet)return EIO;*a=v->attr;if(badReadback)a->va_uid=501;return 0;}
static size_t add(char *p,const char *name){struct dirent *d=(struct dirent *)p;size_t n=strlen(name);d->d_namlen=(uint16_t)n;d->d_reclen=(uint16_t)((offsetof(struct dirent,d_name)+n+8)&~7U);memcpy(d->d_name,name,n+1);return d->d_reclen;}
static int VNOP_READDIR(vnode_t v,uio_t u,int flags,int *eof,int *entries,vfs_context_t ctx){
    assert(v==&nodes[1] && flags==0 && ctx==(void *)1);++reads;if(readError)return EIO;
    char *p=(char *)u->buffer;size_t used=0;*eof=dirMode!=5;*entries=3;
    if(dirMode!=3){used+=add(p+used,".");used+=add(p+used,"..");}
    if(dirMode!=1 && dirMode!=3)used+=add(p+used,"libpicasso-sb.dylib");
    if(dirMode==2)used+=add(p+used,"dyld");
    if(dirMode==4)((struct dirent *)p)->d_reclen=4095;
    u->residual-=used;u->offset+=used;return 0;
}
static int PE_parse_boot_argn(const char *name,void *out,size_t n){
    if (!strcmp(name,"picasso-audio-addon") || !strcmp(name,"sheng-gpu-addon")) {
        assert(n==sizeof(uint32_t)); *(uint32_t *)out=runtime_test; return 1;
    }
    assert(!strcmp(name,"picasso_inject_sb"));snprintf(out,n,"%s",requested);return 1;
}
static void native_gpu_addon_mount(proc_t p,user_addr_t scratch,user_addr_t offset,const char *source,size_t ss,const char *point,size_t ps,const char *visible,const char *own,volatile int *stage,volatile int *status){
    assert(p==(void *)2 && scratch==0x1000 && offset==0xc00);
    assert(!strcmp(source,runtime_test ? "/private/var/ngpu/ui" : "/private/var/picasso/usr-lib") && ss==strlen(source)+1);
    assert(!strcmp(point,"/usr/lib") && ps==strlen(point)+1);
    assert(!strcmp(visible,"/usr/lib/dyld") && !strcmp(own,"/usr/lib/libpicasso-sb.dylib"));
    ++mounts;*stage=5;*status=0;
}
#include "ui-view.inc"
static void reset(void){
    assert(!refs && !allocs);reads=mounts=setattrs=lookups=0;failLookup=failSet=failGet=-1;
    badReadback=failAlloc=dirMode=readError=0;
    native_gpu_addon_failed=native_gpu_addon_error=0;native_gpu_addon_files=runtime_test?13:12;
    memset(nodes,0,sizeof nodes);nodes[0].type=nodes[1].type=VDIR;nodes[2].type=VREG;
}
int main(void){
 for(int profile=0;profile<2;++profile){
    runtime_test=profile;snprintf(requested,sizeof requested,"/usr/lib/libpicasso-sb.dylib");
    reset();picasso_ui_library_mount((void *)2,0x1000);assert(mounts==1 && !refs && !allocs && setattrs==3);
    assert(nodes[0].attr.va_mode==0755 && nodes[2].attr.va_mode==0644 && !nodes[2].attr.va_uid);
    for(unsigned n=0;n<3;++n){
        reset();failLookup=(int)n;picasso_ui_library_mount((void *)2,0x1000);assert(!mounts);
        reset();nodes[n].type=9;picasso_ui_library_mount((void *)2,0x1000);assert(!mounts);
        reset();failSet=(int)n;picasso_ui_library_mount((void *)2,0x1000);assert(!mounts);
        reset();failGet=(int)n;picasso_ui_library_mount((void *)2,0x1000);assert(!mounts);
    }
    for(int mode=1;mode<=5;++mode){reset();dirMode=mode;picasso_ui_library_mount((void *)2,0x1000);assert(!mounts && reads<=4);}
    reset();badReadback=1;picasso_ui_library_mount((void *)2,0x1000);assert(!mounts);
    reset();failAlloc=1;picasso_ui_library_mount((void *)2,0x1000);assert(!mounts);
    reset();readError=1;picasso_ui_library_mount((void *)2,0x1000);assert(!mounts);
    if(runtime_test){
        reset();native_gpu_addon_failed=1;picasso_ui_library_mount((void *)2,0x1000);assert(!mounts && !lookups);
        reset();native_gpu_addon_error=EIO;picasso_ui_library_mount((void *)2,0x1000);assert(!mounts && !lookups);
    }
    reset();snprintf(requested,sizeof requested,"/private/var/other.dylib");picasso_ui_library_mount((void *)2,0x1000);assert(!mounts && !lookups);
 }
    assert(!refs && !allocs);puts("PASS UI view: exact library, malformed/missing/extra directory entries, type/permission/readback failures");
}
