/*
 * libdrm for Darwin, over the NativeAdreno user client.
 *
 * Mesa's freedreno speaks Linux's MSM DRM interface: parameters, GEM buffers, submissions, fences and
 * submit queues, with buffers at fixed GPU addresses ("softpin") and, on this platform, IOSurfaces
 * imported as textures. There is no DRM device here, so this file implements those requests directly on
 * the NativeAdreno ABI (NativeAdrenoABI.h, version 2): each GEM handle is a userspace record over a
 * NativeAdreno BO, each GPU address comes from this process's aperture (the client chooses BO addresses,
 * NativeAdrenoDeviceClientIova), and each submission becomes one NativeAdreno Submit of the command
 * buffers the msm submit names. Fences are the kernel's 64-bit values behind the 32-bit ids msm expects.
 *
 * The connection is opened the way the Turnip backend opens it (tu_knl_darwin.cc): the AGX accelerator
 * service with open type 0x4147, every ABI method tunnelled through the selector the sandbox admits
 * ({'NTNL', method, payload}); NATIVEGL_SERVICE=NativeAdrenoGpuPublished with type 0 is the direct,
 * unsandboxed form.
 *
 * Not supported, and refused rather than approximated: relocation patches (softpin means the command
 * stream already holds addresses), dma-buf fds in either direction (an IOSurface travels by its ID),
 * sync objects (numeric fences are enough for freedreno's fallback path) and per-submit input fences.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <IOSurface/IOSurfaceRef.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>

#include <drm-uapi/drm.h>
#include <drm-uapi/msm_drm.h>

#include "NativeAdrenoABI.h"
#include "xf86drm.h"

#define NG_SERVICE_DEFAULT   "IOAcceleratorES"
#define NG_OPEN_TYPE_DEFAULT 0x4147u
#define NG_TUNNEL_MAGIC      0x4e544e4cu   /* 'NTNL', as NativeAdrenoPublish.cpp serves it */

#define NG_MAX_BOS     16384
#define NG_MAX_FENCES  65536
#define NG_MAX_QUEUES  256

struct ng_bo {
    unsigned used, cpu_mapped, imported;
    uint32_t flags, token;
    uint64_t khandle, size, iova;
    uint64_t fence;               /* the last submit that named this BO: freedreno, etc. */
    void *map;
    IOSurfaceRef surface;
};

struct ng_range {
    struct ng_range *next;
    uint64_t start, end;
};

static struct {
    int state;                     /* 0 unopened, 1 open, -1 failed */
    io_connect_t conn;
    int tunnel;
    uint32_t selector;
    NativeAdrenoAbiDeviceInfo info;
    NativeAdrenoAbiParams params;
} ng;

static pthread_mutex_t ng_lock = PTHREAD_MUTEX_INITIALIZER;   /* tables and connection state */
static pthread_mutex_t ng_call_lock = PTHREAD_MUTEX_INITIALIZER;

static struct ng_bo ng_bos[NG_MAX_BOS];
static struct ng_range *ng_ranges;
static struct { uint32_t id; uint64_t fence; } ng_fences[NG_MAX_FENCES];
static uint32_t ng_fence_seq = 1;
static struct { uint32_t id; uint64_t queue; } ng_queues[NG_MAX_QUEUES];

static int ng_fd = -1;

static int
ng_errno(kern_return_t kr)
{
    switch (kr) {
    case KERN_SUCCESS:          return 0;
    case kIOReturnNoMemory:     return ENOMEM;
    case kIOReturnBadArgument:  return EINVAL;
    case kIOReturnTimeout:      return ETIMEDOUT;
    case kIOReturnBusy:         return EBUSY;
    case kIOReturnNotFound:     return ENOENT;
    case kIOReturnUnsupported:  return ENOSYS;
    default:                    return EIO;
    }
}

static kern_return_t
ng_call(uint32_t method, const void *in, size_t in_size, void *out, size_t out_size)
{
    size_t out_bytes = out_size;
    kern_return_t kr;

    pthread_mutex_lock(&ng_call_lock);
    if (!ng.tunnel) {
        kr = IOConnectCallStructMethod(ng.conn, method, in, in_size, out_size ? out : NULL,
                                       out_size ? &out_bytes : NULL);
    } else {
        uint8_t buffer[8 + sizeof(NativeAdrenoAbiSubmit)];
        if (in_size > sizeof(NativeAdrenoAbiSubmit)) {
            pthread_mutex_unlock(&ng_call_lock);
            return kIOReturnBadArgument;
        }
        const uint32_t header[2] = {NG_TUNNEL_MAGIC, method};
        memcpy(buffer, header, sizeof(header));
        if (in_size)
            memcpy(buffer + sizeof(header), in, in_size);
        kr = IOConnectCallStructMethod(ng.conn, ng.selector, buffer, sizeof(header) + in_size,
                                       out_size ? out : NULL, out_size ? &out_bytes : NULL);
    }
    pthread_mutex_unlock(&ng_call_lock);
    return kr;
}

