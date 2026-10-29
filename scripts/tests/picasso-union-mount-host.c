/* Actual mount/preflight functions; host mount, copyout and trust API stand-ins. */
#define _CRT_SECURE_NO_WARNINGS
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define XNU_OWNER_UNION_SYSTEMOS 1
#define XNU_OWNER_BOOT_MANIFEST 1
#define XNU_OWNER_CACHE_STARTUP 1
#define CONFIG_UNION_MOUNTS 1
#define MNT_RDONLY 1
#define MNT_UNION 0x20
#define EAUTH 80
#define EIO 5
#define ENOENT 2
#define NODEV (-1)
#define NULLVP NULL
#define USER_ADDR_NULL 0
#define VREG 1
#define VDIR 2
typedef int dev_t;
typedef void *proc_t;
typedef uint64_t user_addr_t;
typedef uint64_t user64_addr_t;
struct vnode;
struct statfs { char f_fstypename[16]; };
struct mount { unsigned flags; struct vnode *device; struct statfs stat; };
typedef struct mount *mount_t;
struct vnode { struct mount *mp, *covered; dev_t dev; int type, io; };
typedef struct vnode *vnode_t;
struct mount_args { user_addr_t type, path; int flags; user_addr_t data; };
static struct mount host_os_mount, host_cache_mount;
static struct vnode os_root, cache_root, os_dev, cache_dev, library, cache_file, launchd;
static vnode_t rootvnode;
static unsigned char user_memory[2048];
static int scenario, root_auth_calls, mount_calls, path_calls, manifest_calls, copy_calls, checkpoints;
static uint64_t dyld_flags = 0x20, observed_flags;
static int native_owner_cache_stage, native_owner_cache_status, native_owner_cache_mount_status;
static int native_owner_cache_readonly, native_owner_cache_authorized, native_owner_cache_root_recheck;
static int native_owner_cache_native_union;
static uint64_t native_owner_cache_actual_mount_flags, native_owner_cache_dyld_flags;
static uintptr_t native_owner_cache_original_root, native_owner_cache_current_root;
static dev_t native_owner_cache_selected_device, native_owner_cache_mounted_device;
static char native_owner_cache_device_path[64];
static void *vfs_context_current(void) { return NULL; }
static mount_t vnode_mount(vnode_t vp) { return vp->mp; }
static vnode_t vfs_devvp(mount_t m) { ++m->device->io; return m->device; }
static dev_t vnode_specrdev(vnode_t vp) { return vp->dev; }
static unsigned vfs_flags(mount_t m) { return m->flags; }
static struct statfs *vfs_statfs(mount_t m) { return &m->stat; }
static mount_t vnode_mountedhere(vnode_t vp) { return vp->covered; }
static int vnode_vtype(vnode_t vp) { return vp->type; }
static void vnode_put(vnode_t vp) { assert(vp->io > 0); --vp->io; }
static size_t strlcpy(char *dst, const char *src, size_t size) { snprintf(dst, size, "%s", src); return strlen(src); }
static int native_verified_block_root_authorized(dev_t dev) {
    if (dev == 0x11) { ++root_auth_calls; return scenario != 1 && !(scenario == 16 && root_auth_calls == 2); }
    assert(dev == 0x22); return scenario != 17;
}
static int native_verified_block_volume_device(const char *uuid, dev_t *dev, char *path, size_t size) {
    assert(!strcmp(uuid, "8846FE78-2910-4297-AF97-D59BB6F8FC16"));
    if (scenario == 2) return 0;
    *dev = 0x22; strlcpy(path, "/dev/disk1s1", size); return 1;
}
static void *user_pointer(user_addr_t addr, size_t size) {
    assert(addr >= 0x1000 && addr + size <= 0x1000 + sizeof(user_memory));
    return user_memory + (size_t)(addr - 0x1000);
}
static int copyout(const void *src, user_addr_t dst, size_t size) {
    ++copy_calls;
    if ((scenario == 3 && copy_calls == 1) || (scenario == 21 && copy_calls == 4)) return EIO;
    memcpy(user_pointer(dst, size), src, size); return 0;
}
static int mount(proc_t p, struct mount_args *args, int32_t *result) {
    (void)p; (void)result; ++mount_calls;
    assert(!strcmp(user_pointer(args->type, 5), "apfs"));
    assert(!strcmp(user_pointer(args->path, 2), "/"));
    assert(args->flags == (MNT_RDONLY | MNT_UNION));
    const unsigned char *data = user_pointer(args->data, 0x150);
    uint64_t spec, flags; memcpy(&spec, data, 8); memcpy(&flags, data + 8, 8);
    assert(!strcmp(user_pointer(spec, 13), "/dev/disk1s1") && flags == MNT_RDONLY);
    if (scenario == 4) return EIO;
    rootvnode = scenario == 10 ? &os_root : &cache_root;
    os_root.covered = scenario == 18 ? &host_os_mount : &host_cache_mount;
    return 0;
}
static int vnode_lookup(const char *path, int flags, vnode_t *vp, void *ctx) {
    assert(!flags && !ctx);
    if (!strcmp(path, "/")) { if (scenario == 5) return EIO; *vp = &cache_root; }
    else {
        ++path_calls;
        if (!strcmp(path, "/usr/lib/libobjc-trampolines.dylib")) {
            if (scenario == 11) return ENOENT; *vp = &library;
        } else if (!strcmp(path, "/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64")) {
            if (scenario == 19) return EIO; *vp = &cache_file;
        } else {
            assert(!strcmp(path, "/sbin/launchd")); if (scenario == 20) return ENOENT; *vp = &launchd;
        }
    }
    ++(*vp)->io; return 0;
}
static int native_owner_publish_boot_manifest(dev_t os, const char *os_uuid, dev_t cache, const char *cache_uuid) {
    assert(os == 0x11 && cache == 0x22 && path_calls == 3);
    assert(!strcmp(os_uuid, "04769D26-2CA0-4825-B443-E2E5F88300F2"));
    assert(!strcmp(cache_uuid, "8846FE78-2910-4297-AF97-D59BB6F8FC16"));
    ++manifest_calls; return scenario != 15;
}
static void commpage_update_dyld_flags(uint64_t flags) { observed_flags = flags; }
static void native_owner_cache_checkpoint(int error, int authorized) {
    assert(error == native_owner_cache_status && authorized == native_owner_cache_authorized); ++checkpoints;
}
#include "mount-functions.inc"

