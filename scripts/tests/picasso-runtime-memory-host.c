/* Host API models; does not execute page tables, APFS or device SIMD loads. */
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef uint64_t pmap_paddr_t;
typedef int64_t off_t;
#define VM_WIMG_MASK 0xff
#define VM_WIMG_IO 0x7
#define VM_WIMG_POSTED 0x27
#define VM_WIMG_POSTED_REORDERED 0x2e
#define VM_WIMG_POSTED_COMBINED_REORDERED 0x26
#define VM_WIMG_WCOMB 0x6
#define VM_WIMG_RT 0x17
static bool pa_valid(pmap_paddr_t pa) { return pa >= 0xc007c000 && pa < 0x20007c000; }
struct statfs_model { char f_mntonname[64]; };
struct mount_model { struct statfs_model stat; uint64_t blocks; uint32_t blocksize; unsigned supported; int error; };
struct vnode_model { struct mount_model *mount; off_t bytes; int size_error, get_error, path_error; };
typedef struct mount_model *mount_t;
typedef struct vnode_model *vnode_t;
typedef void *vfs_context_t;
struct vfs_attr { uint64_t f_bavail; uint32_t f_bsize; unsigned supported; };
#define VFSATTR_INIT(v) memset((v),0,sizeof *(v))
#define VFSATTR_WANTED(v,k) ((void)(v))
#define VFSATTR_f_bavail 1
#define VFSATTR_f_bsize 2
#define VFSATTR_IS_SUPPORTED(v,k) ((v)->supported & VFSATTR_##k)
static mount_t vnode_mount(vnode_t v) { return v->mount; }
static struct statfs_model *vfs_statfs(mount_t m) { return &m->stat; }
static int vnode_size(vnode_t v,off_t *size,vfs_context_t c) { (void)c;*size=v->bytes;return v->size_error; }
static int vfs_getattr(mount_t m,struct vfs_attr *a,vfs_context_t c) {
 (void)c;a->f_bavail=m->blocks;a->f_bsize=m->blocksize;a->supported=m->supported;return m->error;
}
static unsigned vnode_get_count,puts_count,queries,log_count;
static struct vnode_model node;
static struct proc_regioninfo_internal {
 uint32_t pri_protection,pri_max_protection,pri_flags,pri_user_tag,pri_pages_resident,pri_pages_swapped_out;
 uint64_t pri_address,pri_size,pri_offset;
} region;
static int region_found;static uintptr_t region_vnode;
static void *current_task(void) { return &node; }
static int fill_procregioninfo(void *task,uint64_t a,struct proc_regioninfo_internal *i,uintptr_t *v,uint32_t *vid) {
 assert(task==&node);(void)a;++queries;*i=region;*v=region_vnode;*vid=123;return region_found;
}
static int vnode_getwithvid(vnode_t v,uint32_t vid) { assert(v==&node && vid==123);++vnode_get_count;return node.get_error; }
static void vnode_put(vnode_t v) { assert(v==&node);++puts_count; }
static int vn_getpath(vnode_t v,char *p,int *length) {
 assert(v==&node && *length==256);memset(p,'X',255);p[255]=0;return node.path_error;
}
static vfs_context_t vfs_context_current(void) { return NULL; }
static char output[2048];static unsigned output_size;
static int report(const char *fmt,...) {
 char line[512];va_list args;va_start(args,fmt);int n=vsnprintf(line,sizeof(line),fmt,args);va_end(args);
 assert(n>0 && n<256);assert(output_size+(unsigned)n<sizeof(output));
 memcpy(output+output_size,line,(size_t)n+1);output_size+=(unsigned)n;++log_count;return n;
}
#define printf report
#include "runtime-memory.inc"
#undef printf
static void reset_log(void) { output_size=log_count=0;output[0]=0;vnode_get_count=puts_count=queries=0; }
int main(void) {
 const uint64_t addresses[]={0,0xc007bfff,0xc007c000,0x1b9d55b00,0x20007bfff,0x20007c000,UINT64_MAX};
 for(unsigned k=0;k<sizeof(addresses)/sizeof(addresses[0]);++k)for(unsigned m=0;m<256;++m)for(unsigned flags=0;flags<2;++flags) {
  unsigned input=m|(flags?0x80000000u:0);bool device=m==7 || m==0x27 || m==0x2e || m==0x26 || m==VM_WIMG_RT;
  unsigned expected=pa_valid(addresses[k]) && device ? (input & ~VM_WIMG_MASK)|VM_WIMG_WCOMB : input;
  assert(picasso_dram_io_wimg(input,addresses[k])==expected);
 }
 struct mount_model mp={{{0}},0,4096,3,0};strcpy(mp.stat.f_mntonname,"/private/var");node.mount=&mp;
 const uint64_t reserve=512ULL*1024*1024+65536, amount=128ULL*1024*1024;
 mp.blocks=(reserve+amount)/4096;assert(picasso_swap_headroom(&node,amount,NULL)==0);reset_log();
 --mp.blocks;assert(picasso_swap_headroom(&node,amount,NULL)==ENOSPC);reset_log();
 mp.blocks=4919;assert(picasso_swap_headroom(&node,amount,NULL)==ENOSPC);reset_log();
 node.bytes=(off_t)amount;assert(!picasso_swap_headroom(&node,0,NULL));assert(!picasso_swap_headroom(&node,amount,NULL));
 assert(picasso_swap_headroom(&node,amount+1,NULL)==ENOSPC);reset_log();
 mp.blocks=UINT64_MAX;assert(picasso_swap_headroom(&node,amount+1,NULL)==EINVAL);reset_log();
 mp.blocks=1000000;mp.blocksize=0;assert(picasso_swap_headroom(&node,amount+1,NULL)==EINVAL);reset_log();
 mp.blocksize=4096;mp.supported=1;assert(picasso_swap_headroom(&node,amount+1,NULL)==EINVAL);reset_log();
 mp.supported=3;mp.error=EIO;assert(picasso_swap_headroom(&node,amount+1,NULL)==EIO);reset_log();mp.error=0;
 node.size_error=EIO;assert(picasso_swap_headroom(&node,amount,NULL)==EIO);reset_log();node.size_error=0;
 node.bytes=-1;assert(picasso_swap_headroom(&node,amount,NULL)==EINVAL);reset_log();node.bytes=0;
 strcpy(mp.stat.f_mntonname,"/System/Volumes/VM");assert(!picasso_swap_headroom(&node,UINT64_MAX,NULL));assert(!log_count);
 region_found=1;region.pri_address=0x20000;region.pri_size=0x1000;region_vnode=(uintptr_t)&node;
 picasso_image_mapping("fault",0x1ffff);assert(!vnode_get_count && strstr(output,"contains=0"));reset_log();
 picasso_image_mapping("fault",0x21000);assert(!vnode_get_count && strstr(output,"contains=0"));reset_log();
 picasso_image_mapping("fault",0x20000);assert(vnode_get_count==1 && puts_count==1 && strstr(output,"contains=1"));reset_log();
 node.get_error=ENOENT;picasso_image_mapping("fault",0x20001);assert(vnode_get_count==1 && puts_count==0 && strstr(output,"vnode_error="));reset_log();node.get_error=0;
 node.path_error=ENOSPC;node.size_error=EIO;picasso_image_mapping("fault",0x20fff);
 assert(vnode_get_count==1 && puts_count==1 && strstr(output,"<unavailable>") && strstr(output,"size_error="));reset_log();
 region_found=0;picasso_image_mapping("fault",0);assert(!vnode_get_count && strstr(output,"contains=0"));reset_log();
 region_found=1;region_vnode=0;picasso_image_mapping("fault",0x20000);assert(!vnode_get_count && log_count==2);reset_log();
 region.pri_address=UINT64_MAX-5;region.pri_size=5;picasso_image_mapping("fault",UINT64_MAX);
 assert(!vnode_get_count && strstr(output,"contains=0"));
 puts("PASS: 3584 DRAM/flag/boundary cases, swap growth/shrink/error reserve, region holes/stale vnode/path errors/balanced refs/bounded logs");
 return 0;
}