static void
ng_ranges_add(uint64_t start, uint64_t end)
{
    struct ng_range **at = &ng_ranges, *range;
    while (*at && (*at)->start < start)
        at = &(*at)->next;
    range = malloc(sizeof(*range));
    if (!range)
        return;
    range->start = start;
    range->end = end;
    range->next = *at;
    *at = range;
    /* coalesce with the following and the preceding range */
    if (range->next && range->end == range->next->start) {
        range->end = range->next->end;
        struct ng_range *gone = range->next;
        range->next = gone->next;
        free(gone);
    }
    if (at != &ng_ranges) {
        struct ng_range *prev = ng_ranges;
        while (prev->next != range)
            prev = prev->next;
        if (prev->end == range->start) {
            prev->end = range->end;
            prev->next = range->next;
            free(range);
        }
    }
}

static uint64_t
ng_iova_alloc(uint64_t size)
{
    const uint64_t page = 4096;
    struct ng_range **at = &ng_ranges;

    size = (size + page - 1) & ~(page - 1);
    if (!size)
        return 0;
    while (*at) {
        struct ng_range *range = *at;
        if (range->end - range->start >= size) {
            const uint64_t start = range->start;
            range->start += size;
            if (range->start == range->end) {
                *at = range->next;
                free(range);
            }
            return start;
        }
        at = &range->next;
    }
    return 0;
}

static void
ng_iova_free(uint64_t start, uint64_t size)
{
    size = (size + 4095) & ~(uint64_t)4095;
    if (size)
        ng_ranges_add(start, start + size);
}

static int
ng_open_locked(void)
{
    const char *service = getenv("NATIVEGL_SERVICE");
    const char *type_text = getenv("NATIVEGL_OPEN_TYPE");
    uint32_t open_type = type_text && *type_text ? (uint32_t)strtoul(type_text, NULL, 0) : NG_OPEN_TYPE_DEFAULT;
    const int direct = service && service[0] && !strcmp(service, "NativeAdrenoGpuPublished");
    kern_return_t kr;
    io_service_t svc;

    if (!service || !*service)
        service = NG_SERVICE_DEFAULT;
    /* Both published services accept the plugin's open type (0x4147); "direct" only skips the tunnel. */

    svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(service));
    if (!svc)
        return ENODEV;
    kr = IOServiceOpen(svc, mach_task_self(), open_type, &ng.conn);
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS)
        return EIO;
    ng.tunnel = !direct;
    if (ng.tunnel) {
        const uint32_t header[2] = {NG_TUNNEL_MAGIC, 0xffffffffu};
        unsigned s;
        for (s = 0; s < 256; s++) {
            if (IOConnectCallStructMethod(ng.conn, s, header, sizeof(header), NULL, NULL) == KERN_SUCCESS) {
                ng.selector = s;
                break;
            }
        }
        if (s == 256) {
            IOServiceClose(ng.conn);
            ng.conn = 0;
            return EIO;
        }
    }
    kr = ng_call(NativeAdrenoMethodQuery, NULL, 0, &ng.params, sizeof(ng.params));
    if (kr == KERN_SUCCESS && ng.params.abi_version != NativeAdrenoAbiVersion)
        kr = kIOReturnUnsupported;
    if (kr == KERN_SUCCESS)
        kr = ng_call(NativeAdrenoMethodGetDeviceInfo, NULL, 0, &ng.info, sizeof(ng.info));
    if (kr != KERN_SUCCESS) {
        IOServiceClose(ng.conn);
        ng.conn = 0;
        return EIO;
    }
    if (!(ng.info.flags & NativeAdrenoDeviceClientIova)) {
        IOServiceClose(ng.conn);
        ng.conn = 0;
        return ENODEV;   /* softpin is what this libdrm stands on */
    }
    {
        const uint64_t start = (ng.info.va_start + 4095) & ~(uint64_t)4095;
        const uint64_t end = ng.info.va_start + ng.info.va_size;
        if (end > start)
            ng_ranges_add(start, end);
    }
    return 0;
}

static int
ng_ensure_open(void)
{
    int error = 0;

    pthread_mutex_lock(&ng_lock);
    if (!ng.state)
        ng.state = (error = ng_open_locked()) ? -1 : 1;
    else if (ng.state < 0)
        error = ENODEV;
    pthread_mutex_unlock(&ng_lock);
    return error;
}

/* NATIVEGL_DEBUG=1: why a request was refused (the host check uses it; no line on the phone unless set). */
static void
ng_debug(const char *what, uint64_t a, uint64_t b)
{
    if (getenv("NATIVEGL_DEBUG"))
        fprintf(stderr, "nativegl: %s (%llu, %llu)\n", what, (unsigned long long)a, (unsigned long long)b);
}

static struct ng_bo *
ng_bo_lookup_locked(uint32_t handle)
{
    struct ng_bo *bo;
    if (!handle || handle > NG_MAX_BOS)
        return NULL;
    bo = &ng_bos[handle - 1];
    return bo->used ? bo : NULL;
}

