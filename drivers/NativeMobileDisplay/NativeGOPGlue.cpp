// SPDX-License-Identifier: BSD-2-Clause-Patent
// PICASSO display side of the GPU scanout hand-off.
//
// NativeGOPPresenter.cpp (copied from the upstream NativeMobileDisplay driver)
// owns the firmware GOP mapping and the one-outstanding-swap wire protocol.
// This file creates and attaches the presenter on the same IOPlatformDevice
// nub the display uses, and forwards the display's per-frame offer. The
// presenter's strong native_gop_handoff_call/memory definitions override the
// weak kIOReturnUnsupported fallbacks in NativeAdrenoStubs.c, which is what
// turns the plugin's P2 ee00002c7 into P4 (serving).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" IOService *native_gop_presenter_create(void);
extern "C" bool native_gop_gpu_offer(IOService *, void *, IOMemoryDescriptor *,
    uint64_t, uint32_t, uint32_t);

static IOService *gGopPresenter;

extern "C" bool native_gop_glue_start(IOService *nub) {
    if (gGopPresenter) return true;
    if (!nub) return false;
    IOService *presenter = native_gop_presenter_create();
    if (!presenter) return false;
    if (!presenter->init() || !presenter->attach(nub) || !presenter->start(nub)) {
        if (presenter->getProvider()) presenter->detach(nub);
        presenter->release();
        printf("PICASSO_DISPLAY gpu presenter unavailable\n");
        return false;
    }
    gGopPresenter = presenter;
    printf("PICASSO_DISPLAY gpu presenter attached\n");
    return true;
}

extern "C" bool native_legacy_gpu_offer(void *display, IOMemoryDescriptor *memory,
    uint64_t stride, uint32_t width, uint32_t height) {
    return gGopPresenter &&
        native_gop_gpu_offer(gGopPresenter, display, memory, stride, width, height);
}
