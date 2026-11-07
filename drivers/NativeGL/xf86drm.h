/*
 * The libdrm surface Mesa's freedreno takes, for a system without Linux DRM.
 *
 * The declarations here are the ones freedreno/drm calls; the implementation in nativegl_drm.c carries each
 * request to the NativeAdreno IOKit user client (NativeAdrenoABI.h) instead of to a kernel DRM device. The
 * request numbers and structures are Linux's, from Mesa's include/drm-uapi/msm_drm.h.
 *
 * Nothing in this file is Linux: a "device fd" is /dev/null, a BO handle is a userspace id, and a BO's
 * memory is mapped by the kernel's user client rather than by mmap() on the device.
 */
#ifndef NATIVEGL_XF86DRM_H
#define NATIVEGL_XF86DRM_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_RENDER  2
#define DRM_CLOEXEC      O_CLOEXEC
#define DRM_RDWR         O_RDWR

typedef struct _drmVersion {
    int version_major, version_minor, version_patchlevel;
    int name_len;
    char *name;
    int date_len;
    char *date;
    int desc_len;
    char *desc;
} drmVersion, *drmVersionPtr;

int drmOpenWithType(const char *name, const char *busid, int type);
int drmOpen(const char *name, const char *busid);
int drmClose(int fd);
int drmIoctl(int fd, unsigned long request, void *arg);
int drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);

/*
 * Buffers as file descriptors: there are none here. Memory somebody else made does come in, as Linux's
 * comes in by file descriptor: an IOSurface, by its ID, where freedreno expects the descriptor.
 */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int surface, uint32_t *handle);
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

/*
 * Not libdrm's: a BO's memory in this process, in place of mmap() on the device (freedreno_bo.c is patched
 * to call these), and the size of an IOSurface's allocation for the import path's lseek().
 */
void *nativegl_bo_map(int fd, uint32_t handle, uint64_t size);
void nativegl_bo_unmap(int fd, uint32_t handle, void *address);
int nativegl_surface_size(int surface_id, uint64_t *size);

/*
 * An IOSurface's memory, described as the OSMesa target needs it to make a texture of it: pixels of
 * `pixel_format` ('BGRA') in `planes` planes (0: none), `height` rows of `row_bytes` from `offset` in
 * the allocation of `size` bytes. `surface` is the ID, in and out; fd is the device's, unused.
 */
struct nativegl_surface {
    uint32_t surface;
    uint32_t width;
    uint32_t height;
    uint32_t row_bytes;
    uint32_t offset;
    uint32_t pixel_format;
    uint32_t planes;
    uint64_t size;
};
int nativegl_surface_info(int fd, struct nativegl_surface *info);

#endif