static struct ng_bo *
ng_bo_new_locked(uint64_t khandle, uint64_t size, uint32_t flags, unsigned imported)
{
    unsigned i;
    for (i = 0; i < NG_MAX_BOS; i++) {
        if (!ng_bos[i].used) {
            struct ng_bo *bo = &ng_bos[i];
            memset(bo, 0, sizeof(*bo));
            bo->used = 1;
            bo->khandle = khandle;
            bo->size = size;
            bo->flags = flags;
            bo->imported = imported;
            return bo;
        }
    }
    return NULL;
}

static uint32_t
ng_bo_handle(const struct ng_bo *bo)
{
    return (uint32_t)(bo - ng_bos) + 1;
}

static int
ng_bo_iova_locked(struct ng_bo *bo, uint64_t *out)
{
    NativeAdrenoAbiBoMap map = {};
    NativeAdrenoAbiBoMapReply reply = {};
    kern_return_t kr;

    if (bo->iova) {
        *out = bo->iova;
        return 0;
    }
    bo->iova = ng_iova_alloc(bo->size);
    if (!bo->iova)
        return ENOMEM;
    map.handle = bo->khandle;
    map.iova = bo->iova;
    map.permissions = 3;
    map.writeback = 0;
    map.shareability = 2;
    kr = ng_call(NativeAdrenoMethodBoMap, &map, sizeof(map), &reply, sizeof(reply));
    if (kr != KERN_SUCCESS) {
        ng_iova_free(bo->iova, bo->size);
        bo->iova = 0;
        return ng_errno(kr);
    }
    *out = bo->iova;
    return 0;
}

static int
ng_bo_close_locked(uint32_t handle)
{
    struct ng_bo *bo = ng_bo_lookup_locked(handle);
    NativeAdrenoAbiBoFree free_req = {};

    if (!bo)
        return EINVAL;
    if (bo->cpu_mapped) {
        NativeAdrenoAbiBoCpuUnmap unmap = {.handle = bo->khandle, .token = bo->token};
        IOConnectUnmapMemory64(ng.conn, bo->token, mach_task_self(), (mach_vm_address_t)(uintptr_t)bo->map);
        ng_call(NativeAdrenoMethodBoCpuUnmap, &unmap, sizeof(unmap), NULL, 0);
        bo->cpu_mapped = 0;
    }
    /* The kernel holds this BO's pages for as long as a submitted job names it. Freeing it behind that job
     * would hand its IOVA to the next allocation while the GPU still reads or writes it. Wait for the last
     * submit; if it will not retire, keep the record (the kernel handle and its IOVA stay live) rather than
     * poison the aperture allocator with a range that is still mapped. */
    if (bo->fence) {
        NativeAdrenoAbiWaitFence wait = {.fence = bo->fence, .timeout_ns = 500000000ull};
        if (ng_call(NativeAdrenoMethodWaitFence, &wait, sizeof(wait), NULL, 0) != KERN_SUCCESS) {
            ng_debug("bo close: fence waits", bo->fence, bo->khandle);
            return 0;   /* leaked deliberately: still in use */
        }
        bo->fence = 0;
    }
    free_req.handle = bo->khandle;
    if (ng_call(NativeAdrenoMethodBoFree, &free_req, sizeof(free_req), NULL, 0) != KERN_SUCCESS) {
        /* the kernel refused (a mapping lease or a job still holds it): leave the record and its IOVA alone */
        ng_debug("bo close: free refused", bo->khandle, bo->iova);
        return 0;
    }
    if (bo->iova)
        ng_iova_free(bo->iova, bo->size);
    if (bo->surface)
        CFRelease(bo->surface);
    memset(bo, 0, sizeof(*bo));
    return 0;
}

/* An IOSurface, by its ID, as a BO: the kernel pins the pages the surface's mapping uses. A surface already
 * imported is the same BO (as on Linux, where the same memory is the same GEM handle): CoreAnimation attaches
 * its drawable again and again, and every attach must not pin the pages again. */
