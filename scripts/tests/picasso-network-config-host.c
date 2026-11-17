/* Exercise real startup helpers against exact stock metadata and failing I/O. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <openssl/sha.h>
#define EAUTH 80
typedef void *proc_t; typedef uintptr_t user_addr_t; typedef void *vfs_context_t;
enum { Z_WAITOK=1,UIO_SYSSPACE=1,UIO_READ=1,VNODE_LOOKUP_NOFOLLOW=1,VREG=1,VDIR=2,DT_REG=8,DT_DIR=4,MNT_RDONLY=1,MNT_UNION=32 };
struct vnode_attr {unsigned va_mode,va_uid,va_gid;};
struct vnode {int type,exists;struct vnode_attr attr;unsigned flags;};typedef struct vnode *vnode_t;typedef struct vnode *mount_t;
#define NULLVP NULL
#define VATTR_INIT(v) memset(v,0,sizeof(*(v)))
#define VATTR_SET(v,n,x) ((v)->n=(x))
#define VATTR_WANTED(v,n) ((void)(v))
#define VATTR_IS_SUPPORTED(v,n) (1)
#define CAST_USER_ADDR_T(p) ((uintptr_t)(p))
struct dirent {uint64_t ino;uint16_t d_reclen;uint8_t d_namlen,d_type;char d_name[256];};
struct uio {uintptr_t buffer;size_t residual;uint64_t offset;};typedef struct uio *uio_t;
struct mkdir_args {user_addr_t path;unsigned mode;};struct mount_args {user_addr_t type,path;unsigned flags;user_addr_t data;};
static const uint8_t stock_0[] = {98,112,108,105,115,116,48,48,223,16,23,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,29,33,35,36,37,38,40,24,41,43,44,45,46,27,92,67,70,66,117,110,100,108,101,78,97,109,101,87,68,84,88,99,111,100,101,89,68,84,83,68,75,78,97,109,101,90,68,84,83,68,75,66,117,105,108,100,95,16,25,67,70,66,117,110,100,108,101,68,101,118,101,108,111,112,109,101,110,116,82,101,103,105,111,110,95,16,15,67,70,66,117,110,100,108,101,86,101,114,115,105,111,110,95,16,19,66,117,105,108,100,77,97,99,104,105,110,101,79,83,66,117,105,108,100,94,68,84,80,108,97,116,102,111,114,109,78,97,109,101,95,16,19,67,70,66,117,110,100,108,101,80,97,99,107,97,103,101,84,121,112,101,95,16,26,67,70,66,117,110,100,108,101,83,104,111,114,116,86,101,114,115,105,111,110,83,116,114,105,110,103,95,16,26,67,70,66,117,110,100,108,101,83,117,112,112,111,114,116,101,100,80,108,97,116,102,111,114,109,115,95,16,29,67,70,66,117,110,100,108,101,73,110,102,111,68,105,99,116,105,111,110,97,114,121,86,101,114,115,105,111,110,95,16,18,67,70,66,117,110,100,108,101,69,120,101,99,117,116,97,98,108,101,90,68,84,67,111,109,112,105,108,101,114,95,16,28,85,73,82,101,113,117,105,114,101,100,68,101,118,105,99,101,67,97,112,97,98,105,108,105,116,105,101,115,95,16,16,77,105,110,105,109,117,109,79,83,86,101,114,115,105,111,110,95,16,18,67,70,66,117,110,100,108,101,73,100,101,110,116,105,102,105,101,114,94,85,73,68,101,118,105,99,101,70,97,109,105,108,121,87,66,117,105,108,116,105,110,95,16,17,68,84,80,108,97,116,102,111,114,109,86,101,114,115,105,111,110,92,68,84,88,99,111,100,101,66,117,105,108,100,95,16,17,67,70,66,117,110,100,108,101,83,105,103,110,97,116,117,114,101,95,16,15,68,84,80,108,97,116,102,111,114,109,66,117,105,108,100,95,16,44,99,111,109,46,97,112,112,108,101,46,83,121,115,116,101,109,67,111,110,102,105,103,117,114,97,116,105,111,110,46,73,110,116,101,114,102,97,99,101,78,97,109,101,114,84,49,52,51,48,95,16,21,105,112,104,111,110,101,111,115,49,54,46,55,46,105,110,116,101,114,110,97,108,86,50,48,72,51,52,51,87,69,110,103,108,105,115,104,84,49,46,50,49,89,50,50,65,51,56,48,48,49,55,88,105,112,104,111,110,101,111,115,84,66,78,68,76,161,34,88,105,80,104,111,110,101,79,83,83,54,46,48,94,73,110,116,101,114,102,97,99,101,78,97,109,101,114,95,16,34,99,111,109,46,97,112,112,108,101,46,99,111,109,112,105,108,101,114,115,46,108,108,118,109,46,99,108,97,110,103,46,49,95,48,161,39,85,97,114,109,54,52,84,49,54,46,55,161,42,16,1,9,86,49,54,46,55,46,51,88,49,52,69,54,48,57,55,100,84,63,63,63,63,0,8,0,57,0,70,0,78,0,88,0,99,0,127,0,145,0,167,0,182,0,204,0,233,1,6,1,38,1,59,1,70,1,101,1,120,1,141,1,156,1,164,1,184,1,197,1,217,1,235,2,26,2,31,2,55,2,62,2,70,2,75,2,85,2,94,2,99,2,101,2,110,2,114,2,129,2,166,2,168,2,174,2,179,2,181,2,183,2,184,2,191,2,200,0,0,0,0,0,0,2,1,0,0,0,0,0,0,0,47,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,205};
static const uint8_t stock_1[] = {250,222,12,2,0,0,0,173,0,2,1,0,0,0,0,2,0,0,0,153,0,0,0,48,0,0,0,3,0,0,0,1,0,0,6,71,20,1,0,0,0,0,0,0,0,0,0,0,99,111,109,46,97,112,112,108,101,46,83,121,115,116,101,109,67,111,110,102,105,103,117,114,97,116,105,111,110,46,73,110,116,101,114,102,97,99,101,78,97,109,101,114,0,28,173,156,221,9,161,165,198,18,101,24,36,190,149,30,229,129,226,234,0,58,117,246,219,5,133,41,20,142,20,221,126,161,180,114,156,192,158,201,115,184,127,234,229,158,70,196,49,114,49,145,143,176,200,81,162,45,207,202,147,184,127,234,229,158,70,196,49,114,49,145,143,176,200,81,162,45,207,202,147};
static const uint8_t stock_2[] = {250,222,12,1,0,0,0,12,0,0,0,0};
static const uint8_t stock_3[] = {250,222,12,2,0,0,0,221,0,2,1,0,0,0,0,2,0,0,0,189,0,0,0,48,0,0,0,3,0,0,0,1,0,0,6,71,32,2,0,0,0,0,0,0,0,0,0,0,99,111,109,46,97,112,112,108,101,46,83,121,115,116,101,109,67,111,110,102,105,103,117,114,97,116,105,111,110,46,73,110,116,101,114,102,97,99,101,78,97,109,101,114,0,61,233,237,66,171,146,246,107,44,96,51,181,156,207,164,65,177,208,80,195,25,218,238,181,87,85,27,77,165,19,20,126,152,121,32,144,78,171,101,14,117,120,140,5,74,160,176,82,78,106,128,191,199,26,163,45,248,210,55,166,23,67,249,134,240,73,246,6,11,68,215,84,85,142,142,31,36,136,0,6,205,43,102,111,51,10,195,198,251,251,1,112,114,194,4,47,240,73,246,6,11,68,215,84,85,142,142,31,36,136,0,6,205,43,102,111,51,10,195,198,251,251,1,112,114,194,4,47};
static const uint8_t stock_4[] = {60,63,120,109,108,32,118,101,114,115,105,111,110,61,34,49,46,48,34,32,101,110,99,111,100,105,110,103,61,34,85,84,70,45,56,34,63,62,10,60,33,68,79,67,84,89,80,69,32,112,108,105,115,116,32,80,85,66,76,73,67,32,34,45,47,47,65,112,112,108,101,47,47,68,84,68,32,80,76,73,83,84,32,49,46,48,47,47,69,78,34,32,34,104,116,116,112,58,47,47,119,119,119,46,97,112,112,108,101,46,99,111,109,47,68,84,68,115,47,80,114,111,112,101,114,116,121,76,105,115,116,45,49,46,48,46,100,116,100,34,62,10,60,112,108,105,115,116,32,118,101,114,115,105,111,110,61,34,49,46,48,34,62,10,60,100,105,99,116,62,10,9,60,107,101,121,62,102,105,108,101,115,60,47,107,101,121,62,10,9,60,100,105,99,116,47,62,10,9,60,107,101,121,62,102,105,108,101,115,50,60,47,107,101,121,62,10,9,60,100,105,99,116,47,62,10,9,60,107,101,121,62,114,117,108,101,115,60,47,107,101,121,62,10,9,60,100,105,99,116,62,10,9,9,60,107,101,121,62,94,46,42,60,47,107,101,121,62,10,9,9,60,116,114,117,101,47,62,10,9,9,60,107,101,121,62,94,46,42,92,46,108,112,114,111,106,47,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,111,112,116,105,111,110,97,108,60,47,107,101,121,62,10,9,9,9,60,116,114,117,101,47,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,49,48,48,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,46,42,92,46,108,112,114,111,106,47,108,111,99,118,101,114,115,105,111,110,46,112,108,105,115,116,36,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,111,109,105,116,60,47,107,101,121,62,10,9,9,9,60,116,114,117,101,47,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,49,49,48,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,66,97,115,101,92,46,108,112,114,111,106,47,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,49,48,49,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,118,101,114,115,105,111,110,46,112,108,105,115,116,36,60,47,107,101,121,62,10,9,9,60,116,114,117,101,47,62,10,9,60,47,100,105,99,116,62,10,9,60,107,101,121,62,114,117,108,101,115,50,60,47,107,101,121,62,10,9,60,100,105,99,116,62,10,9,9,60,107,101,121,62,46,42,92,46,100,83,89,77,40,36,124,47,41,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,49,49,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,40,46,42,47,41,63,92,46,68,83,95,83,116,111,114,101,36,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,111,109,105,116,60,47,107,101,121,62,10,9,9,9,60,116,114,117,101,47,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,50,48,48,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,46,42,60,47,107,101,121,62,10,9,9,60,116,114,117,101,47,62,10,9,9,60,107,101,121,62,94,46,42,92,46,108,112,114,111,106,47,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,111,112,116,105,111,110,97,108,60,47,107,101,121,62,10,9,9,9,60,116,114,117,101,47,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,49,48,48,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,46,42,92,46,108,112,114,111,106,47,108,111,99,118,101,114,115,105,111,110,46,112,108,105,115,116,36,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,111,109,105,116,60,47,107,101,121,62,10,9,9,9,60,116,114,117,101,47,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,49,49,48,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,66,97,115,101,92,46,108,112,114,111,106,47,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,49,48,49,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,73,110,102,111,92,46,112,108,105,115,116,36,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,111,109,105,116,60,47,107,101,121,62,10,9,9,9,60,116,114,117,101,47,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,50,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,80,107,103,73,110,102,111,36,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,111,109,105,116,60,47,107,101,121,62,10,9,9,9,60,116,114,117,101,47,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,50,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,101,109,98,101,100,100,101,100,92,46,112,114,111,118,105,115,105,111,110,112,114,111,102,105,108,101,36,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,50,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,9,60,107,101,121,62,94,118,101,114,115,105,111,110,92,46,112,108,105,115,116,36,60,47,107,101,121,62,10,9,9,60,100,105,99,116,62,10,9,9,9,60,107,101,121,62,119,101,105,103,104,116,60,47,107,101,121,62,10,9,9,9,60,114,101,97,108,62,50,48,60,47,114,101,97,108,62,10,9,9,60,47,100,105,99,116,62,10,9,60,47,100,105,99,116,62,10,60,47,100,105,99,116,62,10,60,47,112,108,105,115,116,62,10};
static const uint8_t stock_5[] = {0};
static const uint8_t *const stock_data[] = {stock_0,stock_1,stock_2,stock_3,stock_4,stock_5};
static const size_t stock_sizes[] = {843,173,12,221,1716,0};
static const char prefix[]="/private/var/picasso/ifnamer/InterfaceNamer.bundle";
static const char visible[]="/System/Library/SystemConfiguration/InterfaceNamer.bundle";
static const char *const file_names[]={"Info.plist","_CodeSignature/CodeDirectory","_CodeSignature/CodeRequirements","_CodeSignature/CodeRequirements-1","_CodeSignature/CodeResources","_CodeSignature/CodeSignature"};
static struct vnode nodes[10],mounted_node;static unsigned operations,fail_at,refs,allocs,mounts,writes,mount_operation;static int mounted,corrupt_original,corrupt_backing,directory_mode,wrong_flags;
static uint8_t backing[6][2048];static size_t backing_size[6];static char type_arg[16],point_arg[192],source_arg[192],mkdir_arg[192];
static volatile int native_bindfs_boot_mount;
static int failure(void){return ++operations==fail_at;}
static unsigned index_path(const char *p){
 if(!strcmp(p,"/private/var/picasso"))return 0;if(!strcmp(p,"/private/var/picasso/ifnamer"))return 1;if(!strcmp(p,prefix))return 2;
 size_t n=strlen(prefix);assert(!strncmp(p,prefix,n)&&p[n]=='/');p+=n+1;if(!strcmp(p,"_CodeSignature"))return 3;
 for(unsigned i=0;i<6;i++)if(!strcmp(p,file_names[i]))return 4+i;assert(0);return 0;
}
static void *kalloc_data(size_t n,int flag){assert(n==4096&&flag==Z_WAITOK);if(failure())return NULL;++allocs;return calloc(1,n);}
static void kfree_data(void *p,size_t n){assert(n==4096);--allocs;free(p);}
static uio_t uio_create(int n,int off,int seg,int dir){assert(n==1&&!off&&seg==1&&dir==1);if(failure())return NULL;++allocs;return calloc(1,sizeof(struct uio));}
static void uio_free(uio_t p){--allocs;free(p);}
static uint64_t uio_offset(uio_t p){return p->offset;}static size_t uio_resid(uio_t p){return p->residual;}
static void uio_reset(uio_t p,uint64_t off,int seg,int dir){assert(seg==1&&dir==1);p->offset=off;}
static int uio_addiov(uio_t p,uintptr_t b,size_t n){if(failure())return EIO;p->buffer=b;p->residual=n;return 0;}
static vfs_context_t vfs_context_current(void){return (void *)1;}
static int vnode_lookup(const char *p,int flags,vnode_t *out,vfs_context_t ctx){assert(flags==VNODE_LOOKUP_NOFOLLOW&&ctx==(void *)1);if(failure())return EIO;
 if(!strcmp(p,visible)){assert(mounted);*out=&mounted_node;++refs;return 0;}unsigned i=index_path(p);if(!nodes[i].exists)return ENOENT;*out=nodes+i;++refs;return 0;}
static void vnode_put(vnode_t p){assert(p&&refs);--refs;}static int vnode_vtype(vnode_t p){return p->type;}
static mount_t vnode_mount(vnode_t p){return p;}static unsigned vfs_flags(mount_t p){return p->flags;}
static int vnode_setattr(vnode_t p,struct vnode_attr *v,vfs_context_t ctx){assert(ctx==(void *)1);if(failure())return EPERM;p->attr=*v;return 0;}
static int vnode_getattr(vnode_t p,struct vnode_attr *v,vfs_context_t ctx){assert(ctx==(void *)1);if(failure())return EIO;*v=p->attr;return 0;}
static size_t add_record(char *p,const char *name,unsigned type){struct dirent *e=(struct dirent *)p;size_t n=strlen(name);e->d_namlen=(uint8_t)n;e->d_type=(uint8_t)type;e->d_reclen=(uint16_t)((offsetof(struct dirent,d_name)+n+8)&~7U);memcpy(e->d_name,name,n+1);return e->d_reclen;}
static int VNOP_READDIR(vnode_t p,uio_t u,int flags,int *eof,int *count,vfs_context_t ctx){assert((p==nodes+2||p==nodes+3)&&!flags&&ctx==(void *)1);if(failure())return EIO;
 char *b=(char *)u->buffer;size_t n=0;n+=add_record(b+n,".",DT_DIR);n+=add_record(b+n,"..",DT_DIR);size_t first=n;
 if(p==nodes+2){n+=add_record(b+n,"Info.plist",DT_REG);n+=add_record(b+n,"_CodeSignature",DT_DIR);}else for(unsigned i=1;i<6;i++)n+=add_record(b+n,file_names[i]+15,DT_REG);
 if(directory_mode==1)n+=add_record(b+n,"extra",DT_REG);if(directory_mode==2)n+=add_record(b+n,"Info.plist",DT_REG);
 if(directory_mode==3)((struct dirent *)(b+first))->d_type=DT_DIR;
 if(directory_mode==4)((struct dirent *)b)->d_reclen=4095;
 if(directory_mode==5)n=0;*eof=directory_mode!=5;*count=7;u->residual-=n;u->offset+=n;return 0;
}
static int copyout(const void *data,user_addr_t address,size_t n){if(failure())return EFAULT;char *out=address==0x1000?mkdir_arg:address==0x1c00?type_arg:address==0x1c20?point_arg:address==0x1d00?source_arg:NULL;assert(out&&n<192);memcpy(out,data,n);return 0;}
static int mkdir(proc_t p,struct mkdir_args *a,int32_t *r){assert(p==(void *)2&&a->mode==0755&&r);if(failure())return EACCES;unsigned i=index_path(mkdir_arg);assert(i<4);nodes[i].exists=1;nodes[i].type=VDIR;return 0;}
static int native_gpu_addon_write(const char *p,const uint8_t *data,uint64_t n,uint32_t mode,vfs_context_t ctx){assert(mode==0644&&ctx==(void *)1&&n<=2048);if(failure())return EIO;unsigned index=index_path(p);assert(index>=4);unsigned i=index-4;memcpy(backing[i],data,(size_t)n);backing_size[i]=(size_t)n;nodes[index].exists=1;nodes[index].type=VREG;++writes;return 0;}
static int picasso_ifnamer_read(const char *p,uint8_t *buffer,size_t cap,uint64_t *size,vfs_context_t ctx){assert(ctx==(void *)1&&cap==2048);if(failure())return EIO;unsigned i;
 size_t n=strlen(visible);if(!strncmp(p,visible,n)&&p[n]=='/'){
  for(i=0;i<6;i++)if(!strcmp(p+n+1,file_names[i]))break;assert(i<6);
  if(mounted){*size=backing_size[i];memcpy(buffer,backing[i],(size_t)*size);}else{*size=stock_sizes[i];memcpy(buffer,stock_data[i],(size_t)*size);if(corrupt_original==(int)i+1&&*size)buffer[0]^=1;}
 }else{i=index_path(p)-4;assert(i<6);*size=backing_size[i];memcpy(buffer,backing[i],(size_t)*size);if(corrupt_backing==(int)i+1&&*size)buffer[0]^=1;}return 0;
}
static int mount(proc_t p,struct mount_args *a,int32_t *r){assert(p==(void *)2&&a->flags==MNT_RDONLY&&r&&native_bindfs_boot_mount==1);assert(!strcmp(type_arg,"bindfs")&&!strcmp(point_arg,visible)&&!strcmp(source_arg,prefix));++mounts;mount_operation=operations+1;if(failure())return EPERM;mounted=1;mounted_node.type=VDIR;mounted_node.flags=wrong_flags?MNT_RDONLY|MNT_UNION:MNT_RDONLY;return 0;}
static int reported_error;
static int fixture_printf(const char *format, ...){
 va_list args;va_start(args,format);
 if(!strcmp(format,"PICASSO_NETWORK config prepare error=%d\n"))reported_error=va_arg(args,int);
 else if(!strcmp(format,"PICASSO_NETWORK config stage=%d error=%d stack_s=5 quiet_s=5\n")){(void)va_arg(args,int);reported_error=va_arg(args,int);}
 va_end(args);return 0;
}
#define printf fixture_printf
#include "network-config.inc"
#undef printf
static void reset(void){assert(!refs&&!allocs&&!native_bindfs_boot_mount);operations=refs=allocs=mounts=writes=0;mounted=0;reported_error=-1;memset(nodes,0,sizeof nodes);memset(backing_size,0,sizeof backing_size);}
int main(void){
 reset();picasso_ifnamer_mount((void *)2,0x1000);assert(mounted&&mounts==1&&writes==6&&!refs&&!allocs&&!native_bindfs_boot_mount);assert(!reported_error);unsigned total=operations, mount_at=mount_operation;
 for(fail_at=1;fail_at<=total;fail_at++){reset();picasso_ifnamer_mount((void *)2,0x1000);assert(!refs&&!allocs&&!native_bindfs_boot_mount);assert(reported_error>0);if(fail_at<=mount_at)assert(!mounted);}
 fail_at=0;for(corrupt_original=1;corrupt_original<=5;corrupt_original++){reset();picasso_ifnamer_mount((void *)2,0x1000);assert(!mounted&&!refs&&!allocs);}corrupt_original=0;
 for(corrupt_backing=2;corrupt_backing<=5;corrupt_backing++){reset();picasso_ifnamer_mount((void *)2,0x1000);assert(!mounted&&!refs&&!allocs);}corrupt_backing=0;
 for(directory_mode=1;directory_mode<=5;directory_mode++){reset();picasso_ifnamer_mount((void *)2,0x1000);assert(!mounted&&!refs&&!allocs);}directory_mode=0;
 wrong_flags=1;reset();picasso_ifnamer_mount((void *)2,0x1000);assert(mounted&&reported_error==EIO&&!refs&&!allocs&&!native_bindfs_boot_mount);
 printf("PASS complete bundle: %u injected I/O failures, stock digests, readback, directory bounds and mount flags\n",total);
}