int main(int argc, char **argv) {
    assert(argc == 2); scenario = atoi(argv[1]); assert(scenario >= 0 && scenario < 23);
    host_os_mount.flags = MNT_RDONLY; host_cache_mount.flags = MNT_RDONLY | MNT_UNION;
    strcpy(host_os_mount.stat.f_fstypename, "apfs"); strcpy(host_cache_mount.stat.f_fstypename, "apfs");
    host_os_mount.device = &os_dev; host_cache_mount.device = &cache_dev; os_dev.dev = 0x11; cache_dev.dev = 0x22;
    os_root.mp = &host_os_mount; cache_root.mp = &host_cache_mount; rootvnode = &os_root;
    library.mp = cache_file.mp = &host_cache_mount; launchd.mp = &host_os_mount;
    library.type = cache_file.type = launchd.type = VREG;
    if (scenario == 6) cache_dev.dev = 0x33;
    if (scenario == 7) host_cache_mount.flags &= ~MNT_RDONLY;
    if (scenario == 8) strcpy(host_cache_mount.stat.f_fstypename, "wrongfs");
    if (scenario == 9) host_cache_mount.flags &= ~MNT_UNION;
    if (scenario == 12) library.mp = &host_os_mount;
    if (scenario == 13) cache_file.type = VDIR;
    if (scenario == 14) launchd.mp = &host_cache_mount;
    if (scenario == 22) host_os_mount.flags = 0;
    user_addr_t env = 0xdead;
    int result = native_owner_cache_prepare(NULL, 0x1000, &env);
    if (scenario == 0) {
        assert(!result && env == 0 && path_calls == 3 && manifest_calls == 1);
        assert(observed_flags == 0x28 && dyld_flags == 0x28 && native_owner_cache_dyld_flags == 0x28);
        assert(native_owner_cache_stage == 6 && native_owner_cache_native_union == 1);
    } else {
        assert(result != 0 && !observed_flags && dyld_flags == 0x20);
        assert(manifest_calls == (scenario == 15 ? 1 : 0));
        if (scenario >= 11 && scenario <= 14) assert(native_owner_cache_stage == 5);
        if (scenario == 1 || scenario == 2 || scenario == 3 || scenario == 21 || scenario == 22) assert(!mount_calls);
    }
    assert(checkpoints == 1 && !os_dev.io && !cache_dev.io && !cache_root.io);
    assert(!library.io && !cache_file.io && !launchd.io);
    printf("PASS scenario=%d\n", scenario);
    return 0;
}