static int
ng_surface_import_locked(int surface_id, uint32_t *handle)
{
    IOSurfaceRef surface;
    NativeAdrenoAbiBoImport import = {};
    NativeAdrenoAbiBoAllocReply reply = {};
    struct ng_bo *bo;
    unsigned i;
    kern_return_t kr;

    for (i = 0; i < NG_MAX_BOS; i++) {
        if (ng_bos[i].used && ng_bos[i].surface && IOSurfaceGetID(ng_bos[i].surface) == (IOSurfaceID)surface_id) {
            *handle = ng_bo_handle(&ng_bos[i]);
            return 0;
        }
    }
    surface = IOSurfaceLookup((IOSurfaceID)surface_id);
    if (!surface)
        return ENOENT;
    import.address = (uint64_t)(uintptr_t)IOSurfaceGetBaseAddress(surface);
    import.bytes = IOSurfaceGetAllocSize(surface);
    if (!import.address || !import.bytes || (import.address & 4095) || (import.bytes & 4095)) {
        CFRelease(surface);
        return EINVAL;
    }
    kr = ng_call(NativeAdrenoMethodBoImport, &import, sizeof(import), &reply, sizeof(reply));
    if (kr != KERN_SUCCESS) {
        CFRelease(surface);
        return ng_errno(kr);
    }
    bo = ng_bo_new_locked(reply.handle, import.bytes, 0, 1);
    if (!bo) {
        NativeAdrenoAbiBoFree free_req = {.handle = reply.handle};
        ng_call(NativeAdrenoMethodBoFree, &free_req, sizeof(free_req), NULL, 0);
        CFRelease(surface);
        return ENOMEM;
    }
    bo->surface = surface;
    *handle = ng_bo_handle(bo);
    return 0;
}

static uint64_t
ng_fence_lookup(uint32_t id)
{
    const uint32_t slot = id % NG_MAX_FENCES;
    pthread_mutex_lock(&ng_lock);
    const uint64_t fence = (id && ng_fences[slot].id == id) ? ng_fences[slot].fence : 0;
    pthread_mutex_unlock(&ng_lock);
    return fence;
}

static int
ng_msm_param(void *data)
{
    struct drm_msm_param *req = data;
    NativeAdrenoAbiTimestamp ts = {};
    NativeAdrenoAbiStatus status = {};
    kern_return_t kr;

    switch (req->param) {
    case MSM_PARAM_GPU_ID: {
        /* The kernel reports a chip id, freedreno wants a GPU number; NATIVEGL_GPU_ID overrides it, as the
         * host check's simulated board reports a chip id of its own. */
        const char *override = getenv("NATIVEGL_GPU_ID");
        req->value = override && *override ? strtoul(override, NULL, 0) :
                     ((uint64_t)((ng.info.chip_id >> 24) & 0xff) * 100) +
                     ((uint64_t)((ng.info.chip_id >> 16) & 0xff) * 10) +
                     ((uint64_t)((ng.info.chip_id >> 8) & 0xff));
        return 0;
    }
    case MSM_PARAM_CHIP_ID:
        req->value = ng.info.chip_id;
        return 0;
    case MSM_PARAM_GMEM_SIZE:
        req->value = ng.info.gmem_bytes;
        return 0;
    case MSM_PARAM_GMEM_BASE:
        req->value = ng.info.gmem_base;
        return 0;
    case MSM_PARAM_MAX_FREQ:
        req->value = 800000000;   /* informational; there is no runtime DVFS here */
        return 0;
    case MSM_PARAM_TIMESTAMP:
        kr = ng_call(NativeAdrenoMethodGetTimestamp, NULL, 0, &ts, sizeof(ts));
        if (kr != KERN_SUCCESS)
            return ng_errno(kr);
        req->value = ts.ticks;
        return 0;
    case MSM_PARAM_PRIORITIES:
        req->value = 1;
        return 0;
    case MSM_PARAM_FAULTS:
        kr = ng_call(NativeAdrenoMethodGetStatus, NULL, 0, &status, sizeof(status));
        req->value = kr == KERN_SUCCESS ? status.faults : 0;
        return 0;
    case MSM_PARAM_SUSPENDS:
        kr = ng_call(NativeAdrenoMethodGetStatus, NULL, 0, &status, sizeof(status));
        req->value = kr == KERN_SUCCESS ? status.suspends : 0;
        return 0;
    case MSM_PARAM_VA_START:
        req->value = ng.info.va_start;
        return 0;
    case MSM_PARAM_VA_SIZE:
        req->value = ng.info.va_size;
        return 0;
    case MSM_PARAM_HIGHEST_BANK_BIT:
        req->value = ng.info.highest_bank_bit;
        return 0;
    case MSM_PARAM_UBWC_SWIZZLE:
        req->value = ng.info.ubwc_swizzle;
        return 0;
    case MSM_PARAM_MACROTILE_MODE:
        req->value = ng.info.macrotile_mode;
        return 0;
    default:
        return EINVAL;
    }
}

static int
ng_msm_gem_new(void *data)
{
    struct drm_msm_gem_new *req = data;
    NativeAdrenoAbiBoAlloc alloc = {};
    NativeAdrenoAbiBoAllocReply reply = {};
    struct ng_bo *bo;
    kern_return_t kr;

    if (!req->size)
        return EINVAL;
    alloc.bytes = (req->size + 4095) & ~(uint64_t)4095;
    alloc.align_log2 = 12;
    alloc.contiguous = 0;
    kr = ng_call(NativeAdrenoMethodBoAlloc, &alloc, sizeof(alloc), &reply, sizeof(reply));
    if (kr != KERN_SUCCESS)
        return ng_errno(kr);
    bo = ng_bo_new_locked(reply.handle, req->size, req->flags, 0);
    if (!bo) {
        NativeAdrenoAbiBoFree free_req = {.handle = reply.handle};
        ng_call(NativeAdrenoMethodBoFree, &free_req, sizeof(free_req), NULL, 0);
        return ENOMEM;
    }
    req->handle = ng_bo_handle(bo);
    return 0;
}

