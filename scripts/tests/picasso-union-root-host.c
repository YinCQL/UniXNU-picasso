/* Execute the extracted XNU checkdirs functions with counted vnode stand-ins. */
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DIAGNOSTIC 0
#define PROC_RETURNED 0
#define PROC_ALLPROCLIST 1
#define PROC_NOWAITTRANS 2
typedef struct vnode *vnode_t;
typedef void *vfs_context_t;
struct mount { vnode_t root; };
struct vnode { int v_usecount, io; struct mount *v_mountedhere; };
struct filedesc { vnode_t fd_cdir, fd_rdir; };
struct proc { struct filedesc p_fd; };
typedef struct proc *proc_t;
struct cdirargs { vnode_t olddp, newdp; };
static struct proc processes[3];
static proc_t kernproc = &processes[0];
static vnode_t rootvnode;
static int rootvnode_rw_lock, fd_locked, dirs_locked, ref_calls, ref_fail, root_error;
static int vnode_ref(vnode_t vp) { if (++ref_calls == ref_fail) return 1; ++vp->v_usecount; return 0; }
static void vnode_rele(vnode_t vp) { assert(vp->v_usecount > 0); --vp->v_usecount; }
static void vnode_put(vnode_t vp) { assert(vp->io > 0); --vp->io; }
static void proc_fdlock(proc_t p) { (void)p; assert(!fd_locked); fd_locked = 1; }
static void proc_fdunlock(proc_t p) { (void)p; assert(fd_locked); fd_locked = 0; }
static void proc_dirs_lock_exclusive(proc_t p) { (void)p; assert(!dirs_locked); dirs_locked = 1; }
static void proc_dirs_unlock_exclusive(proc_t p) { (void)p; assert(dirs_locked); dirs_locked = 0; }
static void lck_rw_lock_exclusive(int *p) { assert(!*p); *p = 1; }
static void lck_rw_unlock_exclusive(int *p) { assert(*p); *p = 0; }
static int VFS_ROOT(struct mount *m, vnode_t *vp, vfs_context_t ctx) {
    (void)ctx; if (root_error) return root_error; *vp = m->root; ++(*vp)->io; return 0;
}
static void proc_iterate(unsigned flags, int (*fn)(proc_t, void *), void *arg, void *filter, void *data) {
    assert(flags == (PROC_ALLPROCLIST | PROC_NOWAITTRANS) && !filter && !data);
    for (int i = 0; i < 3; ++i) assert(fn(&processes[i], arg) == PROC_RETURNED);
}
#include "root-functions.inc"

int main(int argc, char **argv) {
    assert(argc == 2); int scenario = atoi(argv[1]);
    struct vnode lower = {3, 0, NULL}, upper = {0}, other = {1};
    struct mount mounted = {&upper}; lower.v_mountedhere = &mounted;
    rootvnode = &lower;
    kernproc->p_fd.fd_cdir = &lower; /* Borrowed root alias: no additional ref. */
    processes[1].p_fd.fd_cdir = &lower; /* Owns one of lower's three refs. */
    /* The remaining refs are rootvnode and the mount's covered vnode. */
    if (scenario == 0 || scenario == 1) {
        if (scenario == 1) { processes[2].p_fd.fd_rdir = &lower; ++lower.v_usecount; }
        assert(checkdirs(&lower, NULL) == 0);
        if (lower.v_usecount != 1) { puts("Detected old root usecount undercount"); return 77; }
        assert(upper.v_usecount == (scenario == 1 ? 3 : 2) && !upper.io);
        assert(rootvnode == &upper && kernproc->p_fd.fd_cdir == &upper);
        assert(processes[1].p_fd.fd_cdir == &upper);
        vnode_rele(processes[1].p_fd.fd_cdir);
        if (scenario == 1) vnode_rele(processes[2].p_fd.fd_rdir);
        vnode_rele(rootvnode); vnode_rele(&lower);
        assert(!upper.v_usecount && !lower.v_usecount);
    } else if (scenario == 2) {
        /* Non-root kernel cwd is an independent ref and must still transfer. */
        rootvnode = &other;
        assert(checkdirs(&lower, NULL) == 0);
        assert(lower.v_usecount == 1 && upper.v_usecount == 2 && !upper.io);
        assert(rootvnode == &other && kernproc->p_fd.fd_cdir == &upper);
    } else if (scenario == 3) {
        /* A kernel rdir is independently owned even beside a borrowed cwd. */
        kernproc->p_fd.fd_rdir = &lower; ++lower.v_usecount;
        assert(checkdirs(&lower, NULL) == 0);
        assert(lower.v_usecount == 1 && upper.v_usecount == 3 && !upper.io);
        assert(kernproc->p_fd.fd_rdir == &upper);
    } else if (scenario == 4 || scenario == 5) {
        struct cdirargs args = {&lower, &upper}; ref_fail = scenario - 3;
        assert(checkdirs_callback(&processes[1], &args) == PROC_RETURNED);
        assert(lower.v_usecount == 3 && !upper.v_usecount);
        assert(processes[1].p_fd.fd_cdir == &lower);
    } else if (scenario == 6) {
        root_error = 5; assert(checkdirs(&lower, NULL) == 5);
        assert(lower.v_usecount == 3 && !upper.v_usecount && !upper.io);
        assert(rootvnode == &lower && kernproc->p_fd.fd_cdir == &lower);
    } else if (scenario == 7) {
        struct cdirargs args = {&lower, &upper};
        assert(checkdirs_callback(&processes[2], &args) == PROC_RETURNED);
        assert(!ref_calls && lower.v_usecount == 3 && !upper.v_usecount);
    } else assert(0);
    assert(!fd_locked && !dirs_locked && !rootvnode_rw_lock);
    printf("PASS scenario=%d\n", scenario);
    return 0;
}
