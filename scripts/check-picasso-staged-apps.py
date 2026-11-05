#!/usr/bin/env python3
"""Execute production staging guards; --xnu also exercises the real copy loop.

VFS operations are fault-injection models, not APFS or device validation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    start = source.index('\nstatic int\n' + name + '(') + 1
    opening = source.index('{', start)
    depth = 1
    at = opening + 1
    while depth:
        depth += (source[at] == '{') - (source[at] == '}')
        at += 1
    return source[start:at] + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--xnu', type=Path)
    args = parser.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=False)
    patch = ROOT / 'patches/picasso-staged-apps-budget-xnu.patch'
    source = '\n' + '\n'.join(line[1:] for line in patch.read_text().splitlines()
                              if line.startswith('+') and not line.startswith('+++'))
    guard = function(source, 'picasso_staged_free_bytes') + function(source, 'picasso_staged_headroom')
    prefix = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void *mount_t;
struct vfs_attr { uint64_t f_bavail; uint32_t f_bsize; unsigned supported; };
static uint64_t available, blocksize;
static int stat_error; static unsigned supported;
#define VFSATTR_INIT(v) memset(v, 0, sizeof *(v))
#define VFSATTR_WANTED(v,k) ((void)(v))
#define VFSATTR_f_bavail 1
#define VFSATTR_f_bsize 2
#define VFSATTR_IS_SUPPORTED(v,k) ((v)->supported & VFSATTR_##k)
static void *vfs_context_current(void) { return NULL; }
static int vfs_getattr(mount_t mp, struct vfs_attr *v, void *ctx) {
    (void)mp; (void)ctx; v->f_bavail=available; v->f_bsize=(uint32_t)blocksize;
    v->supported=supported; return stat_error;
}
'''
    prefix += '\n'.join(line for line in source.splitlines()
                        if line.startswith('#define PICASSO_STAGED_')) + '\n'
    main_body = r'''
    blocksize=4096; supported=3; stat_error=0;
    available=2625; assert(picasso_staged_headroom(NULL,1048576)==ENOSPC);
    available=130381+131072; assert(!picasso_staged_headroom(NULL,1048576));
    available=131072+16; assert(!picasso_staged_headroom(NULL,0));
    assert(picasso_staged_headroom(NULL,1)==ENOSPC);
    assert(picasso_staged_headroom(NULL,UINT64_MAX)==ENOSPC);
    available=131071; assert(picasso_staged_headroom(NULL,0)==ENOSPC);
    supported=0; assert(picasso_staged_headroom(NULL,0)==EINVAL);
    supported=1; assert(picasso_staged_headroom(NULL,0)==EINVAL);
    supported=2; assert(picasso_staged_headroom(NULL,0)==EINVAL);
    supported=3; blocksize=0; assert(picasso_staged_headroom(NULL,0)==EINVAL);
    blocksize=4096; available=UINT64_MAX; assert(picasso_staged_headroom(NULL,0)==EINVAL);
    stat_error=EIO; assert(picasso_staged_headroom(NULL,0)==EIO);
    puts("PASS: reserve, APFS block units, stat failure, unsupported attributes and overflow");
'''
    extra = ''
    copy_hash = None
    if args.xnu:
        copy = function((args.xnu / 'bsd/kern/kern_exec.c').read_text(), 'native_staged_apps_file')
        copy_hash = hashlib.sha256(copy.encode()).hexdigest()
        extra = r'''
typedef void *vnode_t; typedef void *vfs_context_t; typedef char *caddr_t;
#define NULLVP NULL
#define FREAD 1
#define FWRITE 2
#define O_CREAT 4
#define O_TRUNC 8
#define O_NOFOLLOW 16
#define UIO_READ 1
#define UIO_WRITE 2
#define UIO_SYSSPACE 0
#define IO_NOAUTH 1
#define IO_UNIT 2
#define MNT_WAIT 1
typedef long long picasso_test_offset_t;
#define off_t picasso_test_offset_t
struct vnode_attr {int va_mode,va_uid,va_gid;};
#define VATTR_INIT(v) memset(v,0,sizeof *(v))
#define VATTR_SET(v,k,x) ((v)->k=(x))
#define NATIVE_STAGED_APPS_OWNER 501
static uint64_t native_staged_apps_bytes;
static char native_staged_apps_path[96];
static int source_open_error,injected_source_close_error,dest_open_error,dest_close_error,read_error,write_error,sync_error,attr_error,bad_resid,short_write;
static int dest_opens,source_closes,dest_closes,writes;
static int file_size; static struct vnode_attr actual_attr;
static size_t strlcpy(char *d,const char *s,size_t n) {size_t k=strlen(s); if(n){size_t t=k<n?k:n-1; memcpy(d,s,t);d[t]=0;} return k;}
static int vnode_lookup(const char *p,int f,vnode_t *v,vfs_context_t c) {(void)p;(void)f;(void)c;*v=(void *)1;return 0;}
static mount_t vnode_mount(vnode_t v) {return v;}
static void vnode_put(vnode_t v) {(void)v;}
static int VNOP_OPEN(vnode_t v,int f,vfs_context_t c) {(void)v;(void)f;(void)c;return source_open_error;}
static int VNOP_CLOSE(vnode_t v,int f,vfs_context_t c) {(void)v;(void)f;(void)c;source_closes++;return injected_source_close_error;}
static int vnode_open(const char *p,int f,int m,int x,vnode_t *v,vfs_context_t c) {(void)p;(void)f;(void)m;(void)x;(void)c;dest_opens++;*v=dest_open_error?NULL:(void *)2;return dest_open_error;}
static int vnode_close(vnode_t v,int f,vfs_context_t c) {(void)v;(void)f;(void)c;dest_closes++;return dest_close_error;}
static int VNOP_FSYNC(vnode_t v,int f,vfs_context_t c) {(void)v;(void)f;(void)c;return sync_error;}
static int vnode_setattr(vnode_t v,struct vnode_attr *a,vfs_context_t c) {(void)v;(void)c;actual_attr=*a;return attr_error;}
static void *vfs_context_ucred(vfs_context_t c) {return c;}
static void *vfs_context_proc(vfs_context_t c) {return c;}
static int vn_rdwr(int op,vnode_t v,caddr_t b,int n,off_t at,int space,int flags,void *cred,int *resid,void *proc) {
    (void)v;(void)space;(void)flags;(void)cred;(void)proc;
    if(op==UIO_READ){
        int k=file_size-(int)at; if(k<0)k=0;if(k>n)k=n;
        memset(b,0x5a,(size_t)k);*resid=n-k;
        if(bad_resid)*resid=bad_resid<0?-1:n+1;
        return read_error;
    }
    writes++;*resid=short_write?1:0;
    if(!write_error && !short_write){available-=(uint64_t)n;}
    return write_error;
}
static void reset_copy(void) {
    source_open_error=injected_source_close_error=dest_open_error=dest_close_error=read_error=write_error=sync_error=attr_error=bad_resid=short_write=0;
    dest_opens=source_closes=dest_closes=writes=0;native_staged_apps_bytes=0;
    file_size=150;blocksize=1;available=1073741824;supported=3;stat_error=0;
}
'''
        extra += copy
        main_body += r'''
    {uint8_t buffer[64];
    reset_copy();assert(!native_staged_apps_file((void *)3,"/private/var/staged_system_apps/Mail.app",0755,buffer,64,NULL));
    assert(native_staged_apps_bytes==150 && writes==3 && source_closes==1 && dest_closes==1);
    assert(actual_attr.va_uid==501 && actual_attr.va_gid==501 && actual_attr.va_mode==0755);
    reset_copy();available=67108864;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==ENOSPC);assert(!dest_opens && !writes);
    reset_copy();available=PICASSO_STAGED_RESERVE+PICASSO_STAGED_TX_ALLOWANCE+128;
    assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==ENOSPC);assert(native_staged_apps_bytes==128 && writes==2);
    reset_copy();write_error=ENOSPC;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==ENOSPC);assert(!native_staged_apps_bytes);
    reset_copy();short_write=1;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==EIO);assert(!native_staged_apps_bytes);
    reset_copy();bad_resid=-1;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==EIO);assert(!writes);
    reset_copy();bad_resid=1;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==EIO);assert(!writes);
    reset_copy();sync_error=EIO;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==EIO);
    reset_copy();attr_error=EPERM;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==EPERM);
    reset_copy();dest_close_error=EIO;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==EIO);
    reset_copy();injected_source_close_error=EIO;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==EIO);
    reset_copy();read_error=EIO;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==EIO);assert(!writes);
    reset_copy();dest_open_error=EACCES;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==EACCES);assert(!writes && source_closes==1);
    reset_copy();source_open_error=EACCES;assert(native_staged_apps_file((void *)3,"/a",0644,buffer,64,NULL)==EACCES);assert(!dest_opens);
    puts("PASS: real copy loop, owner/mode, actual-byte count, reserve crossing, read/write/close/fsync faults");}
'''
    cfile = out / 'staged.c'
    cfile.write_text(prefix + guard + extra + '\nint main(void) {\n' + main_body + '\nreturn 0;\n}\n', newline='\n')
    compiler = runpy.run_path(str(ROOT / 'scripts/check-picasso-user-fault.py'))
    vcvars = compiler['find_vcvars']() if os.name == 'nt' else None
    checks = []
    for asan in (False, True):
        name = 'staged' + ('-asan' if asan else '-plain')
        result, exe = compiler['compile_case'](out, name, cfile.name, asan, os.environ.get('CC', 'clang'), vcvars)
        assert result['exit'] == 0, result
        log = out / (name + '-run.log')
        if vcvars:
            executed = compiler['batch_run'](out, name + '-run',
                        f'{exe} > {log.name} 2>&1\nexit /b %errorlevel%\n', vcvars)
        else:
            executed = subprocess.run([str(out / exe)], cwd=out, capture_output=True, text=True)
            log.write_text(executed.stdout + executed.stderr)
        run = dict(exit=executed.returncode, log=str(log),
                   copy_faults_tested=bool(args.xnu))
        assert run['exit'] == 0 and 'PASS: reserve' in log.read_text(), run
        checks.append(dict(name=name,compile=result,run=run))
    (out / 'results.json').write_text(json.dumps(dict(checks=checks,patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),copy_sha256=copy_hash,model_vfs=True),indent=2)+'\n')
    print('PASS: production staged apps guards' + (' and copy loop' if args.xnu else '') + ' (plain and ASan)')


if __name__ == '__main__':
    main()