static int
ng_msm_gem_info(void *data, int write_only)
{
    struct drm_msm_gem_info *req = data;
    struct ng_bo *bo = ng_bo_lookup_locked(req->handle);

    if (!bo)
        return EINVAL;
    switch (req->info) {
    case MSM_INFO_GET_OFFSET:
    case MSM_INFO_GET_IOVA: {
        uint64_t iova = 0;
        const int error = ng_bo_iova_locked(bo, &iova);
        if (error)
            return error;
        req->value = iova;
        return 0;
    }
    case MSM_INFO_SET_IOVA: {
        uint64_t iova = req->value & ~(uint64_t)4095;
        if (iova && ng_bo_iova_locked(bo, &iova))
            return EINVAL;
        return 0;
    }
    case MSM_INFO_SET_NAME:
        return 0;   /* debug name only */
    case MSM_INFO_GET_NAME:
        req->len = 0;
        return 0;
    case MSM_INFO_GET_FLAGS:
        req->value = bo->flags;
        return 0;
    case MSM_INFO_SET_METADATA:
    case MSM_INFO_GET_METADATA:
        if (write_only)
            return 0;
        req->len = 0;
        req->value = 0;
        return 0;
    default:
        return EINVAL;
    }
}

static int
ng_msm_submit(void *data)
{
    struct drm_msm_gem_submit *req = data;
    const struct drm_msm_gem_submit_bo *bos = (const void *)(uintptr_t)req->bos;
    const struct drm_msm_gem_submit_cmd *cmds = (const void *)(uintptr_t)req->cmds;
    NativeAdrenoAbiSubmit submit = {};
    NativeAdrenoAbiSubmitReply reply = {};
    uint64_t queue = 0;
    unsigned i, count = 0;
    uint32_t fence_id;
    kern_return_t kr;

    if (!req->nr_cmds || req->nr_cmds > NativeAdrenoAbiMaxBuffers || !cmds) {
        ng_debug("submit: nr_cmds/ cmds", req->nr_cmds, (uint64_t)(uintptr_t)cmds);
        return EINVAL;
    }
    if (req->nr_bos && !bos) {
        ng_debug("submit: nr_bos/ bos", req->nr_bos, (uint64_t)(uintptr_t)bos);
        return EINVAL;
    }
    if (req->queueid) {
        /* ids are the slot plus one (see ng_msm_queue_new) */
        const uint32_t slot = (req->queueid - 1) % NG_MAX_QUEUES;
        if (ng_queues[slot].id != req->queueid) {
            ng_debug("submit: queueid", req->queueid, ng_queues[slot].id);
            return EINVAL;
        }
        queue = ng_queues[slot].queue;
    }
    submit.queue = queue;
    for (i = 0; i < req->nr_cmds; i++) {
        const struct drm_msm_gem_submit_cmd *cmd = &cmds[i];
        struct ng_bo *bo;
        uint64_t iova;

        if (cmd->type != MSM_SUBMIT_CMD_BUF) {
            /* IB2/CTX_RESTORE are msm's legacy pre-IB forms; this kernel takes plain IBs only. */
            ng_debug("submit: cmd type", cmd->type, i);
            return EINVAL;
        }
        if (!cmd->size) {
            /* a zero-length IB has nothing to run; extending it to the rest of the BO would execute a heap block */
            continue;
        }
        if (cmd->submit_idx >= req->nr_bos) {
            ng_debug("submit: submit_idx", cmd->submit_idx, req->nr_bos);
            return EINVAL;
        }
        bo = ng_bo_lookup_locked(bos[cmd->submit_idx].handle);
        if (!bo) {
            ng_debug("submit: handle", bos[cmd->submit_idx].handle, i);
            return EINVAL;
        }
        if (ng_bo_iova_locked(bo, &iova)) {
            ng_debug("submit: iova", bo->khandle, bo->size);
            return EINVAL;
        }
        submit.buffers[count].handle = bo->khandle;
        submit.buffers[count].iova = iova + cmd->submit_offset;
        submit.buffers[count].bytes = cmd->size ? cmd->size : (bo->size - cmd->submit_offset);
        count++;
    }
    if (!count)
        return EINVAL;
    /* This port has no sync objects and no sync_file: a submit that arrives or leaves by file descriptor is
     * refused rather than run without the ordering it asked for. */
    if ((req->flags & (MSM_SUBMIT_FENCE_FD_IN | MSM_SUBMIT_FENCE_FD_OUT)) || req->in_syncobjs || req->out_syncobjs) {
        ng_debug("submit: sync objects/fence fds", req->flags, req->nr_in_syncobjs + req->nr_out_syncobjs);
        return EINVAL;
    }
    submit.buffer_count = count;
    /* A full ring is kIOReturnNoResources/kIOReturnBusy here where Linux msm blocks: wait for space, a few times. */
    for (unsigned attempt = 0; ; attempt++) {
        kr = ng_call(NativeAdrenoMethodSubmit, &submit, sizeof(submit), &reply, sizeof(reply));
        if (kr == KERN_SUCCESS)
            break;
        if (kr != kIOReturnNoResources && kr != kIOReturnBusy)
            return ng_errno(kr);
        if (attempt >= 50) {
            ng_debug("submit: ring full", attempt, 0);
            return EBUSY;
        }
        usleep(200);
    }
    for (i = 0; i < req->nr_bos; i++) {
        struct ng_bo *bo = ng_bo_lookup_locked(bos[i].handle);
        if (bo && reply.fence > bo->fence)
            bo->fence = reply.fence;
    }
    fence_id = ng_fence_seq++;
    if (!ng_fence_seq)
        ng_fence_seq = 1;
    /* the caller holds ng_lock (drmCommandWriteRead's submit case); taking it again here would deadlock */
    ng_fences[fence_id % NG_MAX_FENCES].id = fence_id;
    ng_fences[fence_id % NG_MAX_FENCES].fence = reply.fence;
    req->fence = fence_id;
    return 0;
}

