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
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern "C" IOService *native_gop_presenter_create(void);
extern "C" bool native_gop_gpu_offer(IOService *, void *, IOMemoryDescriptor *,
    uint64_t, uint32_t, uint32_t, uint32_t);

/* wf129: a coarse read-back of the scanout surface, so "the UI is up" can be
 * judged from the serial log alone. A 12x24 luminance grid, one hex digit per
 * cell, is sampled every sheng-display-grid seconds; read-only, and inactive
 * unless that boot argument is present. */
static uint64_t gridPhysical, gridBytes, gridPeriodMs;
static uint32_t gridWidth, gridHeight, gridStride;

static void gridSampleThread(void *, wait_result_t)
{
    IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withPhysicalAddress(
        gridPhysical, gridBytes, kIODirectionIn);
    if (!descriptor) { printf("PICASSO_GRID: descriptor failed\n"); return; }
    IOMemoryMap *map = descriptor->createMappingInTask(kernel_task, 0,
        kIOMapAnywhere | kIOMapWriteCombineCache);
    descriptor->release();
    if (!map) { printf("PICASSO_GRID: mapping failed\n"); return; }
    volatile const uint8_t *fb = reinterpret_cast<volatile const uint8_t *>(
        static_cast<uintptr_t>(map->getAddress()));
    for (unsigned sample = 1;; ++sample) {
        IOSleep(static_cast<unsigned>(gridPeriodMs));
        printf("PICASSO_GRID frame=%u\n", sample);
        /* wf131: the state line carries submissions/accepted/transfers/copies/vsync_ticks, so
         * the grid run also shows whether the display service is being fed frames at all. */

        for (uint32_t row = 0; row < 24; ++row) {
            char text[13];
            for (uint32_t col = 0; col < 12; ++col) {
                const uint32_t x = (col * gridWidth + gridWidth / 2) / 12;
                const uint32_t y = (row * gridHeight + gridHeight / 2) / 24;
                const volatile uint32_t *pixel =
                    reinterpret_cast<volatile const uint32_t *>(
                        fb + uint64_t(y) * gridStride + uint64_t(x) * 4);
                const uint32_t value = *pixel;
                const uint32_t luma = ((value & 0xff) * 11 +
                    ((value >> 8) & 0xff) * 59 + ((value >> 16) & 0xff) * 30) / 100;
                text[col] = "0123456789abcdef"[luma >> 4];
            }
            text[12] = 0;
            printf("PICASSO_GRID %02u %s\n", row, text);
        }
    }
}

static IOService *gGopPresenter;

static void startGrid() {
    PE_Video video = {};
    if (PE_current_console(&video)) return;
    gridPhysical = video.v_baseAddr;
    gridBytes = video.v_rowBytes * video.v_height;
    gridWidth = video.v_width;
    gridHeight = video.v_height;
    gridStride = video.v_rowBytes;
    uint32_t gridSeconds = 0;
    if (PE_parse_boot_argn("sheng-display-grid", &gridSeconds,
            static_cast<int>(sizeof gridSeconds)) && gridSeconds) {
        gridPeriodMs = gridSeconds > 3600u ? 3600000u : uint64_t(gridSeconds) * 1000u;
        thread_t thread = nullptr;
        if (kernel_thread_start(gridSampleThread, nullptr, &thread) == KERN_SUCCESS)
            thread_deallocate(thread);
        else
            printf("PICASSO_GRID: thread failed\n");
    }
}


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
    startGrid();
    printf("PICASSO_DISPLAY gpu presenter attached\n");
    return true;
}

extern "C" bool native_legacy_gpu_offer(void *display, IOMemoryDescriptor *memory,
    uint64_t stride, uint32_t width, uint32_t height, uint32_t destY) {
    return gGopPresenter &&
        native_gop_gpu_offer(gGopPresenter, display, memory, stride, width, height, destY);
}
