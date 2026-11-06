/* Execute the real bounded read helper, including requested attribute bits. */
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
typedef void *vnode_t;typedef void *vfs_context_t;typedef char *caddr_t;
#define NULLVP NULL
enum {FREAD=1,O_NOFOLLOW=2,VREG=3,UIO_READ=4,UIO_SYSSPACE=5,IO_NOAUTH=6};
struct vnode_attr {uint64_t va_data_size;int wanted,supported;};
#define VATTR_INIT(v) memset(v,0,sizeof(*(v)))
#define VATTR_WANTED(v,f) ((v)->wanted=1)
#define VATTR_IS_SUPPORTED(v,f) ((v)->supported)
static int scenario,opens,closes,reads;
static int vnode_open(const char *path,int flags,int mode,int context,vnode_t *vp,vfs_context_t ctx){assert(!strcmp(path,"original") && flags==(FREAD|O_NOFOLLOW) && !mode && !context && ctx==(void *)1);if(scenario==1)return ENOENT;++opens;*vp=(void *)2;return 0;}
static int vnode_vtype(vnode_t vp){assert(vp==(void *)2);return scenario==2?9:VREG;}
static int vnode_getattr(vnode_t vp,struct vnode_attr *v,vfs_context_t ctx){assert(vp==(void *)2 && ctx==(void *)1 && v->wanted);if(scenario==3)return EIO;v->supported=scenario!=4;v->va_data_size=scenario==5?4096:843;return 0;}
static void *vfs_context_ucred(vfs_context_t ctx){return ctx;}
static void *vfs_context_proc(vfs_context_t ctx){return ctx;}
static int vn_rdwr(int rw,vnode_t vp,caddr_t buf,int size,int off,int seg,int flags,void *cred,int *resid,void *proc){assert(rw==4 && vp==(void *)2 && buf && size==843 && !off && seg==5 && flags==6 && cred==(void *)1 && proc==cred);++reads;*resid=scenario==7?1:0;return scenario==6?EIO:0;}
static int vnode_close(vnode_t vp,int flags,vfs_context_t ctx){assert(vp==(void *)2 && flags==1 && ctx==(void *)1);++closes;return scenario==8?EIO:0;}
#include "network-reader.inc"
int main(void){for(scenario=0;scenario<9;++scenario){opens=closes=reads=0;uint8_t b[2048];uint64_t size=0;int result=picasso_ifnamer_read("original",b,sizeof b,&size,(void *)1);assert((result==0)==(scenario==0));assert(opens==closes);assert(reads==(scenario==0 || scenario>=6));}puts("PASS bounded config reader: 9 attribute/type/size/short-read/close cases");}