static int
ng_msm_wait(void *data)
{
    struct drm_msm_wait_fence *req = data;
    struct timespec now;
    int64_t rel;
    NativeAdrenoAbiWaitFence wait = {};
    const uint64_t fence = ng_fence_lookup(req->fence);
    kern_return_t kr;

    if (!fence)
        return 0;   /* an id this process no longer tracks has long retired */
    /* The same clock freedreno builds req->timeout from (msm_priv.h uses CLOCK_MONOTONIC_RAW on Darwin,
     * whose CLOCK_MONOTONIC is another clock): the difference is the relative wait the ABI takes. */
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    rel = ((int64_t)req->timeout.tv_sec - now.tv_sec) * 1000000000LL +
          ((int64_t)req->timeout.tv_nsec - now.tv_nsec);
    if (rel < 0)
        rel = 0;
    wait.fence = fence;
    wait.timeout_ns = (uint64_t)rel;
    kr = ng_call(NativeAdrenoMethodWaitFence, &wait, sizeof(wait), NULL, 0);
    return ng_errno(kr);
}

static int
ng_msm_queue_new(void *data)
{
    struct drm_msm_submitqueue *req = data;
    NativeAdrenoAbiQueue create = {};
    NativeAdrenoAbiQueueReply reply = {};
    unsigned i;
    kern_return_t kr;

    kr = ng_call(NativeAdrenoMethodCreateQueue, &create, sizeof(create), &reply, sizeof(reply));
    if (kr != KERN_SUCCESS)
        return ng_errno(kr);
    /* The id is the slot (plus one): submits look the queue up by id % NG_MAX_QUEUES. */
    for (i = 0; i < NG_MAX_QUEUES; i++) {
        if (!ng_queues[i].id) {
            ng_queues[i].id = i + 1;
            ng_queues[i].queue = reply.queue;
            req->id = i + 1;
            return 0;
        }
    }
    return ENOMEM;
}

static int
ng_msm_queue_close(uint32_t id)
{
    unsigned i;
    for (i = 0; i < NG_MAX_QUEUES; i++) {
        if (ng_queues[i].id == id) {
            NativeAdrenoAbiQueue close = {.queue = ng_queues[i].queue};
            ng_call(NativeAdrenoMethodCloseQueue, &close, sizeof(close), NULL, 0);
            ng_queues[i].id = 0;
            ng_queues[i].queue = 0;
            return 0;
        }
    }
    return EINVAL;
}

static int
ng_msm_queue_query(void *data)
{
    struct drm_msm_submitqueue_query *req = data;
    NativeAdrenoAbiStatus status = {};
    uint64_t *out = (uint64_t *)(uintptr_t)req->data;
    kern_return_t kr;

    if (req->param != MSM_SUBMITQUEUE_PARAM_FAULTS)
        return EINVAL;
    if (!out || req->len < sizeof(*out))
        return EINVAL;
    kr = ng_call(NativeAdrenoMethodGetStatus, NULL, 0, &status, sizeof(status));
    if (kr != KERN_SUCCESS)
        return ng_errno(kr);
    *out = status.faults;
    return 0;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
    int error;
    (void)busid;
    (void)type;
    if (!name || strcmp(name, "msm"))
        return -1;
    if ((error = ng_ensure_open())) {
        errno = error;
        return -1;
    }
    if (ng_fd < 0) {
        ng_fd = open("/dev/null", O_RDWR);
        if (ng_fd < 0)
            return -1;
    }
    return dup(ng_fd);   /* freedreno closes its device fd; the connection stays */
}

int
drmOpen(const char *name, const char *busid)
{
    return drmOpenWithType(name, busid, DRM_NODE_RENDER);
}

