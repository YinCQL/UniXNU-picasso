// SPDX-License-Identifier: BSD-2-Clause-Patent
// Device-side firmware stage of unixnu.com/porting/power. Input is read only;
// then hand off to the ADSP boot transaction, which reopens and revalidates it.
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <libkern/crypto/sha2.h>
#include "PicassoFat.h"
#include "PicassoAdspFirmwareProfile.h"
#include "PicassoAdspLoadPlan.h"
#include "FirmwareWindow.h"
#include "PicassoQmp.h"
#include "PicassoSmp2p.h"
#include "PicassoAdspBoot.h"

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
    uint8_t metadata[picassoAdspMetadataBytes];
    PicassoAdspLoadPlan plan;
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
            if (error) { uint8_t ignored[32]; SHA256_Final(ignored,&hash); return error; }
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
int planFirmware(Firmware *f) {
    static_assert(fileCount == picassoAdspHeaders, "Pinned split file profile changed");
    f->plan = {};
    f->plan.phase = "metadata-read";
    f->plan.index = ~0U;
    // Re-read into owned storage and re-hash it. The earlier FAT/hash survey
    // cannot authorize different metadata read later from the same volume.
    int error = picasso_fat_read(&f->volume, &f->files[fileCount - 1], 0, f->metadata, sizeof f->metadata);
    if (error) return error;
    SHA256_CTX hash; uint8_t digest[32];
    SHA256_Init(&hash); SHA256_Update(&hash, f->metadata, sizeof f->metadata); SHA256_Final(digest, &hash);
    f->plan.phase = "metadata-hash";
    if (memcmp(digest, picassoAdspFiles[fileCount - 1].sha256, sizeof digest)) return -8;
    f->plan.phase = "metadata-split";
    unsigned at = 0;
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t n = f->files[i].size;
        if (n > sizeof f->metadata - at) return -22;
        if ((error = picasso_fat_read(&f->volume, &f->files[i], 0, f->chunk, n))) return error;
        if (memcmp(f->chunk, f->metadata + at, n)) return -8;
        at += n;
    }
    if (at != sizeof f->metadata) return -22;
    uint32_t pieces[picassoAdspHeaders] = {};
    // b00..b32 are present; b33 is pure BSS and must not be read from disk.
    for (unsigned i = 0; i < fileCount - 1; ++i) pieces[i] = f->files[i].size;
    return picasso_adsp_load_plan(f->metadata, sizeof f->metadata, pieces, picassoAdspHeaders, &f->plan);
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
    if (!error) {
        error = planFirmware(f);
        const auto &p = f->plan;
        IOLog("PICASSO_ADSP firmware stage=load-plan error=%d phase=%s segment=%u count=%u entry=%x copy=%u zero=%u image=%u alignment=%x memory_written=0 authenticated=0\n",
            error, p.phase, p.index, p.count, p.entry, p.fileBytes, p.zeroBytes, p.imageBytes, p.alignment);
    }
    if (!error) IOLog("PICASSO_ADSP firmware validated=1 pas=1 carveout=89200000+2800000 pending=power-mailbox-pas pcm=0\n");
    // The boot transaction owns a fresh context, never these stale handles.
    IOFree(f, sizeof(Firmware));
    if (!error && !picasso_aop_probe()) picasso_smp2p_start();
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

// Reopen GPT/FAT and verify all input bytes within the actual PAS transaction.
extern "C" PicassoAdspImage *picasso_adsp_image_open(int *error) {
    if (!error) return nullptr;
    *error = -12;
    auto *f = static_cast<Firmware *>(IOMalloc(sizeof(Firmware)));
    if (!f) return nullptr;
    bzero(f, sizeof(*f));
    *error = native_ufs_modem_open(&f->window, f->chunk, sizeof f->chunk);
    clock_interval_to_deadline(120, kSecondScale, &f->deadline);
    unsigned failed; uint64_t checked;
    if (!*error) *error = validateFirmware(f, (f->window.last-f->window.first+1)*4096ULL, &failed, &checked);
    if (!*error) *error = planFirmware(f);
    if (*error) { IOFree(f, sizeof(*f)); return nullptr; }
    return reinterpret_cast<PicassoAdspImage *>(f);
}
extern "C" const void *picasso_adsp_image_metadata(PicassoAdspImage *image) {
    return image ? reinterpret_cast<Firmware *>(image)->metadata : nullptr;
}
extern "C" int picasso_adsp_image_load(PicassoAdspImage *image, volatile uint8_t *destination, size_t bytes) {
    if (!image || !destination || bytes != picassoAdspBytes) return -22;
    auto *f = reinterpret_cast<Firmware *>(image);
    if(f->plan.count!=32)return -22;
    // Each segment is re-hashed while copying, not authorized by the earlier
    // survey. TZ authentication is never attempted after a changed/short read.
    for (unsigned i = 0; i < f->plan.count; ++i) {
        const auto &s = f->plan.segments[i];
        if (s.memoryOffset > bytes || s.fileBytes > bytes-s.memoryOffset ||
            s.zeroBytes > bytes-s.memoryOffset-s.fileBytes || s.index >= picassoAdspHeaders) return -22;
        SHA256_CTX hash; SHA256_Init(&hash);
        for (uint32_t at = 0; at < s.fileBytes;) {
            uint32_t n = s.fileBytes-at; if (n > sizeof f->chunk) n=sizeof f->chunk;
            int error=picasso_fat_read(&f->volume,&f->files[s.index],at,f->chunk,n);
            if (error) { uint8_t ignored[32]; SHA256_Final(ignored,&hash); return error; }
            SHA256_Update(&hash,f->chunk,n);
            // Device mapping: byte stores avoid unaligned/vector accesses.
            for (uint32_t j=0;j<n;++j) destination[s.memoryOffset+at+j]=f->chunk[j];
            at+=n; IOSleep(1);
        }
        uint8_t digest[32]; SHA256_Final(digest,&hash);
        if (s.fileBytes && memcmp(digest,picassoAdspFiles[s.index].sha256,sizeof digest)) return -8;
        for (uint32_t at=0;at<s.zeroBytes;) {
            if (mach_absolute_time()>=f->deadline) return -60;
            uint32_t n=s.zeroBytes-at;if(n>65536)n=65536;
            for(uint32_t j=0;j<n;++j)destination[s.memoryOffset+s.fileBytes+at+j]=0;
            at+=n;IOSleep(1);
        }
    }
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#endif
    IOLog("PICASSO_ADSP firmware stage=loaded segments=%u copy=%u zero=%u authenticated=0\n",
        f->plan.count,f->plan.fileBytes,f->plan.zeroBytes);
    return 0;
}
extern "C" void picasso_adsp_image_close(PicassoAdspImage *image) {
    if (image) IOFree(reinterpret_cast<Firmware *>(image),sizeof(Firmware));
}
