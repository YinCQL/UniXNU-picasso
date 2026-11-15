/* Execute the production boot-addon authorization against a hostile VFS model.
 * SHA-256 is the host OpenSSL implementation, not an identity comparison stub. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <openssl/sha.h>
#define EAUTH 80
#define VREG 1
#define MNT_RDONLY 1
#define Z_WAITOK 1
#define UIO_READ 1
#define UIO_SYSSPACE 1
#define IO_NOAUTH 1
#define FSIOC_AUTH_FS 0x80084118UL
#define NULLVP NULL
typedef char *caddr_t;
typedef void *vfs_context_t;
struct vfsstatfs { char f_fstypename[16],f_mntonname[128],f_mntfromname[128]; };
struct mount { unsigned flags; struct vfsstatfs stat; };typedef struct mount *mount_t;
struct vnode_attr { unsigned va_mode,va_uid,va_gid,va_nlink;uint64_t va_data_size; };
struct vnode { int type;mount_t mp;struct vnode_attr attr;uint8_t *bytes; };typedef struct vnode *vnode_t;
static struct mount mounts[3];static struct vnode nodes[6];static vnode_t rootvnode=&nodes[0];
static uint8_t binary_data[2][16*1024*1024];
static size_t binary_sizes[2];
static unsigned refs,allocs;static bool owner,device_exists,attr_supported;static int attr_error,read_error,short_read,back_error,allocation_error;
#define VATTR_INIT(v) memset(v,0,sizeof(*(v)))
#define VATTR_WANTED(v,f) ((void)(v))
#define VATTR_IS_SUPPORTED(v,f) (attr_supported)
#define os_atomic_load(p,o) (*(p))
#define os_atomic_inc_orig(p,o) ((*(p))++)
static uint8_t picasso_bt_runtime_hashes[2][32];static uint64_t picasso_bt_runtime_sizes[2];
static unsigned picasso_bt_runtime_ready;
static int native_gpu_addon_files,native_gpu_addon_failed,native_gpu_addon_error;
static int vnode_vtype(vnode_t v){return v->type;}
static mount_t vnode_mount(vnode_t v){return v->mp;}
static unsigned vfs_flags(mount_t m){return m->flags;}
static struct vfsstatfs *vfs_statfs(mount_t m){return &m->stat;}
static vnode_t vfs_devvp(mount_t m){assert(m==&mounts[0]);if(!device_exists)return NULL;++refs;return &nodes[1];}
static dev_t vnode_specrdev(vnode_t v){assert(v==&nodes[1]);return 23;}
static bool native_verified_block_root_authorized(dev_t d){assert(d==23);return owner;}
static int vnode_getattr(vnode_t v,struct vnode_attr *a,vfs_context_t c){assert(c==(void *)1);*a=v->attr;return attr_error;}
static void *kalloc_data(size_t n,int f){assert(n==4096 && f==Z_WAITOK);if(allocation_error)return NULL;++allocs;return malloc(n);}
static void kfree_data(void *p,size_t n){assert(p && n==4096 && allocs);--allocs;free(p);}
static void vnode_put(vnode_t v){assert(v && refs);--refs;}
static void *vfs_context_ucred(vfs_context_t c){return c;}
static void *vfs_context_proc(vfs_context_t c){return c;}
static int vn_rdwr(int op,vnode_t v,caddr_t data,int n,off_t off,int space,int flags,void *cred,int *resid,void *p){
    assert(op==UIO_READ && n>0 && n<=4096 && off>=0 && (uint64_t)(off+n)<=v->attr.va_data_size && space==UIO_SYSSPACE && flags==IO_NOAUTH && cred==(void *)1 && p==(void *)1);
    if(read_error)return read_error;memcpy(data,v->bytes+off,(size_t)n);*resid=short_read;return 0;
}
static vnode_t backing;
int bindfs_getbackingvnode(vnode_t v,vnode_t *out){assert(v==&nodes[2]);if(back_error)return back_error;*out=backing;++refs;return 0;}
#include "runtime-capture.inc"
#include "runtime-auth.inc"
typedef struct {vnode_t authvp;} fsioc_auth_fs_t;
struct vnop_ioctl_args {unsigned long a_command;void *a_data;int a_fflag;vnode_t a_vp;vfs_context_t a_context;};
#include "runtime-ioctl.inc"
static void reset(void){
    assert(!refs && !allocs);memset(mounts,0,sizeof mounts);memset(nodes,0,sizeof nodes);
    owner=device_exists=attr_supported=true;attr_error=read_error=short_read=back_error=allocation_error=0;
    rootvnode=&nodes[0];rootvnode->mp=&mounts[0];mounts[0].flags=MNT_RDONLY;strcpy(mounts[0].stat.f_fstypename,"apfs");
    nodes[2].mp=&mounts[1];nodes[2].type=VREG;mounts[1].flags=MNT_RDONLY;strcpy(mounts[1].stat.f_fstypename,"bindfs");
    strcpy(mounts[1].stat.f_mntonname,"/usr/sbin");strcpy(mounts[1].stat.f_mntfromname,"/private/var/ngpu/bluetooth-bin");
    native_gpu_addon_files=19;native_gpu_addon_failed=native_gpu_addon_error=0;picasso_bt_runtime_ready=1;
    for(unsigned i=0;i<2;++i){vnode_t v=&nodes[3+i];v->mp=&mounts[2];v->type=VREG;v->attr.va_mode=0755;v->attr.va_nlink=1;v->attr.va_data_size=binary_sizes[i];v->bytes=binary_data[i];
        picasso_bt_runtime_sizes[i]=0;assert(!picasso_bt_runtime_capture(v->bytes,binary_sizes[i],19,14+i,true,true,-1));picasso_bt_runtime_vnodes[i]=v;}
    backing=&nodes[3];
}
static int run(void){fsioc_auth_fs_t auth={0};struct vnop_ioctl_args a={FSIOC_AUTH_FS,&auth,0,&nodes[2],(void *)1};int e=bindfs_ioctl(&a);assert(!refs && !allocs);return e;}
int main(int argc,char **argv){
    assert(argc==3);
    for(unsigned i=0;i<2;++i){FILE *f=fopen(argv[i+1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0 && n<=16*1024*1024);assert(!fseek(f,0,SEEK_SET));binary_sizes[i]=(size_t)n;assert(fread(binary_data[i],1,binary_sizes[i],f)==binary_sizes[i]);assert(!fclose(f));}
    assert(binary_sizes[0]==8930688 && binary_sizes[0]>8*1024*1024 && binary_sizes[1]==69344);
    for(unsigned i=0;i<2;++i){
        assert(!picasso_bt_runtime_capture(binary_data[i],binary_sizes[i],19,14+i,true,true,0));
        assert(picasso_bt_runtime_sizes[i]==binary_sizes[i]);
        uint8_t digest[32];SHA256(binary_data[i],binary_sizes[i],digest);assert(!memcmp(digest,picasso_bt_runtime_hashes[i],32));
    }
    for(unsigned i=0;i<9;++i){picasso_bt_runtime_sizes[0]=0;const uint8_t *p=binary_data[0];uint64_t size=binary_sizes[0];unsigned count=19,index=14;bool audio=true,gpu=true;int error=0;
        switch(i){case 0:p=NULL;break;case 1:size=0;break;case 2:size=16*1024*1024+1;break;case 3:count=13;break;case 4:index=13;break;case 5:index=16;break;case 6:audio=false;break;case 7:gpu=false;break;case 8:error=EIO;break;}
        assert(picasso_bt_runtime_capture(p,size,count,index,audio,gpu,error)==EAUTH && !picasso_bt_runtime_sizes[0]);
    }
    assert(!picasso_bt_runtime_capture(binary_data[0],16*1024*1024,19,14,true,true,-1));
    reset();assert(!run());backing=&nodes[4];assert(!run());
    for(unsigned i=0;i<26;++i){reset();switch(i){
        case 0:picasso_bt_runtime_ready=0;break;case 1:native_gpu_addon_files=13;break;
        case 2:native_gpu_addon_failed=1;break;case 3:native_gpu_addon_error=EIO;break;
        case 4:nodes[2].type=2;break;case 5:rootvnode=NULL;break;case 6:mounts[1].flags=0;break;case 7:mounts[0].flags=0;break;
        case 8:strcpy(mounts[1].stat.f_fstypename,"apfs");break;case 9:strcpy(mounts[0].stat.f_fstypename,"fake");break;
        case 10:strcpy(mounts[1].stat.f_mntonname,"/usr/bin");break;case 11:strcat(mounts[1].stat.f_mntfromname,"x");break;
        case 12:device_exists=false;break;case 13:owner=false;break;case 14:backing=&nodes[5];break;
        case 15:nodes[3].attr.va_uid=501;break;case 16:nodes[3].attr.va_gid=501;break;case 17:nodes[3].attr.va_mode=0777;break;
        case 18:nodes[3].attr.va_nlink=2;break;case 19:--nodes[3].attr.va_data_size;break;case 20:nodes[3].bytes[4096]^=1;break;
        case 21:read_error=EIO;break;case 22:short_read=1;break;case 23:allocation_error=1;break;case 24:attr_supported=false;break;case 25:back_error=EIO;break;}
        assert(run()!=0);
    }
    reset();fsioc_auth_fs_t auth={0};struct vnop_ioctl_args a={0,&auth,0,&nodes[2],(void *)1};assert(bindfs_ioctl(&a)==ENOTSUP);
    a.a_command=FSIOC_AUTH_FS;a.a_fflag=1;assert(bindfs_ioctl(&a)==ENOTSUP);a.a_fflag=0;auth.authvp=&nodes[0];assert(bindfs_ioctl(&a)==ENOTSUP);auth.authvp=NULL;a.a_data=NULL;assert(bindfs_ioctl(&a)==ENOTSUP);
    puts("PASS exact boot identity, mounted view, root authorization, tampering, ownership and I/O failures");
}