int
drmClose(int fd)
{
    return close(fd);
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
    int error;
    (void)fd;
    if ((error = ng_ensure_open())) {
        errno = error;
        return -1;
    }
    pthread_mutex_lock(&ng_lock);
    switch (request) {
    case DRM_IOCTL_GEM_CLOSE: {
        struct drm_gem_close *req = arg;
        error = ng_bo_close_locked(req->handle);
        break;
    }
    case DRM_IOCTL_PRIME_FD_TO_HANDLE: {
        struct drm_prime_handle *req = arg;
        error = ng_surface_import_locked(req->fd, &req->handle);
        break;
    }
    default:
        error = ENOSYS;
        break;
    }
    pthread_mutex_unlock(&ng_lock);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
    int error;
    (void)fd;
    (void)size;
    if ((error = ng_ensure_open()))
        return -error;
    switch (index) {
    case DRM_MSM_GEM_CPU_PREP:
        return 0;   /* buffers are CPU-write-combined: CPU stores are visible to the GPU after a barrier */
    case DRM_MSM_GEM_CPU_FINI:
        return 0;
    case DRM_MSM_WAIT_FENCE:
        /* ng_msm_wait takes the lock for the fence it looks up; taking it here too would deadlock on it. */
        error = ng_msm_wait((void *)data);
        return error ? -error : 0;
    case DRM_MSM_SUBMITQUEUE_CLOSE: {
        uint32_t id;
        memcpy(&id, data, sizeof(id));
        pthread_mutex_lock(&ng_lock);
        error = ng_msm_queue_close(id);
        pthread_mutex_unlock(&ng_lock);
        return error ? -error : 0;
    }
    case DRM_MSM_GEM_INFO:
        pthread_mutex_lock(&ng_lock);
        error = ng_msm_gem_info((void *)data, 1);
        pthread_mutex_unlock(&ng_lock);
        return error ? -error : 0;
    default:
        return -ENOSYS;
    }
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
    int error;
    (void)fd;
    (void)size;
    if ((error = ng_ensure_open()))
        return -error;
    pthread_mutex_lock(&ng_lock);
    switch (index) {
    case DRM_MSM_GET_PARAM:
        error = ng_msm_param(data);
        break;
    case DRM_MSM_SET_PARAM:
        error = 0;   /* SYSPROF/comm overrides have no kernel meaning here */
        break;
    case DRM_MSM_GEM_NEW:
        error = ng_msm_gem_new(data);
        break;
    case DRM_MSM_GEM_INFO:
        error = ng_msm_gem_info(data, 0);
        break;
    case DRM_MSM_GEM_SUBMIT:
        error = ng_msm_submit(data);
        break;
    case DRM_MSM_GEM_MADVISE: {
        struct drm_msm_gem_madvise *req = data;
        struct ng_bo *bo = ng_bo_lookup_locked(req->handle);
        if (!bo)
            error = EINVAL;
        else {
            req->retained = 1;   /* pages are not dropped on this port */
            error = 0;
        }
        break;
    }
    case DRM_MSM_SUBMITQUEUE_NEW:
        error = ng_msm_queue_new(data);
        break;
    case DRM_MSM_SUBMITQUEUE_QUERY:
        error = ng_msm_queue_query(data);
        break;
    default:
        error = ENOSYS;
        break;
    }
    pthread_mutex_unlock(&ng_lock);
    return error ? -error : 0;
}

drmVersionPtr
drmGetVersion(int fd)
{
    /* 1.7: softpin, submit queues, madvise and fences are available; cached-coherent memory is not (8). */
    drmVersionPtr version = calloc(1, sizeof(*version));
    (void)fd;
    if (!version)
        return NULL;
    version->version_major = 1;
    version->version_minor = 7;
    version->name = strdup("msm");
    version->name_len = 3;
    version->date = strdup("");
    version->desc = strdup("Adreno on Darwin (NativeAdreno)");
    version->desc_len = (int)strlen(version->desc);
    return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
    if (!version)
        return;
    free(version->name);
    free(version->date);
    free(version->desc);
    free(version);
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
    (void)fd;
    (void)capability;
    if (value)
        *value = 0;
    return 0;
}

/* Nothing here is a sync object: DRM_CAP_SYNCOBJ reads zero, so freedreno takes its numeric-fence paths.
 * The functions are still referred to by the library. */
int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
    (void)fd; (void)flags; (void)handle;
    errno = ENOSYS;
    return -1;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
    (void)fd; (void)handle;
    errno = ENOSYS;
    return -1;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
    (void)fd; (void)handles; (void)count;
    errno = ENOSYS;
    return -1;
}

int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
    (void)fd; (void)obj_fd; (void)handle;
    errno = ENOSYS;
    return -1;
}

