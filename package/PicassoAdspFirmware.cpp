// SPDX-License-Identifier: BSD-2-Clause-Patent
// Device-side firmware stage of unixnu.com/porting/power. Input is read only;
// then exercise the AOP transport. No power vote or DSP execution yet.
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <libkern/crypto/sha2.h>
#include "PicassoFat.h"
#include "PicassoAdspFirmwareProfile.h"
#include "FirmwareWindow.h"
#include "PicassoQmp.h"

extern "C" unsigned native_ufs_store_ready(void);
extern "C" void picasso_adsp_firmware_start(void);

namespace {
constexpr unsigned fileCount = sizeof(picassoAdspFiles) / sizeof(picassoAdspFiles[0]);
struct Firmware {
    UfsFirmwareWindow window;
    FatVolume volume;
    FatFile files[fileCount];
    uint8_t seen[8192], bounce[4096], chunk[65536];
    uint64_t deadline;
    unsigned calls;
};
int readFirmware(void *context, uint64_t at, void *to, size_t length) {
    auto *f = static_cast<Firmware *>(context);
    if (mach_absolute_time() >= f->deadline) return -1;
    // Yield outside the UFS lock, including long directory/FAT walks.
    if (!(++f->calls % 16)) IOSleep(1);
    auto *out = static_cast<uint8_t *>(to);
    while (length) {
        if (mach_absolute_time() >= f->deadline) return -1;
        uint32_t take = length > 65536 ? 65536 : static_cast<uint32_t>(length);
        if (native_ufs_modem_read(&f->window, at, out, take, f->bounce)) return -1;
        at += take; out += take; length -= take;
    }
    return 0;
}
int validateFirmware(Firmware *f, uint64_t bytes, unsigned *failed, uint64_t *checked) {
    *failed = fileCount; *checked = 0;
    int error = picasso_fat_mount(&f->volume, bytes, readFirmware, f);
    if (error) return error;
    for (unsigned i = 0; i < fileCount; ++i) {
        *failed = i;
        const PicassoAdspFile &expected = picassoAdspFiles[i];
        error = picasso_fat_open(&f->volume, expected.path, &f->files[i]);
        if (error) return error;
        if (f->files[i].size != expected.bytes) return FatRange;
        error = picasso_fat_validate(&f->volume, &f->files[i], f->seen);
        if (error) return error;
        SHA256_CTX hash;
        SHA256_Init(&hash);
        for (uint32_t offset = 0; offset < expected.bytes;) {
            uint32_t take = expected.bytes - offset;
            if (take > sizeof f->chunk) take = sizeof f->chunk;
            error = picasso_fat_read(&f->volume, &f->files[i], offset, f->chunk, take);
            if (error) return error;
            SHA256_Update(&hash, f->chunk, take);
            offset += take; *checked += take;
            IOSleep(1);
        }
        uint8_t digest[32];
        SHA256_Final(digest, &hash);
        if (memcmp(digest, expected.sha256, sizeof digest)) return -8;
    }
    *failed = fileCount;
    return 0;
}
void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "picasso-adsp-fw");
    unsigned ready = 0;
    for (unsigned i = 0; i < 120 && !ready; ++i) {
        ready = native_ufs_store_ready();
        if (!ready) IOSleep(1000);
    }
    if (!ready) {
        IOLog("PICASSO_ADSP firmware source=modem error=-11 stage=root-ready checked=0 authenticated=0\n");
        return;
    }
    auto *f = static_cast<Firmware *>(IOMalloc(sizeof(Firmware)));
    if (!f) { IOLog("PICASSO_ADSP firmware error=-12 stage=allocation\n"); return; }
    bzero(f, sizeof(*f));
    int error = native_ufs_modem_open(&f->window, f->chunk, sizeof f->chunk);
    if (error) {
        IOLog("PICASSO_ADSP firmware source=modem error=%d stage=partition checked=0 authenticated=0\n", error);
        IOFree(f, sizeof(Firmware));
        return;
    }
    uint64_t bytes = (f->window.last - f->window.first + 1) * 4096ULL;
    clock_interval_to_deadline(60, kSecondScale, &f->deadline);
    unsigned failed = fileCount;
    uint64_t checked = 0;
    error = validateFirmware(f, bytes, &failed, &checked);
    IOLog("PICASSO_ADSP firmware source=modem lun=%u first=%llx last=%llx partition_bytes=%llu error=%d file=%s checked=%llu files=%u authenticated=0\n",
        f->window.lun, static_cast<unsigned long long>(f->window.first), static_cast<unsigned long long>(f->window.last),
        static_cast<unsigned long long>(bytes), error, failed < fileCount ? picassoAdspFiles[failed].path : "volume",
        static_cast<unsigned long long>(checked), error ? (failed < fileCount ? failed : 0) : fileCount);
    if (!error) IOLog("PICASSO_ADSP firmware validated=1 pas=1 carveout=89200000+2800000 pending=power-mailbox-pas pcm=0\n");
    // Do not retain tens of MiB or stale handles before the PAS lifecycle exists.
    // A future loader must re-open/revalidate input as part of its own transaction.
    IOFree(f, sizeof(Firmware));
    if (!error) picasso_aop_probe();
}
}

extern "C" void picasso_adsp_firmware_start(void) {
    static unsigned started;
    if (__atomic_exchange_n(&started, 1U, __ATOMIC_ACQ_REL)) return;
    thread_t thread = nullptr;
    kern_return_t error = kernel_thread_start(worker, nullptr, &thread);
    if (error == KERN_SUCCESS) thread_deallocate(thread);
    else IOLog("PICASSO_ADSP firmware error=%x stage=thread\n", error);
}