int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
    (void)fd; (void)handle; (void)obj_fd;
    errno = ENOSYS;
    return -1;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
    (void)fd; (void)handle; (void)sync_file_fd;
    errno = ENOSYS;
    return -1;
}

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
    (void)fd; (void)handle; (void)sync_file_fd;
    errno = ENOSYS;
    return -1;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
    int error;
    (void)fd;
    if ((error = ng_ensure_open())) {
        errno = error;
        return -1;
    }
    pthread_mutex_lock(&ng_lock);
    error = ng_surface_import_locked(surface, handle);
    pthread_mutex_unlock(&ng_lock);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
    (void)fd;
    (void)handle;
    (void)flags;
    (void)prime_fd;
    errno = ENOSYS;
    return -1;
}

int
nativegl_surface_info(int fd, struct nativegl_surface *info)
{
    IOSurfaceRef surface;
    (void)fd;
    if (!info || !(surface = IOSurfaceLookup((IOSurfaceID)info->surface)))
        return -1;
    info->width = (uint32_t)IOSurfaceGetWidth(surface);
    info->height = (uint32_t)IOSurfaceGetHeight(surface);
    info->row_bytes = (uint32_t)IOSurfaceGetBytesPerRow(surface);
    info->offset = 0;
    info->size = IOSurfaceGetAllocSize(surface);
    info->pixel_format = (uint32_t)IOSurfaceGetPixelFormat(surface);
    info->planes = (uint32_t)IOSurfaceGetPlaneCount(surface);
    CFRelease(surface);
    return 0;
}

int
nativegl_surface_size(int surface_id, uint64_t *size)
{
    IOSurfaceRef surface = IOSurfaceLookup((IOSurfaceID)surface_id);
    if (!surface)
        return -1;
    *size = IOSurfaceGetAllocSize(surface);
    CFRelease(surface);
    return *size ? 0 : -1;
}

void *
nativegl_bo_map(int fd, uint32_t handle, uint64_t size)
{
    (void)fd;
    mach_vm_address_t address = 0;
    mach_vm_size_t mapped = 0;
    NativeAdrenoAbiBoCpuMap req = {};
    NativeAdrenoAbiBoCpuMapReply reply = {};
    struct ng_bo *bo;
    kern_return_t kr;

    if (ng_ensure_open())
        return (void *)-1;
    pthread_mutex_lock(&ng_lock);
    bo = ng_bo_lookup_locked(handle);
    if (!bo || bo->imported) {
        pthread_mutex_unlock(&ng_lock);
        errno = EINVAL;
        return (void *)-1;
    }
    if (bo->cpu_mapped) {
        void *map = bo->map;
        pthread_mutex_unlock(&ng_lock);
        return map;
    }
    req.handle = bo->khandle;
    kr = ng_call(NativeAdrenoMethodBoCpuMap, &req, sizeof(req), &reply, sizeof(reply));
    if (kr == KERN_SUCCESS) {
        /* the ABI: BOs that are cacheable for the CPU (owned, coherent) are mapped with the default cache mode;
         * the rest are write-combined */
        const IOOptionBits cache = (ng.params.flags & NativeAdrenoParamsOwnedCoherent) ? kIOMapDefaultCache
                                                                                       : kIOMapWriteCombineCache;
        kr = IOConnectMapMemory64(ng.conn, reply.token, mach_task_self(), &address, &mapped,
                                  kIOMapAnywhere | cache);
        if (kr == KERN_SUCCESS && (!address || mapped < size)) {
            if (address)
                IOConnectUnmapMemory64(ng.conn, reply.token, mach_task_self(), address);
            kr = kIOReturnNoSpace;
        }
        if (kr != KERN_SUCCESS) {
            NativeAdrenoAbiBoCpuUnmap unmap = {.handle = bo->khandle, .token = reply.token};
            ng_call(NativeAdrenoMethodBoCpuUnmap, &unmap, sizeof(unmap), NULL, 0);
        }
    }
    if (kr != KERN_SUCCESS) {
        pthread_mutex_unlock(&ng_lock);
        errno = EIO;
        return (void *)-1;
    }
    bo->cpu_mapped = 1;
    bo->token = reply.token;
    bo->map = (void *)(uintptr_t)address;
    pthread_mutex_unlock(&ng_lock);
    return bo->map;
}

void
nativegl_bo_unmap(int fd, uint32_t handle, void *address)
{
    struct ng_bo *bo;
    (void)fd;
    (void)address;
    pthread_mutex_lock(&ng_lock);
    bo = ng_bo_lookup_locked(handle);
    if (bo && bo->cpu_mapped) {
        NativeAdrenoAbiBoCpuUnmap unmap = {.handle = bo->khandle, .token = bo->token};
        IOConnectUnmapMemory64(ng.conn, bo->token, mach_task_self(), (mach_vm_address_t)(uintptr_t)bo->map);
        ng_call(NativeAdrenoMethodBoCpuUnmap, &unmap, sizeof(unmap), NULL, 0);
        bo->cpu_mapped = 0;
        bo->map = NULL;
    }
    pthread_mutex_unlock(&ng_lock);
}
